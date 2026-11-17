#include <srtmux/module.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# include <windows.h>
#else
# include <dlfcn.h>
#endif

namespace srtmux::host {
namespace {
#if defined(_WIN32)
using NativeModule = HMODULE;
void close_module(NativeModule h) { FreeLibrary(h); }
NativeModule open_module(const std::filesystem::path& path) {
    return LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
std::string loader_error() { return "Windows loader error " + std::to_string(GetLastError()); }
#else
using NativeModule = void*;
void close_module(NativeModule h) { dlclose(h); }
NativeModule open_module(const std::filesystem::path& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
std::string loader_error() {
    const char* error = dlerror();
    return error ? error : "Unknown dynamic loader error";
}
#endif

struct NativeOwner {
    NativeModule value;
    ~NativeOwner() { if (value) close_module(value); }
};

using Entry = decltype(&srtm_provider_get_api);
Entry find_entry(NativeModule handle) {
#if defined(_WIN32)
    auto symbol = GetProcAddress(handle, "srtm_provider_get_api");
#else
    auto symbol = dlsym(handle, "srtm_provider_get_api");
#endif
    // Supported native loader ABIs represent function and symbol pointers at
    // the same size. memcpy avoids a C++ object/function pointer cast warning.
    static_assert(sizeof(Entry) == sizeof(symbol));
    Entry entry = nullptr;
    std::memcpy(&entry, &symbol, sizeof(entry));
    return entry;
}

std::string provider_error(const srtm_error_v1& error) {
    const auto size = (error.message_length < sizeof(error.message))
        ? error.message_length : sizeof(error.message) - 1;
    return std::string(error.message, size);
}

bool valid_text(srtm_text value, bool required) {
    constexpr uint64_t max_descriptor_bytes = 64 * 1024;
    return value.size <= max_descriptor_bytes && (!required || value.size != 0)
        && (value.size == 0 || value.data != nullptr);
}
std::string copy_text(srtm_text value) {
    return value.size ? std::string(value.data, static_cast<std::size_t>(value.size)) : std::string{};
}

std::optional<LoadError> validate_table(const srtm_provider_api_v1* api) {
    if (!api) return LoadError{LoadErrorCode::invalid_table, "Provider returned a null table"};
    // Bootstrap guarantees that the size prefix itself is readable.
    if (api->struct_size < offsetof(srtm_provider_api_v1, runtime_create))
        return LoadError{LoadErrorCode::invalid_table, "Provider table header is truncated"};
    static_assert(SRTM_ABI_MINOR == 0, "Add minimum-minor validation when advancing the host ABI");
    if (api->abi_major != SRTM_ABI_MAJOR)
        return LoadError{LoadErrorCode::incompatible_abi, "Provider returned an incompatible ABI"};
    if (api->struct_size < sizeof(srtm_provider_api_v1))
        return LoadError{LoadErrorCode::invalid_table, "Provider table omits required v1.0 fields"};
    if (api->reserved != 0)
        return LoadError{LoadErrorCode::invalid_table, "Provider table reserved field is nonzero"};
#define SRTMUX_REQUIRE(field) \
    if (!api->field) return LoadError{LoadErrorCode::invalid_table, "Missing required function: " #field}
    SRTMUX_REQUIRE(provider_info);
    SRTMUX_REQUIRE(runtime_create);
    SRTMUX_REQUIRE(runtime_begin_shutdown);
    SRTMUX_REQUIRE(runtime_shutdown_status);
    SRTMUX_REQUIRE(runtime_destroy);
    SRTMUX_REQUIRE(clock_now_us);
    SRTMUX_REQUIRE(capability);
    SRTMUX_REQUIRE(validate);
    SRTMUX_REQUIRE(connection_create);
    SRTMUX_REQUIRE(connection_bind);
    SRTMUX_REQUIRE(connection_connect);
    SRTMUX_REQUIRE(connection_info);
    SRTMUX_REQUIRE(listener_create);
    SRTMUX_REQUIRE(listener_endpoint);
    SRTMUX_REQUIRE(listener_accept);
    SRTMUX_REQUIRE(send_message);
    SRTMUX_REQUIRE(receive_message);
    SRTMUX_REQUIRE(write_stream);
    SRTMUX_REQUIRE(read_stream);
    SRTMUX_REQUIRE(statistics);
    SRTMUX_REQUIRE(object_close);
    SRTMUX_REQUIRE(object_close_status);
    SRTMUX_REQUIRE(object_release);
    SRTMUX_REQUIRE(poller_create);
    SRTMUX_REQUIRE(poller_register);
    SRTMUX_REQUIRE(poller_remove);
    SRTMUX_REQUIRE(poller_wait);
    SRTMUX_REQUIRE(poller_wake);
    SRTMUX_REQUIRE(poller_release);
    SRTMUX_REQUIRE(query_extension);
#undef SRTMUX_REQUIRE
    return std::nullopt;
}

std::optional<LoadError> match_identity(const ModuleInfo& info, const ModuleSpec& spec) {
    if (info.implementation != spec.implementation || info.release != spec.release
        || (spec.build_fingerprint && info.build_fingerprint != *spec.build_fingerprint))
        return LoadError{LoadErrorCode::identity_mismatch, "Provider identity does not match the requested artifact"};
    return std::nullopt;
}
} // namespace

struct Module::State {
    NativeModule handle;
    std::filesystem::path path;
    ModuleInfo info;
    const srtm_provider_api_v1* api;
    // Native handle intentionally remains loaded, including at static teardown.
};

Module::Module(std::shared_ptr<const State> state) : state_(std::move(state)) {}
const ModuleInfo& Module::info() const noexcept { return state_->info; }
const std::filesystem::path& Module::path() const noexcept { return state_->path; }
const srtm_provider_api_v1& Module::api() const noexcept { return *state_->api; }

std::variant<Module, LoadError> Module::load(const ModuleSpec& spec) {
    if (!spec.path.is_absolute() || spec.implementation.empty() || spec.release.empty())
        return LoadError{LoadErrorCode::invalid_spec, "An absolute path and expected implementation/release are required"};
    std::error_code ec;
    auto path = std::filesystem::canonical(spec.path, ec);
    if (ec || !std::filesystem::is_regular_file(path, ec) || ec)
        return LoadError{LoadErrorCode::open_failed, "Provider path is not an accessible regular file"};

    // Loading is cold-path work. No registry access is performed by api(), and
    // no mutex or shared_ptr increment is required when calling a cached slot.
    struct Registry {
        std::mutex mutex;
        std::map<std::filesystem::path, NativeModule> opened;
        std::map<std::filesystem::path, std::shared_ptr<const State>> loaded;
    };
    static Registry registry;
    std::lock_guard lock(registry.mutex);
    if (auto found = registry.loaded.find(path); found != registry.loaded.end()) {
        if (auto error = match_identity(found->second->info, spec)) return *error;
        return Module(found->second);
    }

    NativeModule handle = nullptr;
    if (auto found = registry.opened.find(path); found != registry.opened.end()) {
        handle = found->second;
    } else {
        NativeOwner native{open_module(path)};
        if (!native.value) return LoadError{LoadErrorCode::open_failed, loader_error()};
        handle = native.value;
        registry.opened.emplace(path, handle);
        // Loading already executed static initializers. Pin even rejected
        // modules; no dlclose/reload cycle or address reuse after validation.
        native.value = nullptr;
    }
    const auto entry = find_entry(handle);
    if (!entry) return LoadError{LoadErrorCode::missing_entry, "Missing srtm_provider_get_api export"};
    const srtm_provider_api_v1* api = nullptr;
    srtm_error_v1 detail{};
    detail.struct_size = sizeof(detail);
    const auto status = entry(SRTM_ABI_MAJOR, SRTM_ABI_MINOR, &api, &detail);
    if (status != SRTM_OK)
        return LoadError{detail.category == SRTM_ERR_ABI_MISMATCH
                ? LoadErrorCode::incompatible_abi : LoadErrorCode::provider_failed,
            "Provider bootstrap failed: " + provider_error(detail)};
    if (auto error = validate_table(api)) return *error;

    srtm_provider_info_v1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    if (api->provider_info(&descriptor, &detail) != SRTM_OK)
        return LoadError{LoadErrorCode::provider_failed, "Provider descriptor failed: " + provider_error(detail)};
    if (descriptor.struct_size < sizeof(descriptor) || descriptor.reserved != 0
        || !valid_text(descriptor.implementation, true) || !valid_text(descriptor.release, true)
        || !valid_text(descriptor.source_revision, true) || !valid_text(descriptor.build_fingerprint, true)
        || !valid_text(descriptor.target_triple, true) || !valid_text(descriptor.build_features, false))
        return LoadError{LoadErrorCode::invalid_descriptor, "Provider returned an invalid descriptor"};

    ModuleInfo info{copy_text(descriptor.implementation), copy_text(descriptor.release),
        copy_text(descriptor.source_revision), copy_text(descriptor.build_fingerprint),
        copy_text(descriptor.target_triple), copy_text(descriptor.build_features)};
    if (auto error = match_identity(info, spec)) return *error;
    auto state = std::make_shared<State>(State{handle, path, std::move(info), api});
    registry.loaded.emplace(path, state);
    return Module(std::move(state));
}
} // namespace srtmux::host
