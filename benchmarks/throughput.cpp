// Identical local transfer workload for direct native, C ABI and C++ facade paths.
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
#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
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
bool use_poll = false;
bool native_legacy = false;
size_t chunk_bytes = 32768;
bool live = false, encrypted = false;
constexpr char benchmark_secret[] = "srtmux-public-benchmark-secret"; // Test fixture, never a production credential.
uint64_t transfer_bytes(uint64_t mib) {
    const auto bytes = mib*1024*1024;
    return live ? bytes/chunk_bytes*chunk_bytes : bytes;
}
uint64_t setting(const char* name, uint64_t fallback, uint64_t maximum) {
    const char* text = std::getenv(name); if (!text) return fallback;
    size_t used = 0; auto value = std::stoull(text, &used);
    require(used == std::string(text).size() && value <= maximum, "Invalid benchmark setting"); return value;
}
double thread_cpu() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    require(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0, "thread CPU clock");
    auto ticks=[](FILETIME t) { return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; };
    return double(ticks(kernel)+ticks(user))*1e-7;
#else
    timespec t{}; require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0, "thread CPU clock");
    return double(t.tv_sec)+double(t.tv_nsec)*1e-9;
#endif
}
template<class T> T record() { T v{}; v.struct_size = sizeof(v); return v; }
#ifdef SRTMUX_DIRECT
struct Backend {
    SRTSOCKET listener = -1, tx = -1, rx = -1;
    int tp = -1, rp = -1;
    const char* name = BENCHMARK_PROVIDER;
    const char* fingerprint = BENCHMARK_FINGERPRINT;
    explicit Backend(char**) {
        require(srt_startup() == 0, "startup"); listener = srt_create_socket(); tx = srt_create_socket();
        for (auto socket : {listener, tx}) {
            auto set = [socket](SRT_SOCKOPT opt, auto value) { require(srt_setsockflag(socket, opt, &value, sizeof(value)) == 0, "set option"); };
            set(SRTO_TRANSTYPE, live ? SRTT_LIVE : SRTT_FILE); set(SRTO_MESSAGEAPI, live); set(SRTO_SNDSYN, false); set(SRTO_RCVSYN, false);
            set(SRTO_MSS, 1500); set(SRTO_SNDBUF, 8*1024*1024); set(SRTO_RCVBUF, 8*1024*1024);
            set(SRTO_CONNTIMEO, 3000); set(SRTO_PEERIDLETIMEO, 5000); set(SRTO_ENFORCEDENCRYPTION, true); set(SRTO_LINGER, linger{});
            if (live) { set(SRTO_PAYLOADSIZE, int(chunk_bytes)); set(SRTO_RCVLATENCY, 120); set(SRTO_PEERLATENCY, 120); }
            if (encrypted) {
                set(SRTO_PBKEYLEN, 16);
                require(srt_setsockflag(socket, SRTO_PASSPHRASE, benchmark_secret, sizeof(benchmark_secret)-1)==0,"passphrase");
            }
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
    void verify_security() {
        if (!encrypted) return;
        for (auto socket : {tx, rx}) {
            int key=0, snd=0, rcv=0, n=sizeof(int);
            require(srt_getsockflag(socket,SRTO_PBKEYLEN,&key,&n)==0,"key readback"); n=sizeof(int);
            require(srt_getsockflag(socket,SRTO_SNDKMSTATE,&snd,&n)==0,"send KM state"); n=sizeof(int);
            require(srt_getsockflag(socket,SRTO_RCVKMSTATE,&rcv,&n)==0,"receive KM state");
            require(key==16 && (snd==SRT_KM_S_SECURED || rcv==SRT_KM_S_SECURED),"encryption not negotiated");
        }
    }
    void prepare_poll() {
        tp=srt_epoll_create(); rp=srt_epoll_create(); require(tp>=0 && rp>=0,"epoll create");
        int out=SRT_EPOLL_OUT|SRT_EPOLL_ERR, in=SRT_EPOLL_IN|SRT_EPOLL_ERR;
        require(srt_epoll_add_usock(tp,tx,&out)==0 && srt_epoll_add_usock(rp,rx,&in)==0,"epoll register");
    }
    void wait(bool send) {
        if (native_legacy) {
            SRTSOCKET socket; int n=1;
            int rc=send ? srt_epoll_wait(tp,nullptr,nullptr,&socket,&n,5000,nullptr,nullptr,nullptr,nullptr)
                        : srt_epoll_wait(rp,&socket,&n,nullptr,nullptr,5000,nullptr,nullptr,nullptr,nullptr);
            require(rc>0,"native readiness timeout/failure");
        } else {
            SRT_EPOLL_EVENT event{};
            require(srt_epoll_uwait(send?tp:rp,&event,1,5000)>0,"native readiness timeout/failure");
        }
    }
    int send(const uint8_t* data, int size) {
        SRT_MSGCTRL ctrl=srt_msgctrl_default;
        return result(live ? srt_sendmsg2(tx,reinterpret_cast<const char*>(data),size,&ctrl)
                           : srt_send(tx,reinterpret_cast<const char*>(data),size));
    }
    int receive(uint8_t* data, int size) {
        SRT_MSGCTRL ctrl=srt_msgctrl_default;
        return result(live ? srt_recvmsg2(rx,reinterpret_cast<char*>(data),size,&ctrl)
                           : srt_recv(rx,reinterpret_cast<char*>(data),size));
    }
    static int result(int n) {
        if (n >= 0) return n;
        const auto error = srt_getlasterror(nullptr);
        if (error == SRT_EASYNCSND || error == SRT_EASYNCRCV) return 0;
        throw std::runtime_error(srt_getlasterror_str());
    }
    ~Backend() { if(tp>=0) srt_epoll_release(tp); if(rp>=0) srt_epoll_release(rp); srt_close(tx); srt_close(rx); srt_close(listener); srt_cleanup(); }
};
#elif defined(SRTMUX_FACADE)
template<class T> T unwrap(srtmux::Result<T> value) {
    if(auto* e=std::get_if<srtmux::Error>(&value)) throw std::runtime_error(e->message);
    return std::get<T>(std::move(value));
}
srtmux::TransportConfig config() {
    auto c=live ? srtmux::live_profile() : srtmux::file_stream_profile();
    if (live) c.payload_bytes=static_cast<uint32_t>(chunk_bytes);
    if (encrypted) {
        c.encryption=srtmux::Encryption::aes_ctr_required; c.key_bits=128;
        c.passphrase=unwrap(srtmux::Secret::copy(std::as_bytes(std::span(benchmark_secret, sizeof(benchmark_secret)-1))));
    }
    return c;
}
struct Backend {
    srtmux::TransportRuntime runtime = unwrap(srtmux::TransportRuntime::create());
    srtmux::Provider provider;
    srtmux::Listener listener;
    srtmux::Connection tx;
    std::optional<srtmux::Connection> rx;
    std::optional<srtmux::Poller> tp, rp;
    std::string name, fingerprint;
    explicit Backend(char** args) : provider(unwrap(runtime.load({args[2], args[3], args[4], {}}))),
        listener(unwrap(provider.listen({{"127.0.0.1",0,0},8,config(),{}}))),
        tx(unwrap(provider.create_connection(config()))),
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
    void verify_security() {
        if (!encrypted) return;
        for (auto* connection : {&tx, &*rx}) {
            auto info=unwrap(connection->info());
            require(info.encryption==srtmux::Encryption::aes_ctr_required && info.key_bits==128,"encryption not negotiated");
        }
    }
    void prepare_poll() {
        tp.emplace(unwrap(provider.create_poller())); rp.emplace(unwrap(provider.create_poller()));
        unwrap(tp->add(tx,srtmux::Interest::write)); unwrap(rp->add(*rx,srtmux::Interest::read));
    }
    void wait(bool send) {
        std::array<srtmux::Event,1> events{};
        auto result=(send?*tp:*rp).wait(events,5000ms);
        require(std::holds_alternative<size_t>(result) && std::get<size_t>(result)>0,"facade readiness timeout/failure");
    }
    int send(const uint8_t* data,int size) {
        if (live) {
            auto result=tx.send_message({reinterpret_cast<const std::byte*>(data),static_cast<size_t>(size)},{});
            if(std::holds_alternative<srtmux::WouldBlock>(result)) return 0;
            require(std::holds_alternative<srtmux::Success>(result),"facade message send failed"); return size;
        }
        auto result=tx.write_stream({reinterpret_cast<const std::byte*>(data),static_cast<size_t>(size)});
        if(std::holds_alternative<srtmux::WouldBlock>(result)) return 0;
        require(std::holds_alternative<size_t>(result),"facade send failed"); return static_cast<int>(std::get<size_t>(result));
    }
    int receive(uint8_t* data,int size) {
        if (live) {
            auto result=rx->receive_message({reinterpret_cast<std::byte*>(data),static_cast<size_t>(size)});
            if(std::holds_alternative<srtmux::WouldBlock>(result)) return 0;
            require(std::holds_alternative<srtmux::ReceivedMessage>(result),"facade message receive failed");
            return static_cast<int>(std::get<srtmux::ReceivedMessage>(result).bytes);
        }
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
    srtm_handle listener = 0, tx = 0, rx = 0, tp = 0, rp = 0;
    std::string name, fingerprint;
    static srtmux::host::Module load(char** args) {
        auto m = srtmux::host::Module::load({args[2], args[3], args[4], {}});
        if (auto* e = std::get_if<srtmux::host::LoadError>(&m)) throw std::runtime_error(e->message);
        return std::get<srtmux::host::Module>(std::move(m));
    }
    explicit Backend(char** args) : module(load(args)), api(module.api()), name(module.info().implementation), fingerprint(module.info().build_fingerprint) {
        auto host = record<srtm_host_v1>(); auto error = record<srtm_error_v1>();
        require(api.runtime_create(&host, &runtime, &error) == SRTM_OK, error.message);
        auto c = record<srtm_config_v1>(); c.mode = live ? SRTM_LIVE_MESSAGES : SRTM_FILE_STREAM; c.mss_bytes = 1500;
        c.connect_timeout_ms = 3000; c.peer_idle_timeout_ms = 5000; c.send_buffer_bytes = 8*1024*1024; c.receive_buffer_bytes = 8*1024*1024;
        if (live) { c.payload_bytes=static_cast<uint32_t>(chunk_bytes); c.receive_latency_ms=c.peer_latency_ms=120; }
        if (encrypted) {
            c.encryption=SRTM_ENCRYPTION_AES_CTR_REQUIRED; c.key_bits=128;
            c.passphrase={reinterpret_cast<const uint8_t*>(benchmark_secret),sizeof(benchmark_secret)-1};
        }
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
    void verify_security() {
        if (!encrypted) return;
        for (auto socket : {tx, rx}) {
            auto info=record<srtm_connection_info_v1>(); info.terminal_error=record<srtm_error_v1>();
            auto error=record<srtm_error_v1>();
            require(api.connection_info(runtime,socket,&info,&error)==SRTM_OK,error.message);
            require((info.valid_fields&8) && info.encryption==SRTM_ENCRYPTION_AES_CTR_REQUIRED && info.key_bits==128,"encryption not negotiated");
        }
    }
    void prepare_poll() {
        auto e=record<srtm_error_v1>();
        require(api.poller_create(runtime,&tp,&e)==SRTM_OK && api.poller_create(runtime,&rp,&e)==SRTM_OK,e.message);
        require(api.poller_register(runtime,tp,tx,SRTM_READY_WRITE,1,&e)==SRTM_OK &&
                api.poller_register(runtime,rp,rx,SRTM_READY_READ,2,&e)==SRTM_OK,e.message);
    }
    void wait(bool send) {
        auto event=record<srtm_event_v1>(); auto error=record<srtm_io_error_v1>(); uint32_t n=0;
        require(api.poller_wait(runtime,send?tp:rp,5000,&event,1,sizeof(event),&n,&error,nullptr)==SRTM_OK && n>0,
                "module readiness timeout/failure");
    }
    int send(const uint8_t* data, int size) {
        uint64_t n = 0; auto error = record<srtm_io_error_v1>();
        auto result = live ? api.send_message(runtime,tx,{data,static_cast<uint64_t>(size)},nullptr,&error,nullptr)
                           : api.write_stream(runtime, tx, {data, static_cast<uint64_t>(size)}, &n, &error, nullptr);
        if (live && result==SRTM_OK) n=static_cast<uint64_t>(size);
        require(result == SRTM_OK || result == SRTM_WOULD_BLOCK, "module send failed"); return static_cast<int>(n);
    }
    int receive(uint8_t* data, int size) {
        uint64_t n = 0; auto error = record<srtm_io_error_v1>();
        auto message=record<srtm_message_info_v1>();
        auto result = live ? api.receive_message(runtime,rx,{data,static_cast<uint64_t>(size)},&n,&message,&error,nullptr)
                           : api.read_stream(runtime, rx, {data, static_cast<uint64_t>(size)}, &n, &error, nullptr);
        require(result == SRTM_OK || result == SRTM_WOULD_BLOCK, "module receive failed"); return static_cast<int>(n);
    }
    ~Backend() {
        if (!runtime) return;
        for (auto h : {tp,rp}) if(h) api.poller_release(runtime,h,nullptr);
        for (auto h : {tx, rx, listener}) { api.object_close(runtime, h, SRTM_CLOSE_ABORT, 0, nullptr); api.object_release(runtime, h, nullptr); }
        api.runtime_begin_shutdown(runtime, nullptr); api.runtime_destroy(runtime, nullptr);
    }
};
#endif
struct Counts { uint64_t calls=0, would_block=0; double cpu=0; };
struct Sample { double seconds, cpu_seconds; Counts tx, rx; };
Sample transfer(Backend& backend, uint64_t bytes) {
    std::vector<uint8_t> input(chunk_bytes, 0x5a), output(chunk_bytes);
    Counts tx, rx;
    std::atomic<bool> cancel{false}; std::exception_ptr sender_error;
    auto start = Clock::now(); auto cpu = std::clock();
    std::jthread sender([&] {
        try {
            uint64_t sent = 0; Counts counters; auto cpu_start=thread_cpu();
            while (sent < bytes && !cancel.load(std::memory_order_relaxed)) {
                if (live) { const uint64_t sequence=sent/chunk_bytes; std::memcpy(input.data(),&sequence,sizeof(sequence)); }
                int n = backend.send(input.data(), static_cast<int>(std::min<uint64_t>(input.size(), bytes-sent)));
                ++counters.calls;
                if (!n) { ++counters.would_block; require(Clock::now()-start < 180s, "sender deadline");
                    if(use_poll) backend.wait(true); else std::this_thread::yield(); }
                require(!live || n==0 || n==static_cast<int>(chunk_bytes),"partial Live message send");
                sent += static_cast<uint64_t>(n);
            }
            counters.cpu=thread_cpu()-cpu_start; tx=counters;
        } catch (...) { sender_error = std::current_exception(); cancel.store(true); }
    });
    try {
        uint64_t received = 0; auto cpu_start=thread_cpu();
        while (received < bytes && !cancel.load(std::memory_order_relaxed)) {
            int n = backend.receive(output.data(), static_cast<int>(std::min<uint64_t>(output.size(), bytes-received)));
            ++rx.calls;
            if (!n) { ++rx.would_block; require(Clock::now()-start < 180s, "receiver deadline");
                if(use_poll) backend.wait(false); else std::this_thread::yield(); }
            else {
                size_t header=0;
                if (live) {
                    require(n==static_cast<int>(chunk_bytes),"Live message boundary mismatch");
                    uint64_t sequence; std::memcpy(&sequence,output.data(),sizeof(sequence));
                    require(sequence==received/chunk_bytes,"Live message lost, reordered or duplicated"); header=sizeof(sequence);
                }
                require(std::all_of(output.begin()+static_cast<ptrdiff_t>(header), output.begin()+n, [](uint8_t b) { return b == 0x5a; }), "payload corruption");
            }
            received += static_cast<uint64_t>(n);
        }
        require(received==bytes,"incomplete delivery"); rx.cpu=thread_cpu()-cpu_start;
    } catch (...) { cancel.store(true); sender.join(); throw; }
    sender.join(); if (sender_error) std::rethrow_exception(sender_error);
    return {std::chrono::duration<double>(Clock::now()-start).count(), double(std::clock()-cpu)/CLOCKS_PER_SEC, tx, rx};
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
        use_poll = setting("SRTMUX_BENCH_POLL",0,1)!=0;
        native_legacy = setting("SRTMUX_BENCH_NATIVE_LEGACY",0,1)!=0;
        live=setting("SRTMUX_BENCH_LIVE",0,1)!=0; encrypted=setting("SRTMUX_BENCH_ENCRYPTED",0,1)!=0;
        chunk_bytes=static_cast<size_t>(setting("SRTMUX_BENCH_CHUNK",live?1316:32768,1048576)); require(chunk_bytes>0,"zero chunk");
        require(!live || (chunk_bytes>=sizeof(uint64_t) && chunk_bytes<=1456),"Live payload must be 8..1456 bytes");
        const auto bytes=transfer_bytes(mib);
        auto warmup=setting("SRTMUX_BENCH_WARMUP_MIB",8,4096);
        auto probes=setting("SRTMUX_BENCH_PROBES",0,10000000);
        Backend backend(argv); if(use_poll) backend.prepare_poll();
        // Connected, idle receive: same native WouldBlock call, no payload or retry scheduler.
        std::vector<uint8_t> probe_buffer(chunk_bytes);
        auto probe_start=thread_cpu();
        for(uint64_t i=0;i<probes;++i) require(backend.receive(probe_buffer.data(),static_cast<int>(chunk_bytes))==0,"probe not idle");
        double probe_cpu=thread_cpu()-probe_start;
        if(warmup) transfer(backend,transfer_bytes(warmup));
        backend.verify_security();
        const auto sample = transfer(backend, bytes);
        backend.verify_security();
        std::cout << std::setprecision(12) << "{\"provider\":\"" << backend.name << "\",\"path\":\"" << path << "\",\"bytes\":" << bytes
            << ",\"build_fingerprint\":\"" << backend.fingerprint << "\",\"seconds\":" << sample.seconds << ",\"cpu_seconds\":" << sample.cpu_seconds
            << ",\"wait\":\"" << (use_poll?"poll":"yield") << "\",\"chunk_bytes\":" << chunk_bytes
            << ",\"mode\":\"" << (live?"live":"file") << "\",\"encryption\":\"" << (encrypted?"aes-128-ctr":"none") << "\""
            << ",\"messages\":" << (live?bytes/chunk_bytes:0)
            << ",\"warmup_mib\":" << warmup << ",\"tx_calls\":" << sample.tx.calls << ",\"rx_calls\":" << sample.rx.calls
            << ",\"tx_would_block\":" << sample.tx.would_block << ",\"rx_would_block\":" << sample.rx.would_block
            << ",\"application_cpu_seconds\":" << sample.tx.cpu+sample.rx.cpu
            << ",\"sender_cpu_seconds\":" << sample.tx.cpu << ",\"receiver_cpu_seconds\":" << sample.rx.cpu
            << ",\"probe_calls\":" << probes << ",\"probe_cpu_seconds\":" << probe_cpu
            << ",\"mbps\":" << double(bytes)*8/sample.seconds/1e6 << "}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
