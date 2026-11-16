# Real provider milestone

This implementation uses the public C ABI in `include/srtmux/provider_abi.h`.
The higher-level C++ Connection/Listener facade remains a design draft. The host
Module loader supplies the real modules' function tables to C++ applications now.

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
   using application scheduling; the test retry loop is not a production poller.
6. Stop and join application I/O, then abort-close and release each handle.
7. Begin runtime shutdown, check shutdown status, then destroy the empty runtime.

One sending thread and one receiving thread may use an established connection
concurrently. Statistics may sample their optional counters concurrently. Each
same-direction I/O stream must have one owner. **Connection control operations
(including connection_info), close, release and runtime shutdown must be serialized
by the caller with I/O on the affected objects.** Quiesce all calls before runtime
shutdown/destroy. Concurrent lifecycle cancellation is not implemented yet; callers
must not infer it from the broader design draft. Publication/removal locks are
cold-path only; established I/O performs no global locking or ownership increments.

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
provider polling/wake, admission/log callbacks, graceful drain-close, groups,
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

Build the benchmark option above, then run:

```sh
python3 benchmarks/compare.py --build build-real --mib 128 --runs 3 \
  --output results.json
```

The runner randomizes/interleaves direct-library and module-ABI transfers built
from the same vendor static targets. Both use File/IPv4, plaintext, 32 KiB calls,
8 MiB warmup, full payload validation, a sender thread and a receiving thread,
and identical transport settings. It records receiver-completed goodput, total
process CPU time, configuration fingerprints and raw samples. CPU seconds/byte
is a portable proxy, not hardware cycle measurement. Retries yield only on
WouldBlock in both paths. The C++ transport facade is not included because it
is not implemented yet.

This is an exploratory baseline, not the performance release gate. Shared-host
noise, short trials and a narrow workload cannot prove the 2%/5% draft budgets.
Formal qualification still requires saturated message-rate workloads, full duplex,
many connections, encryption/loss profiles, latency/drop checks and confidence
bounds. Do not trade integrity, concurrency safety or delivery latency for a
passing throughput number.
