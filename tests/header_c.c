#include <srtmux/provider_abi.h>
_Static_assert(sizeof(srtm_handle) == 8, "ABI handles must remain 64 bit");
_Static_assert(sizeof(srtm_io_error_v1) == 24, "Compact error layout changed");

#include <extensions/robotweax_extensions.h>
_Static_assert(sizeof(((srtmx_member_v1*)0)->token) == 8, "Member token width");
