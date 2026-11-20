# Dual provider SRT interface draft

Status: design draft v0.2, 16 November 2026. These are declaration-only design
artifacts for private modules containing Robotweax SRT 0.3.4 and Haivision SRT
1.5.7. The transport facade is implemented in `include/srtmux/transport.hpp`. The provider ABI is now exercised by
the host module loader and test fixtures; it is not a released ABI. Real I/O, polling and concurrent abort-close adapters now exist. Their narrower implemented profile, including
caller-serialized lifecycle operations and explicit unsupported features, is
documented in [the provider milestone](../../providers/README.md). The full
contract below remains the target design, not a claim of completed implementation.

Throughput preservation is a release requirement. The abstraction must not
significantly reduce the performance attainable with either vendor library used
directly. The constraints below govern implementation choices; declaration syntax
checks do not establish throughput or CPU overhead.

- [transport.hpp](transport.hpp): C++20 application interface.
- [provider_abi.h](../../include/srtmux/provider_abi.h): C11-compatible module interface, also valid C++.

## Scope

The initial interface covers ordinary Caller/Listener connections, Live messages,
File streams, IPv4/IPv6, optional required AES-CTR encryption, Stream ID,
synchronous admission, level-triggered polling, errors and basic statistics.
Transport methods are nonblocking. Poller wait is the only deliberately blocking
operation. Allocation, configuration and OS work may take time; this is not a
hard real-time contract. Blocking convenience APIs and an application executor
integration can be added without changing the module boundary.

Groups, rendezvous, FEC, GCM, native UDP adoption and extended telemetry are
reserved for separately specified extension tables. Their capability names may
be queried now, but the core table provides no operations to activate them.
The listener here accepts only ordinary connections. A future group extension
must introduce a tagged ordinary/group accept result and preserve the separate
group admission security capability; it cannot reinterpret Connection handles.

## Performance contract

The wrapper and adapter are a direct in-process call path. Steady-state successful
I/O and WouldBlock handling MUST introduce:

- No additional payload copies or staging buffers. Pass the application's buffer
  directly to the vendor call. This does not claim zero-copy inside the vendor
  implementation or kernel.
- No heap allocation/deallocation, diagnostic string construction, provider-info
  copying, per-call shared_ptr ownership changes or exception-based control flow.
- No global registry mutex, global payload counter, central dispatcher or added
  serialization across unrelated connections or providers.
- No worker-thread handoff, queued payload task, forced context switch or wrapper
  syscall on successful I/O. Vendor-internal work remains vendor-owned.
- No implicit statistics query, log emission, tracing record, timestamp read or
  configuration/capability lookup. Omitted source time uses the vendor's native
  default behavior without a separate wrapper clock read.
- No mandatory wait before or after successful I/O. Applications can send/receive
  continuously until WouldBlock and then wait once. Do not force one message per
  readiness notification or insert sleeps, rate limits or periodic scanning.

Resolve the provider function table and static policy at object setup. Cache the
negotiated payload ceiling at connection completion rather than querying it per
receive. The dispatch budget is one indirect module-table call per ordinary I/O
operation, followed by a statically bound vendor call. The C++ facade introduces
no additional virtual strategy dispatch. Small fixed checks and metadata
translation are permitted and must be measured, not assumed free.

Handle validation must retain kind/generation/lifetime safety without a globally
locked lookup. Connection-local or sharded designs are permitted; the ABI does
not mandate a hash map. Lifetime ownership is established at creation and teardown,
not by incrementing a shared_ptr ownership counter on each packet. The implemented
close safety uses separate per-direction atomic call gates; benchmark their two
RMW operations per call against direct SRT. Concurrency safety
must not be removed to meet a benchmark. One send and one receive may proceed
concurrently per connection; same-direction concurrent operations require caller
serialization. Lifecycle coordination must not add a per-packet coarse mutex.

Polling uses caller-owned event arrays and scratch capacity reserved when interests
are registered. It must not allocate during steady-state waits or scan every
registered connection to synthesize readiness. Multiple independent pollers can
serve different connection sets. No mandatory combined-provider event thread is
introduced. Optional application event aggregation is outside the direct path.

