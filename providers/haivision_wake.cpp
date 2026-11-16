// Private bridge compiled AS PART OF the pinned vendor object target, with its
// exact headers/definitions. No C++ vendor layout crosses into the adapter.
#include "api.h"
#include "core.h"
#include "sync.h"
#include <memory>
#include <set>

namespace {
struct Wake {
    SRTSOCKET socket = SRT_INVALID_SOCK;
    std::set<int> polls;
    ~Wake() { if (socket != SRT_INVALID_SOCK) srt_close(socket); }
};
}
extern "C" void* srtmux_hv_wake_create(int eid) noexcept {
    try {
        std::unique_ptr<Wake> wake(new Wake);
        wake->polls.insert(eid);
        // An unbound INIT socket has no UDP transport or worker of its own.
        // It is exclusively an owned notification identity, never exposed.
        wake->socket = srt_create_socket();
        const int events = SRT_EPOLL_IN;
        if (wake->socket == SRT_INVALID_SOCK || srt_epoll_add_usock(eid, wake->socket, &events) != 0) return nullptr;
        return wake.release();
    } catch (...) { return nullptr; }
}
extern "C" int srtmux_hv_wake_set(void* opaque, int ready) noexcept {
    try {
        Wake& wake = *static_cast<Wake*>(opaque);
        int n = srt::CUDT::uglobal().epoll_ref().update_events(wake.socket, wake.polls, SRT_EPOLL_IN, ready != 0);
        if (n < 0) return -1;
        if (ready) srt::sync::CGlobEvent::triggerEvent();
        return 0;
    } catch (...) { return -1; }
}
extern "C" void srtmux_hv_wake_destroy(void* opaque) noexcept {
    delete static_cast<Wake*>(opaque);
}
