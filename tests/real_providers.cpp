#include <future>
#include <atomic>
#include <srtmux/module.hpp>
#include <srtmux/extensions.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using srtmux::host::Module;
#if defined(__SANITIZE_ADDRESS__)
constexpr bool instrumented = true; // Sanitizer builds are not timed.
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool instrumented = true;
#else
constexpr bool instrumented = false;
#endif
#else
constexpr bool instrumented = false;
#endif
void require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
template<class T> T record() { T value{}; value.struct_size = sizeof(value); return value; }
struct Session {
    Module module;
    const srtm_provider_api_v1& api;
    srtm_runtime* runtime = nullptr;
    const srtmx_profiles_api_v1* profiles = nullptr;
    const srtmx_telemetry_api_v1* telemetry = nullptr;
    srtm_error_v1 error = record<srtm_error_v1>();
    std::vector<srtm_handle> objects;
    static Module load(const char* path, const char* name, const char* release) {
        auto result = Module::load({path, name, release, {}});
        if (auto* e = std::get_if<srtmux::host::LoadError>(&result)) throw std::runtime_error(e->message);
        return std::get<Module>(std::move(result));
    }
    Session(const char* path, const char* name, const char* release)
        : module(load(path, name, release)), api(module.api()) {
        auto host = record<srtm_host_v1>();
        check(api.runtime_create(&host, &runtime, &error));
        const srtm_extension_header_v1* table = nullptr;
        check(api.query_extension(runtime, {SRTMX_PROFILES, sizeof(SRTMX_PROFILES)-1}, 1, 0, &table, &error));
        profiles = reinterpret_cast<const srtmx_profiles_api_v1*>(table);
        check(api.query_extension(runtime, {SRTMX_TELEMETRY, sizeof(SRTMX_TELEMETRY)-1}, 1, 0, &table, &error));
        telemetry = reinterpret_cast<const srtmx_telemetry_api_v1*>(table);
    }
    void check(srtm_status status) {
        if (status != SRTM_OK) throw std::runtime_error(module.info().implementation + ": " + error.message + " (status " + std::to_string(status) + ")");
    }
    srtm_handle connection(const srtm_config_v1& cfg) {
        srtm_handle h = 0; check(api.connection_create(runtime, &cfg, &h, &error)); objects.push_back(h); return h;
    }
    void drop(srtm_handle h) {
        check(api.object_close(runtime, h, SRTM_CLOSE_ABORT, 0, &error));
        check(api.object_close(runtime, h, SRTM_CLOSE_ABORT, 0, &error));
        check(api.object_release(runtime, h, &error));
        objects.erase(std::find(objects.begin(), objects.end(), h));
    }
    void shutdown() {
        if (!runtime) return;
        while (!objects.empty()) drop(objects.back());
        check(api.runtime_begin_shutdown(runtime, &error));
        check(api.runtime_shutdown_status(runtime, &error));
        check(api.runtime_destroy(runtime, &error)); runtime = nullptr;
    }
    ~Session() { try { shutdown(); } catch (...) { std::terminate(); } }
};
srtm_config_v1 config(uint32_t mode, bool encrypted, bool accounting = false) {
    auto c = record<srtm_config_v1>(); c.mode = mode;
    c.accounting = accounting ? SRTM_ACCOUNTING_PAYLOAD_BYTES : SRTM_ACCOUNTING_DISABLED;
    c.encryption = encrypted ? SRTM_ENCRYPTION_AES_CTR_REQUIRED : SRTM_ENCRYPTION_DISABLED;
    c.key_bits = encrypted ? 128 : 0;
    static constexpr uint8_t secret[] = "test-only-secret-passphrase";
    if (encrypted) c.passphrase = {secret, sizeof(secret) - 1};
    c.receive_latency_ms = mode == SRTM_LIVE_MESSAGES ? 120 : 0;
    c.peer_latency_ms = c.receive_latency_ms; c.connect_timeout_ms = 3000; c.peer_idle_timeout_ms = 5000;
    c.mss_bytes = 1500; c.payload_bytes = mode == SRTM_LIVE_MESSAGES ? 1316 : 0;
    c.send_buffer_bytes = 8 * 1024 * 1024; c.receive_buffer_bytes = c.send_buffer_bytes;
    return c;
}
srtmx_profile_v1 profile_config(srtm_config_v1 base) {
    auto p = record<srtmx_profile_v1>(); p.mode = base.mode; p.cipher = base.encryption;
    base.encryption = 0; p.base = base; p.max_message_bytes = base.payload_bytes;
    p.fec = record<srtmx_fec_v1>(); p.tuning = record<srtmx_tuning_v1>(); p.tuning.max_bandwidth_bytes_s = -1;
    return p;
}
srtm_handle make_profile(Session& s, srtm_config_v1 cfg) {
    auto p = profile_config(cfg); srtm_handle h = 0;
    std::vector<uint8_t> secret;
    if (cfg.passphrase.size) {
        secret.assign(cfg.passphrase.data, cfg.passphrase.data+cfg.passphrase.size);
        p.base.passphrase = {secret.data(), secret.size()};
    }
    s.check(s.profiles->create_profile(s.runtime, &p, &h, &s.error));
    std::fill(secret.begin(), secret.end(), uint8_t(0)); // Provider must own its copy.
    return h;
}
struct Pair {
    Session &caller, &server;
    srtm_handle listener = 0, tx = 0, rx = 0;
    Pair(Session& c, Session& s, const srtm_config_v1& cfg, bool ipv6 = false, bool profiles = false) : caller(c), server(s) {
        auto lc = record<srtm_listener_config_v1>(); lc.transport = cfg; lc.backlog = 8;
        lc.local.family = ipv6 ? SRTM_AF_IPV6 : SRTM_AF_IPV4;
        if (ipv6) lc.local.address[15] = 1; else { lc.local.address[0] = 127; lc.local.address[3] = 1; }
        if (profiles) {
            auto h = make_profile(server, cfg); auto input = record<srtmx_listener_v1>();
            input.local = lc.local; input.backlog = 8; input.allowed_profiles = &h; input.profile_count = 1;
            auto out = record<srtmx_object_v1>();
            server.check(server.profiles->listen(server.runtime, &input, &out, &server.error)); listener = out.handle;
            server.check(server.profiles->release_profile(server.runtime, h, &server.error));
        } else server.check(server.api.listener_create(server.runtime, &lc, &listener, &server.error));
        server.objects.push_back(listener);
        srtm_endpoint ep{}; server.check(server.api.listener_endpoint(server.runtime, listener, &ep, &server.error));
        require(ep.port != 0, "Listener did not obtain a port");
        if (profiles) {
            auto h = make_profile(caller, cfg); auto out = record<srtmx_object_v1>();
            require(caller.profiles->connect(caller.runtime, h, nullptr, &ep, 0, &out, &caller.error) == SRTM_PENDING, caller.error.message);
            tx = out.handle; caller.objects.push_back(tx);
            caller.check(caller.profiles->release_profile(caller.runtime, h, &caller.error));
        } else {
            tx = caller.connection(cfg);
            require(caller.api.connection_connect(caller.runtime, tx, &ep, &caller.error) == SRTM_PENDING, "Connect not initiated");
        }
        auto deadline = Clock::now() + 5s;
        bool connected = false;
        while (!rx || !connected) {
            require(Clock::now() < deadline, "Connection deadline exceeded");
            if (!rx) {
                auto object = record<srtmx_object_v1>(); auto io = record<srtm_io_error_v1>();
                auto status = profiles ? server.profiles->accept(server.runtime, listener, &object, &io) : server.api.listener_accept(server.runtime, listener, &rx, &server.error);
                if (profiles && status == SRTM_OK) { require(object.kind == SRTMX_KIND_CONNECTION, "accept kind"); rx = object.handle; }
                if (status == SRTM_OK) server.objects.push_back(rx);
                else require(status == SRTM_WOULD_BLOCK, server.error.message);
            }
            auto info = record<srtm_connection_info_v1>(); info.terminal_error = record<srtm_error_v1>();
            caller.check(caller.api.connection_info(caller.runtime, tx, &info, &caller.error));
            if (info.state == SRTM_FAILED_STATE) {
                const auto& error = info.terminal_error;
                throw std::runtime_error("Caller setup failed: " + caller.module.info().implementation + " -> " +
                    server.module.info().implementation + " mode=" + std::to_string(cfg.mode) +
                    " encryption=" + std::to_string(cfg.encryption) + " ipv6=" + std::to_string(ipv6) +
                    " profiles=" + std::to_string(profiles) + " category=" + std::to_string(error.category) +
                    " rejection=" + std::to_string(error.rejection_code) + " " + error.message);
            }
            connected = info.state == SRTM_CONNECTED;
            if (!rx || !connected) std::this_thread::sleep_for(1ms);
        }
        auto info = record<srtm_connection_info_v1>(); info.terminal_error = record<srtm_error_v1>();
        server.check(server.api.connection_info(server.runtime, rx, &info, &server.error));
        require(info.state == SRTM_CONNECTED, "Accepted socket is not connected");
        require(!(info.valid_fields & 8) || info.encryption == cfg.encryption, "Negotiated encryption differs from request");
    }
    ~Pair() { try { server.drop(rx); caller.drop(tx); server.drop(listener); } catch (...) { std::terminate(); } }
};
void transfer(Session& sender, srtm_handle tx, Session& receiver, srtm_handle rx, uint32_t mode, size_t total = 256 * 1316) {
    std::vector<uint8_t> data(total), output(total);
    for (size_t i = 0; i < total; ++i) data[i] = static_cast<uint8_t>((i * 37 + i / 1316) % 251);
    std::array<uint8_t, 4096> message_buffer{};
    auto io = record<srtm_io_error_v1>(); io.category = 0x12345678;
    auto detail = record<srtm_error_v1>(); detail.category = 0x87654321;
    auto meta = record<srtm_message_info_v1>();
    size_t sent = 0, received = 0; auto deadline = Clock::now() + 15s;
    while (received < total) {
        require(Clock::now() < deadline, "Transfer deadline exceeded");
        bool progress = false;
        if (sent < total) {
            const auto count = std::min(total - sent, size_t(mode == SRTM_LIVE_MESSAGES ? 1316 : 32768));
            uint64_t n = 0;
            auto status = mode == SRTM_LIVE_MESSAGES
                ? sender.api.send_message(sender.runtime, tx, {data.data()+sent, count}, nullptr, &io, &detail)
                : sender.api.write_stream(sender.runtime, tx, {data.data()+sent, count}, &n, &io, &detail);
            if (status != SRTM_OK && status != SRTM_WOULD_BLOCK) {
                const auto message = std::string(detail.message) + " sent=" + std::to_string(sent) + " received=" + std::to_string(received) + " vendor=" + std::to_string(io.vendor_code);
                std::cerr << message << std::endl; throw std::runtime_error(message);
            }
            if (status == SRTM_OK) { sent += mode == SRTM_LIVE_MESSAGES ? count : static_cast<size_t>(n); progress = true; }
        }
        uint64_t n = 0;
        auto status = mode == SRTM_LIVE_MESSAGES
            ? receiver.api.receive_message(receiver.runtime, rx, {message_buffer.data(), message_buffer.size()}, &n, &meta, &io, &detail)
            : receiver.api.read_stream(receiver.runtime, rx, {output.data()+received, total-received}, &n, &io, &detail);
        if (status != SRTM_OK && status != SRTM_WOULD_BLOCK) {
                const auto message = std::string(detail.message) + " sent=" + std::to_string(sent) + " received=" + std::to_string(received) + " vendor=" + std::to_string(io.vendor_code);
                std::cerr << message << std::endl; throw std::runtime_error(message);
            }
        if (status == SRTM_OK) {
            require(n > 0 && n <= total-received, "Invalid receive byte count");
            if (mode == SRTM_LIVE_MESSAGES) {
                require(n == std::min(size_t(1316), total-received), "Message boundary changed");
                std::memcpy(output.data()+received, message_buffer.data(), static_cast<size_t>(n));
            }
            received += static_cast<size_t>(n); progress = true;
        }
        require(io.category == 0x12345678 && detail.category == 0x87654321, "Success/WouldBlock mutated diagnostics");
        if (!progress) std::this_thread::sleep_for(100us);
    }
    require(output == data, "Payload corruption");
}
void checks(Session& a, Session& b) {
    auto cfg = config(SRTM_LIVE_MESSAGES, false);
    auto h = a.connection(cfg);
    auto io = record<srtm_io_error_v1>();
    const std::array<uint8_t, 1> data{42};
    require(b.api.send_message(b.runtime, h, {data.data(), 1}, nullptr, &io, &b.error) == SRTM_FAILED && io.category == SRTM_ERR_INVALID_HANDLE, "Cross-provider handle accepted");
    require(a.api.object_release(a.runtime, h, &a.error) == SRTM_FAILED, "Open object released");
    a.drop(h);
    auto replacement = a.connection(cfg);
    require(h != replacement, "Stale handle reused");
    require(a.api.send_message(a.runtime, h, {data.data(), 1}, nullptr, &io, &a.error) == SRTM_FAILED && io.category == SRTM_ERR_INVALID_HANDLE, "Stale handle accepted");
    a.drop(replacement);
    srtm_handle poller = 0;
    a.check(a.api.poller_create(a.runtime, &poller, &a.error));
    a.check(a.api.poller_release(a.runtime, poller, &a.error));
    cfg.encryption = SRTM_ENCRYPTION_AES_CTR_REQUIRED; cfg.key_bits = 128;
    srtm_handle invalid = 0;
    require(a.api.connection_create(a.runtime, &cfg, &invalid, &a.error) == SRTM_FAILED && invalid == 0, "Invalid encryption silently accepted");
}
void polling_checks(Session& a, Session& b) {
    srtm_handle poller = 0; a.check(a.api.poller_create(a.runtime, &poller, &a.error));
    auto h = a.connection(config(SRTM_FILE_STREAM, false));
    auto foreign = b.connection(config(SRTM_FILE_STREAM, false));
    require(a.api.poller_register(a.runtime, poller, foreign, SRTM_READY_READ, 1, &a.error) == SRTM_FAILED,
        "foreign poll registration accepted"); b.drop(foreign);
    a.check(a.api.poller_register(a.runtime, poller, h, SRTM_READY_STATE, 123, &a.error));
    a.check(a.api.object_close(a.runtime, h, SRTM_CLOSE_ABORT, 0, &a.error));
    struct Extended { srtm_event_v1 event; uint64_t tail; } event{{sizeof(Extended), 0, 0, 0}, 0x12345678};
    uint32_t count = 0; auto io = record<srtm_io_error_v1>();
    require(a.api.poller_wait(a.runtime, poller, 0, &event.event, 1, sizeof(event), &count, &io, &a.error) == SRTM_OK,
        "closed object poll event");
    require(count == 1 && event.event.registration_token == 123 && event.event.object == h &&
        event.event.readiness == (SRTM_READY_ERROR|SRTM_READY_STATE) && event.tail == 0x12345678 && event.event.struct_size == sizeof(Extended),
        "poll output/token/tail contract");
    a.drop(h);
    require(a.api.poller_wait(a.runtime, poller, 0, &event.event, 1, sizeof(event), &count, &io, &a.error) == SRTM_TIMED_OUT && count == 0,
        "released generation retained readiness");
    auto wait = std::async(std::launch::async, [&] {
        auto output = record<srtm_event_v1>(); auto error = record<srtm_io_error_v1>(); uint32_t n = 0;
        while (true) {
            auto status = a.api.poller_wait(a.runtime, poller, SRTM_WAIT_FOREVER, &output, 1, sizeof(output), &n, &error, nullptr);
            if (status != SRTM_FAILED || error.category != SRTM_ERR_BUSY) return status;
            std::this_thread::yield();
        }
    });
    // Observe Busy rather than assuming the worker has entered after a sleep.
    auto until = Clock::now()+2s;
    while (true) {
        auto status = a.api.poller_wait(a.runtime, poller, 0, &event.event, 1, sizeof(event), &count, &io, &a.error);
        if (status == SRTM_FAILED && io.category == SRTM_ERR_BUSY) break;
        require(Clock::now()<until, "waiter did not enter"); std::this_thread::yield();
    }
    const auto released = a.api.poller_release(a.runtime, poller, &a.error);
    require(released == SRTM_OK || (released == SRTM_FAILED && a.error.category == SRTM_ERR_BUSY), "poller release failed");
    require(wait.get() == SRTM_INTERRUPTED, "release did not cancel active wait");
    if (released != SRTM_OK) a.check(a.api.poller_release(a.runtime, poller, &a.error));
    // Race delayed stale lookups with slot reuse. No old handle may reach a new socket.
    auto stale = a.connection(config(SRTM_FILE_STREAM, false)); a.drop(stale);
    std::atomic<bool> stop{false}; std::atomic<bool> bad{false};
    std::jthread worker([&] {
        auto error = record<srtm_io_error_v1>(); uint64_t n = 0; const uint8_t byte = 1;
        while(!stop.load()) if(a.api.write_stream(a.runtime, stale, {&byte,1}, &n, &error, nullptr) != SRTM_FAILED || error.category != SRTM_ERR_INVALID_HANDLE) bad=true;
    });
    for(int i=0;i<100;++i) { auto replacement=a.connection(config(SRTM_FILE_STREAM,false)); a.drop(replacement); }
    stop=true; worker.join(); require(!bad.load(), "stale call reached reused slot");
}
void reject_bad_secret(Session& caller, Session& server) {
    auto cfg = config(SRTM_LIVE_MESSAGES, true);
    auto lc = record<srtm_listener_config_v1>(); lc.transport = cfg; lc.backlog = 8;
    lc.local.family = SRTM_AF_IPV4; lc.local.address[0] = 127; lc.local.address[3] = 1;
    srtm_handle listener = 0;
    server.check(server.api.listener_create(server.runtime, &lc, &listener, &server.error)); server.objects.push_back(listener);
    srtm_endpoint ep{}; server.check(server.api.listener_endpoint(server.runtime, listener, &ep, &server.error));
    static const uint8_t wrong[] = "different-test-secret"; cfg.passphrase = {wrong, sizeof(wrong)-1};
    auto connection = caller.connection(cfg);
    auto status = caller.api.connection_connect(caller.runtime, connection, &ep, &caller.error);
    require(status == SRTM_PENDING || status == SRTM_FAILED, "Unexpected rejection initiation");
    auto until = Clock::now()+5s;
    auto state = record<srtm_connection_info_v1>(); state.terminal_error = record<srtm_error_v1>();
    while (true) {
        caller.check(caller.api.connection_info(caller.runtime, connection, &state, &caller.error));
        require(state.state != SRTM_CONNECTED, "Wrong passphrase was accepted");
        if (state.state == SRTM_FAILED_STATE) break;
        require(Clock::now() < until, "Wrong-passphrase rejection did not complete");
        std::this_thread::sleep_for(1ms);
    }
    require(state.terminal_error.category == SRTM_ERR_AUTHENTICATION && (state.terminal_error.detail_flags & 4),
        "Wrong passphrase must be reported as an authentication failure with its rejection code");
    caller.drop(connection); server.drop(listener);
}
void extension_checks(Session& s, Session& other) {
    const srtm_extension_header_v1* table = nullptr;
    require(s.api.query_extension(s.runtime, {SRTMX_PROFILES, sizeof(SRTMX_PROFILES)-1}, 2, 0, &table, &s.error) == SRTM_FAILED && !table && s.error.category == SRTM_ERR_ABI_MISMATCH, "extension major mismatch");
    require(s.api.query_extension(s.runtime, {SRTMX_TELEMETRY, sizeof(SRTMX_TELEMETRY)-1}, 1, 1, &table, &s.error) == SRTM_FAILED && !table, "extension minor mismatch");
    auto p = profile_config(config(SRTM_LIVE_MESSAGES, false)); srtm_handle h = 0;
    p.mode = SRTMX_CONTROL;
    require(s.profiles->create_profile(s.runtime, &p, &h, &s.error) == SRTM_FAILED && s.error.category == SRTM_ERR_UNSUPPORTED && !h, "unsupported channel");
    p.mode = SRTM_LIVE_MESSAGES; p.base.struct_size = 1;
    require(s.profiles->create_profile(s.runtime, &p, &h, &s.error) == SRTM_FAILED && !h, "short nested profile");
    h = make_profile(s, config(SRTM_FILE_STREAM, false));
    require(other.profiles->release_profile(other.runtime, h, &other.error) == SRTM_FAILED, "foreign profile");
    s.check(s.profiles->release_profile(s.runtime, h, &s.error));
    require(s.profiles->release_profile(s.runtime, h, &s.error) == SRTM_FAILED, "stale profile");
    // Nondefault bounded tuning is validated before publishing an immutable profile.
    p = profile_config(config(SRTM_LIVE_MESSAGES, false)); p.tuning.flow_window_packets = 8192;
    p.tuning.max_bandwidth_bytes_s = 20000000; p.tuning.ip_ttl = 32; p.tuning.overhead_percent = 25;
    s.check(s.profiles->create_profile(s.runtime, &p, &h, &s.error)); s.check(s.profiles->release_profile(s.runtime, h, &s.error));
    Pair pair(s, other, config(SRTM_FILE_STREAM, true), false, true);
    transfer(s, pair.tx, other, pair.rx, SRTM_FILE_STREAM);
    auto metric = record<srtmx_metric_info_v1>(); const char name[] = "native.byteSentUniqueTotal";
    s.check(s.telemetry->resolve(s.runtime, {name, sizeof(name)-1}, &metric, &s.error));
    auto object = record<srtmx_object_v1>(); object.kind = SRTMX_KIND_CONNECTION; object.handle = pair.tx;
    auto snap = record<srtmx_snapshot_v1>(); uint32_t required = 0;
    struct Output { srtmx_metric_v1 metric; uint64_t tail; } out{record<srtmx_metric_v1>(), UINT64_C(0x123456789abcdef)};
    out.metric.struct_size = sizeof(out);
    require(s.telemetry->snapshot(s.runtime, object, &metric.id, 1, &out, sizeof(out), 0, &required, &snap, &s.error) == SRTM_BUFFER_TOO_SMALL && required == 1 && out.metric.id == 0, "snapshot capacity transaction");
    s.check(s.telemetry->snapshot(s.runtime, object, &metric.id, 1, &out, sizeof(out), 1, &required, &snap, &s.error));
    require(out.tail == UINT64_C(0x123456789abcdef) && out.metric.value.u64 >= 256*1316 && snap.counter_epoch == pair.tx, "native snapshot values/tail");
    std::array<uint32_t,82> all_ids{}; std::array<srtmx_metric_v1,82> all_values{};
    for (uint32_t i=0;i<all_ids.size();++i) { all_ids[i]=i+1; all_values[i]=record<srtmx_metric_v1>(); }
    s.check(s.telemetry->snapshot(s.runtime, object, all_ids.data(), uint32_t(all_ids.size()), all_values.data(), sizeof(all_values[0]), uint32_t(all_values.size()), &required, &snap, &s.error));
    require(snap.count==82 && all_values[81].id==82, "complete native catalogue");
    auto previous = out.metric.value.u64;
    s.check(s.telemetry->snapshot(s.runtime, object, &metric.id, 1, &out, sizeof(out), 1, &required, &snap, &s.error));
    require(out.metric.value.u64 >= previous, "snapshot cleared counters");
    uint32_t bad = UINT32_MAX;
    require(s.telemetry->snapshot(s.runtime, object, &bad, 1, &out, sizeof(out), 1, &required, &snap, &s.error) == SRTM_FAILED && out.metric.value.u64 >= previous, "unknown metric ID mutated output");
    std::atomic<bool> entered{false}, done{false}; std::atomic<int> successes{0};
    auto reader = std::async(std::launch::async, [&] {
        auto value = record<srtmx_metric_v1>(); auto snapshot = record<srtmx_snapshot_v1>(); auto d = record<srtm_error_v1>(); uint32_t n;
        do {
            auto status = s.telemetry->snapshot(s.runtime, object, &metric.id, 1, &value, sizeof(value), 1, &n, &snapshot, &d);
            if (status == SRTM_OK) ++successes;
            else require(status == SRTM_FAILED && d.category == SRTM_ERR_INVALID_HANDLE, "snapshot close race");
            entered = true;
        } while (!done.load());
    });
    while (!entered.load()) std::this_thread::yield();
    s.check(s.profiles->close(s.runtime, object, &s.error)); done = true; reader.get();
    require(successes > 0, "no concurrent telemetry observation");
    require(s.telemetry->snapshot(s.runtime, object, &metric.id, 1, &out, sizeof(out), 1, &required, &snap, &s.error) == SRTM_FAILED, "snapshot after close");
}
// Robotweax null packet deletion through the profile extension: explicit
// failures where it cannot apply, bit-exact delivery where it is negotiated, and
// unchanged service for a peer without it.
void npd_checks(Session& rw, Session& hv) {
    auto with_npd = [](uint32_t mode, uint32_t policy) { auto p = profile_config(config(mode, false)); p.null_packet_deletion = policy; return p; };
    srtm_handle h = 0;
    auto p = with_npd(SRTM_LIVE_MESSAGES, SRTMX_REQUIRED);
    require(hv.profiles->create_profile(hv.runtime, &p, &h, &hv.error) == SRTM_FAILED && hv.error.category == SRTM_ERR_UNSUPPORTED && !h, "Haivision accepted null packet deletion");
    p = with_npd(SRTM_FILE_STREAM, SRTMX_REQUIRED);
    require(rw.profiles->create_profile(rw.runtime, &p, &h, &rw.error) == SRTM_FAILED && rw.error.category == SRTM_ERR_UNSUPPORTED && !h, "File profile accepted null packet deletion");
    p = with_npd(SRTM_LIVE_MESSAGES, 3);
    require(rw.profiles->create_profile(rw.runtime, &p, &h, &rw.error) == SRTM_FAILED && rw.error.category == SRTM_ERR_INVALID_ARGUMENT && !h, "Unknown null packet deletion policy accepted");
    auto metric = record<srtmx_metric_info_v1>(); const char active_name[] = "robotweax.npd.active";
    require(hv.telemetry->resolve(hv.runtime, {active_name, sizeof(active_name)-1}, &metric, &hv.error) == SRTM_FAILED && hv.error.category == SRTM_ERR_UNSUPPORTED, "Haivision resolved a Robotweax metric");
    srtmx_feature_v1 feature = record<srtmx_feature_v1>(); const char feature_name[] = "null_packet_deletion";
    rw.check(rw.profiles->describe(rw.runtime, {feature_name, sizeof(feature_name)-1}, &feature, &rw.error));
    require(feature.native_built && feature.adapter_available && feature.experimental, "Robotweax feature description");
    hv.check(hv.profiles->describe(hv.runtime, {feature_name, sizeof(feature_name)-1}, &feature, &hv.error));
    require(!feature.native_built && !feature.adapter_available, "Haivision feature description");

    auto counter = [](Session& s, srtm_handle connection, const char* name) {
        auto info = record<srtmx_metric_info_v1>(); s.check(s.telemetry->resolve(s.runtime, {name, std::strlen(name)}, &info, &s.error));
        auto object = record<srtmx_object_v1>(); object.kind = SRTMX_KIND_CONNECTION; object.handle = connection;
        auto value = record<srtmx_metric_v1>(); auto snap = record<srtmx_snapshot_v1>(); uint32_t required = 0;
        s.check(s.telemetry->snapshot(s.runtime, object, &info.id, 1, &value, sizeof(value), 1, &required, &snap, &s.error));
        require(value.value_kind == 1, "NPD metric kind"); return value.value.u64;
    };
    // Seven 188-byte units per message; odd units are canonical null packets.
    auto transport_stream = [](uint8_t seed, bool all_null) {
        std::array<uint8_t, 1316> message{};
        for (size_t unit = 0; unit < 7; ++unit) {
            auto* bytes = message.data() + unit*188;
            if (all_null || unit % 2) { bytes[0] = 0x47; bytes[1] = 0x1f; bytes[2] = 0xff; bytes[3] = 0x10; std::fill(bytes+4, bytes+188, uint8_t(0xff)); }
            else { bytes[0] = 0x47; bytes[1] = 0x01; bytes[2] = uint8_t(unit); bytes[3] = 0x10; for (size_t i = 4; i < 188; ++i) bytes[i] = uint8_t(seed + unit*31 + i); }
        }
        return message;
    };
    struct Link { srtm_handle listener = 0, tx = 0, rx = 0; };
    auto connect = [&](Session& caller, uint32_t caller_policy, Session& server, uint32_t server_policy) {
        Link link; auto profile = with_npd(SRTM_LIVE_MESSAGES, server_policy); srtm_handle sp = 0, cp = 0;
        server.check(server.profiles->create_profile(server.runtime, &profile, &sp, &server.error));
        auto input = record<srtmx_listener_v1>(); input.local.family = SRTM_AF_IPV4; input.local.address[0] = 127; input.local.address[3] = 1;
        input.backlog = 8; input.allowed_profiles = &sp; input.profile_count = 1; auto out = record<srtmx_object_v1>();
        server.check(server.profiles->listen(server.runtime, &input, &out, &server.error)); link.listener = out.handle; server.objects.push_back(link.listener);
        server.check(server.profiles->release_profile(server.runtime, sp, &server.error));
        srtm_endpoint ep{}; server.check(server.api.listener_endpoint(server.runtime, link.listener, &ep, &server.error));
        profile = with_npd(SRTM_LIVE_MESSAGES, caller_policy);
        caller.check(caller.profiles->create_profile(caller.runtime, &profile, &cp, &caller.error));
        require(caller.profiles->connect(caller.runtime, cp, nullptr, &ep, 0, &out, &caller.error) == SRTM_PENDING, caller.error.message);
        link.tx = out.handle; caller.objects.push_back(link.tx);
        caller.check(caller.profiles->release_profile(caller.runtime, cp, &caller.error));
        const auto deadline = Clock::now() + 5s; bool connected = false, accepted = false;
        while (!connected || !accepted) {
            require(Clock::now() < deadline, "NPD connection deadline exceeded");
            if (!link.rx) {
                auto object = record<srtmx_object_v1>(); auto io = record<srtm_io_error_v1>();
                if (server.profiles->accept(server.runtime, link.listener, &object, &io) == SRTM_OK) { link.rx = object.handle; server.objects.push_back(link.rx); }
            }
            auto info = record<srtm_connection_info_v1>(); info.terminal_error = record<srtm_error_v1>();
            caller.check(caller.api.connection_info(caller.runtime, link.tx, &info, &caller.error));
            require(info.state != SRTM_FAILED_STATE, "NPD caller setup failed");
            connected = info.state == SRTM_CONNECTED;
            if (link.rx) { server.check(server.api.connection_info(server.runtime, link.rx, &info, &server.error)); accepted = info.state == SRTM_CONNECTED; }
            if (!connected || !accepted) std::this_thread::sleep_for(1ms);
        }
        return link;
    };
    auto exchange = [&](Session& caller, Session& server, const Link& link) {
        std::vector<std::array<uint8_t, 1316>> sent;
        for (uint8_t i = 0; i < 20; ++i) sent.push_back(transport_stream(i, i == 7)); // One message is all null packets.
        sent[11].fill(0x5a); // Not a transport stream: must pass through unchanged.
        auto io = record<srtm_io_error_v1>(); size_t delivered = 0; const auto deadline = Clock::now() + 10s;
        for (size_t next = 0; delivered < sent.size();) {
            require(Clock::now() < deadline, "NPD transfer deadline exceeded");
            if (next < sent.size()) {
                const auto status = caller.api.send_message(caller.runtime, link.tx, {sent[next].data(), sent[next].size()}, nullptr, &io, nullptr);
                require(status == SRTM_OK || status == SRTM_WOULD_BLOCK, "NPD send failed"); if (status == SRTM_OK) ++next;
            }
            std::array<uint8_t, 1456> received{}; uint64_t n = 0; auto meta = record<srtm_message_info_v1>();
            const auto status = server.api.receive_message(server.runtime, link.rx, {received.data(), received.size()}, &n, &meta, &io, nullptr);
            require(status == SRTM_OK || status == SRTM_WOULD_BLOCK, "NPD receive failed");
            if (status == SRTM_OK) {
                require(n == 1316 && std::memcmp(received.data(), sent[delivered].data(), 1316) == 0, "Null packet deletion changed delivered bytes or boundaries");
                ++delivered;
            } else std::this_thread::sleep_for(1ms);
        }
    };
    auto drop = [](Session& caller, Session& server, const Link& link) { server.drop(link.rx); caller.drop(link.tx); server.drop(link.listener); };
    for (uint32_t listener_policy : {uint32_t(SRTMX_REQUIRED), uint32_t(SRTMX_IF_OFFERED)}) {
        auto link = connect(rw, SRTMX_REQUIRED, rw, listener_policy);
        require(counter(rw, link.tx, "robotweax.npd.active") == 1 && counter(rw, link.rx, "robotweax.npd.active") == 1, "Null packet deletion was not negotiated");
        exchange(rw, rw, link);
        // 18 messages carry three null packets each and one carries seven.
        require(counter(rw, link.tx, "robotweax.npd.deleted_units") == 18*3 + 7, "Unexpected deleted null packet count");
        require(counter(rw, link.rx, "robotweax.npd.restored_units") == 18*3 + 7, "Unexpected restored null packet count");
        require(counter(rw, link.rx, "robotweax.npd.malformed_payloads") == 0, "Malformed NPD payloads reported");
        drop(rw, rw, link);
    }
    { // A caller without the feature is served unchanged by an IF_OFFERED listener.
        auto link = connect(hv, SRTMX_OFF, rw, SRTMX_IF_OFFERED);
        require(counter(rw, link.rx, "robotweax.npd.active") == 0, "Null packet deletion active with a peer that lacks it");
        exchange(hv, rw, link);
        require(counter(rw, link.rx, "robotweax.npd.restored_units") == 0, "Restored packets without negotiation");
        drop(hv, rw, link);
    }
    { // IF_OFFERED is not a caller policy.
        p = with_npd(SRTM_LIVE_MESSAGES, SRTMX_IF_OFFERED); srtm_handle profile = 0;
        rw.check(rw.profiles->create_profile(rw.runtime, &p, &profile, &rw.error));
        srtm_endpoint ep{}; ep.family = SRTM_AF_IPV4; ep.address[0] = 127; ep.address[3] = 1; ep.port = 9; auto out = record<srtmx_object_v1>();
        require(rw.profiles->connect(rw.runtime, profile, nullptr, &ep, 0, &out, &rw.error) == SRTM_FAILED && rw.error.category == SRTM_ERR_INVALID_ARGUMENT && !out.handle, "Caller accepted IF_OFFERED");
        rw.check(rw.profiles->release_profile(rw.runtime, profile, &rw.error));
    }
}
// An idle receive must stay cheap and must never hide data that then arrives.
// The pinned Haivision library reports "would block" through an internal C++
// exception costing several microseconds; the adapter asks for readiness first.
void would_block_checks(Session& s) {
    Pair pair(s, s, config(SRTM_LIVE_MESSAGES, false));
    std::array<uint8_t, 1316> buffer{}; auto io = record<srtm_io_error_v1>(); auto meta = record<srtm_message_info_v1>(); uint64_t n = 0;
    double best = 1e9;
    for (int batch = 0; batch < 5; ++batch) {
        const auto started = Clock::now();
        for (int i = 0; i < 20000; ++i)
            require(s.api.receive_message(s.runtime, pair.rx, {buffer.data(), buffer.size()}, &n, &meta, &io, nullptr) == SRTM_WOULD_BLOCK, "idle receive did not report would-block");
        best = std::min(best, std::chrono::duration<double, std::nano>(Clock::now() - started).count() / 20000);
    }
    std::cout << s.module.info().implementation << " idle receive " << best << " ns/call\n";
    if (!instrumented) require(best < 1500.0, "idle receive is not cheap (above 1.5 microseconds per call)");
    for (uint8_t round = 0; round < 10; ++round) { // Data after a blocked spell is always delivered.
        buffer.fill(round);
        auto until = Clock::now() + 3s;
        while (true) {
            const auto status = s.api.send_message(s.runtime, pair.tx, {buffer.data(), buffer.size()}, nullptr, &io, nullptr);
            if (status == SRTM_OK) break;
            require(status == SRTM_WOULD_BLOCK && Clock::now() < until, "send after blocked spell failed"); std::this_thread::sleep_for(1ms);
        }
        std::array<uint8_t, 1316> received{};
        while (true) {
            const auto status = s.api.receive_message(s.runtime, pair.rx, {received.data(), received.size()}, &n, &meta, &io, nullptr);
            if (status == SRTM_OK) break;
            require(status == SRTM_WOULD_BLOCK && Clock::now() < until, "message hidden after a blocked spell"); std::this_thread::yield();
        }
        require(n == 1316 && received[0] == round && received[1315] == round, "wrong message after a blocked spell");
    }
    // A closed peer is reported by the native call, not masked as would-block.
    s.check(s.api.object_close(s.runtime, pair.tx, SRTM_CLOSE_ABORT, 0, &s.error));
    auto until = Clock::now() + 8s;
    while (true) {
        const auto status = s.api.receive_message(s.runtime, pair.rx, {buffer.data(), buffer.size()}, &n, &meta, &io, nullptr);
        if (status == SRTM_FAILED) {
            if (io.category != SRTM_ERR_CONNECTION_LOST)
                throw std::runtime_error("peer close reported with category " + std::to_string(io.category) + ", native code " + std::to_string(io.vendor_code));
            std::cout << s.module.info().implementation << " peer close seen after " << std::chrono::duration<double>(Clock::now() - (until - 8s)).count() << " s\n"; break;
        }
        require(status == SRTM_WOULD_BLOCK && Clock::now() < until, "peer close masked as would-block"); std::this_thread::sleep_for(1ms);
    }
}
// Regression checks for the foundation review: native wake latency, close
// notification through per-object watcher lists, and per-slot generations.
void review_checks(Session& a) {
    const auto cfg = config(SRTM_FILE_STREAM, false);
    auto wait_zero = [&](srtm_handle poller, srtm_event_v1& event) {
        uint32_t count = 0; auto io = record<srtm_io_error_v1>();
        const auto status = a.api.poller_wait(a.runtime, poller, 0, &event, 1, sizeof(event), &count, &io, &a.error);
        require(status == SRTM_OK || status == SRTM_TIMED_OUT, "zero-timeout poll failed");
        return status == SRTM_OK && count == 1;
    };
    { // A closed object reaches exactly the pollers still subscribed to it.
        srtm_handle one = 0, two = 0, three = 0;
        a.check(a.api.poller_create(a.runtime, &one, &a.error)); a.check(a.api.poller_create(a.runtime, &two, &a.error));
        a.check(a.api.poller_create(a.runtime, &three, &a.error));
        auto h = a.connection(cfg); auto other = a.connection(cfg);
        a.check(a.api.poller_register(a.runtime, one, h, SRTM_READY_READ, 11, &a.error));
        a.check(a.api.poller_register(a.runtime, one, h, SRTM_READY_READ|SRTM_READY_STATE, 12, &a.error)); // Update, not a second watcher.
        a.check(a.api.poller_register(a.runtime, two, h, SRTM_READY_READ, 21, &a.error));
        a.check(a.api.poller_register(a.runtime, three, h, SRTM_READY_READ, 31, &a.error));
        a.check(a.api.poller_register(a.runtime, two, other, SRTM_READY_READ, 22, &a.error));
        a.check(a.api.poller_remove(a.runtime, three, h, &a.error));
        a.check(a.api.object_close(a.runtime, h, SRTM_CLOSE_ABORT, 0, &a.error));
        auto event = record<srtm_event_v1>();
        require(wait_zero(one, event) && event.registration_token == 12 && event.readiness == (SRTM_READY_ERROR|SRTM_READY_STATE), "first watcher missed close");
        require(wait_zero(two, event) && event.registration_token == 21 && event.object == h && event.readiness == SRTM_READY_ERROR, "second watcher missed close");
        require(!wait_zero(three, event), "removed watcher was notified");
        a.drop(h);
        require(!wait_zero(one, event) && !wait_zero(two, event), "released object retained readiness");
        a.check(a.api.poller_release(a.runtime, two, &a.error)); // Releases its remaining subscription to `other`.
        a.drop(other);
        a.check(a.api.poller_release(a.runtime, one, &a.error)); a.check(a.api.poller_release(a.runtime, three, &a.error));
    }
    { // Reusing one slot must not consume generations of any other slot.
        auto generation = [](srtm_handle h) { return static_cast<uint32_t>(h >> 16); };
        auto kept = a.connection(cfg); auto churn = a.connection(cfg);
        const srtm_handle kept_slot = kept & 0xffffu; const uint32_t kept_generation = generation(kept);
        for (int i = 0; i < 50; ++i) {
            const srtm_handle slot = churn & 0xffffu; const uint32_t previous = generation(churn);
            a.drop(churn); churn = a.connection(cfg);
            require((churn & 0xffffu) == slot && generation(churn) == previous + 1, "slot reuse did not advance its own generation by one");
        }
        a.drop(churn); a.drop(kept);
        auto again = a.connection(cfg); // Most recently released slot is reused first.
        require((again & 0xffffu) == kept_slot && generation(again) == kept_generation + 1 && again != kept,
            "unrelated churn consumed another slot's generation range");
        auto state = record<srtm_connection_info_v1>(); state.terminal_error = record<srtm_error_v1>();
        require(a.api.connection_info(a.runtime, kept, &state, &a.error) == SRTM_FAILED && a.error.category == SRTM_ERR_INVALID_HANDLE, "stale handle accepted after slot reuse");
        a.drop(again);
    }
    { // wake() must reach a blocked waiter natively, not on a polling tick.
        srtm_handle poller = 0; a.check(a.api.poller_create(a.runtime, &poller, &a.error));
        auto h = a.connection(cfg); a.check(a.api.poller_register(a.runtime, poller, h, SRTM_READY_READ, 1, &a.error));
        std::vector<double> latency;
        for (int i = 0; i < 25; ++i) {
            std::atomic<Clock::time_point> returned{};
            auto wait = std::async(std::launch::async, [&] {
                auto output = record<srtm_event_v1>(); auto error = record<srtm_io_error_v1>(); uint32_t n = 0;
                while (true) { // The observer below may briefly hold the single waiter slot.
                    const auto status = a.api.poller_wait(a.runtime, poller, SRTM_WAIT_FOREVER, &output, 1, sizeof(output), &n, &error, nullptr);
                    if (status == SRTM_FAILED && error.category == SRTM_ERR_BUSY) { std::this_thread::yield(); continue; }
                    returned = Clock::now(); return status;
                }
            });
            auto event = record<srtm_event_v1>(); uint32_t count = 0; auto io = record<srtm_io_error_v1>(); const auto until = Clock::now()+2s;
            while (!(a.api.poller_wait(a.runtime, poller, 0, &event, 1, sizeof(event), &count, &io, nullptr) == SRTM_FAILED && io.category == SRTM_ERR_BUSY)) {
                require(Clock::now() < until, "waiter did not enter"); std::this_thread::yield();
            }
            std::this_thread::sleep_for(3ms + std::chrono::microseconds(i*397 % 5000)); // Let it block; vary the phase.
            const auto signalled = Clock::now();
            a.check(a.api.poller_wake(a.runtime, poller, &a.error));
            require(wait.get() == SRTM_INTERRUPTED, "wake did not interrupt wait");
            latency.push_back(std::chrono::duration<double, std::milli>(returned.load()-signalled).count());
        }
        std::sort(latency.begin(), latency.end());
        std::cout << a.module.info().implementation << " poller wake median " << latency[latency.size()/2] << " ms\n";
        require(latency[latency.size()/2] < 2.0, "poller wake is not native (median above 2 ms)");
        a.check(a.api.poller_release(a.runtime, poller, &a.error)); a.drop(h);
    }
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "Expected two provider paths and load order");
        const bool reversed = std::string(argv[3]) == "reversed";
        auto first = std::make_unique<Session>(reversed ? argv[2] : argv[1], reversed ? "haivision" : "robotweax", reversed ? "1.5.7" : "0.3.4");
        auto second = std::make_unique<Session>(reversed ? argv[1] : argv[2], reversed ? "robotweax" : "haivision", reversed ? "0.3.4" : "1.5.7");
        Session& rw = reversed ? *second : *first; Session& hv = reversed ? *first : *second;
        checks(rw, hv); checks(hv, rw);
        extension_checks(rw, hv); extension_checks(hv, rw);
        polling_checks(rw,hv); polling_checks(hv,rw);
        review_checks(rw); review_checks(hv);
        npd_checks(rw, hv);
        would_block_checks(hv); would_block_checks(rw);
        reject_bad_secret(rw, hv); reject_bad_secret(hv, rw);
        for (auto* sender : {&rw, &hv}) {
            Session& receiver = sender == &rw ? hv : rw;
            Pair pair(*sender, receiver, config(SRTM_LIVE_MESSAGES, false));
            std::array<uint8_t, 1316> data{}; data[0] = 42;
            auto io = record<srtm_io_error_v1>(); auto detail = record<srtm_error_v1>();
            require(sender->api.send_message(sender->runtime, pair.tx, {data.data(), data.size()}, nullptr, &io, &detail) == SRTM_OK, "Message setup send failed");
            uint64_t n = 99; auto meta = record<srtm_message_info_v1>();
            require(receiver.api.receive_message(receiver.runtime, pair.rx, {data.data(), 1}, &n, &meta, &io, &detail) == SRTM_FAILED &&
                    io.category == SRTM_ERR_INVALID_ARGUMENT && n == 99, "Short buffer not rejected without mutation");
            auto until = Clock::now() + 3s;
            while (true) {
                auto status = receiver.api.receive_message(receiver.runtime, pair.rx, {data.data(), data.size()}, &n, &meta, &io, &detail);
                if (status == SRTM_OK) break;
                require(status == SRTM_WOULD_BLOCK && Clock::now() < until, "Short-buffer rejection consumed message");
                std::this_thread::sleep_for(1ms);
            }
            require(n == 1316 && data[0] == 42, "Message changed after short-buffer rejection");
        }
        { // A known unqualified native profile must fail explicitly, not degrade.
            auto cfg = config(SRTM_FILE_STREAM, false);
            auto h = hv.connection(cfg); srtm_endpoint ep{}; ep.family = SRTM_AF_IPV6; ep.port = 9000; ep.address[15] = 1;
            require(hv.api.connection_connect(hv.runtime, h, &ep, &hv.error) == SRTM_FAILED && hv.error.category == SRTM_ERR_UNSUPPORTED,
                "Unqualified Haivision File/IPv6 was silently enabled"); hv.drop(h);
        }
        for (auto mode : {SRTM_LIVE_MESSAGES, SRTM_FILE_STREAM})
            for (bool encrypted : {false, true})
                for (bool ipv6 : {false, true})
                    for (auto* sender : {&rw, &hv}) for (auto* receiver : {&rw, &hv}) {
                        if (mode == SRTM_FILE_STREAM && ipv6 && (sender == &hv || receiver == &hv)) continue;
                        std::cout << sender->module.info().implementation << " -> " << receiver->module.info().implementation
                            << " mode=" << mode << " encrypted=" << encrypted << " ipv6=" << ipv6 << std::endl;
                        Pair pair(*sender, *receiver, config(mode, encrypted), ipv6);
                        transfer(*sender, pair.tx, *receiver, pair.rx, mode);
                        auto state = record<srtm_connection_info_v1>(); state.terminal_error = record<srtm_error_v1>();
                        receiver->check(receiver->api.connection_info(receiver->runtime, pair.rx, &state, &receiver->error));
                        require((state.valid_fields & 8) && state.encryption == (encrypted ? SRTM_ENCRYPTION_AES_CTR_REQUIRED : SRTM_ENCRYPTION_DISABLED), "Encryption readback unavailable or wrong after transfer");
                    }
        for (auto mode : {SRTM_LIVE_MESSAGES, SRTM_FILE_STREAM}) for (bool encrypted : {false, true})
            for (auto* sender : {&rw, &hv}) for (auto* receiver : {&rw, &hv}) {
                Pair pair(*sender, *receiver, config(mode, encrypted), false, true);
                transfer(*sender, pair.tx, *receiver, pair.rx, mode);
            }
        // Both implementations actively transfer at once, in opposite directions.
        {
            Pair ab(rw, hv, config(SRTM_LIVE_MESSAGES, true, true), false, true);
            Pair ba(hv, rw, config(SRTM_FILE_STREAM, true), false, true);
            std::exception_ptr failure1, failure2;
            std::thread one([&] { try { transfer(rw, ab.tx, hv, ab.rx, SRTM_LIVE_MESSAGES); } catch (...) { failure1 = std::current_exception(); } });
            std::thread two([&] { try { transfer(hv, ba.tx, rw, ba.rx, SRTM_FILE_STREAM); } catch (...) { failure2 = std::current_exception(); } });
            one.join(); two.join(); if (failure1) std::rethrow_exception(failure1); if (failure2) std::rethrow_exception(failure2);
            auto stats = record<srtm_statistics_v1>();
            rw.check(rw.api.statistics(rw.runtime, ab.tx, &stats, &rw.error));
            require((stats.valid_fields & 1) && stats.accepted_tx_bytes == 256 * 1316, "Opt-in accounting mismatch");
            hv.check(hv.api.statistics(hv.runtime, ba.tx, &stats, &hv.error));
            require(stats.valid_fields == 0, "Disabled accounting fabricated counters");
        }
        rw.shutdown();
        { Pair remaining(hv, hv, config(SRTM_FILE_STREAM, false)); transfer(hv, remaining.tx, hv, remaining.rx, SRTM_FILE_STREAM); }
        std::cout << "All real-provider pairings and independent shutdown passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
