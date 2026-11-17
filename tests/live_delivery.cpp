// Functional finite-Live delivery contract, deliberately not a throughput trial.
#include "../benchmarks/backend.hpp"

namespace {
bool wait_ready(Backend& backend, bool send) {
#ifdef SRTMUX_DIRECT
    SRT_EPOLL_EVENT event{};
    const auto n = srt_epoll_uwait(send ? backend.tp : backend.rp, &event, 1, 5000);
    if (n < 0) {
        if (srt_getlasterror(nullptr) == SRT_ETIMEOUT) return false;
        throw std::runtime_error(srt_getlasterror_str());
    }
    return n > 0;
#elif defined(SRTMUX_FACADE)
    std::array<srtmux::Event, 1> events{};
    auto result = (send ? *backend.tp : *backend.rp).wait(events, 5000ms);
    if (std::holds_alternative<srtmux::TimedOut>(result)) return false;
    require(std::holds_alternative<size_t>(result), "Live facade poll failure");
    return std::get<size_t>(result) > 0;
#else
    auto event = record<srtm_event_v1>(); auto error = record<srtm_io_error_v1>();
    uint32_t count = 0;
    const auto status = backend.api.poller_wait(backend.runtime, send ? backend.tp : backend.rp,
        5000, &event, 1, sizeof(event), &count, &error, nullptr);
    if (status == SRTM_TIMED_OUT) return false;
    require(status == SRTM_OK, "Live poll failure");
    return count > 0;
#endif
}
}

int main(int argc, char** argv) {
    try {
#ifdef SRTMUX_DIRECT
        require(argc == 2, "Usage: live_delivery MiB");
#else
        require(argc == 5, "Usage: live_delivery MiB module implementation release");
#endif
        live = true; use_poll = true; chunk_bytes = 1316;
        encrypted = setting("SRTMUX_LIVE_ENCRYPTED", 0, 1) != 0;
        profile_setup = setting("SRTMUX_LIVE_PROFILE_SETUP", 0, 1) != 0;
        const auto mib = std::stoull(argv[1]); require(mib > 0 && mib <= 4, "MiB must be 1..4");
        const auto messages = transfer_bytes(mib) / chunk_bytes;
        // Faults operate on actual received messages and are only in this test executable.
        const auto drop_tail = setting("SRTMUX_LIVE_TEST_DROP_TAIL", 0, messages);
        const auto fault = setting("SRTMUX_LIVE_TEST_FAULT", 0, 6);
        Backend backend(argv); backend.prepare_poll();
        std::atomic<bool> cancelled{false}, sent_all{false};
        std::exception_ptr sender_error;
        std::jthread sender([&] {
            try {
                std::vector<uint8_t> payload(chunk_bytes, 0x5a);
                const auto first_send = Clock::now();
                const auto deadline = first_send + 30s;
                for (uint64_t sequence = 0; sequence < messages && !cancelled.load();) {
                    require(fault != 4, "injected sender failure");
                    std::memcpy(payload.data(), &sequence, sizeof(sequence));
                    const auto n = backend.send(payload.data(), static_cast<int>(payload.size()));
                    require(n == 0 || n == static_cast<int>(payload.size()), "partial Live send");
                    if (n) {
                        ++sequence;
                        // A functional Live source, not an unpaced throughput workload.
                        if (sequence % 8 == 0) std::this_thread::sleep_for(2ms);
                    }
                    else {
                        require(Clock::now() < deadline, "Live sender deadline");
                        require(wait_ready(backend, true), "Live sender readiness timeout");
                    }
                    if (sequence == messages) sent_all.store(true);
                }
            } catch (...) { sender_error = std::current_exception(); cancelled.store(true); }
        });
        std::exception_ptr receiver_error;
        uint64_t received = 0;
        try {
            std::vector<uint8_t> payload(chunk_bytes), expected(chunk_bytes, 0x5a);
            const auto deadline = Clock::now() + 30s;
            while (received < messages && !cancelled.load()) {
                require(Clock::now() < deadline, "Live receiver deadline");
                const auto n = backend.receive(payload.data(), static_cast<int>(payload.size()));
                if (!n) {
                    if (wait_ready(backend, false)) continue;
                    require(sent_all.load(), "Live receive timeout before sender completion");
                    require(received > 0, "Live stream delivered no messages");
                    break;
                }
                require(n == static_cast<int>(chunk_bytes), "Live message boundary mismatch");
                uint64_t sequence; std::memcpy(&sequence, payload.data(), sizeof(sequence));
                if (sequence >= messages - drop_tail || fault == 3) continue;
                if (fault == 1 && sequence == messages / 2) continue;
                if (fault == 5 && sequence == messages / 2) --sequence;
                if (fault == 6 && sequence == messages / 2) ++sequence;
                require(sequence == received, "Live internal gap, duplicate or reordering");
                if (fault == 2 && received == 0) payload[sizeof(sequence)] ^= 1;
                require(std::memcmp(payload.data() + sizeof(sequence), expected.data() + sizeof(sequence),
                    chunk_bytes - sizeof(sequence)) == 0, "Live payload corruption");
                ++received;
            }
        } catch (...) { receiver_error = std::current_exception(); cancelled.store(true); }
        sender.join();
        if (sender_error) std::rethrow_exception(sender_error);
        if (receiver_error) std::rethrow_exception(receiver_error);
        require(sent_all.load() && received > 0, "Live transfer incomplete");
        backend.verify_security();
        std::cout << "{\"functional_only\":true,\"qualification_sample\":false,\"messages_sent\":"
            << messages << ",\"messages_received\":" << received << ",\"missing_tail_messages\":"
            << messages - received << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
