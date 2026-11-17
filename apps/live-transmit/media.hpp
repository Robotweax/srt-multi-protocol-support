#pragma once
#include "options.hpp"
#include <chrono>

namespace live_transmit {
using Clock = std::chrono::steady_clock;
struct Packet { std::vector<std::byte> bytes; size_t offset = 0; };
struct ReadResult {
    enum class State { packet, would_block, eof } state;
    Packet packet;
};
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
    virtual ReadResult read(size_t) { throw std::logic_error("Not an input medium"); }
    virtual std::optional<size_t> write(std::span<const std::byte>) { throw std::logic_error("Not an output medium"); }
    virtual void wait(std::chrono::milliseconds) = 0;
    virtual std::string statistics() { return "null"; }
};
std::unique_ptr<Medium> open_medium(EndpointOptions, bool input, const Options&, Providers&);
}
