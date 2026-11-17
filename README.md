# SRT multi protocol support

A C++20 host for selecting independently isolated SRT implementations at runtime.
The intended first providers are Robotweax SRT 0.3.3 and Haivision SRT 1.5.7.
Each provider statically contains its vendor library and exposes a versioned
C function table behind a C++20 transport facade. The interfaces remain unreleased.

## Current development milestone

The host loader and real Robotweax 0.3.3 / Haivision 1.5.7 modules are implemented.
The modules support basic nonblocking caller/listener connections, Live messages,
File streams, AES-CTR, state/error readback and independent runtime cleanup through
our C function table. Provider-local polling, concurrent abort-close/cancellation,
and the C++ Connection/Listener/Poller facade are implemented.

See [real provider build and usage](providers/README.md) for exact source pins,
supported profiles, concurrency rules and unsupported APIs.
Haivision 1.5.7 includes documented native key-management/close lifetime and
[caller-state transition fixes](docs/validation/2026-11-17-ci-connect-state.md).
Haivision File/IPv6 is explicitly rejected pending qualification after a native
failure reproduced on macOS. Admission/log callbacks and graceful drain-close
remain unsupported; they are not silently approximated.

Tests cover all four provider pairings on IPv4, Live IPv6 pairings, simultaneous
use of both providers, encrypted traffic, stale handles, error-buffer preservation,
polling, close races, cancellation, facade transfers and export isolation. A
direct-versus-module-versus-facade benchmark checks throughput and process CPU per
byte against each provider's own native baseline.

The latest [Linux Release results](docs/validation/2026-11-17-linux-release/README.md)
cover eight File/Live, plaintext/AES-128 and core/profile combinations at source
`2e185a9`, using timing protocol `persistent-workers-memcmp-v2`. Of 32 comparisons,
20 pass, four are inconclusive and eight have unavailable qualification data from
two incomplete encrypted File captures. Six complete cases provide 288 validated
samples out of 384 scheduled trials. Overall performance qualification remains
incomplete. The unchanged gates require eight pairs, five-second trials, native
CV at most 5%, throughput lower 95% ratio at least 0.98 and process CPU/byte upper
95% ratio at most 1.05. No confirmed regression is established by the valid
comparisons; inconclusive and failed cases still prevent sign-off.

The [Linux Debug timeout investigation](docs/validation/2026-11-17-debug-verifier.md)
identified excessive sanitizer cost in the benchmark's byte verification loop.
Independent full-body `memcmp` verification preserves integrity checks while
bringing all 16 local Linux ARM64 Debug ASan/UBSan and all 16 Release checks within
their existing deadlines. The benchmark verifier change leaves provider module bytes
unchanged and adds 36 corruption canaries; sanitizer CI includes benchmark smoke
coverage. The separate Release encrypted File capture timeouts remain unresolved.

[Finite Live functional tests](docs/validation/2026-11-17-live-tail-contract.md)
accept an explicitly reported missing tail after source completion, while
rejecting internal gaps, reordering, duplicates and corruption. These separate,
source-paced smoke tests do not produce qualification samples. File tests and
the strict timed benchmark continue to require complete delivery.

The [complete native feature audit and extension design](docs/design/extensions/README.md)
records native feature gaps and the versioned design. The first stage provides
[immutable Live/File profiles and native telemetry](providers/EXTENSIONS.md) in both
providers and the C++ facade; later extension features remain unsupported.
[Polling review fixes](docs/validation/2026-11-17-polling-review-fixes.md) address
ready-event starvation and facade registration cleanup and are included in the
latest measured source. Earlier [controlled File](docs/validation/2026-11-16-controlled-throughput.md),
[Live/encrypted](docs/validation/2026-11-16-live-encrypted-throughput.md),
[profiles/telemetry](docs/validation/2026-11-16-profiles-telemetry.md) and
[File investigation](docs/validation/2026-11-17-file-qualification.md) reports remain
historical evidence for their stated source revisions and timing protocols; their
samples are not pooled with the latest results.

## Build and check

Requirements: CMake 3.20+, a C++20 compiler, and Python 3 for the export test.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

GCC/Clang sanitizer configuration:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSRTMUX_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

`BUILD_TESTING=OFF` builds just the host library without test fixtures or Python.
The default host-only build downloads no vendor sources. Enable
`SRTMUX_BUILD_PROVIDERS=ON` for checksum-pinned real builds requiring OpenSSL 3.

## Interfaces

- [Module loader](include/srtmux/module.hpp): implemented low-level host API.
- [Provider ABI](include/srtmux/provider_abi.h): evolving, unreleased interface.
- [C++ transport facade](include/srtmux/transport.hpp): implemented public interface.
- [Interface and performance contract](docs/design/README.md): normative design
  requirements, lifecycle rules, proposed qualification budgets and open decisions.

The loader accepts absolute module paths and validates expected identity metadata.
Modules execute native code in the host process and must be trusted. Descriptor
matching is not a security boundary or cryptographic verification. Bootstrap and
descriptor functions must be side-effect-free and must not reenter Module::load;
every successfully opened module remains resident until process exit, including
modules rejected by validation and modules with no remaining public Module value.
Opening a module already executes static initializers; validation does not attempt
to undo those side effects by unloading it. Opened handles are cached by canonical
path, so repeated rejected loads do not accumulate loader references. Correcting
a module binary requires restarting the process.
Filesystem aliases resolving to the same canonical path reuse one module entry;
loading duplicate copies/hard links of one provider artifact is not supported.

The loader mutex is confined to setup. Data-path callers cache a reference to the
immutable table and call its slots directly; no loader registry lookup, lock,
allocation or ownership increment is involved in dispatch. Providers must still
meet the no-extra-payload-copy/no-thread-hop contract and measured throughput gate.

## Next milestones

1. Add admission callbacks and complete asynchronous failure diagnostics.
2. Resolve and qualify the native File/IPv6 limitation and broader transport profiles.
3. Qualify throughput, CPU, latency and loss across the deployment workload matrix.

Keep the working ABI unreleased until these checks establish that its dispatch,
lifetime and polling choices meet the performance requirement.
