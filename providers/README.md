# Real provider milestone

This implementation uses the public C ABI in `include/srtmux/provider_abi.h`.
The C++20 facade in `include/srtmux/transport.hpp` owns provider contexts,
connections, listeners and pollers. The lower-level Module loader also exposes
the C table for applications that manage their own lifetimes.

## Build

```sh
cmake -S . -B build-real -DCMAKE_BUILD_TYPE=Release \
  -DSRTMUX_BUILD_PROVIDERS=ON -DSRTMUX_BUILD_BENCHMARKS=ON
cmake --build build-real --parallel
ctest --test-dir build-real --output-on-failure
```

OpenSSL 3 development headers/libraries are required. macOS Homebrew users can
set `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`. The default host-only build
still performs no downloads. Real builds download source archives pinned by both
revision and SHA-256:

| Provider | Release | Source revision |
| --- | --- | --- |
| Robotweax | 0.3.3 | `29c741d35f89a412926c374d6ccb15fa1270833c` |
| Haivision | 1.5.7 | `899348d8318eb9a3c5a5b6ec43c4a1114288773a` |

Haivision uses the pinned 1.5.7 source **with the local
[KM/close lifetime patch](haivision-1.5.7-km-close.patch)**. Its asynchronous KMREQ
and KMRSP handlers otherwise race `closeInternal()` freeing the crypto context.
Release-mode cross-provider encrypted close stress reproduced a use-after-free;
native context tracing confirmed processing after free. CMake verifies the exact
upstream core.cpp checksum and compiles a patched copy, leaving downloaded source
untouched. The patch takes the existing connection lock only in those two
key-management handlers and checks closure before accessing crypto. It adds no
locks to payload or ACK handling. `build_features` records `km-close-fix` with
the patched source hash; the build fingerprint includes it. Both direct and
wrapped benchmark paths link the same patched library. This is not an unmodified
upstream binary. Distribute this patch and the MPL-2.0 upstream source/license.

Each module contains its own static SRT implementation. The OpenSSL library is a
shared system dependency; the adapter never performs process-global OpenSSL
cleanup. ELF links localize vendor symbols using an export map and symbolic
binding; Mach-O uses an export allowlist. Windows relies on explicit exports.
The only module export is `srtm_provider_get_api`.

CMake FetchContent source-directory overrides are available for offline developer
builds, but bypass archive verification. Do not label modified overrides as the
published release. Build fingerprints describe configuration and adapter source;
they are not binary signatures. Source archives and the licenses copied into the
build's `providers/licenses` directory remain part of packaging provenance.

## Implemented profile

- Ordinary IPv4/IPv6 caller/listener setup with nonblocking accept/connect.
- Live message send/receive with retained boundaries and optional source times.
- File byte streams with partial I/O and EOF forwarding.
- AES-CTR with required encryption, configured key size, and secret validation.
- Provider-scoped generation handles, stale-handle rejection and abort-close.
- Numeric error capture, optional failure-only text, endpoint/state readback.
- Optional application-payload counters; disabled accounting does no updates.
- Provider-local native SRT epoll, level-triggered readiness, sticky cancellation.
- Concurrent abort-close with nonblocking send, receive and control calls.
- Runtime startup, serialized shutdown and independent provider cleanup.

All four provider pairings are covered on IPv4 for Live and File, with and without
AES-CTR. Live also covers IPv6 across all four pairings; Robotweax File/IPv6 is
covered against itself. Both modules stay loaded during the matrix, in both load
orders. Additional checks transfer through both providers concurrently and verify
one remains usable after shutting down the other.

**Haivision File/IPv6 is excluded from this initial profile.** On macOS, the pinned
native library established a direct File/IPv6 connection and accepted 32 KiB but
received zero bytes before failure. The independent direct-call reproducer is
[../tests/native_file_ipv6.cpp](../tests/native_file_ipv6.cpp). This also occurred
with 336,896 bytes through the adapter, in both load orders. The adapter rejects
that combination before bind/connect/listen on every platform pending separate
qualification. It does not silently split writes, throttle traffic or switch the
provider. This is a qualification restriction, not a claim that all operating
systems exhibit the native failure.

## Calling sequence and concurrency

1. Load the absolute module path with its expected identity using host::Module.
2. Call runtime_create once per provider with an initialized host record.
3. Validate/populate configuration, create a listener and/or connection, and
   initiate connect. PENDING means setup started, not that data can be sent.
4. Retry accept and inspect connection_info until Connected. Inspect the accepted
   connection too. This explicit completion step caches the negotiated payload
   limit before the first data call. Successful connect always returns PENDING in
   this milestone to enforce that completion step.
5. Send/receive using application-owned buffers. WouldBlock is expected. Retry
   using a provider-local poller. A poller can register only its own provider
   context's connections/listeners.
6. Abort-close to cancel I/O; join application workers before destroying wrappers.
   Release closed C handles explicitly (C++ RAII does this automatically).
7. Begin runtime shutdown, check shutdown status, then destroy the empty runtime.

