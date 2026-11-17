# Linux Debug benchmark verification cost — 17 November 2026

The extra Linux Debug ASan/UBSan campaign at `7086b13` exceeded the unchanged
180-second profile-setup and encrypted-File smoke deadlines. Native, C and C++
paths all reported about 0.7 Mbps; this did not establish a wrapper regression.

## Reproduction and cause

Ubuntu 24.04 ARM64, GCC 13.3, Debug with ASan/UBSan, LinuxKit guest on Mac13,2.
A fresh container with no CPU quota reproduced the slowdown. GDB samples of the
native Robotweax benchmark repeatedly found the receiver in the `std::all_of`
verification loop, including GCC ASan fake-stack allocation in iterator/predicate
calls. The sender was already waiting at the finish barrier. Samples also showed
native scheduler work; the measured slowdown is not attributed entirely to the
wrapper or native I/O.

An unprofiled Robotweax 4 MiB transfer after 1 MiB warmup spent 45.175 seconds
elapsed, 72.463 seconds process CPU and 45.131 seconds receiver CPU. An unprofiled
Haivision 1 MiB transfer after the same warmup spent 10.859 seconds elapsed and
10.846 seconds receiver CPU. These are diagnostic samples, not qualification.

## Correction and controls

The benchmark now compares every body byte with an independent immutable reference
using `memcmp`. It still checks every Live sequence number and message boundary.
The extra reference buffer is allocated before either transfer phase, not in the
payload loop. No receive bytes are skipped. No sanitizer option, test timeout,
performance budget or production transport source was changed.

With the same instrumented native libraries and workload configuration except for
the benchmark verifier, Robotweax's 4 MiB transfer took 3.407 seconds elapsed and
1.707 seconds receiver CPU; Haivision's 1 MiB transfer took 0.683 seconds elapsed
and 0.007 seconds receiver CPU. The small samples illustrate the mechanism and
must not be interpreted as stable throughput estimates.

SHA-256 checks confirm that both provider modules in both Debug and Release builds
are byte-for-byte unchanged by this verifier correction. The runtime still retains
its concurrent-close, cancellation, polling and caller-state protections.

Integrity canaries alter the first, middle and last actual receive body byte on
all six provider/path combinations in File and Live mode (36 cases). Every case
must return a payload-corruption failure and join its worker within the existing
15-second process deadline. Sender/receiver failure cleanup checks remain in the
same test. The Linux sanitizer CI job now includes the benchmark targets and
smoke tests so this Debug path cannot silently fall outside its coverage.

The timing protocol is now `persistent-workers-memcmp-v2`. Earlier v1 data remain
intact and are not pooled with v2: replacing timed verifier work changes the
measured CPU and throughput workload even though production modules are unchanged.
Fresh Release qualification is required with the same 2% throughput, 5% CPU/byte,
5% native CV, minimum eight pairs and minimum five-second gates.

## Correctness validation

The Linux ARM64 GCC 13.3 Debug ASan/UBSan suite passes 16/16 checks in
285.07 seconds, including all benchmark smoke tests at their existing deadlines
and all failure/integrity canaries. The same Linux Release suite passes 16/16
checks in 95.93 seconds. Profile-created encrypted File and original encrypted
File Debug smoke tests pass in 30.76 and 30.73 seconds respectively; Live plaintext
and AES-128 Debug smoke tests pass in 40.28 and 43.00 seconds. These are functional
results, not throughput qualification.
