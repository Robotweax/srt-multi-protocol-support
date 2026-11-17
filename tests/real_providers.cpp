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
    require(state.terminal_error.category != SRTM_ERR_NONE, "Missing terminal rejection diagnostic");
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
int main(int argc, char** argv) {
    try {
        require(argc == 4, "Expected two provider paths and load order");
        const bool reversed = std::string(argv[3]) == "reversed";
        auto first = std::make_unique<Session>(reversed ? argv[2] : argv[1], reversed ? "haivision" : "robotweax", reversed ? "1.5.7" : "0.3.3");
        auto second = std::make_unique<Session>(reversed ? argv[1] : argv[2], reversed ? "robotweax" : "haivision", reversed ? "0.3.3" : "1.5.7");
        Session& rw = reversed ? *second : *first; Session& hv = reversed ? *first : *second;
        checks(rw, hv); checks(hv, rw);
        extension_checks(rw, hv); extension_checks(hv, rw);
        polling_checks(rw,hv); polling_checks(hv,rw);
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
