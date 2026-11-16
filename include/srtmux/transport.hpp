// C++20 transport facade. Working API; not yet ABI-stable. No vendor headers.
#ifndef SRTMUX_TRANSPORT_HPP
#define SRTMUX_TRANSPORT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace srtmux {
// Threading: one sender and one receiver per connection; serialize setup and
// same-direction calls. close()/runtime shutdown may race I/O. One waiter per
// poller; add/update/remove/wake may race wait. Do not move/destroy a wrapper
// while it is in use. Call info() until connected (also on accepted sockets)
// before I/O. Setup operations may throw std::bad_alloc; I/O is allocation-free
// except optional rich error capture on failure. Moved-from wrappers support
// destruction and assignment only. See providers/README.md for lifetime rules.

namespace detail { struct Access; }

enum class ErrorCategory {
    invalid_argument, unsupported, invalid_state, invalid_handle,
    authentication, peer_rejected, connection_lost, resource, internal,
    abi_mismatch, busy, timeout
};
struct ProviderInfo {
    std::string implementation, release, source_revision;
    std::string build_fingerprint, target_triple, build_features;
};
struct Error {
    ErrorCategory category;
    std::string operation, message;
    std::optional<ProviderInfo> provider;
    std::optional<std::int32_t> vendor_code, os_code, rejection_code;
    bool message_truncated;
};
template<class T> using Result = std::variant<T, Error>;
using Success = std::monostate;

struct WouldBlock {};
struct Pending {};
struct EndOfStream {};
struct TimedOut {};
struct Interrupted {};
// Compact, allocation-free failure on the data path. Provider identity is
// already owned by Connection/Poller; optional Error output captures text only
// on failure, on the calling thread, without a later last-error lookup.
struct IoError {
    ErrorCategory category;
    std::uint32_t detail_flags;
    std::int32_t vendor_code, os_code, rejection_code;
};
template<class T> using IoResult = std::variant<T, WouldBlock, IoError>;
using ConnectResult = std::variant<Success, Pending, Error>;
using CloseResult = std::variant<Success, Pending, TimedOut, Error>;

// Numeric IP endpoint, never DNS resolution in a provider operation.
struct Endpoint {
    std::string address;
    std::uint16_t port;
    std::uint32_t scope_id;
};
enum class Mode { live_messages, file_stream };
enum class Role { caller, listener };
enum class Encryption { disabled, aes_ctr_required };
enum class Accounting { disabled, payload_bytes };
enum class State { created, bound, connecting, connected, listening, failed, closing, closed };

