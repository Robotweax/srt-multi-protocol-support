// Private implementation included inside adapter.cpp's anonymous namespace.
// No polling coordinator thread. Vendor epoll waits on SRT plus one wake socket.
constexpr uint32_t poll_batch = 256;

#ifdef PROVIDER_HAIVISION
// The pinned native add/update does not signal a blocked waiter by itself.
constexpr bool registration_wake = true;
#else
constexpr bool registration_wake = false; // Native registration wakes the waiter.
#endif
void forget_watcher(srtm_runtime* r, srtm_handle object, srtm_handle poller) {
    auto* e = raw_lookup(r, object); if (!e) return;
    std::lock_guard watch(e->watch);
    if (raw_lookup(r, object) == e) std::erase(e->watchers, poller);
}
// Visits only the pollers subscribed to this object. Callers have already
// stopped new calls on the object, so its watcher list is stable here.
void notify_closed(srtm_runtime* r, srtm_handle h, bool remove) {
    auto* e = raw_lookup(r, h); if (!e) return;
    std::lock_guard watch(e->watch);
    for (const auto ph : e->watchers) {
        auto owner = lookup(r, ph);
        if (!owner || !owner->poller) continue; // Poller is being released.
        auto& p = *owner->poller; std::lock_guard lock(p.mutex);
        auto found = p.subscriptions.find(h); if (found == p.subscriptions.end()) continue;
        auto& sub = found->second;
        if (sub.socket != SRT_INVALID_SOCK) {
            srt_epoll_remove_usock(p.eid, sub.socket); p.sockets.erase(sub.socket);
            sub.socket = SRT_INVALID_SOCK;
            if (!remove) p.terminal.push_back({sizeof(srtm_event_v1),
                SRTM_READY_ERROR | (sub.interests & SRTM_READY_STATE), h, sub.token});
        }
        if (remove) {
            std::erase_if(p.terminal, [h](const auto& event) { return event.object == h; });
            p.subscriptions.erase(found);
        }
        p.wake.signal();
    }
    if (remove) e->watchers.clear();
}
srtm_status poller_create(srtm_runtime* r, srtm_handle* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !out) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid poller arguments", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    auto p = std::make_unique<PollState>();
    p->eid = srt_epoll_create(); if (p->eid < 0) return native_error(d);
    if (!p->wake.open(p->eid)) return error(SRTM_ERR_RESOURCE, "Cannot create native poller wake", d);
    SocketOwner none{SRT_INVALID_SOCK};
    return publish(r, none, 0, 0, 0, 0, false, false, false, out, d, std::move(p));
}
srtm_status poller_register(srtm_runtime* r, srtm_handle ph, srtm_handle h, uint32_t interests, uint64_t token, srtm_error_v1* d) {
    clear(d); auto owner = lookup(r, ph); auto object = lookup(r, h);
    if (!owner || !owner->poller || !object || object->poller)
        return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller or object", d);
    if (!interests || (interests & ~15u)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid interests", d);
    // Record the back-reference first and separately: the object's watch mutex
    // and the poller mutex are never held together on this path.
    bool watching = false;
    {
        std::lock_guard watch(object->watch);
        if (std::find(object->watchers.begin(), object->watchers.end(), ph) == object->watchers.end()) {
            object->watchers.push_back(ph); watching = true;
        }
    }
    struct Undo {
        Entry& object; srtm_handle poller; bool armed;
        ~Undo() { if (armed) { std::lock_guard watch(object.watch); std::erase(object.watchers, poller); } }
    } undo{*object.entry, ph, watching};
    auto& p = *owner->poller; std::lock_guard lock(p.mutex);
    if (p.closing || r->stopping) return error(SRTM_ERR_INVALID_STATE, "Poller stopping", d);
    p.terminal.reserve(p.subscriptions.size()+1); // Close notification cannot allocate.
    auto found = p.subscriptions.find(h);
    int events = SRT_EPOLL_ERR;
    if (interests & SRTM_READY_READ) events |= SRT_EPOLL_IN;
    if (interests & (SRTM_READY_WRITE | SRTM_READY_STATE)) events |= SRT_EPOLL_OUT;
    if (found == p.subscriptions.end()) {
        auto [sub, inserted] = p.subscriptions.emplace(h, Subscription{h, token, interests, object->socket});
        (void)inserted;
        try { p.sockets.emplace(object->socket, &sub->second); }
        catch (...) { p.subscriptions.erase(sub); throw; }
        if (srt_epoll_add_usock(p.eid, object->socket, &events) != 0) {
            p.sockets.erase(object->socket); p.subscriptions.erase(sub); return native_error(d);
        }
    } else {
        if (srt_epoll_update_usock(p.eid, object->socket, &events) != 0) return native_error(d);
        found->second.interests = interests; found->second.token = token;
    }
    undo.armed = false;
    if (registration_wake) p.wake.signal();
    return SRTM_OK;
}
srtm_status poller_remove(srtm_runtime* r, srtm_handle ph, srtm_handle h, srtm_error_v1* d) {
    clear(d); auto owner = lookup(r, ph);
    if (!owner || !owner->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d);
    bool removed = false;
    {
        auto& p = *owner->poller; std::lock_guard lock(p.mutex);
        auto found = p.subscriptions.find(h);
        if (found != p.subscriptions.end()) {
            if (found->second.socket != SRT_INVALID_SOCK) {
                srt_epoll_remove_usock(p.eid, found->second.socket); p.sockets.erase(found->second.socket);
            }
            p.subscriptions.erase(found);
            std::erase_if(p.terminal, [h](const auto& event) { return event.object == h; });
            if (registration_wake) p.wake.signal();
            removed = true;
        }
    }
    if (removed) forget_watcher(r, h, ph);
    return SRTM_OK;
}
srtm_status poller_wake(srtm_runtime* r, srtm_handle ph, srtm_error_v1* d) {
    clear(d); auto owner = lookup(r, ph);
    if (!owner || !owner->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d);
    auto& p = *owner->poller; std::lock_guard lock(p.mutex);
    p.interrupted = true;
    if (!p.wake.signal()) return error(SRTM_ERR_RESOURCE, "Cannot signal poller wake socket", d);
    return SRTM_OK;
}
srtm_status poller_wait(srtm_runtime* r, srtm_handle ph, uint64_t timeout, srtm_event_v1* events,
    uint32_t capacity, uint32_t stride, uint32_t* count, srtm_io_error_v1* io, srtm_error_v1* d) {
    auto owner = lookup(r, ph);
    if (!owner || !owner->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d, io);
    if (!sized(io) || !events || !count || !capacity || stride < sizeof(srtm_event_v1) ||
        stride % alignof(srtm_event_v1) || reinterpret_cast<uintptr_t>(events) % alignof(srtm_event_v1) ||
        (timeout != SRTM_WAIT_FOREVER && timeout > INT_MAX))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid wait arguments", d, io);
    capacity = std::min(capacity, poll_batch);
    auto record = [&](uint32_t index) { return reinterpret_cast<srtm_event_v1*>(reinterpret_cast<char*>(events)+size_t(index)*stride); };
    // Records are checked as they are used, so a wait costs work proportional
    // to the events it reports, not to the caller's capacity. Readiness is
    // level-triggered: rejecting a malformed later record loses nothing.
    auto usable = [&](uint32_t index) { auto* out = record(index); return sized(out) && out->struct_size <= stride; };
    if (!usable(0)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid event record", d, io);
    auto& p = *owner->poller;
    if (p.waiter.test_and_set(std::memory_order_acquire)) return error(SRTM_ERR_BUSY, "Only one waiter per poller", d, io);
    struct WaitGuard { PollState& p; ~WaitGuard() { p.waiter.clear(std::memory_order_release); } } guard{p};
    using Clock = std::chrono::steady_clock;
    const auto deadline = timeout == SRTM_WAIT_FOREVER ? Clock::time_point::max() : Clock::now()+std::chrono::milliseconds(timeout);
    while (true) {
        bool terminal_ready = false;
        {
            std::lock_guard lock(p.mutex);
            if (p.interrupted || p.closing) {
                p.interrupted = false; p.wake.drain(); *count = 0; return SRTM_INTERRUPTED;
            }
            terminal_ready = !p.terminal.empty();
            p.wake.drain();
        }
        int64_t wait_ms = -1;
        if (terminal_ready) wait_ms = 0;
        else if (timeout != SRTM_WAIT_FOREVER) {
            auto remaining = deadline-Clock::now();
            wait_ms = remaining <= Clock::duration::zero() ? 0 : std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        }
        // One native event per ready socket on both vendors; the private wake
        // identity may use one slot and is skipped below.
        int result = srt_epoll_uwait(p.eid, p.native_events.data(), static_cast<int>(capacity), wait_ms);
        if (result < 0) {
            const int code = srt_getlasterror(nullptr);
            if (code != SRT_ETIMEOUT && code != SRT_EASYNCRCV) return native_error(d, io);
            result = 0;
        }
        std::lock_guard lock(p.mutex);
        if (p.interrupted || p.closing) { p.interrupted = false; p.wake.drain(); *count = 0; return SRTM_INTERRUPTED; }
        p.wake.drain(); // Registration and close notifications are not user cancellation.
        uint32_t written = 0; bool malformed = false;
        // A closed object's socket has left the native set, so a registration is
        // reported by exactly one of the two sources: no merging is required.
        auto append = [&](const Subscription& sub, uint32_t ready) {
            if (!ready || written == capacity) return;
            if (!usable(written)) { malformed = true; return; }
            auto* out = record(written++); const auto size = out->struct_size;
            *out = {sizeof(srtm_event_v1), ready, sub.object, sub.token}; out->struct_size = size;
        };
        // Closed objects remain level-ready. Rotate their bounded prefix and
        // alternate priority with native events so neither class can monopolize
        // even a one-slot output. Pending terminal events make the native poll
        // nonblocking; spare slots still include native readiness in this call.
        auto append_terminal = [&] {
            if (p.terminal.empty()) return;
            p.terminal_cursor %= p.terminal.size();
            const auto n = std::min<size_t>(capacity-written, p.terminal.size());
            for (size_t i = 0; i < n; ++i) {
                const auto& event = p.terminal[(p.terminal_cursor+i)%p.terminal.size()];
                auto found = p.subscriptions.find(event.object);
                if (found != p.subscriptions.end()) append(found->second, event.readiness);
            }
            p.terminal_cursor = (p.terminal_cursor+n)%p.terminal.size();
        };
        const bool terminal_first = p.terminal_first;
        if (!p.terminal.empty()) p.terminal_first = !p.terminal_first;
        if (terminal_first) append_terminal();
        for (int i = 0; i < result; ++i) {
            const auto& event = p.native_events[static_cast<size_t>(i)];
            auto found = p.sockets.find(event.fd); if (found == p.sockets.end()) continue; // Private wake identity.
            const auto& sub = *found->second;
            uint32_t ready = 0;
            if (event.events & SRT_EPOLL_IN) ready |= SRTM_READY_READ;
            if (event.events & SRT_EPOLL_OUT) ready |= SRTM_READY_WRITE;
            ready &= sub.interests;
            if (event.events & SRT_EPOLL_ERR) ready |= SRTM_READY_ERROR | (sub.interests & SRTM_READY_STATE);
            else if ((sub.interests & SRTM_READY_STATE) && srt_getsockstate(event.fd) == SRTS_CONNECTED)
                ready |= SRTM_READY_STATE;
            append(sub, ready);
        }
        if (!terminal_first) append_terminal();
        if (malformed) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid event record", d, io);
        if (written) { *count = written; return SRTM_OK; }
        if (timeout != SRTM_WAIT_FOREVER && Clock::now() >= deadline) { *count = 0; return SRTM_TIMED_OUT; }
    }
}
srtm_status poller_release(srtm_runtime* r, srtm_handle ph, srtm_error_v1* d) {
    clear(d); auto* e = raw_lookup(r, ph);
    if (!e) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d);
    std::lock_guard life(e->lifecycle);
    if (raw_lookup(r, ph) != e || !e->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d);
    // Do not retire storage underneath a blocked waiter; wake it and report Busy.
    {
        std::lock_guard lock(e->poller->mutex);
        e->poller->closing = true; e->poller->interrupted = true; e->poller->wake.signal();
    }
    for (auto& gate : e->calls) if (gate.count.load() & ~shut_gate) return error(SRTM_ERR_BUSY, "Poller has active calls", d);
    block_calls(*e); // Also closes the small race with a new call after the check.
    // Exclusive now: drop this poller from every subscribed object's watcher list.
    for (const auto& [object, sub] : e->poller->subscriptions) { (void)sub; forget_watcher(r, object, ph); }
    std::lock_guard registry_lock(r->control);
    e->generation.store(0, std::memory_order_release); e->poller.reset();
    r->free_slots.push_back(static_cast<uint32_t>(ph & 0xffffu)); --r->objects;
    return SRTM_OK;
}
