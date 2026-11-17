# Haivision URI parser

`uriparser.cpp` and `uriparser.hpp` originate from Haivision SRT 1.5.7,
revision `899348d8318eb9a3c5a5b6ec43c4a1114288773a`, `apps/`.
They retain their MPL-2.0 license and original copyright notices.

Local changes remove unused native `utilities.h`/MapProxy dependencies and the
standalone test driver; add standard headers, unsigned character case conversion,
host/file-URI path percent decoding, and rejection of malformed percent escapes,
duplicate query fields and NULs.
The new relay and transport adapter are in the parent directory. They use the
multi-SRT facade rather than Haivision's native SRT/UDT API. This is a port of the
application workflow and supported CLI, not an unmodified upstream executable.
