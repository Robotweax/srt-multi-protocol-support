# Versioned provider extensions — design draft

**Original design draft; implementation status has advanced.** See
[implemented profiles and telemetry](../../../providers/EXTENSIONS.md) for the supported
subset and explicit differences from this proposed contract. The remaining sections
are design requirements, not a runtime availability declaration. Start with the [feature audit](AUDIT.md),
then review the [C module ABI declarations](robotweax_extensions.h) and
[C++ facade declarations](robotweax_extensions.hpp). The C declarations now forward to the public extension header. The speculative
C++ classes remain compile-checked design declarations; implemented facade methods
are in `include/srtmux/transport.hpp`. C++ telemetry/adoption/hybrid convenience classes are deferred;
their explicit C contracts are drafted here.

## Negotiation and compatibility

Use existing `query_extension(name, major=1, min_minor=0)`. Proposed tables are
`srtmux.profiles`, `srtmux.groups`, `srtmux.telemetry` and
`srtmux.robotweax.experimental.hybrid`. Common tables are not Robotweax-only:
Haivision may implement its supported subset. Groups and hybrid depend on profiles
for extension object lifecycle. Hybrid additionally requires groups. A module
must reject table negotiation when its dependencies cannot be satisfied.

Cache immutable module-owned tables once per runtime; retain the module until all
objects, callbacks and active operations quiesce. Check major, minimum minor,
header size and every required slot before caching. Minor changes append table
slots only. Never reorder slots or reinterpret existing enum values. New required
semantics or changed embedded-record layouts require a new record/table major.
These nested v1 records are frozen: appending a nested record in place would shift
its enclosing fields and is forbidden. Optional future records need independent
size/version and pointer slots.

Every extensible input/output carries its size; zero reserved fields. Validate
size before reading any field, reject unknown required values, preserve output
bytes beyond the supported prefix. Arrays use caller capacity and byte stride;
check alignment, overflow, record sizes and stride before calling native code.
Integer constants cross C, never compiler enum layouts or native handles/classes.
Table/string views remain valid only while the module is retained. `describe`
reports known feature names even if adapter availability is false; unknown names
return Unsupported. Constraints are human-readable diagnostics, not policy to
parse. Actual immutable profile creation validates the entire combination.

Initially recognize feature names `control`, `sensor`, `fec`, `aes_gcm`,
`session_auth`, `acp1`, `groups`, `path_ids`, `standby_rtt`, `arq_first_fec`,
`multicast`, `independent_multicast`, `coordinated_multicast`, `hybrid`,
`compiled_controllers`, `compiled_filters`, `rendezvous`, and `native_telemetry`.
Qualification IDs refer to immutable reports with build, OS, topology and load;
an empty ID means unqualified. Never treat availability as interoperability or
performance qualification. Profiles may succeed only for implemented combinations.

## Profiles, admission and message semantics

`create_profile` validates and copies all setup values and secrets transactionally;
on failure it creates nothing. Profiles are immutable, runtime-scoped handles.
Connections/listeners/groups retain their profile independently of the caller's
profile handle. Retained secrets are erased at final release; callers erase their
original buffers. Foreign-runtime handles are rejected.

`mode` and `cipher` are authoritative. C `base.mode` is ignored and
`base.encryption` must be disabled; `base.key_bits` and passphrase still apply.
C++ builders translate the selected mode/cipher to these fields. C++ transport
mode must be Live for Live/Sensor, File for File/Control; encryption must be
disabled in the embedded transport record. No silent defaults from a different
vendor profile. Named host builders must supply documented explicit defaults
before implementation is published. Setup rejects incompatible or unavailable
features instead of accepting them and ignoring their options.

Live/File extended setup returns ordinary core Connection handles. Control/Sensor
return MessageChannel handles with dedicated operations. `connect` returns Pending
with an owned object when establishment is asynchronous; terminal pre-creation
failure leaves the result invalid. C++ returns the connecting object and exposes
state/poll readiness. No payload I/O before connected. Control requires ordered,
infinite-TTL messages, disables TSBPD/late-drop/FEC and exposes a negotiated whole
message limit. Sensor selects exactly the native fixed bundle and only permits
single-sample messages within the effective payload ceiling. Finite expiry/loss
is part of its semantics. No profile promises application exactly-once execution.

