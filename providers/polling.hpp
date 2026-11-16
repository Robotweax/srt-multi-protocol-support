// Private implementation included inside adapter.cpp's anonymous namespace.
// No polling coordinator thread. Vendor epoll waits on SRT plus one wake socket.
constexpr uint32_t poll_batch = 256;

void notify_closed(srtm_runtime* r, srtm_handle h, bool remove) {
    std::lock_guard registry_lock(r->control);
    for (uint32_t i = 0; i < r->next_slot; ++i) {
        auto& slot = (*r->pages[i / page_size].load())[i % page_size];
        if (!slot.generation.load() || !slot.poller) continue;
        auto& p = *slot.poller; std::lock_guard lock(p.mutex);
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
}
srtm_status poller_create(srtm_runtime* r, srtm_handle* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !out) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid poller arguments", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    auto p = std::make_unique<PollState>();
    p->eid = srt_epoll_create(); if (p->eid < 0) return native_error(d);
#ifdef PROVIDER_HAIVISION
    if (!p->wake.open(p->eid)) return error(SRTM_ERR_RESOURCE, "Cannot create native poller wake", d);
#else
    if (!p->wake.open()) return error(SRTM_ERR_RESOURCE, "Cannot create poller wake sockets", d);
    int events = SRT_EPOLL_IN;
    if (srt_epoll_add_ssock(p->eid, p->wake.input, &events) != 0) return native_error(d);
#endif
    SocketOwner none{SRT_INVALID_SOCK};
    return publish(r, none, 0, 0, 0, 0, false, false, false, out, d, std::move(p));
}
srtm_status poller_register(srtm_runtime* r, srtm_handle ph, srtm_handle h, uint32_t interests, uint64_t token, srtm_error_v1* d) {
    clear(d); auto owner = lookup(r, ph); auto object = lookup(r, h);
    if (!owner || !owner->poller || !object || object->poller)
        return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller or object", d);
    if (!interests || (interests & ~15u)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid interests", d);
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
        try { p.sockets.emplace(object->socket, h); }
        catch (...) { p.subscriptions.erase(sub); throw; }
        if (srt_epoll_add_usock(p.eid, object->socket, &events) != 0) {
            p.sockets.erase(object->socket); p.subscriptions.erase(sub); return native_error(d);
        }
    } else {
        if (srt_epoll_update_usock(p.eid, object->socket, &events) != 0) return native_error(d);
        found->second.interests = interests; found->second.token = token;
    }
    p.wake.signal(); return SRTM_OK;
}
srtm_status poller_remove(srtm_runtime* r, srtm_handle ph, srtm_handle h, srtm_error_v1* d) {
    clear(d); auto owner = lookup(r, ph);
    if (!owner || !owner->poller) return error(SRTM_ERR_INVALID_HANDLE, "Invalid poller", d);
    auto& p = *owner->poller; std::lock_guard lock(p.mutex);
    auto found = p.subscriptions.find(h);
    if (found != p.subscriptions.end()) {
        if (found->second.socket != SRT_INVALID_SOCK) {
            srt_epoll_remove_usock(p.eid, found->second.socket); p.sockets.erase(found->second.socket);
        }
        p.subscriptions.erase(found);
        std::erase_if(p.terminal, [h](const auto& event) { return event.object == h; });
        p.wake.signal();
    }
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
    for (uint32_t i = 0; i < capacity; ++i) {
        auto* out = reinterpret_cast<srtm_event_v1*>(reinterpret_cast<char*>(events)+size_t(i)*stride);
        if (!sized(out) || out->struct_size > stride) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid event record", d, io);
    }
    auto& p = *owner->poller;
    if (p.waiter.test_and_set(std::memory_order_acquire)) return error(SRTM_ERR_BUSY, "Only one waiter per poller", d, io);
    struct WaitGuard { PollState& p; ~WaitGuard() { p.waiter.clear(std::memory_order_release); } } guard{p};
    using Clock = std::chrono::steady_clock;
    const auto deadline = timeout == SRTM_WAIT_FOREVER ? Clock::time_point::max() : Clock::now()+std::chrono::milliseconds(timeout);
    auto output = [&](uint32_t index, const srtm_event_v1& value) {
        auto* out = reinterpret_cast<srtm_event_v1*>(reinterpret_cast<char*>(events)+size_t(index)*stride);
        auto size = out->struct_size; *out = value; out->struct_size = size;
    };
    // A bounded, stack-resident merge table avoids per-event allocation and
    // quadratic merging of read/write events. It is independent of registry size.
    struct Merge { srtm_handle handle; uint32_t index; };
    const size_t merge_size = std::bit_ceil(capacity*2u);
    while (true) {
        {
            std::lock_guard lock(p.mutex);
            if (p.interrupted || p.closing) {
                p.interrupted = false; p.wake.drain(); *count = 0; return SRTM_INTERRUPTED;
            }
            if (!p.terminal.empty()) {
                auto n = std::min(capacity, static_cast<uint32_t>(p.terminal.size()));
                for (uint32_t i = 0; i < n; ++i) output(i, p.terminal[i]);
                *count = n; return SRTM_OK;
            }
            p.wake.drain();
        }
#ifndef PROVIDER_HAIVISION
        int rn = static_cast<int>(capacity), wn = static_cast<int>(capacity), sn = 1;
        SYSSOCKET native_ready = SYSSOCKET_INVALID;
#endif
        int64_t wait_ms = -1;
        if (timeout != SRTM_WAIT_FOREVER) {
            auto remaining = deadline-Clock::now();
            wait_ms = remaining <= Clock::duration::zero() ? 0 : std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        }
#ifdef PROVIDER_HAIVISION
        int result = srt_epoll_uwait(p.eid, p.native_events.data(), static_cast<int>(capacity), wait_ms);
#else
        int result = srt_epoll_wait(p.eid, p.reads.data(), &rn, p.writes.data(), &wn,
            wait_ms, &native_ready, &sn, nullptr, nullptr);
#endif
        if (result < 0) {
            const int code = srt_getlasterror(nullptr);
            if (code != SRT_ETIMEOUT && code != SRT_EASYNCRCV) return native_error(d, io);
#ifdef PROVIDER_HAIVISION
            result = 0;
#else
            rn = wn = sn = 0;
#endif
        }
        std::lock_guard lock(p.mutex);
        if (p.interrupted || p.closing) { p.interrupted = false; p.wake.drain(); *count = 0; return SRTM_INTERRUPTED; }
#ifdef PROVIDER_HAIVISION
        p.wake.drain(); // Registration notifications are not user cancellation.
#else
        if (sn) p.wake.drain(true);
#endif
        uint32_t written = 0;
        std::array<Merge, poll_batch*2> merge;
        std::fill_n(merge.begin(), merge_size, Merge{0, 0});
        auto append = [&](const Subscription& sub, uint32_t ready) {
            if (!ready) return;
            size_t key = static_cast<size_t>((sub.object ^ (sub.object >> 16))*UINT64_C(11400714819323198485)) & (merge_size-1);
            while (merge[key].handle && merge[key].handle != sub.object) key = (key+1)&(merge_size-1);
            if (merge[key].handle) {
                auto* out = reinterpret_cast<srtm_event_v1*>(reinterpret_cast<char*>(events)+size_t(merge[key].index)*stride);
                out->readiness |= ready;
            } else if (written < capacity) {
                merge[key] = {sub.object, written};
                output(written++, {sizeof(srtm_event_v1), ready, sub.object, sub.token});
            }
        };
#ifdef PROVIDER_HAIVISION
        for (int i = 0; i < result; ++i) {
            const auto& event = p.native_events[static_cast<size_t>(i)];
            auto found = p.sockets.find(event.fd); if (found == p.sockets.end()) continue; // Private wake identity.
            auto sub = p.subscriptions.find(found->second); if (sub == p.subscriptions.end()) continue;
            uint32_t ready = 0;
            if (event.events & SRT_EPOLL_IN) ready |= SRTM_READY_READ;
            if (event.events & SRT_EPOLL_OUT) ready |= SRTM_READY_WRITE;
            ready &= sub->second.interests;
            if (event.events & SRT_EPOLL_ERR) ready |= SRTM_READY_ERROR | (sub->second.interests & SRTM_READY_STATE);
            else if ((sub->second.interests & SRTM_READY_STATE) && srt_getsockstate(event.fd) == SRTS_CONNECTED)
                ready |= SRTM_READY_STATE;
            append(sub->second, ready);
        }
#else
        auto translate = [&](SRTSOCKET socket, uint32_t direction) {
            auto found = p.sockets.find(socket); if (found == p.sockets.end()) return;
            auto sub = p.subscriptions.find(found->second); if (sub == p.subscriptions.end()) return;
            uint32_t ready = direction & sub->second.interests;
            // Both pinned vendors report ERR in BOTH native sets (we always
            // supply nonempty arrays for both). With an empty opposite set,
            // ordinary readiness needs no additional global socket-state lookup.
            // STATE or mixed batches retain the full query, including overflow
            // and concurrent registration changes. Readiness remains advisory.
            const bool opposite_ready = direction == SRTM_READY_READ ? wn != 0 : rn != 0;
            if ((sub->second.interests & SRTM_READY_STATE) || opposite_ready) {
                const auto state = srt_getsockstate(socket);
                if (state == SRTS_CONNECTED) ready |= sub->second.interests & SRTM_READY_STATE;
                if (state == SRTS_BROKEN || state == SRTS_CLOSING || state == SRTS_CLOSED || state == SRTS_NONEXIST)
                    ready |= SRTM_READY_ERROR | (sub->second.interests & SRTM_READY_STATE);
            }
            append(sub->second, ready);
        };
        for (int i = 0; i < rn; ++i) translate(p.reads[static_cast<size_t>(i)], SRTM_READY_READ);
        for (int i = 0; i < wn; ++i) translate(p.writes[static_cast<size_t>(i)], SRTM_READY_WRITE);
#endif
        for (auto& event : p.terminal) {
            auto found = p.subscriptions.find(event.object);
            if (found != p.subscriptions.end()) append(found->second, event.readiness);
        }
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
    std::lock_guard registry_lock(r->control);
    e->generation.store(0, std::memory_order_release); e->poller.reset();
    r->free_slots.push_back(static_cast<uint32_t>(ph & 0xffffu)); --r->objects;
    return SRTM_OK;
}
