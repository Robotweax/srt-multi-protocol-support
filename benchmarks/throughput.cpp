// Same File/IPv4 transfer loop for direct static-library and module ABI paths.
#include <srtmux/module.hpp>
#ifdef SRTMUX_FACADE
#include <srtmux/transport.hpp>
#include <optional>
#endif
#ifdef SRTMUX_DIRECT
#include <srt.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T record() { T v{}; v.struct_size = sizeof(v); return v; }
#ifdef SRTMUX_DIRECT
struct Backend {
    SRTSOCKET listener = -1, tx = -1, rx = -1;
    const char* name = BENCHMARK_PROVIDER;
    const char* fingerprint = BENCHMARK_FINGERPRINT;
    explicit Backend(char**) {
        require(srt_startup() == 0, "startup"); listener = srt_create_socket(); tx = srt_create_socket();
        for (auto socket : {listener, tx}) {
            auto set = [socket](SRT_SOCKOPT opt, auto value) { require(srt_setsockflag(socket, opt, &value, sizeof(value)) == 0, "set option"); };
            set(SRTO_TRANSTYPE, SRTT_FILE); set(SRTO_MESSAGEAPI, false); set(SRTO_SNDSYN, false); set(SRTO_RCVSYN, false);
            set(SRTO_MSS, 1500); set(SRTO_SNDBUF, 8*1024*1024); set(SRTO_RCVBUF, 8*1024*1024);
            set(SRTO_CONNTIMEO, 3000); set(SRTO_PEERIDLETIMEO, 5000); set(SRTO_ENFORCEDENCRYPTION, true); set(SRTO_LINGER, linger{});
        }
        sockaddr_in ep{}; ep.sin_family = AF_INET; ep.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(srt_bind(listener, reinterpret_cast<sockaddr*>(&ep), sizeof(ep)) == 0, "bind");
        require(srt_listen(listener, 8) == 0, "listen"); int size = sizeof(ep);
        require(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&ep), &size) == 0, "name");
        require(srt_connect(tx, reinterpret_cast<sockaddr*>(&ep), size) == 0, "connect");
        auto until = Clock::now() + 5s;
        while (rx == SRT_INVALID_SOCK || srt_getsockstate(tx) != SRTS_CONNECTED) {
            require(Clock::now() < until, "connect timeout");
            if (rx == SRT_INVALID_SOCK) rx = srt_accept(listener, nullptr, nullptr);
            std::this_thread::sleep_for(1ms);
        }
    }
    int send(const uint8_t* data, int size) { return result(srt_send(tx, reinterpret_cast<const char*>(data), size)); }
    int receive(uint8_t* data, int size) { return result(srt_recv(rx, reinterpret_cast<char*>(data), size)); }
    static int result(int n) {
        if (n >= 0) return n;
        const auto error = srt_getlasterror(nullptr);
        if (error == SRT_EASYNCSND || error == SRT_EASYNCRCV) return 0;
        throw std::runtime_error(srt_getlasterror_str());
    }
    ~Backend() { srt_close(tx); srt_close(rx); srt_close(listener); srt_cleanup(); }
};
#elif defined(SRTMUX_FACADE)
template<class T> T unwrap(srtmux::Result<T> value) {
    if(auto* e=std::get_if<srtmux::Error>(&value)) throw std::runtime_error(e->message);
    return std::get<T>(std::move(value));
}
struct Backend {
    srtmux::TransportRuntime runtime = unwrap(srtmux::TransportRuntime::create());
    srtmux::Provider provider;
    srtmux::Listener listener;
    srtmux::Connection tx;
    std::optional<srtmux::Connection> rx;
    std::string name, fingerprint;
    explicit Backend(char** args) : provider(unwrap(runtime.load({args[2], args[3], args[4], {}}))),
        listener(unwrap(provider.listen({{"127.0.0.1",0,0},8,srtmux::file_stream_profile(),{}}))),
        tx(unwrap(provider.create_connection(srtmux::file_stream_profile()))),
        name(provider.info().implementation), fingerprint(provider.info().build_fingerprint) {
        require(std::holds_alternative<srtmux::Pending>(tx.connect(unwrap(listener.local_endpoint()))), "connect");
        auto until=Clock::now()+5s; bool connected=false;
        while(!rx || !connected) {
            require(Clock::now()<until,"connect timeout");
            if(!rx) {
                auto accepted=listener.accept();
                if(auto* e=std::get_if<srtmux::Error>(&accepted)) throw std::runtime_error(e->message);
                if(std::holds_alternative<srtmux::Connection>(accepted)) rx.emplace(std::get<srtmux::Connection>(std::move(accepted)));
            }
            connected=unwrap(tx.info()).state==srtmux::State::connected;
            std::this_thread::sleep_for(1ms);
        }
        unwrap(rx->info());
    }
    int send(const uint8_t* data,int size) {
        auto result=tx.write_stream({reinterpret_cast<const std::byte*>(data),static_cast<size_t>(size)});
        if(std::holds_alternative<srtmux::WouldBlock>(result)) return 0;
        require(std::holds_alternative<size_t>(result),"facade send failed"); return static_cast<int>(std::get<size_t>(result));
    }
    int receive(uint8_t* data,int size) {
        auto result=rx->read_stream({reinterpret_cast<std::byte*>(data),static_cast<size_t>(size)});
        if(std::holds_alternative<srtmux::WouldBlock>(result)) return 0;
        require(std::holds_alternative<size_t>(result),"facade receive failed"); return static_cast<int>(std::get<size_t>(result));
    }
};
#else
struct Backend {
    srtmux::host::Module module;
    const srtm_provider_api_v1& api;
    srtm_runtime* runtime = nullptr;
    srtm_handle listener = 0, tx = 0, rx = 0;
    std::string name, fingerprint;
    static srtmux::host::Module load(char** args) {
        auto m = srtmux::host::Module::load({args[2], args[3], args[4], {}});
        if (auto* e = std::get_if<srtmux::host::LoadError>(&m)) throw std::runtime_error(e->message);
        return std::get<srtmux::host::Module>(std::move(m));
    }
    explicit Backend(char** args) : module(load(args)), api(module.api()), name(module.info().implementation), fingerprint(module.info().build_fingerprint) {
        auto host = record<srtm_host_v1>(); auto error = record<srtm_error_v1>();
        require(api.runtime_create(&host, &runtime, &error) == SRTM_OK, error.message);
        auto c = record<srtm_config_v1>(); c.mode = SRTM_FILE_STREAM; c.mss_bytes = 1500;
        c.connect_timeout_ms = 3000; c.peer_idle_timeout_ms = 5000; c.send_buffer_bytes = 8*1024*1024; c.receive_buffer_bytes = 8*1024*1024;
        auto lc = record<srtm_listener_config_v1>(); lc.transport = c; lc.backlog = 8;
        lc.local.family = SRTM_AF_IPV4; lc.local.address[0] = 127; lc.local.address[3] = 1;
        require(api.listener_create(runtime, &lc, &listener, &error) == SRTM_OK, error.message);
        srtm_endpoint ep{}; require(api.listener_endpoint(runtime, listener, &ep, &error) == SRTM_OK, error.message);
        require(api.connection_create(runtime, &c, &tx, &error) == SRTM_OK, error.message);
        require(api.connection_connect(runtime, tx, &ep, &error) == SRTM_PENDING, error.message);
        auto until = Clock::now() + 5s;
        bool connected = false;
        while (!rx || !connected) {
            require(Clock::now() < until, "connect timeout");
            if (!rx) { auto result = api.listener_accept(runtime, listener, &rx, &error); require(result == SRTM_OK || result == SRTM_WOULD_BLOCK, error.message); }
            auto info = record<srtm_connection_info_v1>(); info.terminal_error = record<srtm_error_v1>();
            require(api.connection_info(runtime, tx, &info, &error) == SRTM_OK, error.message); connected = info.state == SRTM_CONNECTED;
            std::this_thread::sleep_for(1ms);
        }
        auto info = record<srtm_connection_info_v1>(); info.terminal_error = record<srtm_error_v1>();
        require(api.connection_info(runtime, rx, &info, &error) == SRTM_OK, error.message);
    }
    int send(const uint8_t* data, int size) {
        uint64_t n = 0; auto error = record<srtm_io_error_v1>();
        auto result = api.write_stream(runtime, tx, {data, static_cast<uint64_t>(size)}, &n, &error, nullptr);
        require(result == SRTM_OK || result == SRTM_WOULD_BLOCK, "module send failed"); return static_cast<int>(n);
    }
    int receive(uint8_t* data, int size) {
        uint64_t n = 0; auto error = record<srtm_io_error_v1>();
        auto result = api.read_stream(runtime, rx, {data, static_cast<uint64_t>(size)}, &n, &error, nullptr);
        require(result == SRTM_OK || result == SRTM_WOULD_BLOCK, "module receive failed"); return static_cast<int>(n);
    }
    ~Backend() {
        if (!runtime) return;
        for (auto h : {tx, rx, listener}) { api.object_close(runtime, h, SRTM_CLOSE_ABORT, 0, nullptr); api.object_release(runtime, h, nullptr); }
        api.runtime_begin_shutdown(runtime, nullptr); api.runtime_destroy(runtime, nullptr);
    }
};
#endif
struct Sample { double seconds, cpu_seconds; };
Sample transfer(Backend& backend, uint64_t bytes) {
    std::vector<uint8_t> input(32768, 0x5a), output(32768);
    std::atomic<bool> cancel{false}; std::exception_ptr sender_error;
    auto start = Clock::now(); auto cpu = std::clock();
    std::jthread sender([&] {
        try {
            uint64_t sent = 0;
            while (sent < bytes && !cancel.load(std::memory_order_relaxed)) {
                int n = backend.send(input.data(), static_cast<int>(std::min<uint64_t>(input.size(), bytes-sent)));
                if (!n) { require(Clock::now()-start < 60s, "sender deadline"); std::this_thread::yield(); }
                sent += static_cast<uint64_t>(n);
            }
        } catch (...) { sender_error = std::current_exception(); cancel.store(true); }
    });
    try {
        uint64_t received = 0;
        while (received < bytes && !cancel.load(std::memory_order_relaxed)) {
            int n = backend.receive(output.data(), static_cast<int>(std::min<uint64_t>(output.size(), bytes-received)));
            if (!n) { require(Clock::now()-start < 60s, "receiver deadline"); std::this_thread::yield(); }
            else require(std::all_of(output.begin(), output.begin()+n, [](uint8_t b) { return b == 0x5a; }), "payload corruption");
            received += static_cast<uint64_t>(n);
        }
    } catch (...) { cancel.store(true); sender.join(); throw; }
    sender.join(); if (sender_error) std::rethrow_exception(sender_error);
    return {std::chrono::duration<double>(Clock::now()-start).count(), double(std::clock()-cpu)/CLOCKS_PER_SEC};
}
int main(int argc, char** argv) {
    try {
#ifdef SRTMUX_DIRECT
        require(argc == 2, "Usage: direct_benchmark MiB"); const char* path = "direct";
#elif defined(SRTMUX_FACADE)
        require(argc == 5, "Usage: facade_benchmark MiB module implementation release"); const char* path = "facade";
#else
        require(argc == 5, "Usage: module_benchmark MiB module implementation release"); const char* path = "module";
#endif
        const auto mib = std::stoull(argv[1]); require(mib > 0 && mib <= 4096, "MiB must be 1..4096");
        Backend backend(argv); transfer(backend, 8*1024*1024);
        const auto sample = transfer(backend, mib*1024*1024);
        std::cout << "{\"provider\":\"" << backend.name << "\",\"path\":\"" << path << "\",\"bytes\":" << mib*1024*1024
            << ",\"build_fingerprint\":\"" << backend.fingerprint << "\",\"seconds\":" << sample.seconds << ",\"cpu_seconds\":" << sample.cpu_seconds
            << ",\"mbps\":" << double(mib)*1024*1024*8/sample.seconds/1e6 << "}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
