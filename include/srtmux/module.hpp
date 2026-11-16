#ifndef SRTMUX_MODULE_HPP
#define SRTMUX_MODULE_HPP

#include <srtmux/provider_abi.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace srtmux::host {

struct ModuleSpec {
    std::filesystem::path path; // Absolute; resolved canonically before loading.
    std::string implementation;
    std::string release;
    std::optional<std::string> build_fingerprint;
};

struct ModuleInfo {
    std::string implementation, release, source_revision;
    std::string build_fingerprint, target_triple, build_features;
};

enum class LoadErrorCode {
    invalid_spec, open_failed, missing_entry, incompatible_abi,
    invalid_table, invalid_descriptor, identity_mismatch, provider_failed
};

struct LoadError {
    LoadErrorCode code;
    std::string message;
};

// Low-level host infrastructure, not the planned application TransportRuntime.
// A loaded module is pinned for process lifetime. Copies share immutable state.
// Loading validates the table but does NOT create a provider runtime.
class Module final {
public:
    static std::variant<Module, LoadError> load(const ModuleSpec&);

    const ModuleInfo& info() const noexcept;
    const std::filesystem::path& path() const noexcept;
    const srtm_provider_api_v1& api() const noexcept;

private:
    struct State;
    explicit Module(std::shared_ptr<const State>);
    std::shared_ptr<const State> state_;
};

} // namespace srtmux::host
#endif
