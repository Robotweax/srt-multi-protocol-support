#include <srtmux/transport.hpp>
#include <srtmux/module.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace srtmux {
struct Secret::Impl { std::vector<std::byte> bytes; };
Secret::Secret() : impl_(std::make_unique<Impl>()) {}
Secret::~Secret() noexcept {
    volatile std::byte* p = impl_->bytes.data();
    for (size_t i = 0; i < impl_->bytes.size(); ++i) p[i] = std::byte{0};
}
namespace detail {
struct Access {
    static std::span<const std::byte> secret(const Secret& s) { return s.impl_->bytes; }
};
}
namespace {
template<class T> T record() { T v{}; v.struct_size = sizeof(v); return v; }
ErrorCategory category(uint32_t code) {
    return code >= SRTM_ERR_INVALID_ARGUMENT && code <= SRTM_ERR_TIMEOUT
        ? static_cast<ErrorCategory>(code-1) : ErrorCategory::internal;
}
ProviderInfo translate(const host::ModuleInfo& v) {
    return {v.implementation, v.release, v.source_revision, v.build_fingerprint, v.target_triple, v.build_features};
}
Error local_error(ErrorCategory code, const char* operation, const char* message) {
    return {code, operation, message, {}, {}, {}, {}, false};
}
struct Context {
    host::Module module;
    const srtm_provider_api_v1& api;
    ProviderInfo info;
    srtm_runtime* runtime = nullptr;
    uint64_t domain;
    Context(host::Module m, uint64_t clock_domain) : module(std::move(m)), api(module.api()), info(translate(module.info())), domain(clock_domain) {}
    ~Context() {
        if (runtime) { api.runtime_begin_shutdown(runtime, nullptr); api.runtime_destroy(runtime, nullptr); }
    }
};
Error rich_error(const Context* c, const char* operation, const srtm_error_v1& value) {
    Error out = local_error(category(value.category), operation, "");
    out.message.assign(value.message, std::min<size_t>(value.message_length, sizeof(value.message)-1));
    out.message_truncated = value.message_truncated != 0;
    if (c) out.provider = c->info;
    if (value.detail_flags & 1) out.vendor_code = value.vendor_code;
    if (value.detail_flags & 2) out.os_code = value.os_code;
    if (value.detail_flags & 4) out.rejection_code = value.rejection_code;
    return out;
}
IoError io_error(const srtm_io_error_v1& v) { return {category(v.category), v.detail_flags, v.vendor_code, v.os_code, v.rejection_code}; }
IoError invalid_io(Error* detail, const char* operation) {
    if (detail) *detail = local_error(ErrorCategory::invalid_argument, operation, "Invalid argument or clock domain");
    return {ErrorCategory::invalid_argument, 0, 0, 0, 0};
}
Result<srtm_endpoint> endpoint(const Endpoint& input) {
    srtm_endpoint value{}; value.port = input.port; value.scope_id = input.scope_id;
    if (inet_pton(AF_INET, input.address.c_str(), value.address) == 1) {
        if (input.scope_id) return local_error(ErrorCategory::invalid_argument, "endpoint", "IPv4 scope must be zero");
        value.family = SRTM_AF_IPV4;
    } else if (inet_pton(AF_INET6, input.address.c_str(), value.address) == 1) value.family = SRTM_AF_IPV6;
    else return local_error(ErrorCategory::invalid_argument, "endpoint", "A numeric IPv4/IPv6 address is required");
    return value;
}
Endpoint endpoint(const srtm_endpoint& value) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    inet_ntop(value.family == SRTM_AF_IPV4 ? AF_INET : AF_INET6, value.address, text.data(), static_cast<unsigned>(text.size()));
    return {text.data(), value.port, value.scope_id};
}
Result<srtm_config_v1> config(const TransportConfig& input) {
    for (auto duration : {input.receive_latency, input.peer_latency, input.connect_timeout, input.peer_idle_timeout})
        if (duration.count() < 0 || duration.count() > INT_MAX)
            return local_error(ErrorCategory::invalid_argument, "configuration", "Duration is outside the supported range");
    auto c = record<srtm_config_v1>(); c.mode = static_cast<uint32_t>(input.mode)+1;
    c.accounting = static_cast<uint32_t>(input.accounting); c.encryption = static_cast<uint32_t>(input.encryption);
    c.key_bits = input.key_bits; c.receive_latency_ms = static_cast<uint32_t>(input.receive_latency.count());
    c.peer_latency_ms = static_cast<uint32_t>(input.peer_latency.count()); c.connect_timeout_ms = static_cast<uint32_t>(input.connect_timeout.count());
    c.peer_idle_timeout_ms = static_cast<uint32_t>(input.peer_idle_timeout.count()); c.mss_bytes = input.mss_bytes; c.payload_bytes = input.payload_bytes;
    c.send_buffer_bytes = input.send_buffer_bytes; c.receive_buffer_bytes = input.receive_buffer_bytes;
    c.stream_id = {reinterpret_cast<const uint8_t*>(input.stream_id.data()), input.stream_id.size()};
    if (input.passphrase) { auto secret = detail::Access::secret(*input.passphrase); c.passphrase = {reinterpret_cast<const uint8_t*>(secret.data()), secret.size()}; }
    return c;
}
constexpr const char* features[] = {"core.ipv4", "core.ipv6", "core.live_messages", "core.file_stream", "crypto.aes_ctr",
    "core.admission", "core.polling", "core.statistics", "transport.rendezvous", "recovery.fec", "groups.broadcast",
    "groups.backup", "crypto.aes_gcm", "transport.native_udp_adoption", "groups.admission_domains", "telemetry.rtt_observation"};
