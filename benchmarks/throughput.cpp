// Identical timed workload for direct native, C ABI and C++ facade paths.
#include "backend.hpp"
struct Counts { uint64_t calls=0, would_block=0; double cpu=0; };
struct Sample { double seconds, cpu_seconds; Counts tx, rx; SchedulerDiagnostics scheduler; };
// A single application sender serves warmup and measurement. Neither thread
// creation/join nor buffer allocation is included in the timed data path.
class Transfer {
    Backend& backend;
    std::vector<uint8_t> input, output;
    const std::vector<uint8_t> expected; // Immutable reference, allocated before timing.
    uint64_t bytes = 0;
    bool stopping = false; // Published by the start barrier, never read earlier.
    std::atomic<bool> cancel{false};
    std::exception_ptr sender_error;
    Clock::time_point start;
    std::clock_t cpu_start{};
    Sample sample{};
    struct Start {
        Transfer* owner;
        void operator()() noexcept {
            if (!owner->stopping) { owner->start=Clock::now(); owner->cpu_start=std::clock(); }
        }
    };
    struct Finish {
        Transfer* owner;
        void operator()() noexcept {
            owner->sample.seconds=std::chrono::duration<double>(Clock::now()-owner->start).count();
            owner->sample.cpu_seconds=double(std::clock()-owner->cpu_start)/CLOCKS_PER_SEC;
        }
    };
    std::barrier<Start> begin{2,Start{this}};
    std::barrier<Finish> end{2,Finish{this}};
    std::jthread sender;
    void send_loop() noexcept {
        while (true) {
            begin.arrive_and_wait();
            if (stopping) return;
            try {
                require(fail_side!=1,"injected sender failure");
                uint64_t sent=0; Counts counters; auto thread_start=thread_cpu();
                while (sent<bytes && !cancel.load(std::memory_order_relaxed)) {
                    if (live) { const uint64_t sequence=sent/chunk_bytes; std::memcpy(input.data(),&sequence,sizeof(sequence)); }
                    int n=backend.send(input.data(),static_cast<int>(std::min<uint64_t>(input.size(),bytes-sent)));
                    ++counters.calls;
                    if (!n) {
                        ++counters.would_block; require(Clock::now()-start<180s,"sender deadline");
                        if (use_poll) backend.wait(true); else std::this_thread::yield();
                    }
                    require(!live || n==0 || n==static_cast<int>(chunk_bytes),"partial Live message send");
                    sent+=static_cast<uint64_t>(n);
                }
                counters.cpu=thread_cpu()-thread_start; sample.tx=counters;
            } catch (...) { sender_error=std::current_exception(); cancel.store(true); }
            end.arrive_and_wait();
        }
    }
public:
    explicit Transfer(Backend& b) : backend(b), input(chunk_bytes,0x5a), output(chunk_bytes), expected(chunk_bytes,0x5a), sender([this] { send_loop(); }) {}
    ~Transfer() { stopping=true; begin.arrive_and_wait(); } // jthread joins before barriers are destroyed.
    Sample run(uint64_t total) {
        bytes=total; sample={}; sender_error=nullptr; cancel=false;
        auto scheduler_before=scheduler_diagnostics();
        begin.arrive_and_wait();
        std::exception_ptr receiver_error;
        try {
            require(fail_side!=2,"injected receiver failure");
            uint64_t received=0; auto thread_start=thread_cpu();
            while (received<bytes && !cancel.load(std::memory_order_relaxed)) {
                int n=backend.receive(output.data(),static_cast<int>(std::min<uint64_t>(output.size(),bytes-received)));
                ++sample.rx.calls;
                if (!n) {
                    ++sample.rx.would_block; require(Clock::now()-start<180s,"receiver deadline");
                    if (use_poll) backend.wait(false); else std::this_thread::yield();
                } else {
                    size_t header=0;
                    if (live) {
                        require(n==static_cast<int>(chunk_bytes),"Live message boundary mismatch");
                        uint64_t sequence; std::memcpy(&sequence,output.data(),sizeof(sequence));
                        require(sequence==received/chunk_bytes,"Live message lost, reordered or duplicated"); header=sizeof(sequence);
                    }
                    const auto body = static_cast<size_t>(n)-header;
                    if (fail_side >= 3 && received == 0 && body) {
                        const auto offset = fail_side == 3 ? 0 : fail_side == 4 ? body/2 : body-1;
                        output[header+offset] ^= 1; // Integrity canary, never enabled in qualification.
                    }
                    require(std::memcmp(output.data()+header, expected.data()+header, body)==0,"payload corruption");
                }
                received+=static_cast<uint64_t>(n);
            }
            require(received==bytes,"incomplete delivery"); sample.rx.cpu=thread_cpu()-thread_start;
        } catch (...) { receiver_error=std::current_exception(); cancel.store(true); }
        end.arrive_and_wait();
        auto scheduler=scheduler_diagnostics();
        scheduler.voluntary-=scheduler_before.voluntary; scheduler.involuntary-=scheduler_before.involuntary;
        scheduler.major_faults-=scheduler_before.major_faults; sample.scheduler=scheduler;
        if (sender_error) std::rethrow_exception(sender_error);
        if (receiver_error) std::rethrow_exception(receiver_error);
        return sample;
    }
};

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
        fail_side=setting("SRTMUX_BENCH_FAIL_SIDE",0,5);
        profile_setup = setting("SRTMUX_BENCH_PROFILE_SETUP",0,1)!=0;
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
        Transfer transfer(backend);
        if(warmup) transfer.run(transfer_bytes(warmup));
        backend.verify_security();
        const auto tx_before=backend.diagnostics(true), rx_before=backend.diagnostics(false);
        const auto sample = transfer.run(bytes);
        const auto tx_after=backend.diagnostics(true), rx_after=backend.diagnostics(false);
        backend.verify_security();
        std::cout << std::setprecision(12) << "{\"provider\":\"" << backend.name << "\",\"path\":\"" << path << "\",\"bytes\":" << bytes
            << ",\"build_fingerprint\":\"" << backend.fingerprint << "\",\"seconds\":" << sample.seconds << ",\"cpu_seconds\":" << sample.cpu_seconds
            << ",\"wait\":\"" << (use_poll?"poll":"yield") << "\",\"chunk_bytes\":" << chunk_bytes
            << ",\"mode\":\"" << (live?"live":"file") << "\",\"encryption\":\"" << (encrypted?"aes-128-ctr":"none") << "\""
            << ",\"timing_protocol\":\"persistent-workers-memcmp-v2\""
            << ",\"profile_setup\":" << ((profile_setup && std::strcmp(path,"direct")!=0)?"true":"false")
            << ",\"messages\":" << (live?bytes/chunk_bytes:0)
            << ",\"warmup_mib\":" << warmup << ",\"tx_calls\":" << sample.tx.calls << ",\"rx_calls\":" << sample.rx.calls
            << ",\"tx_would_block\":" << sample.tx.would_block << ",\"rx_would_block\":" << sample.rx.would_block
            << ",\"application_cpu_seconds\":" << sample.tx.cpu+sample.rx.cpu
            << ",\"sender_cpu_seconds\":" << sample.tx.cpu << ",\"receiver_cpu_seconds\":" << sample.rx.cpu
            << ",\"probe_calls\":" << probes << ",\"probe_cpu_seconds\":" << probe_cpu
            << ",\"tx_loss_packets\":" << tx_after.loss-tx_before.loss
            << ",\"tx_retransmitted_packets\":" << tx_after.retransmitted-tx_before.retransmitted
            << ",\"rx_loss_packets\":" << rx_after.loss-rx_before.loss
            << ",\"scheduler_diagnostics_available\":" << (sample.scheduler.available?"true":"false")
            << ",\"voluntary_context_switches\":" << sample.scheduler.voluntary
            << ",\"involuntary_context_switches\":" << sample.scheduler.involuntary
            << ",\"major_page_faults\":" << sample.scheduler.major_faults
            << ",\"mbps\":" << double(bytes)*8/sample.seconds/1e6 << "}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
