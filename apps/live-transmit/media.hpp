#pragma once
#include "options.hpp"
#include "stats.hpp"
#include <chrono>

namespace live_transmit {
using Clock = std::chrono::steady_clock;
// A read fills the caller's buffer, which the relay reuses for every packet:
// no allocation or zero-fill per read attempt.
struct ReadResult {
    // dropped: an input unit larger than the buffer was discarded, as lost data.
    enum class State { packet, would_block, eof, dropped } state;
    size_t bytes = 0; // Bytes written for packet; bytes discarded for dropped.
};
// done: accepted whole. blocked: the medium cannot take it now. rejected: this
// packet can never be sent on this medium (for example above the SRT payload).
enum class Write { done, blocked, rejected };
bool stop_requested() noexcept; // SIGINT/SIGTERM seen; defined by the relay.
class Providers {
public:
    explicit Providers(const Options& options) : options_(options), runtime_(checked(srtmux::TransportRuntime::create())) {}
    srtmux::Provider get(const std::string& name);
private:
    const Options& options_;
    srtmux::TransportRuntime runtime_;
    std::map<std::string, srtmux::Provider> loaded_;
};
class Medium {
public:
    virtual ~Medium() = default;
    virtual void tick() {}
    virtual bool connected() const { return true; }
    virtual bool ended() const { return false; }
    // A source that loses nothing when it is simply not read for a while.
    virtual bool pausable() const { return false; }
    // Largest unit one write accepts; 0 means no medium limit.
    virtual size_t message_limit() const { return 0; }
    virtual ReadResult read(std::span<std::byte>) { throw std::logic_error("Not an input medium"); }
    virtual Write write(std::span<const std::byte>) { throw std::logic_error("Not an output medium"); }
    virtual void wait(std::chrono::milliseconds) = 0;
    virtual std::string statistics() { return "null"; }
    // Native counters of a connected SRT endpoint; false for anything else.
    virtual bool native_stats(NativeStats&) { return false; }
};
std::unique_ptr<Medium> open_medium(EndpointOptions, bool input, const Options&, Providers&);
}
