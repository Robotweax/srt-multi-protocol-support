# Plaintext File qualification investigation, 2026-11-17

**Performance qualification remains incomplete for both providers.** The fixed
campaign completed all 144 transfers (576 GiB measured, plus 72 GiB warmup),
but all eight new comparisons are inconclusive: six fail baseline-variability
controls and two cross the throughput confidence gate. No confirmed regression
is established under the existing rule. This does not resolve the three earlier
inconclusive comparisons, and no gate has been relaxed.

## Investigation and measurement correction

The [previous results](2026-11-16-profiles-telemetry.md) left three plaintext File
comparisons inconclusive. Historical slow wall-time clusters did not scale uniformly
with process CPU. A [short diagnostic pilot](2026-11-17-file-diagnostics.json)
added non-clearing native counters and OS resource deltas at measurement boundaries.
It recorded no major page faults and mostly zero loss/retransmissions, with 195
retransmissions in one native Robotweax trial. That pilot did not establish the cause
of all historical slow trials.

Review found a benchmark-control weakness: warmup and measurement created separate
sender threads, and thread creation/join was inside the measured interval. The
benchmark now reuses one sender and the same buffers across warmup and measurement,
with start/finish barriers bounding transfer timing equally for native, C, and C++.
Native worker CPU, readiness waits and payload verification remain included.
Native counter reads and OS resource collection are outside the timed interval;
asynchronous native activity can straddle those boundaries. Counters are diagnostic,
not an excuse to exclude samples. OS context-switch counts do not prove CPU starvation
or thermal throttling.

The new timing protocol is `persistent-workers-barriers-v1`. Older fresh-thread
captures remain intact and are **not pooled** with the new protocol. This correction
improves steady-state measurement control; it does not prove that thread startup
caused the previous inconclusive results.

## Fixed qualification plan

The [plan and source hashes](2026-11-17-file-qualification-plan.json) were written
before the first qualification sample. Two sequential captures cover original and
profile-created plaintext File connections. Each contains twelve complete blocks,
with both pinned providers and native/C/C++ paths in every block. Every transfer
measures 4 GiB after 512 MiB warmup, using local IPv4, 32 KiB calls, 8 MiB native
buffers, provider-local polling, accounting disabled and 20,000 separately timed
idle receive probes. Execution order rotates across blocks. All received bytes
are verified. All samples are retained; no adaptive early stopping or exclusions.

The unchanged gates require lower paired 95% throughput ratio >=0.98, upper paired
95% whole-process CPU/byte ratio <=1.05, at least eight pairs, every trial >=5 seconds,
and native throughput CV <=5%. Analysis uses paired geometric ratios and 10,000
percentile-bootstrap draws. CPU seconds are not hardware cycles. No task-owned
builds, tests or profilers overlap the captures. This is not an exclusive-host,
CPU-affinity or thermal-control claim.

## Completed original-setup result

[Original File raw capture](2026-11-17-core-file-qualified.json): 72 transfers,
288 GiB measured. All four comparisons are inconclusive. The filename identifies
the qualification run, not a passing decision; consult each summary decision.

| Provider/path | Pairs | Throughput lower 95% | CPU/byte upper 95% | Native CV | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| Robotweax C | 12 | +1.304% | -14.702% | 6.350% | Inconclusive: controls |
| Robotweax C++ | 12 | -0.626% | -13.779% | 6.350% | Inconclusive: controls |
| Haivision C | 12 | -3.051% | +4.405% | 4.690% | Inconclusive: interval |
| Haivision C++ | 12 | -3.758% | +4.144% | 4.690% | Inconclusive: interval |

The original-setup data include 2,916 sender retransmissions and no major page
faults. Several slow native trials report zero loss and zero retransmissions;
packet recovery cannot account for the full slowdown. Process CPU also rises in
late native trials, so a pure idle-wait explanation is insufficient. The available
measurements do not identify a single causal mechanism.

## Profile-created result