All sends are nonblocking: OK accepts the entire message, WouldBlock accepts
nothing. Group OK means native logical/queue acceptance, never all-member delivery.
Never return a retryable short-metadata error after native payload acceptance.
Receives require caller storage at least the configured maximum whole-message
size; smaller storage is rejected before native receive, leaving data untouched. The connected object info reports
the effective maximum; setup fails if the requested bound cannot be maintained.
This deliberately avoids promising native peek/truncation behavior. File byte
streams retain core partial-send semantics. Source time uses the provider clock,
not wall time. Control rejects finite TTL; no timestamp conversion or staging
copy on the payload path.

Admission uses a retained allowlist of prevalidated profiles. The callback sees
untrusted observations and returns an index, rejection or trusted group domain
and inline dispatch key. It never receives a provisional native socket. Only
native-supported per-child transport/security policies may vary; binding and
path/standby policy stay frozen across the allowlist. Apply transport before
security and crypto creation. Failed validation rejects the child. StreamID and
remote group ID are routing inputs, not authorization. Group domain must be
nonzero and selected from application authorization, consistently across listeners.

Callbacks must be bounded, nonblocking, nonthrowing and non-reentrant; no logging,
allocation, network lookup or calls into the same runtime. C++ uses a noexcept
function pointer and retains shared callback context. Listener close stops new
admissions and drains in-flight callbacks through the lifecycle below. Acceptance
is tagged Connection/MessageChannel/Group; never reinterpret a mirror group as a
single connection. Facade object allocation is permitted at acceptance, not send.

## Group ownership and UDP adoption

Bound maximum members, message/window/FEC memory and native queues at setup.
Do not implement broadcast by copying payloads into wrapper per-member queues:
call the native group API once. Broadcast may succeed with only one member accepting.
Backup replay/selection remains native. Path IDs are opaque bytes; standby RTT is
monitoring only and must not be advertised as the native selection metric.

`add` can return Pending plus a generation-checked member token. Per-member errors
and eventual state are distinct from group state. Token scope includes owning
group; removed tokens never alias replacement members. Snapshots provide coherent
membership revision, state and detached status. An undersized snapshot returns
BufferTooSmall and required count without partial records; callers may retry if
membership changes. Native group IDs never cross the ABI. Serialize membership
changes with other setup; close may race them. Same-direction I/O remains serialized.

Optional UDP adoption is explicit ownership transfer of a platform descriptor.
Validate and reserve all wrapper bookkeeping first. Set consumed=1 immediately
when native adoption succeeds, even if later connection fails. Once consumed,
provider cleanup owns the descriptor and the caller must not close/reuse it.
Consumed=0 leaves ownership with the caller. Initialize consumed=0 on input;
never reuse a receipt as a new adoption request. No silent dup, borrowed adoption or
ambiguous success-only receipt. A multi-endpoint caller performs individual adds
and retains every receipt; a failed later add does not undo earlier members.
An ordinary-connection adoption convenience API and multi-listener accept-bond
convenience API are deliberately deferred, not covered by this group slot.

Multicast setup selects legacy/independent/coordinated explicitly and rejects
unsupported roles/families. Independent requires concrete IPv4 source before
bind; port zero resolves at listen. Coordinated is listener-sender only with
independent mode implied, positive frozen MAXBW, repair threshold zero and native
64-frame queue/64 receiver-slot limit including detached members. Original wire
rate includes IP/UDP, SRT and crypto overhead. Shared native repair budget is
four-times-MAXBW with native burst rules; it does not guarantee fairness/deadlines.
No wrapper repair scheduler, worker queue or automatic fallback is introduced.

## Safe close, polling and cancellation

