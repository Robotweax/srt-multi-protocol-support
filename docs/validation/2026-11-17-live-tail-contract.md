# Finite Live functional tests — 17 November 2026

A finite Live source may finish with undelivered trailing messages. Without later
DATA to expose the missing sequence, periodic-NAK recovery does not guarantee a
lossless end of stream. This is an accepted Live behavior. File transfers retain
their complete-delivery requirement.

The existing Robotweax protocol documents this boundary in
[Live recovery with periodic NAKs](https://github.com/Robotweax/srt/blob/29c741d35f89a412926c374d6ccb15fa1270833c/docs/limitations.md#live-recovery-with-periodic-naks).
The earlier short Live CI checks used the strict throughput benchmark as a
functional test and incorrectly required every final message to arrive.

## Functional acceptance

The separate `srtmux_live_*` executables exercise native, C ABI and C++ facade
paths for both providers, with plaintext/AES-128 and original/profile-created
connections. They send 4 MiB of 1,316-byte messages in source-paced batches of at
most eight messages every two milliseconds, using the existing 120-ms Live latency.
They are functional
tests and produce no throughput estimate or qualification sample.

Every received message must have the expected length, sequence and full body.
A missing suffix is accepted only after all source messages have been successfully
submitted and a five-second receive readiness wait expires. At least one intact
message must have arrived. An internal gap, duplicate, reordering, corruption,
sender failure, non-timeout polling error or no delivery still fails. There is
no arbitrary numeric tail-loss allowance. Successful output explicitly records
sent, received and missing-tail message counts.

The contract tests discard the last three actual received messages and require
successful tail acceptance on all six provider/API combinations. Separate faults
exercise internal loss, duplicate/reordered sequence numbers, body corruption,
no delivery and sender failure. Each functional child has a 30-second process
deadline; each CTest group retains a 180-second deadline. Existing strict benchmark
and integrity-canary deadlines are unchanged.

## Performance and historical evidence

The strict timed benchmark and public comparison script retain their complete
delivery checks. The shared backend was extracted verbatim so both functional
and timed workloads use the same connection configuration and actual I/O APIs.
No transport or native-provider behavior changes for this correction.

The [eight-case Release results](2026-11-17-linux-release/README.md) remain tied to
source `2e185a9` and timing protocol `persistent-workers-memcmp-v2`. No historical
failure or incomplete capture is reclassified. Overall qualification remains
incomplete, including the separate encrypted File capture timeouts.

Before this correction, the [push](https://github.com/Robotweax/srt-multi-protocol-support/actions/runs/44100545252)
and [PR](https://github.com/Robotweax/srt-multi-protocol-support/actions/runs/44100782672)
sanitizer jobs exposed Live readiness failures. The initial PR attempt also
recorded late-packet dropping followed by a sequence gap. These original failures
are retained. Logs without delivered sequence/count information do not establish
that every readiness failure was tail loss; an internal gap remains an error.
The single failed-job reruns are diagnostics, not root-cause fixes or performance
qualification.

## Local validation

The 14 unchanged macOS ARM64 Release checks pass. All five new Live CTest groups
pass in 168.19 seconds, including the 42 injected-tail/failure cases. Linux ARM64
Debug ASan/UBSan provider-module SHA-256 values are unchanged after extracting the
shared backend and adding the functional executables. Reconstructing the old
benchmark translation unit from the extracted header and remaining source gives
the original source byte for byte; the timed transfer and comparison code are
unchanged.