One sending thread and one receiving thread may use an established connection
concurrently. Serialize same-direction calls and bind/connect setup. Info and
statistics may run alongside I/O. Abort-close and runtime shutdown may race I/O,
accept and poll waits: three per-object atomic gates stop new operations, wait
for admitted nonblocking calls to return, then retire the native socket. TX and
RX gates use separate cache lines. There is no global mutex, shared_ptr increment,
allocation, timer read or payload copy on the send/receive path. The gates add
one acquisition and one release atomic RMW per call; their cost must be measured.

Raw `runtime_destroy` requires all application calls to have returned and all
handles to be released. It must never race a raw runtime call. C++ objects retain
the provider context until their own destruction, but callers still must not
move or destroy a wrapper while another thread uses that wrapper. Destroying a
TransportRuntime initiates shutdown; surviving objects retain storage for cleanup.
Repeated loads share one context per module, including across TransportRuntime
owners: shutdown by any such owner affects that shared context. Use one runtime
owner for a cooperating group of application components. Setup/control operations
may allocate and propagate `std::bad_alloc`; compact I/O outcomes do not allocate.

Pollers have one waiter, with concurrent register/update/remove/wake supported.
Haivision uses native `srt_epoll_uwait` with a private unbound SRT notification
socket. A small bridge, compiled with the pinned vendor target’s own headers and
definitions, sets its native readiness and signals the vendor event. It creates
no UDP transport or helper thread. The bridge is private and is included in build
fingerprints as `native-wake`; no vendor C++ layout crosses the module boundary.
Robotweax uses `srt_epoll_wait` with a private OS wake socket (Unix datagram socket
pair on POSIX, loopback UDP on Windows). There is no coordinator thread,
wrapper timer slicing or scan of all registered sockets on each wait. Native
vendor scheduling still determines wake latency. Wait translates at most 256
ready events per batch using fixed scratch storage. Finite waits accept 0 through
INT_MAX milliseconds; `SRTM_WAIT_FOREVER`/`InfiniteWait` is unbounded. Wake is sticky,
coalesces and wins over readiness. Closed registrations remain ERROR/STATE-ready
until removed or their object is released. Returned tokens may already be stale;
applications must check them against their own registration state. Poller release
with active calls initiates cancellation and returns Busy; join calls and retry.
Shutdown wakes both providers independently. No library unload occurs during I/O.

A native socket already retired by the vendor is treated as closed when its
current adapter handle is closed. Unknown or released adapter handles still fail
InvalidHandle; this does not weaken generation checks. Validation currently
reports the first invalid configuration field.

The registry grows in stable pages during connection creation and reuses released
slots with new generations. It supports 65,536 concurrent handles per provider;
capacity exhaustion reports Resource without changing existing connections.
Generation exhaustion also fails rather than reusing a stale identity. Pages are
retained until runtime destruction. No registry allocation occurs in I/O.

Unsupported operations fail with Unsupported rather than succeeding silently:
admission/log callbacks, graceful drain-close, groups,
rendezvous, FEC and GCM extensions. Required function-table entries remain present
so the host can load the evolving ABI; presence is not a capability assertion.
Receive buffers smaller than the cached Live ceiling fail before vendor receive
and do not consume the pending message. Error outputs are untouched on success,
WouldBlock and EOF. Endpoint/state readback flags identify available fields; RTT
and bandwidth metrics are not synthesized. Setup failure detail currently reports
connection failure plus an available native rejection reason; exact asynchronous
vendor error preservation is a later lifecycle milestone.

## Throughput validation

The data path directly passes buffer pointers to the statically linked vendor C
API. It adds neither payload copies nor per-call heap allocation, queued work,
thread handoffs, global mutexes, timing reads or implicit statistics requests.
Provider dispatch and configuration are resolved before steady-state I/O. Compact
error records are populated only on failure; text formatting is opt-in on failure.

Build the benchmark option above, then run the controlled local matrix:

```sh
python3 benchmarks/compare.py --build build-real --mib 2048 --runs 8 \
  --warmup-mib 256 --probe-calls 20000 --wait poll --output results.json
```

See [the benchmark protocol](../benchmarks/README.md) for paired confidence
intervals, controls, baseline selection and diagnostic modes. Both providers and
all three paths (direct native, module ABI, C++ facade) are always included.
Direct and wrapped paths use the same vendor static targets, including the
Haivision close fix. The default direct baseline uses native `srt_epoll_uwait`.
Measurements include receiver-completed goodput, whole-process CPU/byte,
application-thread CPU, call counts, idle receive probes and artifact hashes.
CPU seconds/byte is a portable proxy, not hardware cycle measurement.

The [controlled local results](../docs/validation/2026-11-16-controlled-throughput.md)
pass the measured 2% throughput / 5% process-CPU budgets for both wrapped paths
and both providers, with a narrow Haivision C ABI throughput margin.

Local File/IPv4 findings do not qualify Live, encryption, full duplex, many
connections, lossy networks or other hosts. Do not trade integrity, concurrency
safety or delivery latency for a passing throughput number.
