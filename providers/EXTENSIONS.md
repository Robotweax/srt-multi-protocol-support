# Implemented profile and telemetry extensions

Both pinned providers expose `srtmux.profiles` 1.0 and `srtmux.telemetry` 1.0 through
`query_extension`. See [public C declarations](../include/srtmux/extensions.h) and
[the C++ facade](../include/srtmux/transport.hpp). The existing core ABI is unchanged.
Group and hybrid declarations in the extension header remain reserved proposals;
those tables are not available. This milestone implements the first stage of the
[audited extension sequence](../docs/design/extensions/README.md).

## Supported profiles

Immutable Live/File caller/listener profiles support existing plaintext and
AES-CTR settings plus typed flow window, maximum/input bandwidth, overhead,
IP TTL/TOS, IPv6-only, key rotation and UDP buffer tuning. Creation validates
record sizes/ranges, copies StreamID/passphrase, and applies the complete native
configuration to a temporary unconnected socket before publishing a handle.
This catches native constraints without publishing a partly valid profile.
Secrets are erased when the last setup use of the profile finishes. Setup may
allocate; no profile lookup occurs during data transfer.

Profile creation and release use a separate runtime-owned registry, protected on
setup only. Handles use a disjoint type tag and monotonic generations shared with
ordinary object generation. A released/foreign profile cannot alias a connection.
Connect/listen retain a profile snapshot for the call, then the native socket owns
its configured values; the application can release the profile immediately.
Existing connections and listeners continue to work, including encrypted accept.
Raw runtime destruction, as in the core contract, must not race any application
call and requires all profile handles to be released. Shutdown prevents
new profile or connection publication; profile release remains valid after shutdown.

C configuration: set every `struct_size`; zero reserved and unselected feature
fields. `mode` and `cipher` are authoritative; embedded `base.encryption` must be
zero, while key size and passphrase remain populated for CTR. For Live,
`max_message_bytes` must equal `base.payload_bytes`; File requires zero.
`max_bandwidth_bytes_s` is explicit: -1 unlimited, 0 relative, positive absolute.
Other tuning zeros inherit the pinned native profile defaults. This is an explicit
inheritance rule for v1, not a claim that vendor defaults are identical; resolve
native telemetry where applicable and qualify nondefault tuning separately.
Nonzero settings are applied or fail. Runtime cannot turn on blocking send/receive,
blocking linger, arbitrary setsockopt, or replace polling/close ownership.

Only single-profile listeners without admission callbacks are supported. Accepted
objects are ordinary Connections; listeners also work with core endpoint, accept,
poller and lifecycle calls. The profile table supplies typed info, accept and
abort-close/release. For ordinary objects, `info.revision` is the handle epoch,
not a state-change sequence number; inspect `state` and poll readiness for changes. These use the existing operation gates and provider-local
poller notifications. Close can wait for already active nonblocking calls to
quiesce, exactly as the core currently does; it introduces no new asynchronous
Pending/cleanup contract. Typed release closes before releasing. Ordinary core
release still requires close first. Sticky poller wake/cancellation is unchanged.

Control/Sensor MessageChannels, rendezvous, FEC, GCM, session authentication, ACP1,
identity advertisement, compiled plugins, group/standby policies and admission
callbacks are explicitly Unsupported. The channel send/receive slots are present
but unsupported. Unknown modes and requested nonzero unsupported options never
silently fall back. `describe` separates native build presence from adapter
availability; its qualification ID is empty, avoiding deployment certification
from feature presence alone. The `experimental` bit is provider-integration policy, not an upstream release
label: AEAD integration is conservatively marked experimental here even though
Robotweax calls its default-off native GCM extension released. AEAD remains
not-built and unavailable in these modules. Experimental switches and AEAD/bonding/MAXREXMITBW
are fixed off by the reference build and recorded in build features; Robotweax's
built-in group API is still native-present despite Haivision bonding being off.