struct OwnedObject {
    std::shared_ptr<Context> context;
    srtm_handle handle;
    OwnedObject(std::shared_ptr<Context> c, srtm_handle h) : context(std::move(c)), handle(h) {}
    ~OwnedObject() {
        if (!handle) return;
        context->api.object_close(context->runtime, handle, SRTM_CLOSE_ABORT, 0, nullptr);
        context->api.object_release(context->runtime, handle, nullptr);
    }
};
CloseResult close_object(OwnedObject& object, ClosePolicy policy) {
    uint32_t mode = SRTM_CLOSE_ABORT; uint64_t timeout = 0;
    if (auto* drain = std::get_if<DrainClose>(&policy)) {
        if (drain->timeout.count() < 0) return local_error(ErrorCategory::invalid_argument, "close", "Negative drain timeout");
        mode = SRTM_CLOSE_DRAIN; timeout = static_cast<uint64_t>(drain->timeout.count());
    }
    auto d = record<srtm_error_v1>();
    auto result = object.context->api.object_close(object.context->runtime, object.handle, mode, timeout, &d);
    if (result == SRTM_OK) return Success{};
    if (result == SRTM_PENDING) return Pending{};
    if (result == SRTM_TIMED_OUT) return TimedOut{};
    return rich_error(object.context.get(), "close", d);
}
CloseResult close_status(const OwnedObject& object) {
    auto d = record<srtm_error_v1>(); auto result = object.context->api.object_close_status(object.context->runtime, object.handle, &d);
    if (result == SRTM_OK) return Success{};
    if (result == SRTM_PENDING) return Pending{};
    if (result == SRTM_TIMED_OUT) return TimedOut{};
    return rich_error(object.context.get(), "close_status", d);
}
}
Result<std::shared_ptr<const Secret>> Secret::copy(std::span<const std::byte> bytes) {
    auto secret = std::shared_ptr<Secret>(new Secret);
    secret->impl_->bytes.assign(bytes.begin(), bytes.end());
    return std::shared_ptr<const Secret>(std::move(secret));
}
TransportConfig live_profile() {
    TransportConfig c{}; c.mode = Mode::live_messages; c.accounting = Accounting::disabled; c.encryption = Encryption::disabled;
    c.receive_latency = c.peer_latency = std::chrono::milliseconds(120);
    c.connect_timeout = std::chrono::milliseconds(3000); c.peer_idle_timeout = std::chrono::milliseconds(5000);
    c.mss_bytes = 1500; c.payload_bytes = 1316; c.send_buffer_bytes = c.receive_buffer_bytes = 8*1024*1024;
    return c;
}
TransportConfig file_stream_profile() { auto c = live_profile(); c.mode = Mode::file_stream; c.payload_bytes = 0; c.receive_latency = c.peer_latency = std::chrono::milliseconds(0); return c; }
Interest operator|(Interest a, Interest b) noexcept { return static_cast<Interest>(static_cast<uint32_t>(a)|static_cast<uint32_t>(b)); }
bool contains(Interest mask, Interest bit) noexcept { return (static_cast<uint32_t>(mask)&static_cast<uint32_t>(bit)) == static_cast<uint32_t>(bit); }

