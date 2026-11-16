#include <srtmux/module.hpp>
#include <transport.hpp>
#include <type_traits>
static_assert(std::is_trivially_copyable_v<srtmux::IoError>);
static_assert(std::is_standard_layout_v<srtm_provider_api_v1>);
static_assert(!std::is_copy_constructible_v<srtmux::Connection>);