C++ setup uses ordinary Provider and Connection objects:

```cpp
auto settings = srtmux::ProfileConfig::basic(srtmux::live_profile());
// Handle Result<T> errors in each call.
auto profile_result = provider.create_profile(settings);
// provider.listen_profile(profile, {"127.0.0.1", 0, 0});
// provider.connect_profile(profile, remote);  // connecting Connection
```

Use `info()`/poll readiness until connected, then the existing send/receive API.
Profile is move-only and retains the provider context. Foreign or moved profiles
are rejected; ordinary object destruction and runtime shutdown semantics remain.
The speculative `extensions::v1` C++ classes remain design-only; the implemented
foundation integrates into the existing facade instead of duplicating ownership.

## Native telemetry

The [82-field catalogue](../docs/design/extensions/METRICS.md) maps stable names
`native.<CBytePerfMon field>` to value type, native meaning and unit. Resolve IDs
once and cache them within the provider context; numeric IDs are not a persistent
cross-provider schema. Unsupported names/IDs fail explicitly. Peer identity and
Robotweax's dedicated RTT/standby/group telemetry are not implemented in this stage.

Each snapshot pins the same cold-operation gate used by safe close and reads one
native `srt_bstats(..., clear=0)` result. No snapshot is taken implicitly during
payload I/O. Native interval counters retain their native interpretation; calls
never clear them. Byte totals are native counters, not wrapper-delivered payload
or wire-rate counters. Signed values stay signed, including native overflow limits.
No derived values or missing-feature counters are fabricated.

Callers supply IDs and initialized output records, capacity and stride. Maximum
82 distinct IDs; duplicates and invalid record sizes fail before native sampling.
Capacity failure reports required count and changes no record/header. Successful
calls preserve each record's size and bytes beyond its known prefix. Snapshot
metadata contains provider time/clock domain, connection-handle counter epoch,
and zero membership revision for ordinary connections. Native measurement fields
need not be mutually instantaneous beyond the vendor's own snapshot guarantees.
Do not infer source health from an RTT estimate. C++ translates clock domain to the
existing Provider clock and returns compact errors or required capacity without
allocating. No trace logging or native error-string formatting occurs on successful
snapshot calls.

`Connection::resolve_metric(name)` and `native_snapshot(ids, records)` expose the
same contract. Output records must have `struct_size` initialized. Descriptor
strings are module-owned; the connection/provider pins their module.

## Performance and validation

Ordinary send/receive functions, operation gates and polling code are unchanged.
The Entry layout is unchanged. Profile maps live only in runtime setup state;
telemetry tables are cached when loading the C++ provider. No extra profile branch,
allocation, payload copy, registry lookup or mandatory statistics sampling is
added to the I/O path.

The benchmark `--profile-setup` option creates module/facade endpoints through the
new immutable profiles and compares them to the same native configuration. Native
I/O itself is unchanged. Default runs still exercise original setup. Both paths
retain the existing 2% throughput / 5% process CPU-per-byte gates and paired
confidence/variance/duration requirements. Sampling telemetry at application-chosen
frequencies has native statistics cost and needs its own workload qualification;
no claim of free continuous telemetry is made.

See the [validation report](../docs/validation/2026-11-16-profiles-telemetry.md):
14/14 Release and optimized ASan/UBSan checks pass. With all initial and follow-up
samples retained, 17/20 performance comparisons pass and three plaintext File
comparisons remain inconclusive. No significant regression is established; this
is not a blanket performance qualification.

The [subsequent File investigation](../docs/validation/2026-11-17-file-qualification.md)
corrects benchmark timing and adds a fixed 144-transfer campaign. Release and
optimized ASan/UBSan now pass 15/15 tests. All eight new File comparisons remain
inconclusive (six baseline-control failures, two throughput-interval failures),
so both providers still lack complete performance qualification. Provider modules
and runtime sources remain unchanged; all observations and gates are preserved.
