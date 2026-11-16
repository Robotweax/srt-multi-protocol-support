# SRT multi protocol support

A C++20 host for selecting independently isolated SRT implementations at runtime.
The intended first providers are Robotweax SRT 0.3.3 and Haivision SRT 1.5.7.
Each provider will statically contain its vendor library and expose a versioned
C function table. The application-facing transport design is in progress.

## Current development milestone

Implemented: cross-platform module loading, exact implementation/release matching,
optional build fingerprint matching, ABI and required-slot validation, immutable
cached dispatch, process-lifetime module pinning and concurrent-load coordination.
Module loading does not create a vendor runtime or transfer any network data.

Tests load two deliberately colliding fixture modules in both orders, exercise
independent runtime state/cleanup, reject malformed ABI tables and descriptors,
and check the dynamic export allowlist on Linux/macOS. The fixtures are test-only;
they are not Robotweax or Haivision adapters. Windows loader/build support has a
CI job; it must pass there before platform qualification is claimed.

Not implemented yet: real SRT provider adapters, application Connection/Listener/
Poller implementations, network interoperability, installation/package exports,
and throughput qualification. No performance claim follows from loader tests.

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
There are no vendor downloads during configure/build in this milestone.

## Interfaces

- [Module loader](include/srtmux/module.hpp): implemented low-level host API.
- [Provider ABI](include/srtmux/provider_abi.h): evolving, unreleased interface.
- [C++ transport draft](docs/design/transport.hpp): declaration-only proposal.
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

1. Build pinned static Robotweax/Haivision artifacts into private modules and
   qualify exported symbols plus crypto dependency binding on each platform.
2. Implement runtime/connection ownership and a common ordinary Live/Message
   vertical slice, with cancellation and close correctness tests.
3. Add listener admission, readiness polling, File/Stream I/O and encrypted
   interoperability across all four provider pairings.
4. Measure direct-library versus C ABI versus C++ paths under saturated traffic,
   small messages, full duplex and many connections. Significant throughput
   regression blocks release; the numeric budgets remain draft proposals.

Keep the working ABI explicitly unreleased until these checks establish that its
dispatch, lifetime and polling choices meet the performance requirement.