Hot-path C++ results carry compact numeric IoError rather than the owning Error
record. An optional caller-supplied Error captures rich detail only on failure.
The C ABI likewise separates compact error codes from optional text. Success,
WouldBlock, EOF, timeout and explicit wake leave error storage untouched. On
FAILED, codes are captured immediately from the calling thread; if failure_detail
is supplied, copy the exact diagnostic then. Later diagnostic requests cannot
recover uncaptured thread-local text. Error-detail construction is a cold-path
cost, not something paid on every packet.

Payload accounting is explicitly opt-in and disabled in both default profiles.
Disabled accounting produces unavailable byte counters and adds no counter update.
When enabled, use independent per-direction/per-connection accounting and avoid
shared cache-line contention; no global total is updated by each send/receive.
Statistics are sampled only on explicit calls. Logging and tracing are disabled
on the per-packet path by default; configured diagnostics cannot stall data I/O.

No bulk API is mandated merely to amortize adapter overhead. If measurements show
per-call dispatch costs violate the gate at high message rates, revise the ABI
before freezing it (for example, add caller-owned batch descriptors with explicit
partial-completion semantics). A wrapper loop is not automatically a vendor-native
batching optimization. The single-message path must still meet its own gate.

### Performance acceptance gate

The required outcome is no significant degradation. Proposed initial numeric
budgets are at most 2% relative loss of sustained receiver-delivered payload
throughput and at most 5% increase in total CPU cycles per delivered payload byte.
These numbers make the draft measurable; they are proposed engineering budgets,
not user-approved tolerances or measured results. Any reproducible regression
outside the agreed budget blocks release and requires redesign or optimization.
Do not average a failing important workload into passing results from others.

Measure each provider against direct use of that same source revision, compiler,
optimization settings, crypto backend, feature flags and effective transport
configuration. Also compare against the intended deployed direct-library build
so private-module packaging costs cannot be hidden by a slower baseline. Use the
same hardware, affinity, payloads, offered load, peer implementation and network
conditions. Compare direct, C ABI and full C++ facade paths separately to locate
costs, while the full-path result is the acceptance measurement.

Measure saturated goodput, messages per second, total process CPU (including any
helper threads), cycles per delivered byte, p50/p99 latency and dropped/late data.
A link-limited throughput tie does not prove low overhead: include CPU-limited,
small-message/high-packet-rate, typical Live, large File-stream, full-duplex and
many-connection cases, plus concurrent use of both providers. Repeat with the
production encryption and diagnostics profiles and with configured network loss.
A throughput gain bought through more drops or material latency degradation fails.

Use warmup, sufficiently long trials and randomized/interleaved repeated baseline
and wrapped runs. Report variability and confidence bounds for the relative
regression; noise is not evidence of equivalence. If uncertainty exceeds the
budget, the result is inconclusive and requires more precise measurement. Report
allocation, copy and scheduling evidence alongside throughput. There are no
performance results yet, and literal zero overhead is not asserted.

## Ownership and provider selection

TransportRuntime loads an absolute module path and verifies its descriptor
against ProviderSpec before creating a runtime. Identity metadata identifies the
artifact; it is not cryptographic verification of an untrusted plugin.
ProviderInfo strings are copied into the C++ layer. C descriptor and capability
strings are immutable module-owned storage valid for the loaded module lifetime.

Provider is a shared context. Creating or copying it does not start another
vendor-global runtime. Each module permits at most one active srtm_runtime in a
process; a second runtime_create fails Busy. The host registry shares that context
across TransportRuntime instances loading the same module artifact. An explicit
shutdown affects all objects sharing that context and must be coordinated by the
application. Multiple separately loaded copies of one module are outside v1.

Connection, Listener and Poller are move-only. They retain their provider
context. Destroying the TransportRuntime owner begins shutdown, but context
storage and callback state remain alive until dependent work is quiescent.
Use-after-move is permitted only for destruction or assignment. The application
must not concurrently destroy or move a wrapper being used by another thread.

Native vendor handles never escape. C handles are nonzero, runtime-scoped,
kind-checked, and generation-protected. A handle cannot be reused as a different
kind, resurrected after release, or passed to another runtime. C++ checks provider
identity before dispatch. A Connection keeps its selected provider permanently.

## Module ABI and linking

