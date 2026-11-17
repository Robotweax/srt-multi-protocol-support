// Declaration-only C++20 proposal; no linkable implementation. See README.md.
#ifndef SRTMUX_DRAFT_EXTENSIONS_HPP
#define SRTMUX_DRAFT_EXTENSIONS_HPP
#include <srtmux/transport.hpp>
#include <string_view>
#include "robotweax_extensions.h"
namespace srtmux::extensions::v1 {
enum class ProfileMode { live, file, control, sensor };
enum class Cipher { none, aes_ctr, aes_gcm };
enum class Policy { off, required, if_offered };
enum class GroupType { broadcast, backup };
enum class MulticastMode { disabled, legacy, independent, coordinated };
struct Feature {
    bool native_built, adapter_available, experimental;
    std::string constraints, qualification_id, build_fingerprint;
};
struct ProfileConfig {
    ProfileMode mode;
    Cipher cipher;
    TransportConfig transport; // see README for embedded mode/encryption mapping
    std::uint32_t max_message_bytes;
    srtmx_fec_v1 fec;
    srtmx_tuning_v1 tuning;
    bool session_auth_required, acp1_required;
    Policy paths, standby_rtt;
    std::uint32_t identity_advertisement;
    std::string compiled_controller, compiled_filter;
};
struct Message {
    std::chrono::microseconds source_time;
    std::optional<std::chrono::milliseconds> ttl;
    bool ordered;
};
struct Received { std::size_t bytes; Message message; };
struct MemberConfig {
    Endpoint local, remote;
    std::uint32_t weight;
    std::vector<std::byte> path_id; // setup only, 1..32 when enabled
};
struct MulticastConfig {
    MulticastMode mode;
    Endpoint group, source;
    std::uint32_t interface_index, ttl, repair_threshold;
    std::int64_t max_wire_bytes_per_second;
};
// Setup may allocate; payload calls, snapshots and cancellation do not.
// Runtime ownership pins cached tables/module. Never move/destroy during calls.
class Profile final {
public:
    Profile(Profile&&) noexcept;
    Profile& operator=(Profile&&) noexcept;
    ~Profile();
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
private:
    friend class Extensions;
    Profile();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class MessageChannel final {
public:
    MessageChannel(MessageChannel&&) noexcept;
    MessageChannel& operator=(MessageChannel&&) noexcept;
    ~MessageChannel();
    MessageChannel(const MessageChannel&) = delete;
    MessageChannel& operator=(const MessageChannel&) = delete;
    IoResult<Success> send(std::span<const std::byte>, const Message&) noexcept;
    IoResult<Received> receive(std::span<std::byte>) noexcept;
    Result<State> state() const;
    CloseResult close() noexcept;
    Result<Registration> register_with(Poller&, std::uint32_t interests, std::uint64_t user_tag);
private:
    friend class Extensions;
    MessageChannel();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct MemberToken { std::uint64_t value; }; // usable only with originating Group
struct SnapshotSize { std::size_t required; std::uint64_t revision; };
class Group final {
public:
    Group(Group&&) noexcept;
    Group& operator=(Group&&) noexcept;
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Result<MemberToken> add(const MemberConfig&);
    Result<Success> remove(MemberToken);
    IoResult<Success> send(std::span<const std::byte>, const Message&) noexcept;
    IoResult<Received> receive(std::span<std::byte>) noexcept;
    std::variant<SnapshotSize, IoError> members(std::span<srtmx_member_v1>) const noexcept;
    Result<State> state() const;
    CloseResult close() noexcept;
    Result<Registration> register_with(Poller&, std::uint32_t interests, std::uint64_t user_tag);
private:
    friend class Extensions;
    Group();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using Accepted = std::variant<Connection, MessageChannel, Group>;
using Admission = void (*)(void*, const srtmx_admission_request_v1&, srtmx_admission_decision_v1&) noexcept;
class Listener final {
public:
    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    // Acceptance may allocate facade storage; payload calls never do.
    std::variant<Accepted, WouldBlock, Error> accept();
    CloseResult close() noexcept;
    Result<Registration> register_with(Poller&, std::uint32_t interests, std::uint64_t user_tag);
private:
    friend class Extensions;
    Listener();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Extensions final {
public:
    static Result<Extensions> bind(Provider&); // queries/caches tables once
    Extensions(Extensions&&) noexcept;
    Extensions& operator=(Extensions&&) noexcept;
    ~Extensions();
    Extensions(const Extensions&) = delete;
    Extensions& operator=(const Extensions&) = delete;
    Result<Feature> describe(std::string_view feature) const;
    Result<Profile> profile(const ProfileConfig&);
    Result<Accepted> connect(const Profile&, const Endpoint& local, const Endpoint& remote, bool rendezvous = false);
    Result<Group> group(const Profile&, GroupType, std::uint32_t capacity, const MulticastConfig&);
    Result<Listener> listen(const Endpoint&, std::span<const Profile* const> allowed,
                            Admission, std::shared_ptr<void> callback_context);
    Result<Listener> listen_group(const Endpoint&, const Profile&, GroupType,
                                  std::uint32_t capacity, const MulticastConfig&,
                                  Admission, std::shared_ptr<void> callback_context);
private:
    Extensions();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Telemetry, native UDP adoption and experimental hybrid retain their explicit
// C draft contracts for now; no unsafe generic native-handle escape hatch.
} // namespace srtmux::extensions::v1
#endif