struct Connection::Impl : OwnedObject { using OwnedObject::OwnedObject; };
struct Listener::Impl : OwnedObject { using OwnedObject::OwnedObject; };
struct Provider::Impl { std::shared_ptr<Context> context; explicit Impl(std::shared_ptr<Context> c) : context(std::move(c)) {} };
struct Poller::Impl {
    std::shared_ptr<Context> context;
    srtm_handle handle;
    std::mutex mutex;
    std::unordered_map<uint64_t, srtm_handle> registrations;
    uint64_t next_token = 1;
    std::array<srtm_event_v1, 256> events{};
    std::atomic_flag waiting = ATOMIC_FLAG_INIT;
    Impl(std::shared_ptr<Context> c, srtm_handle h) : context(std::move(c)), handle(h) { for (auto& e : events) e.struct_size = sizeof(e); }
    ~Impl() { context->api.poller_release(context->runtime, handle, nullptr); }
    Result<Registration> add(const OwnedObject& object, Interest interests) {
        if (object.context.get() != context.get()) return local_error(ErrorCategory::invalid_argument, "poller.add", "Objects and poller must use the same provider context");
        std::lock_guard lock(mutex);
        for (const auto& [token, h] : registrations) { (void)token; if (h == object.handle) return local_error(ErrorCategory::invalid_argument, "poller.add", "Object already registered"); }
        if (!next_token) return local_error(ErrorCategory::resource, "poller.add", "Registration token exhausted");
        const auto token = next_token++;
        registrations.emplace(token, object.handle);
        auto d = record<srtm_error_v1>();
        if (context->api.poller_register(context->runtime, handle, object.handle, static_cast<uint32_t>(interests), token, &d) != SRTM_OK) {
            registrations.erase(token); return rich_error(context.get(), "poller.add", d);
        }
        return Registration{token};
    }
};
struct TransportRuntime::Impl {
    std::mutex mutex;
    bool stopping = false;
    std::vector<std::shared_ptr<Context>> contexts;
    ~Impl() { for (auto& c : contexts) c->api.runtime_begin_shutdown(c->runtime, nullptr); }
};
#define MOVE_ONLY(Type) \
Type::Type(std::unique_ptr<Impl> p) : impl_(std::move(p)) {} \
Type::~Type() noexcept = default; \
Type::Type(Type&&) noexcept = default; \
Type& Type::operator=(Type&&) noexcept = default
MOVE_ONLY(Connection);
MOVE_ONLY(Listener);
MOVE_ONLY(Poller);
#undef MOVE_ONLY
Provider::Provider(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
Provider::~Provider() noexcept = default;
Provider::Provider(const Provider&) noexcept = default;
Provider& Provider::operator=(const Provider&) noexcept = default;
Provider::Provider(Provider&&) noexcept = default;
Provider& Provider::operator=(Provider&&) noexcept = default;
TransportRuntime::TransportRuntime(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
TransportRuntime::~TransportRuntime() noexcept = default;
TransportRuntime::TransportRuntime(TransportRuntime&&) noexcept = default;
TransportRuntime& TransportRuntime::operator=(TransportRuntime&&) noexcept = default;

const ProviderInfo& Provider::info() const noexcept { return impl_->context->info; }
const ProviderInfo& Connection::provider_info() const noexcept { return impl_->context->info; }
const ProviderInfo& Listener::provider_info() const noexcept { return impl_->context->info; }
Result<TransportRuntime> TransportRuntime::create(RuntimeOptions options) {
    if (options.log) return local_error(ErrorCategory::unsupported, "runtime.create", "Log callbacks are not implemented");
    return TransportRuntime(std::make_shared<Impl>());
}
Result<Provider> TransportRuntime::load(const ProviderSpec& spec) {
    std::lock_guard owner_lock(impl_->mutex);
    if (impl_->stopping) return local_error(ErrorCategory::invalid_state, "runtime.load", "Runtime is stopping");
    auto module = host::Module::load({spec.module_path, spec.expected_implementation, spec.expected_release, spec.expected_build_fingerprint});
    if (auto* failure = std::get_if<host::LoadError>(&module)) return local_error(ErrorCategory::invalid_argument, "runtime.load", failure->message.c_str());
    auto& m = std::get<host::Module>(module);
    static std::mutex registry_mutex;
    static std::unordered_map<const srtm_provider_api_v1*, std::weak_ptr<Context>> registry;
    static uint64_t next_domain = 1;
    std::lock_guard registry_lock(registry_mutex);
    auto context = registry[&m.api()].lock();
    if (!context) {
        if (!next_domain) return local_error(ErrorCategory::resource, "runtime.load", "Clock domain exhausted");
        context = std::make_shared<Context>(std::move(m), next_domain++);
        auto host = record<srtm_host_v1>(); auto d = record<srtm_error_v1>();
        if (context->api.runtime_create(&host, &context->runtime, &d) != SRTM_OK) return rich_error(context.get(), "runtime.create", d);
        registry[&context->api] = context;
    }
    if (std::find(impl_->contexts.begin(), impl_->contexts.end(), context) == impl_->contexts.end()) impl_->contexts.push_back(context);
    return Provider(std::make_shared<Provider::Impl>(std::move(context)));
}
Result<Success> TransportRuntime::begin_shutdown() {
    std::lock_guard lock(impl_->mutex); impl_->stopping = true;
    for (auto& c : impl_->contexts) {
        auto d = record<srtm_error_v1>();
        if (c->api.runtime_begin_shutdown(c->runtime, &d) != SRTM_OK) return rich_error(c.get(), "runtime.shutdown", d);
    }
    return Success{};
}
std::variant<Success, Pending, Error> TransportRuntime::shutdown_status() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopping) return Pending{};
    for (auto& c : impl_->contexts) {
        auto d = record<srtm_error_v1>(); auto status = c->api.runtime_shutdown_status(c->runtime, &d);
        if (status == SRTM_PENDING) return Pending{};
        if (status != SRTM_OK) return rich_error(c.get(), "runtime.shutdown_status", d);
    }
    return Success{};
}
Result<Capability> Provider::capability(Feature feature, Role role, const TransportConfig& input) const {
    auto cfg = config(input); if (auto* failure = std::get_if<Error>(&cfg)) return *failure;
    auto index = static_cast<size_t>(feature);
    if (index >= std::size(features)) return local_error(ErrorCategory::invalid_argument, "capability", "Unknown feature");
    auto query = record<srtm_capability_query_v1>(); query.role = static_cast<uint32_t>(role)+1;
    query.config = &std::get<srtm_config_v1>(cfg); query.feature = {features[index], std::strlen(features[index])};
    auto out = record<srtm_capability_v1>(); auto d = record<srtm_error_v1>(); auto& c = *impl_->context;
    if (c.api.capability(c.runtime, &query, &out, &d) != SRTM_OK) return rich_error(&c, "capability", d);
    return Capability{feature, static_cast<Availability>(out.availability-1), out.requires_peer != 0, std::string(out.constraints.data, static_cast<size_t>(out.constraints.size))};
}
Result<ValidationReport> Provider::validate(Role role, const TransportConfig& input) const {
    auto cfg = config(input); if (auto* failure = std::get_if<Error>(&cfg)) return *failure;
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); uint32_t count = 0;
    auto status = c.api.validate(c.runtime, static_cast<uint32_t>(role)+1, &std::get<srtm_config_v1>(cfg), nullptr, 0, sizeof(srtm_validation_issue_v1), &count, &d);
    if (status == SRTM_OK) return ValidationReport{};
    if (status != SRTM_BUFFER_TOO_SMALL) return rich_error(&c, "validate", d);
    std::vector<srtm_validation_issue_v1> issues(count);
    for (auto& issue : issues) { issue.struct_size = sizeof(issue); issue.detail.struct_size = sizeof(issue.detail); }
    if (c.api.validate(c.runtime, static_cast<uint32_t>(role)+1, &std::get<srtm_config_v1>(cfg), issues.data(), count, sizeof(issues[0]), &count, &d) != SRTM_OK) return rich_error(&c, "validate", d);
    ValidationReport out;
    for (uint32_t i = 0; i < count; ++i) out.issues.push_back({issues[i].severity == 1 ? ValidationIssue::Severity::error : ValidationIssue::Severity::peer_negotiation_required, issues[i].field, rich_error(&c, "validate", issues[i].detail)});
    return out;
}
Result<SourceTime> Provider::now() const {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); uint64_t value = 0;
    if (c.api.clock_now_us(c.runtime, &value, &d) != SRTM_OK) return rich_error(&c, "now", d);
    return SourceTime{c.domain, std::chrono::microseconds(value)};
}
Result<Connection> Provider::create_connection(const TransportConfig& input) const {
    auto cfg = config(input); if (auto* failure = std::get_if<Error>(&cfg)) return *failure;
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); srtm_handle handle = 0;
    // Allocate facade ownership before native resource publication.
    auto object = std::make_unique<Connection::Impl>(impl_->context, 0);
    if (c.api.connection_create(c.runtime, &std::get<srtm_config_v1>(cfg), &handle, &d) != SRTM_OK) return rich_error(&c, "connection.create", d);
    object->handle = handle; return Connection(std::move(object));
}
Result<Listener> Provider::listen(const ListenerConfig& input) const {
    if (input.admission) return local_error(ErrorCategory::unsupported, "listen", "Admission callbacks are not implemented");
    auto cfg = config(input.transport); if (auto* failure = std::get_if<Error>(&cfg)) return *failure;
    auto ep = endpoint(input.local); if (auto* failure = std::get_if<Error>(&ep)) return *failure;
    auto lc = record<srtm_listener_config_v1>(); lc.local = std::get<srtm_endpoint>(ep); lc.transport = std::get<srtm_config_v1>(cfg); lc.backlog = input.backlog;
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); auto object = std::make_unique<Listener::Impl>(impl_->context, 0);
    if (c.api.listener_create(c.runtime, &lc, &object->handle, &d) != SRTM_OK) return rich_error(&c, "listen", d);
    return Listener(std::move(object));
}
Result<Poller> Provider::create_poller() const {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); auto object = std::make_unique<Poller::Impl>(impl_->context, 0);
    if (c.api.poller_create(c.runtime, &object->handle, &d) != SRTM_OK) return rich_error(&c, "poller.create", d);
    return Poller(std::move(object));
}
Result<Success> Connection::bind(const Endpoint& input) {
    auto ep = endpoint(input); if (auto* failure = std::get_if<Error>(&ep)) return *failure;
    auto& c = *impl_->context; auto d = record<srtm_error_v1>();
    if (c.api.connection_bind(c.runtime, impl_->handle, &std::get<srtm_endpoint>(ep), &d) != SRTM_OK) return rich_error(&c, "bind", d);
    return Success{};
}
ConnectResult Connection::connect(const Endpoint& input) {
    auto ep = endpoint(input); if (auto* failure = std::get_if<Error>(&ep)) return *failure;
    auto& c = *impl_->context; auto d = record<srtm_error_v1>();
    auto status = c.api.connection_connect(c.runtime, impl_->handle, &std::get<srtm_endpoint>(ep), &d);
    if (status == SRTM_OK) return Success{};
    if (status == SRTM_PENDING) return Pending{};
    return rich_error(&c, "connect", d);
}
Result<ConnectionInfo> Connection::info() const {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); auto value = record<srtm_connection_info_v1>(); value.terminal_error.struct_size = sizeof(value.terminal_error);
    if (c.api.connection_info(c.runtime, impl_->handle, &value, &d) != SRTM_OK) return rich_error(&c, "info", d);
    ConnectionInfo out{}; out.state = static_cast<State>(value.state-1);
    if (value.valid_fields & 1) out.local = endpoint(value.local);
    if (value.valid_fields & 2) out.remote = endpoint(value.remote);
    if (value.valid_fields & 4) out.max_message_bytes = value.max_message_bytes;
    if (value.valid_fields & 8) { out.encryption = static_cast<Encryption>(value.encryption); out.key_bits = value.key_bits; }
    if (value.valid_fields & 16) { out.receive_latency = std::chrono::milliseconds(value.receive_latency_ms); out.peer_latency = std::chrono::milliseconds(value.peer_latency_ms); }
    if (value.terminal_error.category) out.terminal_error = rich_error(&c, "connection", value.terminal_error);
    return out;
}
IoResult<Success> Connection::send_message(std::span<const std::byte> data, const MessageInfo& message, Error* failure_detail) {
    auto& c = *impl_->context; auto io = record<srtm_io_error_v1>(); srtm_error_v1 d; d.struct_size = sizeof(d);
    auto meta = record<srtm_message_info_v1>();
    if (message.source_time) {
        if (message.source_time->clock_domain != c.domain || message.source_time->value.count() < 0) return invalid_io(failure_detail, "send_message");
        meta.valid_fields = 1; meta.source_time_us = message.source_time->value.count();
    }
    auto status = c.api.send_message(c.runtime, impl_->handle, {reinterpret_cast<const uint8_t*>(data.data()), data.size()}, &meta, &io, failure_detail ? &d : nullptr);
    if (status == SRTM_OK) return Success{};
    if (status == SRTM_WOULD_BLOCK) return WouldBlock{};
    if (failure_detail) *failure_detail = rich_error(&c, "send_message", d);
    return io_error(io);
}
IoResult<ReceivedMessage> Connection::receive_message(std::span<std::byte> data, Error* failure_detail) {
    auto& c = *impl_->context; auto io = record<srtm_io_error_v1>(); srtm_error_v1 d; d.struct_size = sizeof(d);
    auto meta = record<srtm_message_info_v1>(); uint64_t n = 0;
    auto status = c.api.receive_message(c.runtime, impl_->handle, {reinterpret_cast<uint8_t*>(data.data()), data.size()}, &n, &meta, &io, failure_detail ? &d : nullptr);
    if (status == SRTM_OK) { ReceivedMessage out{static_cast<size_t>(n), {}}; if (meta.valid_fields & 1) out.info.source_time = SourceTime{c.domain, std::chrono::microseconds(meta.source_time_us)}; return out; }
    if (status == SRTM_WOULD_BLOCK) return WouldBlock{};
    if (failure_detail) *failure_detail = rich_error(&c, "receive_message", d);
    return io_error(io);
}
IoResult<std::size_t> Connection::write_stream(std::span<const std::byte> data, Error* failure_detail) {
    auto& c = *impl_->context; auto io = record<srtm_io_error_v1>(); srtm_error_v1 d; d.struct_size = sizeof(d); uint64_t n = 0;
    auto status = c.api.write_stream(c.runtime, impl_->handle, {reinterpret_cast<const uint8_t*>(data.data()), data.size()}, &n, &io, failure_detail ? &d : nullptr);
    if (status == SRTM_OK) return static_cast<size_t>(n);
    if (status == SRTM_WOULD_BLOCK) return WouldBlock{};
    if (failure_detail) *failure_detail = rich_error(&c, "write_stream", d);
    return io_error(io);
}
std::variant<std::size_t, WouldBlock, EndOfStream, IoError> Connection::read_stream(std::span<std::byte> data, Error* failure_detail) {
    auto& c = *impl_->context; auto io = record<srtm_io_error_v1>(); srtm_error_v1 d; d.struct_size = sizeof(d); uint64_t n = 0;
    auto status = c.api.read_stream(c.runtime, impl_->handle, {reinterpret_cast<uint8_t*>(data.data()), data.size()}, &n, &io, failure_detail ? &d : nullptr);
    if (status == SRTM_OK) return static_cast<size_t>(n);
    if (status == SRTM_WOULD_BLOCK) return WouldBlock{};
    if (status == SRTM_END_OF_STREAM) return EndOfStream{};
    if (failure_detail) *failure_detail = rich_error(&c, "read_stream", d);
    return io_error(io);
}
Result<Statistics> Connection::statistics() const {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); auto value = record<srtm_statistics_v1>();
    if (c.api.statistics(c.runtime, impl_->handle, &value, &d) != SRTM_OK) return rich_error(&c, "statistics", d);
    Statistics out{{c.domain, std::chrono::microseconds(value.sample_time_us)}, value.counter_epoch, {}, {}, {}};
    if (value.valid_fields & 1) out.accepted_tx_bytes = value.accepted_tx_bytes;
    if (value.valid_fields & 2) out.delivered_rx_bytes = value.delivered_rx_bytes;
    if (value.valid_fields & 4) out.rtt_estimate = std::chrono::microseconds(value.rtt_estimate_us);
    return out;
}
CloseResult Connection::close(ClosePolicy policy) { return close_object(*impl_, policy); }
CloseResult Connection::close_status() const { return srtmux::close_status(*impl_); }
CloseResult Listener::close() { return close_object(*impl_, AbortClose{}); }
CloseResult Listener::close_status() const { return srtmux::close_status(*impl_); }
Result<Endpoint> Listener::local_endpoint() const {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); srtm_endpoint value{};
    if (c.api.listener_endpoint(c.runtime, impl_->handle, &value, &d) != SRTM_OK) return rich_error(&c, "listener.endpoint", d);
    return endpoint(value);
}
std::variant<Connection, WouldBlock, Error> Listener::accept() {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>(); srtm_handle handle = 0;
    auto status = c.api.listener_accept(c.runtime, impl_->handle, &handle, &d);
    if (status == SRTM_WOULD_BLOCK) return WouldBlock{};
    if (status != SRTM_OK) return rich_error(&c, "accept", d);
    try { return Connection(std::make_unique<Connection::Impl>(impl_->context, handle)); }
    catch (...) { c.api.object_close(c.runtime, handle, SRTM_CLOSE_ABORT, 0, nullptr); c.api.object_release(c.runtime, handle, nullptr); throw; }
}
Result<Registration> Poller::add(const Connection& c, Interest interest) { return impl_->add(*c.impl_, interest); }
Result<Registration> Poller::add(const Listener& l, Interest interest) { return impl_->add(*l.impl_, interest); }
Result<Success> Poller::update(Registration registration, Interest interests) {
    std::lock_guard lock(impl_->mutex); auto found = impl_->registrations.find(registration.id);
    if (found == impl_->registrations.end()) return local_error(ErrorCategory::invalid_argument, "poller.update", "Unknown registration");
    auto& c = *impl_->context; auto d = record<srtm_error_v1>();
    if (c.api.poller_register(c.runtime, impl_->handle, found->second, static_cast<uint32_t>(interests), registration.id, &d) != SRTM_OK) return rich_error(&c, "poller.update", d);
    return Success{};
}
Result<Success> Poller::remove(Registration registration) {
    std::lock_guard lock(impl_->mutex); auto found = impl_->registrations.find(registration.id);
    if (found == impl_->registrations.end()) return local_error(ErrorCategory::invalid_argument, "poller.remove", "Unknown registration");
    auto& c = *impl_->context; auto d = record<srtm_error_v1>();
    if (c.api.poller_remove(c.runtime, impl_->handle, found->second, &d) != SRTM_OK) return rich_error(&c, "poller.remove", d);
    impl_->registrations.erase(found); return Success{};
}
WaitResult Poller::wait(std::span<Event> output, WaitTimeout timeout, Error* failure_detail) {
    uint64_t ms = SRTM_WAIT_FOREVER;
    if (auto* finite = std::get_if<std::chrono::milliseconds>(&timeout)) {
        if (finite->count() < 0 || finite->count() > INT_MAX) return invalid_io(failure_detail, "poller.wait");
        ms = static_cast<uint64_t>(finite->count());
    }
    if (output.empty()) return invalid_io(failure_detail, "poller.wait");
    if (impl_->waiting.test_and_set(std::memory_order_acquire)) {
        if (failure_detail) *failure_detail = local_error(ErrorCategory::busy, "poller.wait", "Only one waiter per poller");
        return IoError{ErrorCategory::busy, 0, 0, 0, 0};
    }
    struct Waiting { std::atomic_flag& flag; ~Waiting() { flag.clear(std::memory_order_release); } } waiting{impl_->waiting};
    auto& c = *impl_->context; auto io = record<srtm_io_error_v1>(); srtm_error_v1 d; d.struct_size = sizeof(d); uint32_t count = 0;
    auto capacity = static_cast<uint32_t>(std::min(output.size(), impl_->events.size()));
    auto status = c.api.poller_wait(c.runtime, impl_->handle, ms, impl_->events.data(), capacity, sizeof(srtm_event_v1), &count, &io, failure_detail ? &d : nullptr);
    if (status == SRTM_OK) { for (uint32_t i = 0; i < count; ++i) output[i] = {{impl_->events[i].registration_token}, static_cast<Interest>(impl_->events[i].readiness)}; return size_t(count); }
    if (status == SRTM_TIMED_OUT) return TimedOut{};
    if (status == SRTM_INTERRUPTED) return Interrupted{};
    if (failure_detail) *failure_detail = rich_error(&c, "poller.wait", d);
    return io_error(io);
}
Result<Success> Poller::wake() {
    auto& c = *impl_->context; auto d = record<srtm_error_v1>();
    if (c.api.poller_wake(c.runtime, impl_->handle, &d) != SRTM_OK) return rich_error(&c, "poller.wake", d);
    return Success{};
}
} // namespace srtmux
