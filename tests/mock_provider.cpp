// Test fixture only. This module is not an SRT implementation.
#include <srtmux/provider_abi.h>
#include <atomic>
#include <cstddef>
#include <new>

#ifndef FIXTURE_NAME
# define FIXTURE_NAME "fixture"
#endif
#ifndef FIXTURE_VALUE
# define FIXTURE_VALUE 7
#endif

// Deliberately colliding vendor-like names in independent modules. Export maps
// must localize these symbols, and each module's clock must resolve its own one.
extern "C" int srt_getversion() { return FIXTURE_VALUE; }

struct srtm_runtime { bool closing = false; };

namespace {
std::atomic<bool> active{false};

template<class> struct Unsupported;
template<class... Args>
struct Unsupported<srtm_status (SRTM_CALL *)(Args...)> {
    static srtm_status SRTM_CALL call(Args...) { return SRTM_FAILED; }
};

template<std::size_t N>
srtm_text text(const char (&value)[N]) { return {value, N - 1}; }

srtm_status SRTM_CALL info(srtm_provider_info_v1* out, srtm_error_v1*) {
    if (!out || out->struct_size < sizeof(*out)) return SRTM_FAILED;
    *out = {sizeof(*out), 0, text(FIXTURE_NAME), text("test-1"), text("fixture-source"),
        text("fixture-build"), text("test-platform"), text("test-only")};
#if defined(FIXTURE_BAD_DESCRIPTOR)
    out->implementation.data = nullptr;
#endif
    return SRTM_OK;
}

srtm_status SRTM_CALL create(const srtm_host_v1*, srtm_runtime** out, srtm_error_v1* error) {
    if (active.exchange(true)) {
        if (error) error->category = SRTM_ERR_BUSY;
        return SRTM_FAILED;
    }
    *out = new (std::nothrow) srtm_runtime;
    if (!*out) {
        active = false;
        return SRTM_FAILED;
    }
    return SRTM_OK;
}
srtm_status SRTM_CALL shutdown(srtm_runtime* runtime, srtm_error_v1*) {
    runtime->closing = true;
    return SRTM_OK;
}
srtm_status SRTM_CALL shutdown_status(srtm_runtime* runtime, srtm_error_v1*) {
    return runtime->closing ? SRTM_OK : SRTM_PENDING;
}
srtm_status SRTM_CALL destroy(srtm_runtime* runtime, srtm_error_v1*) {
    if (!runtime->closing) return SRTM_FAILED;
    delete runtime;
    active = false;
    return SRTM_OK;
}
srtm_status SRTM_CALL clock_now(srtm_runtime*, uint64_t* out, srtm_error_v1*) {
    *out = static_cast<uint64_t>(srt_getversion());
    return SRTM_OK;
}

const srtm_provider_api_v1 table = [] {
    srtm_provider_api_v1 api{};
    api.struct_size = sizeof(api);
    api.abi_major = SRTM_ABI_MAJOR;
    api.abi_minor = SRTM_ABI_MINOR;
#define SLOT(field) api.field = &Unsupported<decltype(api.field)>::call
    SLOT(provider_info);
    SLOT(runtime_create);
    SLOT(runtime_begin_shutdown);
    SLOT(runtime_shutdown_status);
    SLOT(runtime_destroy);
    SLOT(clock_now_us);
    SLOT(capability);
    SLOT(validate);
    SLOT(connection_create);
    SLOT(connection_bind);
    SLOT(connection_connect);
    SLOT(connection_info);
    SLOT(listener_create);
    SLOT(listener_endpoint);
    SLOT(listener_accept);
    SLOT(send_message);
    SLOT(receive_message);
    SLOT(write_stream);
    SLOT(read_stream);
    SLOT(statistics);
    SLOT(object_close);
    SLOT(object_close_status);
    SLOT(object_release);
    SLOT(poller_create);
    SLOT(poller_register);
    SLOT(poller_remove);
    SLOT(poller_wait);
    SLOT(poller_wake);
    SLOT(poller_release);
    SLOT(query_extension);
#undef SLOT
    api.provider_info = info;
    api.runtime_create = create;
    api.runtime_begin_shutdown = shutdown;
    api.runtime_shutdown_status = shutdown_status;
    api.runtime_destroy = destroy;
    api.clock_now_us = clock_now;
#if defined(FIXTURE_SHORT)
    api.struct_size = offsetof(srtm_provider_api_v1, read_stream);
#endif
#if defined(FIXTURE_NULL_SLOT)
    api.read_stream = nullptr;
#endif
#if defined(FIXTURE_WRONG_ABI)
    api.abi_major = 99;
#endif
#if defined(FIXTURE_FUTURE)
    api.abi_minor = 12;
#endif
    return api;
}();
} // namespace

extern "C" SRTM_EXPORT srtm_status SRTM_CALL srtm_provider_get_api(
    uint32_t major, uint32_t minor, const srtm_provider_api_v1** out, srtm_error_v1* error) {
    *out = nullptr;
    if (major != SRTM_ABI_MAJOR || minor > table.abi_minor) {
        if (error) error->category = SRTM_ERR_ABI_MISMATCH;
        return SRTM_FAILED;
    }
#if defined(FIXTURE_BOOTSTRAP_FAILURE)
    if (error) error->category = SRTM_ERR_RESOURCE;
    return SRTM_FAILED;
#elif defined(FIXTURE_NULL_TABLE)
    return SRTM_OK;
#else
    *out = &table;
    return SRTM_OK;
#endif
}
