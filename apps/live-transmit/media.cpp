#include "media.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace live_transmit {
namespace {
[[noreturn]] void system_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Address { sockaddr_storage storage{}; socklen_t size = 0; };
Address resolve(std::string host, uint16_t port, bool passive, int family = AF_UNSPEC) {
    addrinfo hints{}; hints.ai_socktype = SOCK_DGRAM; hints.ai_family = family;
    if (host.empty()) { hints.ai_flags = AI_PASSIVE; hints.ai_family = family == AF_UNSPEC ? AF_INET : family; }
    if (!passive && host.empty()) throw std::invalid_argument("Remote endpoint needs a host");
    addrinfo* raw = nullptr;
    const auto status = getaddrinfo(host.empty() ? nullptr : host.c_str(), std::to_string(port).c_str(), &hints, &raw);
    if (status != 0) throw std::runtime_error(std::string("Address resolution failed: ") + gai_strerror(status));
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
    for (auto* address = raw; address; address = address->ai_next) {
        if ((address->ai_family == AF_INET || address->ai_family == AF_INET6) && address->ai_addrlen <= sizeof(sockaddr_storage)) {
            Address out; out.size = static_cast<socklen_t>(address->ai_addrlen);
            std::memcpy(&out.storage, address->ai_addr, address->ai_addrlen); return out;
        }
    }
    throw std::runtime_error("No supported IP address");
}
srtmux::Endpoint numeric(const Address& address) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address.storage.ss_family == AF_INET) {
        const auto& ip = reinterpret_cast<const sockaddr_in&>(address.storage);
        require(inet_ntop(AF_INET, &ip.sin_addr, text.data(), text.size()) != nullptr, "IPv4 conversion failed");
        return {text.data(), ntohs(ip.sin_port), 0};
    }
    const auto& ip = reinterpret_cast<const sockaddr_in6&>(address.storage);
    require(inet_ntop(AF_INET6, &ip.sin6_addr, text.data(), text.size()) != nullptr, "IPv6 conversion failed");
    return {text.data(), ntohs(ip.sin6_port), ip.sin6_scope_id};
}
struct Descriptor {
    int value = -1, original_flags = -1; bool borrowed = false;
    ~Descriptor() {
        if (value < 0) return;
        if (borrowed) { if (original_flags >= 0) (void)fcntl(value, F_SETFL, original_flags); }
        else (void)close(value);
    }
    void nonblocking() {
        const auto flags = fcntl(value, F_GETFL, 0);
        if (flags < 0 || fcntl(value, F_SETFL, flags | O_NONBLOCK) < 0) system_error("Nonblocking descriptor");
        if (borrowed) original_flags = flags;
    }
    void wait(bool input, std::chrono::milliseconds timeout) const {
        pollfd event{value, static_cast<short>(input ? POLLIN : POLLOUT), 0};
        if (poll(&event, 1, static_cast<int>(timeout.count())) < 0 && errno != EINTR) system_error("Descriptor poll");
    }
};
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
class FileMedium final : public Medium {
    Descriptor fd_; bool input_;
public:
    FileMedium(const EndpointOptions& endpoint, bool input) : input_(input) {
        if (endpoint.path == "-") { fd_.value = input ? STDIN_FILENO : STDOUT_FILENO; fd_.borrowed = true; }
        else {
            fd_.value = open(endpoint.path.c_str(), (input ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC) | O_NONBLOCK, 0666);
            if (fd_.value < 0) system_error("Open media file");
            struct stat info{};
            if (fstat(fd_.value, &info) < 0) system_error("Inspect media file");
            require(S_ISREG(info.st_mode), "Use file://con for pipes/devices; file paths must be regular files");
        }
        fd_.nonblocking();
    }
    ReadResult read(size_t chunk) override {
        Packet packet; packet.bytes.resize(chunk);
        const auto n = ::read(fd_.value, packet.bytes.data(), packet.bytes.size());
        if (n < 0) { if (would_block()) return {ReadResult::State::would_block, {}}; system_error("Read media file"); }
        if (n == 0) return {ReadResult::State::eof, {}};
        packet.bytes.resize(static_cast<size_t>(n)); return {ReadResult::State::packet, std::move(packet)};
    }
    std::optional<size_t> write(std::span<const std::byte> bytes) override {
        const auto n = ::write(fd_.value, bytes.data(), bytes.size());
        if (n < 0) { if (would_block()) return {}; system_error("Write media file"); }
        return static_cast<size_t>(n);
    }
    void wait(std::chrono::milliseconds timeout) override { fd_.wait(input_, timeout); }
};
class UdpMedium final : public Medium {
    Descriptor fd_; Address peer_; bool input_;
public:
    UdpMedium(const EndpointOptions& endpoint, bool input) : peer_(resolve(endpoint.host, endpoint.port, input)), input_(input) {
        fd_.value = socket(peer_.storage.ss_family, SOCK_DGRAM, 0);
        if (fd_.value < 0) system_error("UDP socket");
        fd_.nonblocking();
        auto set = [&](int level, int option, auto value) {
            if (setsockopt(fd_.value, level, option, &value, sizeof(value)) < 0) system_error("UDP socket option");
        };
        auto option = [&](const char* name, int fallback, int low, int high) {
            const auto found = endpoint.udp_options.find(name);
            return found == endpoint.udp_options.end() ? fallback : static_cast<int>(number(found->second, low, high, name));
        };
        set(SOL_SOCKET, input ? SO_RCVBUF : SO_SNDBUF, option("buffer_size", 8*1024*1024, 1, INT32_MAX));
        bool multicast = false;
        if (peer_.storage.ss_family == AF_INET) {
            const auto& group = reinterpret_cast<const sockaddr_in&>(peer_.storage);
            multicast = IN_MULTICAST(ntohl(group.sin_addr.s_addr));
            in_addr adapter{};
            if (const auto it = endpoint.udp_options.find("adapter"); it != endpoint.udp_options.end())
                require(inet_pton(AF_INET, it->second.c_str(), &adapter) == 1, "UDP adapter must be a numeric IPv4 address");
            set(IPPROTO_IP, IP_TTL, option("ttl", 64, 1, 255));
            set(IPPROTO_IP, IP_TOS, option("tos", 0, 0, 255));
            if (input) {
                set(SOL_SOCKET, SO_REUSEADDR, 1);
                auto local = group; if (multicast) local.sin_addr.s_addr = htonl(INADDR_ANY);
                if (!multicast && adapter.s_addr) {
                    require(group.sin_addr.s_addr == htonl(INADDR_ANY) || group.sin_addr.s_addr == adapter.s_addr,
                            "UDP adapter conflicts with input address");
                    local.sin_addr = adapter;
                }
                if (::bind(fd_.value, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) system_error("UDP bind");
                if (multicast) { ip_mreq membership{group.sin_addr, adapter}; set(IPPROTO_IP, IP_ADD_MEMBERSHIP, membership); }
            } else if (multicast) {
                set(IPPROTO_IP, IP_MULTICAST_TTL, static_cast<unsigned char>(option("ttl", 1, 1, 255)));
                if (adapter.s_addr) set(IPPROTO_IP, IP_MULTICAST_IF, adapter);
            } else if (adapter.s_addr) {
                sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr = adapter;
#ifdef __APPLE__
                local.sin_len = sizeof(local);
#endif
                if (::bind(fd_.value, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) system_error("UDP adapter bind");
            }
        } else {
            const auto& group = reinterpret_cast<const sockaddr_in6&>(peer_.storage);
            if (IN6_IS_ADDR_MULTICAST(&group.sin6_addr)) throw std::invalid_argument("IPv6 multicast is not implemented");
            if (endpoint.udp_options.contains("adapter")) throw std::invalid_argument("IPv6 UDP adapter selection is not implemented");
            set(IPPROTO_IPV6, IPV6_UNICAST_HOPS, option("ttl", 64, 1, 255));
            set(IPPROTO_IPV6, IPV6_TCLASS, option("tos", 0, 0, 255));
            if (input && ::bind(fd_.value, reinterpret_cast<const sockaddr*>(&peer_.storage), peer_.size) < 0) system_error("UDP bind");
        }
        if (const auto it = endpoint.udp_options.find("multicast"); it != endpoint.udp_options.end())
            require(boolean(it->second) == multicast, "UDP multicast option does not match destination address");
    }
    ReadResult read(size_t chunk) override {
        Packet packet; packet.bytes.resize(chunk);
        iovec data{packet.bytes.data(), packet.bytes.size()}; msghdr message{}; message.msg_iov = &data; message.msg_iovlen = 1;
        const auto n = recvmsg(fd_.value, &message, 0);
        if (n < 0) { if (would_block()) return {ReadResult::State::would_block, {}}; system_error("UDP receive"); }
        require(!(message.msg_flags & MSG_TRUNC), "UDP datagram exceeds chunk size; increase -c (no truncation performed)");
        packet.bytes.resize(static_cast<size_t>(n)); return {ReadResult::State::packet, std::move(packet)};
    }
    std::optional<size_t> write(std::span<const std::byte> bytes) override {
        const auto n = sendto(fd_.value, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&peer_.storage), peer_.size);
        if (n < 0) { if (would_block()) return {}; system_error("UDP send"); }
        require(static_cast<size_t>(n) == bytes.size(), "Partial UDP datagram write");
        return static_cast<size_t>(n);
    }
    void wait(std::chrono::milliseconds timeout) override { fd_.wait(input_, timeout); }
};
class SrtMedium final : public Medium {
    EndpointOptions endpoint_;
    bool input_, ready_ = false, ended_ = false;
    const Options& options_;
    srtmux::Provider provider_;
    srtmux::Profile profile_;
    srtmux::Poller poller_;
    std::optional<srtmux::Listener> listener_;
    std::optional<srtmux::Connection> connection_;
    std::optional<srtmux::Registration> listener_registration_, connection_registration_;
    srtmux::Endpoint address_;
    std::optional<srtmux::Endpoint> local_;
    Clock::time_point retry_at_{};
    void log(const char* event) const {
        if (!options_.quiet) std::cerr << event << ' ' << (input_ ? "source" : "target") << " provider=" << endpoint_.provider << '\n';
    }
    void register_connection() {
        connection_registration_ = checked(poller_.add(*connection_, (input_ ? srtmux::Interest::read : srtmux::Interest::write) | srtmux::Interest::state | srtmux::Interest::error));
    }
    void disconnected(const srtmux::Error* error = nullptr) {
        if (error && error->category == srtmux::ErrorCategory::authentication) throw std::runtime_error("SRT authentication failed");
        if (!options_.reconnect && !ready_) throw std::runtime_error("SRT connection failed (auto-reconnect disabled)");
        if (!options_.reconnect) ended_ = true;
        log("Disconnected"); ready_ = false;
        if (connection_registration_) { checked(poller_.remove(*connection_registration_)); connection_registration_.reset(); }
        connection_.reset();
        if (!ended_ && listener_ && !listener_registration_) listener_registration_ = checked(poller_.add(*listener_, srtmux::Interest::read | srtmux::Interest::error));
        retry_at_ = Clock::now() + std::chrono::milliseconds(250);
    }
    void io_failed(const srtmux::IoError& error, const srtmux::Error& detail) {
        if (error.category == srtmux::ErrorCategory::connection_lost || error.category == srtmux::ErrorCategory::timeout || error.category == srtmux::ErrorCategory::peer_rejected) disconnected(&detail);
        else throw std::runtime_error("SRT I/O: " + detail.message);
    }
public:
    SrtMedium(EndpointOptions endpoint, bool input, const Options& options, Providers& providers)
        : endpoint_(std::move(endpoint)), input_(input), options_(options), provider_(providers.get(endpoint_.provider)),
          profile_(checked(provider_.create_profile(endpoint_.profile))), poller_(checked(provider_.create_poller())) {
        auto host = endpoint_.listener && endpoint_.local && !endpoint_.local->address.empty() ? endpoint_.local->address : endpoint_.host;
        auto resolved = resolve(host, endpoint_.port, endpoint_.listener);
        address_ = numeric(resolved);
        if (!endpoint_.listener && endpoint_.local) local_ = numeric(resolve(endpoint_.local->address, endpoint_.local->port, true, resolved.storage.ss_family));
        if (endpoint_.listener) {
            if (endpoint_.local && endpoint_.local->port && endpoint_.local->port != endpoint_.port) throw std::invalid_argument("Listener bind port must match URI port");
            listener_.emplace(checked(provider_.listen_profile(profile_, address_)));
            listener_registration_ = checked(poller_.add(*listener_, srtmux::Interest::read | srtmux::Interest::error));
            log("Listening");
        }
        if (options_.verbose && !options_.quiet) std::cerr << "Loaded " << (input_ ? "source" : "target") << " provider=" << provider_.info().implementation << " release=" << provider_.info().release << " fingerprint=" << provider_.info().build_fingerprint << '\n';
    }
    void tick() override {
        if (ended_) return;
        if (!connection_ && Clock::now() >= retry_at_) {
            if (listener_) {
                std::array<srtmux::Event, 1> events{};
                auto ready = poller_.wait(events, std::chrono::milliseconds(0));
                if (std::holds_alternative<srtmux::TimedOut>(ready)) return;
                require(std::holds_alternative<size_t>(ready), "SRT listener poll failed");
                if (!std::get<size_t>(ready)) return;
                auto accepted = listener_->accept();
                if (auto* error = std::get_if<srtmux::Error>(&accepted)) throw std::runtime_error("SRT accept: " + error->message);
                if (std::holds_alternative<srtmux::WouldBlock>(accepted)) return;
                connection_.emplace(std::get<srtmux::Connection>(std::move(accepted)));
                checked(poller_.remove(*listener_registration_)); listener_registration_.reset();
            } else connection_.emplace(checked(provider_.connect_profile(profile_, address_, local_)));
            register_connection();
        }
        if (!connection_) return;
        auto info = checked(connection_->info());
        if (info.state == srtmux::State::connected) {
            if (!ready_) { ready_ = true; log("Connected"); }
        } else if (info.state == srtmux::State::failed || info.state == srtmux::State::closed || info.state == srtmux::State::closing) disconnected(info.terminal_error ? &*info.terminal_error : nullptr);
    }
    bool connected() const override { return ready_; }
    bool ended() const override { return ended_; }
    ReadResult read(size_t chunk) override {
        if (!ready_) return {ReadResult::State::would_block, {}};
        Packet packet; packet.bytes.resize(chunk); srtmux::Error detail{};
        auto result = connection_->receive_message(packet.bytes, &detail);
        if (auto* error = std::get_if<srtmux::IoError>(&result)) { io_failed(*error, detail); return {ReadResult::State::would_block, {}}; }
        if (std::holds_alternative<srtmux::WouldBlock>(result)) return {ReadResult::State::would_block, {}};
        packet.bytes.resize(std::get<srtmux::ReceivedMessage>(result).bytes);
        return {ReadResult::State::packet, std::move(packet)};
    }
    std::optional<size_t> write(std::span<const std::byte> bytes) override {
        if (!ready_) return {};
        srtmux::Error detail{}; auto result = connection_->send_message(bytes, {}, &detail);
        if (auto* error = std::get_if<srtmux::IoError>(&result)) { io_failed(*error, detail); return {}; }
        if (std::holds_alternative<srtmux::WouldBlock>(result)) return {};
        return bytes.size();
    }
    void wait(std::chrono::milliseconds timeout) override {
        // An empty caller poller cannot wait until the next scheduled retry.
        if (!connection_registration_ && !listener_registration_) { (void)poll(nullptr, 0, static_cast<int>(timeout.count())); return; }
        std::array<srtmux::Event, 4> events{}; srtmux::Error detail{};
        auto result = poller_.wait(events, timeout, &detail);
        if (std::holds_alternative<srtmux::IoError>(result)) throw std::runtime_error("SRT poll: " + detail.message);
    }
    std::string statistics() override {
        if (!ready_) return "null";
        constexpr const char* names[] = {"native.pktSentTotal", "native.pktRecvTotal", "native.pktRetransTotal", "native.pktRcvDropTotal", "native.pktSndDropTotal", "native.msRTT"};
        std::array<uint32_t, 6> ids{}; std::array<srtmx_metric_v1, 6> values{};
        for (size_t i = 0; i < ids.size(); ++i) { ids[i] = checked(connection_->resolve_metric(names[i])).id; values[i].struct_size = sizeof(values[i]); }
        auto result = connection_->native_snapshot(ids, values);
        if (!std::holds_alternative<srtmx_snapshot_v1>(result)) throw std::runtime_error("SRT statistics snapshot failed");
        std::ostringstream text; text << "{\"provider\":\"" << endpoint_.provider << '"';
        for (size_t i = 0; i < ids.size(); ++i) {
            text << ",\"" << names[i] << "\":";
            if (values[i].value_kind == 1) text << values[i].value.u64;
            else if (values[i].value_kind == 2) text << values[i].value.i64;
            else text << values[i].value.f64;
        }
        text << '}'; return text.str();
    }
};
}
srtmux::Provider Providers::get(const std::string& name) {
    auto found = loaded_.find(name);
    if (found == loaded_.end()) found = loaded_.emplace(name, checked(runtime_.load({module_path(options_, name), name, name == "haivision" ? "1.5.7" : "0.3.3", {}}))).first;
    return found->second;
}
std::unique_ptr<Medium> open_medium(EndpointOptions endpoint, bool input, const Options& options, Providers& providers) {
    if (endpoint.kind == EndpointOptions::Kind::file) return std::make_unique<FileMedium>(endpoint, input);
    if (endpoint.kind == EndpointOptions::Kind::udp) return std::make_unique<UdpMedium>(endpoint, input);
    return std::make_unique<SrtMedium>(std::move(endpoint), input, options, providers);
}
}