// Secret storage is owned, noncopyable, and erased on release. TransportConfig
// shares ownership rather than copying secret bytes. Import copies its input;
// the caller remains responsible for erasing its original buffer.
class Secret final {
public:
    static Result<std::shared_ptr<const Secret>> copy(std::span<const std::byte>);
    ~Secret() noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
private:
    friend struct detail::Access;
    Secret();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// No implicit vendor defaults. Named profile factories return fully populated
// settings. All settings in this initial configuration are required.
struct TransportConfig {
    Mode mode;
    Accounting accounting; // Disabled by default; no mandatory per-I/O counters.
    Encryption encryption;
    std::uint32_t key_bits;
    std::chrono::milliseconds receive_latency, peer_latency;
    std::chrono::milliseconds connect_timeout, peer_idle_timeout;
    std::uint32_t mss_bytes, payload_bytes;
    std::uint64_t send_buffer_bytes, receive_buffer_bytes;
    std::vector<std::byte> stream_id;
    std::shared_ptr<const Secret> passphrase;
};
TransportConfig live_profile();
TransportConfig file_stream_profile();

enum class Feature {
    ipv4, ipv6, live_messages, file_stream, aes_ctr,
    admission, polling, basic_statistics,
    rendezvous, fec, broadcast_groups, backup_groups, aes_gcm,
    native_udp_adoption, group_admission_domains, rtt_observation
};
enum class Availability { available, not_built, unsupported, experimental };
struct Capability {
    Feature feature;
    Availability availability;
    bool requires_peer;
    std::string constraints;
};
struct ValidationIssue {
    enum class Severity { error, peer_negotiation_required };
    Severity severity;
    std::string field;
    Error detail;
};
struct ValidationReport { std::vector<ValidationIssue> issues; };

struct AdmissionRequest {
    Endpoint remote;
    std::span<const std::byte> stream_id; // Borrowed for callback only.
};
struct Admit {};
struct Reject { std::int32_t application_rejection_code; };
using AdmissionDecision = std::variant<Admit, Reject>;
using AdmissionHandler = std::function<AdmissionDecision(const AdmissionRequest&)>;
struct ListenerConfig {
    Endpoint local;
    std::uint32_t backlog;
    TransportConfig transport;
    AdmissionHandler admission; // Empty accepts ordinary connections.
};

// Clock identity is provider-runtime-specific. Host steady_clock and timestamps
// from a different Provider are not interchangeable with this value.
struct SourceTime {
    std::uint64_t clock_domain;
    std::chrono::microseconds value;
};
struct MessageInfo { std::optional<SourceTime> source_time; };
struct ReceivedMessage { std::size_t bytes; MessageInfo info; };
struct ConnectionInfo {
    State state;
    std::optional<Endpoint> local, remote;
    std::optional<std::uint32_t> max_message_bytes;
    std::optional<Encryption> encryption;
    std::optional<std::uint32_t> key_bits;
    std::optional<std::chrono::milliseconds> receive_latency, peer_latency;
    std::optional<Error> terminal_error;
};
struct Statistics {
    SourceTime sampled_at;
    std::uint64_t counter_epoch;
    std::optional<std::uint64_t> accepted_tx_bytes, delivered_rx_bytes;
    std::optional<std::chrono::microseconds> rtt_estimate;
};
struct AbortClose {};
struct DrainClose { std::chrono::milliseconds timeout; };
using ClosePolicy = std::variant<AbortClose, DrainClose>;

class Provider;
class Connection;
class Listener;
class Poller;

class Connection final {
public:
    ~Connection() noexcept;
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    const ProviderInfo& provider_info() const noexcept;
    Result<Success> bind(const Endpoint&);
    ConnectResult connect(const Endpoint&);
    Result<ConnectionInfo> info() const;
    IoResult<Success> send_message(std::span<const std::byte>, const MessageInfo&,
                                   Error* failure_detail = nullptr);
    IoResult<ReceivedMessage> receive_message(std::span<std::byte>,
                                              Error* failure_detail = nullptr);
    IoResult<std::size_t> write_stream(std::span<const std::byte>,
                                     Error* failure_detail = nullptr);
    std::variant<std::size_t, WouldBlock, EndOfStream, IoError>
        read_stream(std::span<std::byte>, Error* failure_detail = nullptr);
    Result<Statistics> statistics() const;
    CloseResult close(ClosePolicy);
    CloseResult close_status() const;
private:
    friend class Provider;
    friend class Listener;
    friend class Poller;
    struct Impl;
    explicit Connection(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

class Listener final {
public:
    ~Listener() noexcept;
    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    const ProviderInfo& provider_info() const noexcept;
    Result<Endpoint> local_endpoint() const;
    std::variant<Connection, WouldBlock, Error> accept();
    CloseResult close();
    CloseResult close_status() const;
private:
    friend class Provider;
    friend class Poller;
    struct Impl;
    explicit Listener(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

enum class Interest : std::uint32_t {
    read = 1, write = 2, error = 4, state = 8
};
Interest operator|(Interest, Interest) noexcept;
bool contains(Interest mask, Interest bit) noexcept;
struct Registration { std::uint64_t id; }; // Poller-scoped generation token.
struct Event { Registration registration; Interest readiness; };
struct InfiniteWait {};
using WaitTimeout = std::variant<std::chrono::milliseconds, InfiniteWait>;
using WaitResult = std::variant<std::size_t, TimedOut, Interrupted, IoError>;

class Poller final {
public:
    ~Poller() noexcept;
    Poller(Poller&&) noexcept;
    Poller& operator=(Poller&&) noexcept;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    Result<Registration> add(const Connection&, Interest);
    Result<Registration> add(const Listener&, Interest);
    Result<Success> update(Registration, Interest);
    Result<Success> remove(Registration);
    WaitResult wait(std::span<Event>, WaitTimeout, Error* failure_detail = nullptr);
    Result<Success> wake(); // Sticky/coalescing wake; safe concurrent with wait.
private:
    friend class Provider;
    struct Impl;
    explicit Poller(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

// Copyable shared context, not a second vendor initialization per copy.
class Provider final {
public:
    ~Provider() noexcept;
    Provider(const Provider&) noexcept;
    Provider& operator=(const Provider&) noexcept;
    Provider(Provider&&) noexcept;
    Provider& operator=(Provider&&) noexcept;

    const ProviderInfo& info() const noexcept;
    Result<Capability> capability(Feature, Role, const TransportConfig&) const;
    Result<ValidationReport> validate(Role, const TransportConfig&) const;
    Result<SourceTime> now() const;
    Result<Connection> create_connection(const TransportConfig&) const;
    Result<Listener> listen(const ListenerConfig&) const;
    Result<Poller> create_poller() const;
private:
    friend class TransportRuntime;
    struct Impl;
    explicit Provider(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

struct ProviderSpec {
    std::filesystem::path module_path; // Absolute path, no ambient search.
    std::string expected_implementation, expected_release;
    std::optional<std::string> expected_build_fingerprint;
};
struct LogRecord {
    ProviderInfo provider;
    enum class Severity { trace, debug, info, warning, error } severity;
    std::string message;
};
struct RuntimeOptions {
    std::function<void(const LogRecord&)> log;
    std::size_t max_queued_logs = 1024; // Bounded; drop excess diagnostic records.
};
class TransportRuntime final {
public:
    static Result<TransportRuntime> create(RuntimeOptions = {});
    ~TransportRuntime() noexcept;
    TransportRuntime(TransportRuntime&&) noexcept;
    TransportRuntime& operator=(TransportRuntime&&) noexcept;
    TransportRuntime(const TransportRuntime&) = delete;
    TransportRuntime& operator=(const TransportRuntime&) = delete;

    Result<Provider> load(const ProviderSpec&);
    Result<Success> begin_shutdown();
    std::variant<Success, Pending, Error> shutdown_status() const;
private:
    struct Impl;
    explicit TransportRuntime(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
};

} // namespace srtmux
#endif
