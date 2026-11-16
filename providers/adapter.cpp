// Compiled separately against each vendor. No vendor symbol is exported.
#include <srtmux/provider_abi.h>
#include <srt.h>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace {
constexpr uint32_t page_size = 256, page_count = 256;
struct Entry {
    std::atomic<uint32_t> generation{0};
    SRTSOCKET socket = SRT_INVALID_SOCK;
    uint32_t mode = 0, payload = 0, encryption = 0, mss = 0;
    bool listener = false, ready = false, closed = false, accounting = false;
    alignas(64) std::atomic<uint64_t> tx{0};
    alignas(64) std::atomic<uint64_t> rx{0};
};
using Page = std::array<Entry, page_size>;
std::atomic<uint32_t> generation_source{1};
}
struct srtm_runtime {
    std::array<std::atomic<Page*>, page_count> pages{};
    std::mutex control;
    std::vector<uint32_t> free_slots;
    uint32_t next_slot = 0, objects = 0;
    bool stopping = false;
    ~srtm_runtime() { for (auto& p : pages) delete p.load(); }
};
namespace {
std::atomic<srtm_runtime*> active{nullptr};
std::mutex startup_mutex;

template<class T> bool sized(const T* p) { return p && p->struct_size >= sizeof(T); }
srtm_status error(uint32_t category, const char* message, srtm_error_v1* out,
                  srtm_io_error_v1* io = nullptr, int vendor = 0, int os = 0) {
    const uint32_t flags = (vendor ? 1u : 0u) | (os ? 2u : 0u);
    if (sized(io)) {
        io->category = category; io->detail_flags = flags;
        io->vendor_code = vendor; io->os_code = os; io->rejection_code = 0;
    }
    if (sized(out)) {
        out->category = category; out->detail_flags = flags;
        out->vendor_code = vendor; out->os_code = os; out->rejection_code = 0;
        const auto n = std::strlen(message);
        const auto count = n < sizeof(out->message) - 1 ? n : sizeof(out->message) - 1;
        std::memcpy(out->message, message, count); out->message[count] = 0;
        out->message_length = static_cast<uint32_t>(count);
        out->message_truncated = n != count;
    }
    return SRTM_FAILED;
}
void clear(srtm_error_v1* out) {
    if (sized(out)) { auto size = out->struct_size; *out = {}; out->struct_size = size; }
}
srtm_status native_error(srtm_error_v1* out, srtm_io_error_v1* io = nullptr) {
    int os = 0;
    const int code = srt_getlasterror(&os);
    if (code == SRT_EASYNCSND || code == SRT_EASYNCRCV) return SRTM_WOULD_BLOCK;
    uint32_t category = SRTM_ERR_INTERNAL;
    if (code == SRT_EINVSOCK) category = SRTM_ERR_INVALID_HANDLE;
    else if (code == SRT_EINVPARAM) category = SRTM_ERR_INVALID_ARGUMENT;
    else if (code == SRT_ECONNLOST || code == SRT_ENOCONN) category = SRTM_ERR_CONNECTION_LOST;
    else if (code == SRT_ECONNREJ) category = SRTM_ERR_PEER_REJECTED;
    else if (code == SRT_ESECFAIL) category = SRTM_ERR_AUTHENTICATION;
    else if (code == SRT_ETIMEOUT || code == SRT_ENOSERVER) category = SRTM_ERR_TIMEOUT;
    return error(category, out ? srt_getlasterror_str() : "", out, io, code, os);
}
bool runtime_ok(srtm_runtime* r) { return r && active.load(std::memory_order_acquire) == r; }
Entry* lookup(srtm_runtime* r, srtm_handle h) {
    if (!runtime_ok(r) || (h >> 48) != PROVIDER_TAG) return nullptr;
    const auto index = static_cast<uint32_t>(h & 0xffffu);
    auto* page = r->pages[index / page_size].load(std::memory_order_acquire);
    if (!page) return nullptr;
    auto* e = &(*page)[index % page_size];
    const auto generation = e->generation.load(std::memory_order_acquire);
    return generation != 0 && generation == ((h >> 16) & 0xffffffffu) ? e : nullptr;
}
struct SocketOwner {
    SRTSOCKET value;
    ~SocketOwner() { if (value != SRT_INVALID_SOCK) srt_close(value); }
};
srtm_status publish(srtm_runtime* r, SocketOwner& socket, uint32_t mode,
                    uint32_t payload, uint32_t encryption, uint32_t mss, bool listener, bool ready, bool accounting,
                    srtm_handle* out, srtm_error_v1* detail) {
    std::lock_guard lock(r->control);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime is stopping", detail);
    if (r->free_slots.empty() && r->next_slot == page_size * page_count)
        return error(SRTM_ERR_RESOURCE, "Concurrent handle capacity exhausted", detail);
    // Generation exhaustion permanently fails rather than allowing stale aliasing.
    auto generation = generation_source.load();
    do {
        if (generation == UINT32_MAX) return error(SRTM_ERR_RESOURCE, "Handle generation exhausted", detail);
    } while (!generation_source.compare_exchange_weak(generation, generation + 1));
    const uint32_t index = r->free_slots.empty() ? r->next_slot : r->free_slots.back();
    auto* page = r->pages[index / page_size].load();
    if (!page) {
        auto owned = std::make_unique<Page>();
        page = owned.release();
        r->pages[index / page_size].store(page, std::memory_order_release);
    }
    if (r->free_slots.empty()) ++r->next_slot; else r->free_slots.pop_back();
    auto& e = (*page)[index % page_size];
    e.socket = socket.value; e.mode = mode; e.payload = payload; e.encryption = encryption; e.mss = mss;
    e.listener = listener; e.ready = ready; e.closed = false; e.accounting = accounting;
    e.tx.store(0); e.rx.store(0);
    e.generation.store(generation, std::memory_order_release);
    ++r->objects;
    *out = (uint64_t(PROVIDER_TAG) << 48) | (uint64_t(generation) << 16) | index;
    socket.value = SRT_INVALID_SOCK;
    return SRTM_OK;
}
int address(const srtm_endpoint* ep, sockaddr_storage& out) {
    if (!ep || ep->reserved) return 0;
    std::memset(&out, 0, sizeof(out));
    if (ep->family == SRTM_AF_IPV4) {
        if (ep->scope_id) return 0;
        for (int i = 4; i < 16; ++i) if (ep->address[i]) return 0;
        auto* a = reinterpret_cast<sockaddr_in*>(&out);
        a->sin_family = AF_INET; a->sin_port = htons(ep->port);
        std::memcpy(&a->sin_addr, ep->address, 4); return sizeof(*a);
    }
    if (ep->family == SRTM_AF_IPV6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&out);
        a->sin6_family = AF_INET6; a->sin6_port = htons(ep->port); a->sin6_scope_id = ep->scope_id;
        std::memcpy(&a->sin6_addr, ep->address, 16); return sizeof(*a);
    }
    return 0;
}
srtm_endpoint endpoint(const sockaddr_storage& value) {
    srtm_endpoint out{};
    if (value.ss_family == AF_INET) {
        auto& a = reinterpret_cast<const sockaddr_in&>(value);
        out.family = SRTM_AF_IPV4; out.port = ntohs(a.sin_port);
        std::memcpy(out.address, &a.sin_addr, 4);
    } else if (value.ss_family == AF_INET6) {
        auto& a = reinterpret_cast<const sockaddr_in6&>(value);
        out.family = SRTM_AF_IPV6; out.port = ntohs(a.sin6_port); out.scope_id = a.sin6_scope_id;
        std::memcpy(out.address, &a.sin6_addr, 16);
    }
    return out;
}
bool qualified_address(uint32_t mode, const srtm_endpoint* ep) {
#ifdef PROVIDER_HAIVISION
    // Native 1.5.7 File/IPv6 regression reproduced independently on macOS.
    // Exclude from this initial qualified profile; never fragment/throttle to hide it.
    return !(mode == SRTM_FILE_STREAM && ep && ep->family == SRTM_AF_IPV6);
#else
    (void)mode; (void)ep;
    return true;
#endif
}
const char* invalid_config(const srtm_config_v1* c) {
    if (!sized(c)) return "struct_size";
    if (c->reserved) return "reserved";
    if (c->mode != SRTM_LIVE_MESSAGES && c->mode != SRTM_FILE_STREAM) return "mode";
    if (c->accounting > SRTM_ACCOUNTING_PAYLOAD_BYTES) return "accounting";
    if (c->encryption > SRTM_ENCRYPTION_AES_CTR_REQUIRED) return "encryption";
    if (c->encryption == SRTM_ENCRYPTION_DISABLED && (c->key_bits || c->passphrase.size)) return "encryption";
    if (c->encryption && (c->key_bits != 128 && c->key_bits != 192 && c->key_bits != 256)) return "key_bits";
    if (c->encryption && (!c->passphrase.data || c->passphrase.size < 10 || c->passphrase.size > 79)) return "passphrase";
    if (c->mss_bytes < 76 || c->mss_bytes > 1500) return "mss_bytes";
    if ((c->mode == SRTM_FILE_STREAM && (c->payload_bytes || c->receive_latency_ms || c->peer_latency_ms)) ||
        (c->mode == SRTM_LIVE_MESSAGES && (c->payload_bytes == 0 || c->payload_bytes > c->mss_bytes - 44))) return "payload_bytes";
    if (!c->send_buffer_bytes || c->send_buffer_bytes > INT_MAX || !c->receive_buffer_bytes || c->receive_buffer_bytes > INT_MAX) return "buffers";
    if (!c->connect_timeout_ms || c->connect_timeout_ms > INT_MAX || !c->peer_idle_timeout_ms || c->peer_idle_timeout_ms > INT_MAX ||
        c->receive_latency_ms > INT_MAX || c->peer_latency_ms > INT_MAX) return "timeouts";
    if (c->stream_id.size > 512 || (c->stream_id.size && !c->stream_id.data)) return "stream_id";
    return nullptr;
}
bool configure(SRTSOCKET s, const srtm_config_v1& c) {
    auto set = [s](SRT_SOCKOPT opt, auto value) { return srt_setsockflag(s, opt, &value, sizeof(value)) == 0; };
    if (!set(SRTO_TRANSTYPE, c.mode == SRTM_LIVE_MESSAGES ? SRTT_LIVE : SRTT_FILE) ||
        !set(SRTO_MESSAGEAPI, c.mode == SRTM_LIVE_MESSAGES) || !set(SRTO_SNDSYN, false) || !set(SRTO_RCVSYN, false) ||
        !set(SRTO_MSS, int(c.mss_bytes)) || !set(SRTO_SNDBUF, int(c.send_buffer_bytes)) || !set(SRTO_RCVBUF, int(c.receive_buffer_bytes)) ||
        !set(SRTO_CONNTIMEO, int(c.connect_timeout_ms)) || !set(SRTO_PEERIDLETIMEO, int(c.peer_idle_timeout_ms)) ||
        !set(SRTO_ENFORCEDENCRYPTION, true)) return false;
    linger no_linger{};
    if (!set(SRTO_LINGER, no_linger)) return false;
    if (c.mode == SRTM_LIVE_MESSAGES && (!set(SRTO_PAYLOADSIZE, int(c.payload_bytes)) ||
        !set(SRTO_RCVLATENCY, int(c.receive_latency_ms)) || !set(SRTO_PEERLATENCY, int(c.peer_latency_ms)))) return false;
    if (c.stream_id.size && srt_setsockflag(s, SRTO_STREAMID, c.stream_id.data, int(c.stream_id.size)) != 0) return false;
    if (c.encryption && (!set(SRTO_PBKEYLEN, int(c.key_bits / 8)) ||
        srt_setsockflag(s, SRTO_PASSPHRASE, c.passphrase.data, int(c.passphrase.size)) != 0)) return false;
    return true;
}
template<size_t N> srtm_text text(const char (&s)[N]) { return {s, N - 1}; }
srtm_status provider_info(srtm_provider_info_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!sized(out)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid descriptor buffer", d);
    const auto size = out->struct_size;
    *out = {size, 0, text(PROVIDER_NAME), text(PROVIDER_RELEASE), text(PROVIDER_REVISION),
        text(PROVIDER_FINGERPRINT), text(PROVIDER_PLATFORM), text("static-srt;shared-openssl;basic-io;no-poller")};
    return SRTM_OK;
}
srtm_status runtime_create(const srtm_host_v1* host, srtm_runtime** out, srtm_error_v1* d) {
    clear(d);
    if (!sized(host) || !out || host->reserved) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid runtime arguments", d);
    if (host->log) return error(SRTM_ERR_UNSUPPORTED, "Host logging callback is not implemented", d);
    std::lock_guard lock(startup_mutex);
    if (active.load()) return error(SRTM_ERR_BUSY, "One runtime per provider is supported", d);
    auto r = std::make_unique<srtm_runtime>();
    r->free_slots.reserve(page_size * page_count);
    if (srt_startup() != 0) return native_error(d);
    *out = r.release(); active.store(*out, std::memory_order_release); return SRTM_OK;
}
srtm_status begin_shutdown(srtm_runtime* r, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid runtime", d);
    // Caller has quiesced I/O and lifecycle operations before entering shutdown.
    r->stopping = true;
    for (uint32_t i = 0; i < r->next_slot; ++i) {
        auto& e = (*r->pages[i / page_size].load())[i % page_size];
        if (e.generation.load() && !e.closed) {
            if (srt_close(e.socket) != 0) return native_error(d);
            e.closed = true; e.ready = false;
        }
    }
    return SRTM_OK;
}
srtm_status shutdown_status(srtm_runtime* r, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid runtime", d);
    return r->stopping ? SRTM_OK : SRTM_PENDING;
}
srtm_status runtime_destroy(srtm_runtime* r, srtm_error_v1* d) {
    clear(d); std::lock_guard lock(startup_mutex);
    if (!runtime_ok(r)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid runtime", d);
    if (!r->stopping || r->objects) return error(SRTM_ERR_BUSY, "Shutdown and release objects first", d);
    if (srt_cleanup() != 0) return native_error(d);
    active.store(nullptr); delete r; return SRTM_OK;
}
srtm_status clock_now(srtm_runtime* r, uint64_t* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !out) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid clock arguments", d);
    *out = static_cast<uint64_t>(srt_time_now()); return SRTM_OK;
}
srtm_status validate(srtm_runtime* r, uint32_t role, const srtm_config_v1* c,
    srtm_validation_issue_v1* issues, uint32_t capacity, uint32_t stride, uint32_t* count, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !count || (role != 1 && role != 2)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid validation arguments", d);
    const char* invalid = invalid_config(c);
    if (!invalid) { *count = 0; return SRTM_OK; }
    if (!capacity) { *count = 1; return SRTM_BUFFER_TOO_SMALL; }
    if (!sized(issues) || !sized(&issues->detail) || stride < issues->struct_size)
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid issue buffer", d);
    issues->severity = 1;
    std::memset(issues->field, 0, sizeof(issues->field));
    std::memcpy(issues->field, invalid, std::strlen(invalid));
    error(SRTM_ERR_INVALID_ARGUMENT, "Unsupported or invalid configuration field", &issues->detail);
    *count = 1; return SRTM_OK;
}
srtm_status capability(srtm_runtime* r, const srtm_capability_query_v1* q, srtm_capability_v1* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !sized(q) || !sized(out) || !q->feature.data || q->feature.size > 128 || (q->role != 1 && q->role != 2))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid capability arguments", d);
    const std::string_view name(q->feature.data, static_cast<size_t>(q->feature.size));
    srtm_endpoint v6{}; v6.family = SRTM_AF_IPV6;
    const bool supported = !invalid_config(q->config) && (name != "core.ipv6" || qualified_address(q->config->mode, &v6)) && (name == "core.ipv4" || name == "core.ipv6" ||
        name == "core.live_messages" || name == "core.file_stream" || name == "crypto.aes_ctr" || name == "core.statistics");
    const auto size = out->struct_size;
    *out = {size, supported ? SRTM_CAP_AVAILABLE : SRTM_CAP_UNSUPPORTED,
        supported && name != "core.statistics" ? 1u : 0u, 0,
        text("Basic I/O; caller-serialized lifecycle; no poller, callbacks or drain-close")};
    return SRTM_OK;
}
srtm_status connection_create(srtm_runtime* r, const srtm_config_v1* c, srtm_handle* out, srtm_error_v1* d) {
    clear(d);
    if (!runtime_ok(r) || !out || invalid_config(c)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid connection configuration", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    SocketOwner socket{srt_create_socket()};
    if (socket.value == SRT_INVALID_SOCK || !configure(socket.value, *c)) return native_error(d);
    return publish(r, socket, c->mode, c->payload_bytes, c->encryption, c->mss_bytes, false, false, c->accounting != 0, out, d);
}
srtm_status bind(srtm_runtime* r, srtm_handle h, const srtm_endpoint* ep, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h); sockaddr_storage addr{}; const auto len = address(ep, addr);
    if (!e || e->closed) return error(SRTM_ERR_INVALID_HANDLE, "Invalid connection", d);
    if (!len || e->listener) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid bind arguments", d);
    if (e->mode == SRTM_LIVE_MESSAGES && ep->family == SRTM_AF_IPV6 && e->payload > e->mss - 64) return error(SRTM_ERR_INVALID_ARGUMENT, "Payload exceeds IPv6 MSS budget", d);
    if (!qualified_address(e->mode, ep)) return error(SRTM_ERR_UNSUPPORTED, "Haivision File/IPv6 is not qualified", d);
    return srt_bind(e->socket, reinterpret_cast<sockaddr*>(&addr), len) == 0 ? SRTM_OK : native_error(d);
}
srtm_status connect(srtm_runtime* r, srtm_handle h, const srtm_endpoint* ep, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h); sockaddr_storage addr{}; const auto len = address(ep, addr);
    if (!e || e->closed) return error(SRTM_ERR_INVALID_HANDLE, "Invalid connection", d);
    if (!len || !ep->port || e->listener) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid connect arguments", d);
    if (e->mode == SRTM_LIVE_MESSAGES && ep->family == SRTM_AF_IPV6 && e->payload > e->mss - 64) return error(SRTM_ERR_INVALID_ARGUMENT, "Payload exceeds IPv6 MSS budget", d);
    if (!qualified_address(e->mode, ep)) return error(SRTM_ERR_UNSUPPORTED, "Haivision File/IPv6 is not qualified", d);
    if (srt_connect(e->socket, reinterpret_cast<sockaddr*>(&addr), len) != 0) return native_error(d);
    return SRTM_PENDING; // connection_info finalizes negotiated metadata before I/O.
}
srtm_status info(srtm_runtime* r, srtm_handle h, srtm_connection_info_v1* out, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e) return error(SRTM_ERR_INVALID_HANDLE, "Invalid connection", d);
    if (!sized(out) || !sized(&out->terminal_error)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid info buffer", d);
    srtm_connection_info_v1 value{}; value.struct_size = out->struct_size;
    value.terminal_error.struct_size = out->terminal_error.struct_size;
    auto state = e->closed ? SRTS_CLOSED : srt_getsockstate(e->socket);
    switch (state) {
        case SRTS_INIT: value.state = SRTM_CREATED; break;
        case SRTS_OPENED: value.state = SRTM_BOUND; break;
        case SRTS_LISTENING: value.state = SRTM_LISTENING; break;
        case SRTS_CONNECTING: value.state = SRTM_CONNECTING; break;
        case SRTS_CONNECTED: value.state = SRTM_CONNECTED; break;
        case SRTS_CLOSING: value.state = SRTM_CLOSING; break;
        case SRTS_CLOSED: value.state = SRTM_CLOSED; break;
        default: value.state = SRTM_FAILED_STATE; break;
    }
    if (!e->closed) {
        sockaddr_storage local{}, remote{}; int size = sizeof(local);
        if (srt_getsockname(e->socket, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
            value.local = endpoint(local); value.valid_fields |= 1;
        }
        size = sizeof(remote);
        if (srt_getpeername(e->socket, reinterpret_cast<sockaddr*>(&remote), &size) == 0) {
            value.remote = endpoint(remote); value.valid_fields |= 2;
        }
    }
    if (state == SRTS_CONNECTED) {
        if (!e->ready && e->mode == SRTM_LIVE_MESSAGES) {
            int payload = 0, size = sizeof(payload);
            if (srt_getsockflag(e->socket, SRTO_PAYLOADSIZE, &payload, &size) != 0 || payload <= 0) return native_error(d);
            e->payload = static_cast<uint32_t>(payload);
        }
        e->ready = true;
        int key = 0, size = sizeof(key);
        if (srt_getsockflag(e->socket, SRTO_PBKEYLEN, &key, &size) == 0) {
            int km = 0; size = sizeof(km);
            int recv_km = 0;
            if (srt_getsockflag(e->socket, SRTO_SNDKMSTATE, &km, &size) == 0) {
                size = sizeof(recv_km);
                if (srt_getsockflag(e->socket, SRTO_RCVKMSTATE, &recv_km, &size) == 0) {
                    const bool secured = km == SRT_KM_S_SECURED || recv_km == SRT_KM_S_SECURED;
                    if (secured || (e->encryption == SRTM_ENCRYPTION_DISABLED && km == SRT_KM_S_UNSECURED && recv_km == SRT_KM_S_UNSECURED)) {
                        value.encryption = secured ? SRTM_ENCRYPTION_AES_CTR_REQUIRED : SRTM_ENCRYPTION_DISABLED;
                        value.key_bits = secured ? static_cast<uint32_t>(key * 8) : 0; value.valid_fields |= 8;
                    }
                }
            }
        }
        int latency = 0, peer_latency = 0; size = sizeof(latency);
        if (srt_getsockflag(e->socket, SRTO_RCVLATENCY, &latency, &size) == 0) {
            size = sizeof(peer_latency);
            if (srt_getsockflag(e->socket, SRTO_PEERLATENCY, &peer_latency, &size) == 0) {
                value.receive_latency_ms = static_cast<uint32_t>(latency); value.peer_latency_ms = static_cast<uint32_t>(peer_latency); value.valid_fields |= 16;
            }
        }
        if (e->mode == SRTM_LIVE_MESSAGES) { value.max_message_bytes = e->payload; value.valid_fields |= 4; }
    }
    if (value.state == SRTM_FAILED_STATE) {
        error(SRTM_ERR_CONNECTION_LOST, "SRT connection failed", &value.terminal_error);
        const int reason = srt_getrejectreason(e->socket);
        if (reason > 0) {
            value.terminal_error.category = SRTM_ERR_PEER_REJECTED;
            value.terminal_error.detail_flags |= 4; value.terminal_error.rejection_code = reason;
        }
    }
    *out = value; return SRTM_OK;
}
srtm_status listener_create(srtm_runtime* r, const srtm_listener_config_v1* c, srtm_handle* out, srtm_error_v1* d) {
    clear(d); sockaddr_storage addr{};
    if (!runtime_ok(r) || !sized(c) || !out || invalid_config(&c->transport) || !c->backlog || c->backlog > INT_MAX)
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid listener configuration", d);
    if (c->admit) return error(SRTM_ERR_UNSUPPORTED, "Admission callbacks not implemented", d);
    if (c->transport.mode == SRTM_LIVE_MESSAGES && c->local.family == SRTM_AF_IPV6 && c->transport.payload_bytes > c->transport.mss_bytes - 64) return error(SRTM_ERR_INVALID_ARGUMENT, "Payload exceeds IPv6 MSS budget", d);
    if (!qualified_address(c->transport.mode, &c->local)) return error(SRTM_ERR_UNSUPPORTED, "Haivision File/IPv6 is not qualified", d);
    const auto len = address(&c->local, addr);
    if (!len) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid listener address", d);
    if (r->stopping) return error(SRTM_ERR_INVALID_STATE, "Runtime stopping", d);
    SocketOwner socket{srt_create_socket()};
    if (socket.value == SRT_INVALID_SOCK || !configure(socket.value, c->transport) ||
        srt_bind(socket.value, reinterpret_cast<sockaddr*>(&addr), len) != 0 || srt_listen(socket.value, int(c->backlog)) != 0) return native_error(d);
    return publish(r, socket, c->transport.mode, c->transport.payload_bytes, c->transport.encryption, c->transport.mss_bytes, true, false, c->transport.accounting != 0, out, d);
}
srtm_status listener_endpoint(srtm_runtime* r, srtm_handle h, srtm_endpoint* out, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e || e->closed || !e->listener) return error(SRTM_ERR_INVALID_HANDLE, "Invalid listener", d);
    if (!out) return error(SRTM_ERR_INVALID_ARGUMENT, "Null endpoint", d);
    sockaddr_storage addr{}; int size = sizeof(addr);
    if (srt_getsockname(e->socket, reinterpret_cast<sockaddr*>(&addr), &size) != 0) return native_error(d);
    *out = endpoint(addr); return SRTM_OK;
}
srtm_status accept(srtm_runtime* r, srtm_handle h, srtm_handle* out, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e || e->closed || !e->listener) return error(SRTM_ERR_INVALID_HANDLE, "Invalid listener", d);
    if (!out) return error(SRTM_ERR_INVALID_ARGUMENT, "Null accepted handle", d);
    SocketOwner socket{srt_accept(e->socket, nullptr, nullptr)};
    if (socket.value == SRT_INVALID_SOCK) return native_error(d);
    return publish(r, socket, e->mode, e->payload, e->encryption, e->mss, false, false, e->accounting, out, d);
}
Entry* io_entry(srtm_runtime* r, srtm_handle h, uint32_t mode, srtm_io_error_v1* io, srtm_error_v1* d) {
    if (!sized(io)) { error(SRTM_ERR_INVALID_ARGUMENT, "Invalid I/O error buffer", d); return nullptr; }
    auto* e = lookup(r, h);
    if (!e || e->closed || e->listener) { error(SRTM_ERR_INVALID_HANDLE, "Invalid connection", d, io); return nullptr; }
    if (e->mode != mode || !e->ready) { error(SRTM_ERR_INVALID_STATE, "Wrong mode or call connection_info until connected before I/O", d, io); return nullptr; }
    return e;
}
srtm_status send_message(srtm_runtime* r, srtm_handle h, srtm_bytes buffer, const srtm_message_info_v1* m, srtm_io_error_v1* io, srtm_error_v1* d) {
    auto* e = io_entry(r, h, SRTM_LIVE_MESSAGES, io, d); if (!e) return SRTM_FAILED;
    if (!buffer.data || !buffer.size || buffer.size > e->payload || (m && (!sized(m) || (m->valid_fields & ~1u) || ((m->valid_fields & 1) && m->source_time_us < 0))))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid message", d, io);
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    if (m && (m->valid_fields & 1)) ctrl.srctime = m->source_time_us;
    const int n = srt_sendmsg2(e->socket, reinterpret_cast<const char*>(buffer.data), int(buffer.size), &ctrl);
    if (n < 0) return native_error(d, io);
    if (static_cast<uint64_t>(n) != buffer.size) return error(SRTM_ERR_INTERNAL, "Unexpected partial message send", d, io);
    if (e->accounting) e->tx.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return SRTM_OK;
}
srtm_status receive_message(srtm_runtime* r, srtm_handle h, srtm_mut_bytes buffer, uint64_t* received, srtm_message_info_v1* m, srtm_io_error_v1* io, srtm_error_v1* d) {
    auto* e = io_entry(r, h, SRTM_LIVE_MESSAGES, io, d); if (!e) return SRTM_FAILED;
    if (!buffer.data || buffer.size < e->payload || buffer.size > INT_MAX || !received || !sized(m))
        return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid receive buffer", d, io);
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    const int n = srt_recvmsg2(e->socket, reinterpret_cast<char*>(buffer.data), int(buffer.size), &ctrl);
    if (n < 0) return native_error(d, io);
    *received = static_cast<uint64_t>(n); m->valid_fields = 1; m->source_time_us = ctrl.srctime;
    if (e->accounting) e->rx.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return SRTM_OK;
}
srtm_status stream_io(srtm_runtime* r, srtm_handle h, const uint8_t* data, uint8_t* writable, uint64_t size, uint64_t* count,
                      srtm_io_error_v1* io, srtm_error_v1* d, bool send) {
    auto* e = io_entry(r, h, SRTM_FILE_STREAM, io, d); if (!e) return SRTM_FAILED;
    if (!count || size > INT_MAX || (size && !(send ? data : writable))) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid stream buffer", d, io);
    if (!size) { *count = 0; return SRTM_OK; }
    int n = send ? srt_send(e->socket, reinterpret_cast<const char*>(data), int(size)) : srt_recv(e->socket, reinterpret_cast<char*>(writable), int(size));
    if (n < 0) return native_error(d, io);
    if (!send && n == 0) return SRTM_END_OF_STREAM;
    *count = static_cast<uint64_t>(n);
    if (e->accounting) (send ? e->tx : e->rx).fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return SRTM_OK;
}
srtm_status write_stream(srtm_runtime* r, srtm_handle h, srtm_bytes b, uint64_t* n, srtm_io_error_v1* io, srtm_error_v1* d) { return stream_io(r, h, b.data, nullptr, b.size, n, io, d, true); }
srtm_status read_stream(srtm_runtime* r, srtm_handle h, srtm_mut_bytes b, uint64_t* n, srtm_io_error_v1* io, srtm_error_v1* d) { return stream_io(r, h, nullptr, b.data, b.size, n, io, d, false); }
srtm_status statistics(srtm_runtime* r, srtm_handle h, srtm_statistics_v1* out, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e || e->closed || e->listener) return error(SRTM_ERR_INVALID_HANDLE, "Invalid connection", d);
    if (!sized(out)) return error(SRTM_ERR_INVALID_ARGUMENT, "Invalid statistics buffer", d);
    srtm_statistics_v1 value{}; value.struct_size = out->struct_size;
    value.counter_epoch = h; value.sample_time_us = static_cast<uint64_t>(srt_time_now());
    if (e->accounting) {
        value.valid_fields = 3; value.accepted_tx_bytes = e->tx.load(std::memory_order_relaxed); value.delivered_rx_bytes = e->rx.load(std::memory_order_relaxed);
    }
    *out = value; return SRTM_OK;
}
srtm_status close(srtm_runtime* r, srtm_handle h, uint32_t mode, uint64_t timeout, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e) return error(SRTM_ERR_INVALID_HANDLE, "Invalid object", d);
    if (mode != SRTM_CLOSE_ABORT || timeout) return error(SRTM_ERR_UNSUPPORTED, "Only abort-close is implemented", d);
    if (e->closed) return SRTM_OK;
    if (srt_close(e->socket) != 0) return native_error(d);
    e->closed = true; e->ready = false; return SRTM_OK;
}
srtm_status close_status(srtm_runtime* r, srtm_handle h, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e) return error(SRTM_ERR_INVALID_HANDLE, "Invalid object", d);
    return e->closed ? SRTM_OK : SRTM_PENDING;
}
srtm_status release(srtm_runtime* r, srtm_handle h, srtm_error_v1* d) {
    clear(d); auto* e = lookup(r, h);
    if (!e) return error(SRTM_ERR_INVALID_HANDLE, "Invalid object", d);
    if (!e->closed) return error(SRTM_ERR_BUSY, "Close before release", d);
    std::lock_guard lock(r->control);
    e->generation.store(0, std::memory_order_release);
    r->free_slots.push_back(static_cast<uint32_t>(h & 0xffffu)); --r->objects;
    return SRTM_OK;
}
srtm_status extension(srtm_runtime*, srtm_text, uint32_t, uint32_t, const srtm_extension_header_v1** out, srtm_error_v1* d) {
    if (out) *out = nullptr;
    return error(SRTM_ERR_UNSUPPORTED, "No extensions in the basic adapter", d);
}
// Cold-path allocation failures and unexpected vendor exceptions never cross C.
template<class T> void compact_exception(T, uint32_t) {}
void compact_exception(srtm_io_error_v1* io, uint32_t category) { error(category, "Provider exception", nullptr, io); }
template<auto Fn> struct Guard;
template<class... Args, srtm_status (*Fn)(Args...)> struct Guard<Fn> {
    static srtm_status SRTM_CALL call(Args... args) noexcept {
        try { return Fn(args...); }
        catch (const std::bad_alloc&) {
            (compact_exception(args, SRTM_ERR_RESOURCE), ...);
            return error(SRTM_ERR_RESOURCE, "Provider allocation failed", std::get<sizeof...(Args)-1>(std::tuple{args...}));
        } catch (...) {
            (compact_exception(args, SRTM_ERR_INTERNAL), ...);
            return error(SRTM_ERR_INTERNAL, "Provider exception", std::get<sizeof...(Args)-1>(std::tuple{args...}));
        }
    }
};
template<class> struct Unsupported;
template<class... Args> struct Unsupported<srtm_status (SRTM_CALL *)(Args...)> {
    static srtm_status SRTM_CALL call(Args... args) {
        (compact_exception(args, SRTM_ERR_UNSUPPORTED), ...);
        return error(SRTM_ERR_UNSUPPORTED, "Operation not implemented in basic adapter", std::get<sizeof...(Args)-1>(std::tuple{args...}));
    }
};
const srtm_provider_api_v1 table = [] {
    srtm_provider_api_v1 a{}; a.struct_size = sizeof(a); a.abi_major = SRTM_ABI_MAJOR; a.abi_minor = SRTM_ABI_MINOR;
#define SET(field, fn) a.field = &Guard<fn>::call
    SET(provider_info, provider_info); SET(runtime_create, runtime_create); SET(runtime_begin_shutdown, begin_shutdown);
    SET(runtime_shutdown_status, shutdown_status); SET(runtime_destroy, runtime_destroy); SET(clock_now_us, clock_now);
    SET(capability, capability); SET(validate, validate); SET(connection_create, connection_create);
    SET(connection_bind, bind); SET(connection_connect, connect); SET(connection_info, info);
    SET(listener_create, listener_create); SET(listener_endpoint, listener_endpoint); SET(listener_accept, accept);
    SET(send_message, send_message); SET(receive_message, receive_message); SET(write_stream, write_stream); SET(read_stream, read_stream);
    SET(statistics, statistics); SET(object_close, close); SET(object_close_status, close_status); SET(object_release, release);
    SET(query_extension, extension);
#undef SET
#define UNSUPPORTED(field) a.field = &Unsupported<decltype(a.field)>::call
    UNSUPPORTED(poller_create); UNSUPPORTED(poller_register); UNSUPPORTED(poller_remove);
    UNSUPPORTED(poller_wait); UNSUPPORTED(poller_wake); UNSUPPORTED(poller_release);
#undef UNSUPPORTED
    return a;
}();
}
extern "C" SRTM_EXPORT srtm_status SRTM_CALL srtm_provider_get_api(uint32_t major, uint32_t minor,
    const srtm_provider_api_v1** out, srtm_error_v1* d) {
    clear(d);
    if (!out) return error(SRTM_ERR_INVALID_ARGUMENT, "Null API output", d);
    *out = nullptr;
    if (major != SRTM_ABI_MAJOR || minor > SRTM_ABI_MINOR) return error(SRTM_ERR_ABI_MISMATCH, "Unsupported ABI", d);
    *out = &table; return SRTM_OK;
}
