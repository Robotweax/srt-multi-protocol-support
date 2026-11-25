#include "media.hpp"
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>

namespace live_transmit {
namespace {
[[noreturn]] void system_error(const char* operation) {
#ifdef _WIN32
    throw std::system_error(WSAGetLastError(), std::system_category(), operation);
#else
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
#endif
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
#ifdef _WIN32
#include "windows_media.inc"
#else
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
    ReadResult read(std::span<std::byte> buffer) override {
        const auto n = ::read(fd_.value, buffer.data(), buffer.size());
        if (n < 0) { if (would_block()) return {ReadResult::State::would_block}; system_error("Read media file"); }
        if (n == 0) return {ReadResult::State::eof};
        return {ReadResult::State::packet, static_cast<size_t>(n)};
    }
    // Like the original's blocking console write: the whole packet, in order.
    Write write(std::span<const std::byte> bytes) override {
        while (!bytes.empty()) {
            const auto n = ::write(fd_.value, bytes.data(), bytes.size());
            if (n < 0) {
                if (!would_block()) system_error("Write media file");
                if (stop_requested()) return Write::blocked;
                fd_.wait(false, std::chrono::milliseconds(100)); continue;
            }
            bytes = bytes.subspan(static_cast<size_t>(n));
        }
        return Write::done;
    }
    bool pausable() const override { return input_; }
    void wait(std::chrono::milliseconds timeout) override { fd_.wait(input_, timeout); }
};
using SocketDescriptor = Descriptor;
#endif
class UdpMedium final : public Medium {
    SocketDescriptor fd_; Address peer_; bool input_;
public:
    UdpMedium(const EndpointOptions& endpoint, bool input) : peer_(resolve(endpoint.host, endpoint.port, input)), input_(input) {
        fd_.value = socket(peer_.storage.ss_family, SOCK_DGRAM, 0);
#ifdef _WIN32
        if (fd_.value == INVALID_SOCKET) system_error("UDP socket");
#else
        if (fd_.value < 0) system_error("UDP socket");
#endif
        fd_.nonblocking();
        auto set = [&](int level, int option, auto value) {
            if (setsockopt(fd_.value, level, option, reinterpret_cast<const char*>(&value), sizeof(value)) < 0) system_error("UDP socket option");
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
            if (endpoint.udp_options.contains("tos")) set(IPPROTO_IP, IP_TOS, option("tos", 0, 0, 255));
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
                set(IPPROTO_IP, IP_MULTICAST_TTL, option("ttl", 1, 1, 255));
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
            if (endpoint.udp_options.contains("tos")) set(IPPROTO_IPV6, IPV6_TCLASS, option("tos", 0, 0, 255));
            if (input && ::bind(fd_.value, reinterpret_cast<const sockaddr*>(&peer_.storage), peer_.size) < 0) system_error("UDP bind");
        }
        if (const auto it = endpoint.udp_options.find("multicast"); it != endpoint.udp_options.end())
            require(boolean(it->second) == multicast, "UDP multicast option does not match destination address");
    }
    ReadResult read(std::span<std::byte> buffer) override {
#ifdef _WIN32
        const int n = recv(fd_.value, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (n == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAEMSGSIZE) return {ReadResult::State::dropped, buffer.size()};
            if (would_block() || error == WSAECONNRESET) return {ReadResult::State::would_block};
            system_error("UDP receive");
        }
        return {ReadResult::State::packet, static_cast<size_t>(n)};
#else
        iovec data{buffer.data(), buffer.size()}; msghdr message{}; message.msg_iov = &data; message.msg_iovlen = 1;
        const auto n = recvmsg(fd_.value, &message, 0);
        if (n < 0) { if (would_block()) return {ReadResult::State::would_block}; system_error("UDP receive"); }
        // Never forward a truncated datagram, and never let one stop the relay.
        return {message.msg_flags & MSG_TRUNC ? ReadResult::State::dropped : ReadResult::State::packet, static_cast<size_t>(n)};
#endif
    }
    Write write(std::span<const std::byte> bytes) override {
        const auto n = sendto(fd_.value, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&peer_.storage), peer_.size);
        if (n < 0) {
            // A full buffer or a transient ICMP report loses this datagram only.
#ifdef _WIN32
            const auto error = WSAGetLastError();
            if (would_block() || error == WSAENOBUFS || error == WSAECONNRESET || error == WSAECONNREFUSED) return Write::blocked;
            if (error == WSAEMSGSIZE) return Write::rejected;
#else
            if (would_block() || errno == ENOBUFS || errno == ECONNREFUSED) return Write::blocked;
            if (errno == EMSGSIZE) return Write::rejected;
#endif
            system_error("UDP send");
        }
        require(static_cast<size_t>(n) == bytes.size(), "Partial UDP datagram write");
        return Write::done;
    }
    void wait(std::chrono::milliseconds timeout) override { fd_.wait(input_, timeout); }
};
// One SRT endpoint. As in the original, a listener serves one client at a time:
// it stops listening while that client is connected and listens again after it
// leaves. Connection state is read when the poller reports a change, with a slow
// periodic check for the endpoint the relay is not currently waiting on.
class SrtMedium final : public Medium {
    EndpointOptions endpoint_;
    bool input_, ready_ = false, ended_ = false, dirty_ = false;
    const Options& options_;
    srtmux::Provider provider_;
    srtmux::Profile profile_;
    srtmux::Poller poller_;
    std::optional<srtmux::Listener> listener_;
    std::optional<srtmux::Connection> connection_;
    std::optional<srtmux::Registration> listener_registration_, connection_registration_;
    srtmux::Endpoint address_;
    std::optional<srtmux::Endpoint> local_;
    Clock::time_point retry_at_{}, check_at_{};
    size_t payload_ = 0;
    std::string last_failure_;
    std::vector<uint32_t> stat_ids_;
    uint32_t session_ = 0; // Changes with every connection, like a native socket ID.
    const char* side() const { return input_ ? "source" : "target"; }
    bool npd_requested() const { return endpoint_.profile.null_packet_deletion != srtmux::NullPacketDeletion::off; }
    uint64_t npd_counter(const char* name) {
        std::array<uint32_t, 1> id{checked(connection_->resolve_metric(name)).id};
        std::array<srtmx_metric_v1, 1> value{}; value[0].struct_size = sizeof(value[0]);
        auto result = connection_->native_snapshot(id, value);
        if (!std::holds_alternative<srtmx_snapshot_v1>(result)) throw std::runtime_error("SRT statistics snapshot failed");
        return value[0].value.u64;
    }
    srtmux::Interest interest() const {
        using srtmux::Interest;
        // STATE is level-triggered once connected, so it is requested only
        // while the connection attempt is still in progress.
        if (!ready_) return Interest::state | Interest::error;
        return (input_ ? Interest::read : Interest::write) | Interest::error;
    }
    // A failed attempt is reported once per distinct reason, not on every retry.
    void attempt_failed(const std::string& reason, bool authentication = false) {
        if (!options_.reconnect) throw std::runtime_error(authentication ? "SRT authentication failed: " + reason : "SRT connection failed (auto-reconnect disabled): " + reason);
        if (!options_.quiet && reason != last_failure_) std::cerr << "SRT " << side() << " connection failed: " << reason << "; retrying\n";
        last_failure_ = reason;
    }
    void release_connection() {
        ready_ = false; payload_ = 0;
        if (connection_registration_) { (void)poller_.remove(*connection_registration_); connection_registration_.reset(); }
        connection_.reset();
        retry_at_ = Clock::now() + std::chrono::milliseconds(250);
    }
    void disconnected(const srtmux::Error* error = nullptr) {
        const bool was_connected = ready_;
        release_connection();
        if (was_connected) {
            if (!options_.quiet) std::cerr << "SRT " << side() << " disconnected\n";
            if (!options_.reconnect) ended_ = true;
            return;
        }
        const bool authentication = error && error->category == srtmux::ErrorCategory::authentication;
        std::string reason = authentication ? "wrong or missing passphrase" : error && !error->message.empty() ? error->message : "no connection";
        if (error && error->rejection_code) reason += " (rejection " + std::to_string(*error->rejection_code) + ")";
        attempt_failed(reason, authentication);
    }
    // Works from the compact error alone, so the data path never builds strings.
    void io_failed(const srtmux::IoError& error) {
        if (error.category == srtmux::ErrorCategory::connection_lost || error.category == srtmux::ErrorCategory::timeout || error.category == srtmux::ErrorCategory::peer_rejected) disconnected();
        else throw std::runtime_error("SRT I/O failed (category " + std::to_string(static_cast<int>(error.category)) + ", native code " + std::to_string(error.vendor_code) + ")");
    }
    bool listen() {
        auto listener = provider_.listen_profile(profile_, address_);
        if (auto* error = std::get_if<srtmux::Error>(&listener)) {
            // The port of the previous listener may still be settling; retry.
            attempt_failed("listen: " + error->message); retry_at_ = Clock::now() + std::chrono::milliseconds(250); return false;
        }
        listener_.emplace(std::get<srtmux::Listener>(std::move(listener)));
        listener_registration_ = checked(poller_.add(*listener_, srtmux::Interest::read | srtmux::Interest::error));
        if (options_.verbose && !options_.quiet) std::cerr << "Listening " << side() << " provider=" << endpoint_.provider << '\n';
        return true;
    }
    bool accept() {
        std::array<srtmux::Event, 1> events{};
        auto ready = poller_.wait(events, std::chrono::milliseconds(0));
        if (std::holds_alternative<srtmux::TimedOut>(ready) || std::holds_alternative<srtmux::Interrupted>(ready)) return false;
        require(std::holds_alternative<size_t>(ready), "SRT listener poll failed");
        auto accepted = listener_->accept();
        if (auto* error = std::get_if<srtmux::Error>(&accepted)) throw std::runtime_error("SRT accept: " + error->message);
        if (std::holds_alternative<srtmux::WouldBlock>(accepted)) return false;
        connection_.emplace(std::get<srtmux::Connection>(std::move(accepted)));
        // One client at a time: later callers are refused, not parked unseen.
        (void)poller_.remove(*listener_registration_); listener_registration_.reset(); listener_.reset();
        if (!options_.quiet) std::cerr << "Accepted SRT " << side() << " connection\n";
        return true;
    }
    bool connect() {
        auto connection = provider_.connect_profile(profile_, address_, local_);
        if (auto* error = std::get_if<srtmux::Error>(&connection)) {
            attempt_failed("connect: " + error->message); retry_at_ = Clock::now() + std::chrono::milliseconds(250); return false;
        }
        connection_.emplace(std::get<srtmux::Connection>(std::move(connection)));
        return true;
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
            // The first listen must succeed; only later re-listens are retried.
            listener_.emplace(checked(provider_.listen_profile(profile_, address_)));
            listener_registration_ = checked(poller_.add(*listener_, srtmux::Interest::read | srtmux::Interest::error));
            if (options_.verbose && !options_.quiet) std::cerr << "Listening " << side() << " provider=" << endpoint_.provider << '\n';
        }
        if (options_.verbose && !options_.quiet) std::cerr << "Loaded " << side() << " provider=" << provider_.info().implementation << " release=" << provider_.info().release << " fingerprint=" << provider_.info().build_fingerprint << '\n';
    }
    void tick() override {
        if (ended_) return;
        const auto now = Clock::now();
        if (!connection_) {
            if (now < retry_at_) return;
            if (endpoint_.listener) { if ((!listener_ && !listen()) || !accept()) return; }
            else if (!connect()) return;
            connection_registration_ = checked(poller_.add(*connection_, interest()));
            dirty_ = true;
        }
        if (!dirty_ && now < check_at_) return;
        dirty_ = false;
        auto info = checked(connection_->info());
        if (info.state == srtmux::State::connected) {
            if (!ready_) {
                ready_ = true; last_failure_.clear();
                static uint32_t sessions = 0; session_ = ++sessions;
                payload_ = info.max_message_bytes.value_or(0);
                checked(poller_.update(*connection_registration_, interest()));
                if (!endpoint_.listener && !options_.quiet) std::cerr << "SRT " << side() << " connected\n";
                if (npd_requested() && options_.verbose && !options_.quiet)
                    std::cerr << "Null packet deletion " << (npd_counter("robotweax.npd.active") ? "active" : "not negotiated") << " on " << side() << '\n';
            }
        } else if (info.state == srtmux::State::failed || info.state == srtmux::State::closed || info.state == srtmux::State::closing)
            disconnected(info.terminal_error ? &*info.terminal_error : nullptr);
        check_at_ = now + std::chrono::milliseconds(ready_ ? 100 : 10);
    }
    bool connected() const override { return ready_; }
    bool ended() const override { return ended_; }
    size_t message_limit() const override { return payload_ ? payload_ : endpoint_.profile.transport.payload_bytes; }
    ReadResult read(std::span<std::byte> buffer) override {
        if (!ready_) return {ReadResult::State::would_block};
        // The rich error record is filled only on failure; build it only then.
        auto result = connection_->receive_message(buffer);
        if (auto* error = std::get_if<srtmux::IoError>(&result)) { io_failed(*error); return {ReadResult::State::would_block}; }
        if (std::holds_alternative<srtmux::WouldBlock>(result)) return {ReadResult::State::would_block};
        return {ReadResult::State::packet, std::get<srtmux::ReceivedMessage>(result).bytes};
    }
    Write write(std::span<const std::byte> bytes) override {
        if (!ready_) return Write::blocked;
        if (bytes.empty() || bytes.size() > payload_) return Write::rejected;
        auto result = connection_->send_message(bytes, {});
        if (auto* error = std::get_if<srtmux::IoError>(&result)) { io_failed(*error); return Write::blocked; }
        if (std::holds_alternative<srtmux::WouldBlock>(result)) return Write::blocked;
        return Write::done;
    }
    void wait(std::chrono::milliseconds timeout) override {
        // A poller with no registration cannot wait until the next retry.
        if (!connection_registration_ && !listener_registration_) { std::this_thread::sleep_for(timeout); return; }
        std::array<srtmux::Event, 2> events{}; srtmux::Error detail{};
        auto result = poller_.wait(events, timeout, &detail);
        if (std::holds_alternative<srtmux::IoError>(result)) throw std::runtime_error("SRT poll: " + detail.message);
        if (auto* count = std::get_if<size_t>(&result))
            for (size_t i = 0; i < *count; ++i)
                if (srtmux::contains(events[i].readiness, srtmux::Interest::error) || srtmux::contains(events[i].readiness, srtmux::Interest::state)) dirty_ = true;
    }
    bool native_stats(NativeStats& out) override {
        if (!ready_) return false;
        if (stat_ids_.empty()) { // Metric IDs are fixed for a provider; resolve them once.
            std::vector<uint32_t> ids;
            for (const auto* name : stat_names()) ids.push_back(checked(connection_->resolve_metric(name)).id);
            stat_ids_ = std::move(ids);
        }
        std::array<srtmx_metric_v1, stat_count> values{};
        for (auto& value : values) value.struct_size = sizeof(value);
        auto result = connection_->native_snapshot(stat_ids_, values);
        if (!std::holds_alternative<srtmx_snapshot_v1>(result)) return false;
        out.id = session_;
        for (size_t i = 0; i < stat_count; ++i) {
            auto& value = out.values[i];
            if (values[i].value_kind == 1) { value.kind = StatValue::Kind::unsigned_integer; value.u = values[i].value.u64; }
            else if (values[i].value_kind == 2) { value.kind = StatValue::Kind::signed_integer; value.i = values[i].value.i64; }
            else { value.kind = StatValue::Kind::real; value.f = values[i].value.f64; }
        }
        return true;
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
        if (npd_requested())
            for (const char* name : {"robotweax.npd.active", "robotweax.npd.deleted_units", "robotweax.npd.restored_units", "robotweax.npd.malformed_payloads"})
                text << ",\"" << name << "\":" << npd_counter(name);
        text << '}'; return text.str();
    }
};
}
srtmux::Provider Providers::get(const std::string& name) {
    auto found = loaded_.find(name);
    if (found == loaded_.end()) found = loaded_.emplace(name, checked(runtime_.load({module_path(options_, name), name, name == "haivision" ? "1.5.7" : "0.3.5", {}}))).first;
    return found->second;
}
std::unique_ptr<Medium> open_medium(EndpointOptions endpoint, bool input, const Options& options, Providers& providers) {
    if (endpoint.kind == EndpointOptions::Kind::file) return std::make_unique<FileMedium>(endpoint, input);
    if (endpoint.kind == EndpointOptions::Kind::udp) return std::make_unique<UdpMedium>(endpoint, input);
    return std::make_unique<SrtMedium>(std::move(endpoint), input, options, providers);
}
}
