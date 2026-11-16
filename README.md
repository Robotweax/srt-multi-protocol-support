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
Haivision 1.5.7 includes a documented native key-management/close lifetime fix.
Haivision File/IPv6 is explicitly rejected pending qualification after a native
failure reproduced on macOS. Admission/log callbacks and graceful drain-close
remain unsupported; they are not silently approximated.

Tests cover all four provider pairings on IPv4, Live IPv6 pairings, simultaneous
use of both providers, encrypted traffic, stale handles, error-buffer preservation,
polling, close races, cancellation, facade transfers and export isolation. A
direct-versus-module-versus-facade benchmark preserves the throughput
requirement as a measurable gate. Both providers pass the measured local
File/IPv4 gates after correcting a Haivision polling regression; see the
[controlled File results](docs/validation/2026-11-16-controlled-throughput.md).
[Controlled Live and encrypted File tests](docs/validation/2026-11-16-live-encrypted-throughput.md)
also pass both gates for both providers and API paths: IPv4 loopback, 1,316-byte
Live messages, and AES-128-CTR. Broader deployment profiles remain unqualified.

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