Each private module statically contains one vendor library and exports exactly
srtm_provider_get_api. The host resolves this entry point against the explicit
module handle, never by global lookup. Both modules intentionally use the same
bootstrap name; unique module filenames and handle-specific resolution identify
the provider. All subsequent calls use the returned function table.

Vendor symbols and static dependencies must be hidden and internally bound.
Use the platform's export allowlist and appropriate symbol localization/binding;
do not assume compiler visibility flags alone override vendor export attributes.
Static archives need suitable PIC on platforms requiring it. Crypto dependencies
must be private or use an explicitly qualified shared dependency arrangement.
Module load order and coexistence with an unrelated globally loaded libsrt must
be included in eventual isolation checks. Modules remain loaded until exit.

The ABI assumes matching architecture, pointer width, endianness and platform C
ABI with default packing. Windows uses cdecl. No STL, C++ exceptions, bool objects,
native enum representations or compiler-specific vendor records cross it.
All provider functions and callbacks must contain exceptions at their boundary.

The bootstrap requires a compatible major and at least the requested minor.
The host checks returned table struct_size before reading fields. All v1.0 slots
are mandatory; a null mandatory slot rejects the module. Minor revisions append
fields only and preserve old prefixes and semantics. Changed meanings or value
type layouts require a major revision. Never compare whole structures with memcmp.

Every extensible input/output record, including nested records, is zero-initialized
and has struct_size set to its actual capacity. A provider reads/writes only the
recognized prefix bounded by that size and preserves unknown output tails.
Undersized required prefixes fail InvalidArgument without using partial inputs.
Reserved fields are zero. Frozen byte views and endpoint value types are not
independently extensible. Arrays supply capacity and element stride; each element
also has its own initialized struct_size. Stride must cover each record capacity
and preserve the platform-required alignment. Length conversions are checked.

Unless documented otherwise, output values are valid only on OK; errors leave
payload and result outputs unchanged. Control-path error records are reset and filled
with the result of the call when provided; NULL error is allowed. I/O and wait
use the failure-only compact/error-detail contract above instead. PENDING connect
and close are successful initiation and may mutate state. BUFFER_TOO_SMALL from
validate sets count to the required number and leaves issue elements untouched.
No allocator crosses the boundary. Returned tables are immutable module-owned
storage; caller payload buffers and error records remain caller-owned.

## Configuration

TransportConfig has explicit values; value initialization is not a usable profile.
The proposed named profile factory values are:

| Field | Live profile | File stream profile |
| --- | --- | --- |
| Encryption | Disabled, no secret, key bits 0 | Same |
| Receive and peer latency | 120 ms each | 0 ms each; TSBPD disabled |
| Connect timeout | 3000 ms | 3000 ms |
| Peer idle timeout | 5000 ms | 5000 ms |
| MSS | 1500 bytes | 1500 bytes |
| Requested message payload limit | 1316 bytes | 0, not applicable |
| Send and receive buffers | 8 MiB each | 8 MiB each |
| Stream ID | Empty | Empty |
| Adapter payload accounting | Disabled | Disabled |

These are proposed layer defaults, not assertions of vendor defaults. Encrypted
applications explicitly set aes_ctr_required, a valid key size and a secret.
No "auto" encryption or silent downgrade is exposed. Cipher and effective key
size are reported only when known. Configuration secrets are copied only into
owned secret storage, omitted from logs, and erased when no longer required.
The ABI borrows passphrase bytes for the configuration call; retained copies
must be private. Admission v1 cannot override per-connection secrets.

Adapters apply the transport preset first, then explicit overrides, then bind
and connect/listen. Live sets message and TSBPD behavior; File stream sets stream
behavior. Settings apply only during creation in v1; no mutable post-connect
option interface or vendor integer option passthrough is provided.
Buffer requests may be rounded to packet granularity or adjusted by the OS;
they are requests, not exact memory reservations. Unsupported values/combinations
fail; they are not silently clamped. Current core readback covers endpoints,
negotiated message ceiling, crypto and latencies, not every effective buffer value.