[Profile File raw capture](2026-11-17-profiles-file-qualified.json): 72 transfers,
288 GiB measured. Both native baselines exceed the 5% variability limit.

| Provider/path | Pairs | Throughput lower 95% | CPU/byte upper 95% | Native CV | Decision |
| --- | ---: | ---: | ---: | ---: | --- |
| Robotweax C | 12 | -0.154% | -15.843% | 5.911% | Inconclusive: controls |
| Robotweax C++ | 12 | +3.253% | -18.305% | 5.911% | Inconclusive: controls |
| Haivision C | 12 | -5.752% | +5.629% | 8.124% | Inconclusive: controls |
| Haivision C++ | 12 | -4.931% | +3.468% | 8.124% | Inconclusive: controls |

This capture records 3,619 sender retransmissions and zero major page faults.
Native goodput ranges from 1,544.5 to 1,836.8 Mbps for Robotweax and 1,178.1 to
1,621.0 Mbps for Haivision. Across both captures, measured transfers last
16.456–30.396 seconds; every duration and pair-count requirement is satisfied.
All received payloads pass verification. All CPU upper bounds pass except profile
Haivision C, whose +5.629% bound also crosses the +5% gate. Passing individual
bounds does not override failed controls.

## Provenance and remaining blocker

The final source-hash check matches every entry in the frozen plan. Against the
previous candidate's source manifest, only the benchmark source and runner changed;
all listed runtime sources remain identical. Both provider module SHA-256 hashes
match the prior captures exactly. Each new capture verifies matching native/wrapped
provider build fingerprints and records binary hashes. See the
[verification record](2026-11-17-file-qualification-verification.json).

After capture, an unrestricted host query identified `MacBookAir10,1`, eight CPUs.
`pmset -g therm` reported no recorded thermal warning, performance warning or CPU
power status. This does **not** measure frequency stability during the captures or
establish the absence of throttling. The native speed changes, occasional recovery
activity and process CPU drift leave multiple possible causes; no single cause
is proven. The completed data do not justify modifying production I/O or weakening
safe close/cancellation.

A valid local qualification still needs a sufficiently stable execution environment.
Do not repeat unchanged captures until one happens to pass, pool incompatible timing
protocols, discard slow trials, or raise the 5% native-CV limit. The next useful run
should reserve a host for the capture, record power/frequency and competing-load
conditions, and use a fixed CPU set where supported. The runner already supports
Linux `--cpus`; no equivalent affinity control is claimed here on macOS. Declare the
full run plan before measurement and retain every scheduled sample. A new host also
requires requalification of Live and encrypted File for that host; the previous
results cannot serve as deployment certification there.

Reproduce this campaign from a Release build containing both pinned providers:

```sh
python3 benchmarks/compare.py --build BUILD --mode file --mib 4096 --warmup-mib 512 --runs 12 --probe-calls 20000 --output core-file.json
python3 benchmarks/compare.py --build BUILD --mode file --mib 4096 --warmup-mib 512 --runs 12 --probe-calls 20000 --profile-setup --output profiles-file.json
```

The runner's successful exit means the capture completed, not that every gate
passed. Inspect `summary.*.paths.*.decision`; only `pass_measured_profile` is a pass.

## Safety and scope

Release and optimized ASan/UBSan suites each pass **15/15** checks. The added
benchmark failure test injects sender and receiver failures separately across all
six provider/path combinations and checks bounded failure cleanup. Existing
simultaneous-provider, reverse-load-order, close/cancellation, profile, telemetry,
encryption, ABI/header and export checks remain passing.

This investigation changes the measurement harness, not production payload paths,
Entry layout, polling or lifecycle protection. No speculative runtime optimization
or weakening of safe close/cancellation was used to improve a score.

These local results do not qualify continuous telemetry sampling, nondefault tuning,
WAN loss, many connections, IPv6 throughput, another OS/CPU or unimplemented provider
features. Prior Live and encrypted File results remain evidence for their original
scoped captures; no new pass is inferred for plaintext File from those results.