All extension-owned objects use profiles.close/info/release; ordinary Connections
use existing core lifecycle. Close is abort-only, idempotent, nonblocking and may
return Pending. It atomically bars new native operations, wakes relevant pollers,
and allows already-pinned native calls to finish before destruction. It must not
inherit native blocking linger (notably Control's default). There is no drain-close
promise. Release during active operations initiates close and returns Pending;
retry only until release succeeds. Handle generation and runtime pins prevent ABA
and use-after-free. Runtime shutdown closes children before destroying providers.
Destructors retain deferred cleanup ownership if immediate release cannot finish;
no leaked raw handles, busy-wait loops or destruction under an active call.

The existing provider-local poller must recognize extension object kinds before
any such object is advertised as poll-integrated. Register by owned handle, with
stale generation checks; extension close signals STATE/ERROR. No sleep-based
polling, global lock shared by both providers, helper thread per connection or
mandatory extra wait system call. Existing sticky/coalesced poller cancellation
has priority over readiness and remains the mechanism to cancel blocked waits;
it does not implicitly cancel a transport. C++ registrations return the existing
Registration token, removable through Poller. One waiter per poller and one
sender/receiver per object remain the concurrency contract. Do not move/destroy
facades concurrently with calls. Callbacks are fully quiescent before successful
release; callback context must not be freed merely because close was requested.

## Telemetry and experimental control

Telemetry is pull-only, never implicitly collected per payload. Resolve named
metric IDs at setup. IDs are runtime-local and paired with fixed value type,
unit name and scope; unknown fields return Unsupported. Snapshot counters are
non-clearing, with clock domain, epoch and membership revision. Availability is
expressed by returned records, never fabricated zero measurements. RTT age and
validity/outcomes require separate named metrics; snapshots do not prove source
health. Capture a native snapshot once then translate into caller storage.
Capacity failure is non-clearing/non-consuming. Peer identity is copied into caller
buffers with required lengths and treated as self-reported. No public native
counter exists for some internal FEC policy measurements; do not invent it.
A frozen metric-name catalogue and field-by-field units mapping remain a required
implementation deliverable; this draft defines transport and discovery, not those
measurement definitions. Native message/packet IDs are not portable group tokens.

Hybrid is a separate experimental design requiring an exact-source internal
bridge, explicit build fingerprint and new lifetime/security qualification. It
must not be obtained merely by casting a native internal C++ pointer. The native
coordinator owns budgets, recovery and history. Pairwise credentials use distinct
32-byte high-entropy secrets, exact StreamID, group binding, generation and
provider-clock expiry; never derive them from shared media keys. Provisioning
copies secrets off the payload path, rejects missing/expired/incorrect bindings
and erases retired copies. No key export API or downgrade fallback.

Requests target multicast/unicast; native validated local history supplies cursors,
not host/peer-provided discard authority. Catch-up discards require explicit opt-in
and a maximum skipped-message bound, default zero. If native code cannot enforce
the bound before commit, reject that request. Report discontinuities; do not
promise lossless switching or decoder continuity. Operation IDs are scoped to the
controller and generation. Cancel is sticky before commit; a commit race returns
AlreadyCommitted and authoritative state, never a fictitious rollback.

Events are bounded caller-buffer records. Overflow is sticky, emits an overflow
indicator and pauses automation. Snapshot plus revision-checked resynchronize is
required before resuming; terminal/discontinuity information stays recoverable. Operation results remain queryable until explicit
forget_operation on a terminal operation. The configured operation cap includes
unforgotten terminal records; requests fail with resource exhaustion rather than
evicting them. Snapshot cumulative skipped counts survive event overflow.
STATE readiness remains asserted while events/overflow are pending. Parent close
cancels operations and wakes waiters; release waits for native controller callbacks
and secret ownership to quiesce. This proposal remains blocked from implementation
advertising until exact native APIs can satisfy these requirements. No runtime
custom plugin loader: only trusted source-compiled, fingerprinted registry names.

## Performance acceptance and implementation sequence

No extension discovery, telemetry, mandatory reference-count increment, new lock,
heap allocation or adapter payload copy is added to existing per-message calls.
Retain the current operation pinning that protects close; optimize only with
measured evidence and preserved lifetime invariants. Allocate bounded group,
FEC, callback and event storage at setup. Do not duplicate native recovery queues.

Keep both providers loaded and test native versus module versus C++ paths. For
feature-off ordinary transfers, retain the existing maximum **2% throughput loss**
and **5% whole-process CPU/byte increase**, with paired 95% confidence bounds,
at least eight pairs of at least five seconds and native CV <=5%. For feature-on
traffic compare the same native feature/profile, geometry, crypto, peers and loss
pattern, not an ordinary unicast baseline. Also report application goodput, wire
bytes, latency, loss/discontinuities, queue bounds and peak memory. Native parity,
replication or security costs must be measured separately from adapter overhead;
they do not excuse wrapper copies or contention. An unstable run is inconclusive,
not a pass. No new runtime code in this change means no new throughput claim.

Implement profiles/telemetry and lifecycle qualification first; then Control/Sensor,
authentication/FEC/GCM; then admission and groups/path/standby; then multicast;
finally opt-in hybrid after its native bridge is reviewable. Logging, native file
helpers and raw platform event integration are not dependencies for this sequence.
Each stage requires both-provider tests, explicit unsupported combinations, foreign
and stale handles, close versus I/O/member changes/admission, sticky wait cancellation,
no callback after release, undersized buffers and failed ownership transfer.
Multicast adds bounded fanout, rekey and detached-member cases; hybrid adds expired
credentials, cancel/commit races, overflow and explicit local discard. Existing
sanitizer and throughput gates must remain green before advertising any extension.
