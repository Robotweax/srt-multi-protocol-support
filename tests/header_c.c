#include <srtmux/provider_abi.h>
_Static_assert(sizeof(srtm_handle) == 8, "ABI handles must remain 64 bit");
_Static_assert(sizeof(srtm_io_error_v1) == 24, "Compact error layout changed");