validate returns all determinable configuration issues without creating sockets.
OK means validation executed, not that the issue list is empty. Severity error
prevents creation; peer-negotiation requirements remain provisional. Creation
revalidates, so callers cannot bypass validation by omitting an earlier call.
Endpoint parsing and range checks happen in the wrapper; modules accept only
numeric endpoints, with IPv4 trailing address bytes and reserved bits zero.
Binding to port zero is supported; listener_endpoint reports the assigned port.

## Capabilities and extensions

C capability names map to Feature values as follows:

| Feature | Name |
| --- | --- |
| IPv4 / IPv6 | core.ipv4 / core.ipv6 |
| Live / File | core.live_messages / core.file_stream |
| AES-CTR | crypto.aes_ctr |
| Admission / polling / statistics | core.admission / core.polling / core.statistics |
| Rendezvous / FEC | transport.rendezvous / recovery.fec |
| Broadcast / Backup | groups.broadcast / groups.backup |
| GCM / UDP adoption | crypto.aes_gcm / transport.native_udp_adoption |
| Group domains / RTT observations | groups.admission_domains / telemetry.rtt_observation |

Availability describes operations implemented by the adapter and actual build,
not simply vendor symbols. AVAILABLE applies to the supplied role/configuration;
otherwise report UNSUPPORTED or NOT_BUILT. Constraints text is explanatory only;
validate is the authoritative machine-readable decision for a full core profile.
Capabilities do not certify peer interoperability. Qualification records belong
to deployment metadata keyed by build fingerprints, peer pair and test profile.

query_extension uses exact names and major/minimum-minor versions. Unsupported
names or versions return FAILED/Unsupported and NULL. Each future table starts
with srtm_extension_header_v1 and needs its own published ownership and threading
contract. No extension tables are declared in this draft. A C++ extension facade
will be added alongside each table; the public API does not expose raw C tables.

## I/O, clocks and error semantics

connect returns OK if already completed during the call, otherwise PENDING.
Wait for STATE or ERROR readiness and inspect connection_info for completion.
FAILED_STATE has a copied sticky terminal_error. A failed attempt must close;
reconnect uses a new Connection. Vendor thread-local error data is copied on the
thread where the failing call occurred. WouldBlock is not a terminal error.

Message sends are whole-message acceptance or WouldBlock/error. Successful
acceptance is not a delivery acknowledgment. Source time is optional; omission
uses vendor-default timestamp handling without an extra wrapper clock call. Explicit times must belong to that Provider
runtime, be nonnegative and fit the signed ABI field. Provider::now supplies the
appropriate clock domain. Do not transfer timestamps across providers or compare
them to host steady_clock without a separately specified conversion.

Receive buffers for Live MUST hold at least the reported max_message_bytes.
The adapter checks this before invoking vendor receive; undersized buffers fail
InvalidArgument without consuming a message. Unknown payload ceilings must be
resolved before receiving. The layer does not provide message peeking, truncation
or an automatic retry of a potentially consumed vendor message.

Stream operations return partial byte counts. A successful zero-length request
returns zero without invoking vendor I/O; END_OF_STREAM applies only to nonempty
stream receive requests. Message sends of zero bytes are rejected in core v1.
Buffers are borrowed only through the call; received metadata belongs to the
caller. No scatter/gather, borrowed receive buffers or async buffer retention.

When payload accounting is enabled, statistics TX/RX bytes count application payload accepted/delivered by the adapter,
not wire traffic, acknowledged bytes or retransmissions. Counters begin at zero
for each connection and never clear through this API. With accounting disabled,
these fields are unavailable and no adapter byte counters are updated. Epoch distinguishes reset
or replacement; snapshots need not be atomic across all independent metrics.
RTT is a provider estimate with explicit availability, not measured freshness.
Group statistics and bandwidth estimates deliberately have no core field.

## Polling and callback lifetimes

poller_register adds or replaces a registration; C++ add rejects a duplicate and
update uses the existing registration. One waiter per poller is allowed. Register,
remove and wake may run concurrently with wait. Events use generation-protected
registration tokens; they hold no pointer to a movable Connection wrapper.
Destroying a connection removes its registrations; already returned event tokens
can be stale and must be checked against the application's current registrations.
Polling does not keep a connection open solely because it was registered.

READ means receive/accept can be attempted; WRITE means send can be attempted.
ERROR is a terminal failure indication. STATE is level-triggered for completed
connect or close (including failure), not a transition-history stream; remove
that interest once handled to avoid repeated readiness. Listener closure is
observable through STATE. All readiness is advisory. Concurrent consumption may
cause WouldBlock. Remaining ready objects stay eligible if an output array fills;
wait reports actual count and does not use BUFFER_TOO_SMALL for event overflow.

Wait requires at least one event slot. Timeout zero polls, UINT64_MAX waits
indefinitely; finite durations above INT_MAX milliseconds and negative C++ durations fail. A wake is sticky until consumed by
one wait and coalesces repeated calls. It returns INTERRUPTED and count zero,
preserving readiness for the next wait. Wake takes priority when both are ready.
Timeout returns TIMED_OUT and zero count. Releasing a poller with an active wait
initiates cancellation and fails Busy; join active calls and retry release.
Shutdown wakes waiters before release.

Admission callbacks may run concurrently. The handler must be thread-safe, bounded,
and must not reenter the provider, close the listener or initiate runtime shutdown.
The C++ trampoline catches exceptions and rejects. Stream ID is borrowed and
self-reported, not authentication. Reject codes must be valid common SRT
application rejection codes; invalid decisions reject with the adapter's generic
application rejection. V1 has an immutable handler per listener. Replacement or
per-callback Subscription is deliberately omitted until its drain API is designed.

Listener close stops admission but may return PENDING while callbacks finish.
object_release cannot succeed until all its callbacks and operations are drained.
The C++ wrapper retains the handler and its captures until that point, including
when destruction queues deferred cleanup. Log callbacks at the C boundary copy
into a bounded host queue; the C++ log handler runs on a host dispatcher, never
under provider locks. Exceptions from the log handler are contained; log handlers
must not synchronously wait for their own dispatcher shutdown.

## Close and shutdown

The implemented AbortClose closes admission gates and synchronously waits for
already-entered nonblocking calls before native close. It never waits for
application payload delivery. DrainClose and callback-driven deferred cleanup
below remain future design, explicitly Unsupported in this milestone.

object_close initiates closure without waiting for graceful drain. Abort discards pending
outbound data; Drain permits the configured finite timeout, then aborts. Repeated
close is idempotent; Abort may escalate an existing Drain. New I/O fails after
Closing begins. object_close_status returns PENDING until quiescent, OK after
successful closure, or TIMED_OUT after a requested drain expired and closure
completed. A drain result is not proof of peer application consumption.

Listener close does not close already accepted connections. It cancels pending
accepts and drains admission callbacks. object_release is valid only after close
is quiescent, releases the native record and invalidates the handle. A second
release fails InvalidHandle; wrapper-level repeated close remains harmless.
Ordinary close status remains inspectable until release. Pollers use their own
release operation, not object_close.

runtime_begin_shutdown rejects new work, wakes pollers and initiates abort close
for remaining objects. Close status, inspection, deregistration, release and
shutdown calls remain usable. runtime_shutdown_status returns PENDING until
all I/O and callbacks are quiescent, then OK. runtime_destroy additionally requires
all object and poller handles released and all calls returned; otherwise Busy.
It completes vendor cleanup and invalidates the runtime pointer. No log callback
may occur after successful destroy. Module-owned tables remain resident.

C++ destructors initiate abort cleanup, never throw and do not wait for graceful
drain. Runtime-owned deferred cleanup retains native records and callback captures
until safe release. Explicit begin_shutdown/shutdown_status is the application's
completion mechanism. Cleanup resource exhaustion must retain a safe bounded
fallback; a destructor cannot simply abandon live callbacks pointing into freed
storage. Mechanism and scheduling remain implementation decisions.

## Design validation and next decisions

Header checks compile the transport declarations as C++20 and the ABI as C11.
The host loader also has executable fixture-module tests. These check loading,
isolation and validation only, not real provider interoperability or throughput.

Before freezing the ABI, resolve the extension table contracts, the exact common
application rejection code range, profile buffer rounding/readback requirements,
and target-platform export/dependency rules. The first implementation qualification
must satisfy the performance gate above and exercise both module load orders,
simultaneous traffic, independent cleanup,
stale handles, callback teardown, and all four provider peer pairings.
