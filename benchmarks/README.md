# Controlled throughput comparisons

Build Release with `SRTMUX_BUILD_PROVIDERS=ON` and
`SRTMUX_BUILD_BENCHMARKS=ON` (see [provider build instructions](../providers/README.md)).
Run without other builds, tests or profilers sharing the machine:

```sh
python3 benchmarks/compare.py --build build-real --mib 2048 --runs 8 \
  --warmup-mib 256 --probe-calls 20000 --wait poll --output results.json
```

The runner always includes Robotweax 0.3.3 and Haivision 1.5.7, each using direct
native calls, the module C ABI, and the C++ facade. Each trial is a fresh process
with one local IPv4 connection, accounting disabled, 8 MiB transport buffers, and
identical transport options. The default profile is a plaintext File stream with
32 KiB calls. `--mode live` uses the message API with 1,316-byte messages and
120 ms receive/peer latency. `--encryption aes-128-ctr` enables required AES-128-CTR
in either mode with a public benchmark-only passphrase. Both endpoints’ negotiated
key state and size are checked before and after the measured transfer.

Every received byte is checked and complete delivery is required. Live messages
carry an eight-byte sequence number followed by the test pattern; missing,
duplicated, reordered or truncated messages fail the trial. Live byte counts are
rounded down to whole messages, and goodput uses the exact delivered count.
Native pacing and late-packet-drop defaults are retained; the application adds no
rate cap. The sender retries only after native WouldBlock. A Live failure cannot
be turned into a passing sample by accepting partial delivery.
Connection setup, idle probes and warmup are outside the timed transfer. The
`persistent-workers-barriers-v1` protocol reuses one sender thread and both payload
buffers across warmup and measurement. A two-party barrier starts timing only
when sender and receiver are ready; another stops it after both complete. Thread
creation/join and buffer allocation are outside timing. Earlier reports used a
fresh sender per phase and included its creation/join; do not pool those protocols. SRT worker CPU is included in
whole-process CPU; application-thread CPU is reported separately.

`--wait poll` waits only after WouldBlock. The direct baseline uses native
`srt_epoll_uwait`; module and facade use their real provider pollers, including
cancellation support. `--direct-poll-api legacy` reproduces the older direct
`srt_epoll_wait` baseline. It affects only the direct path. Comparing a before
matrix using legacy with an after matrix using uwait is a comparison against two
different native baselines; it is not a matched before/after causal estimate.
`--wait yield` creates no pollers and yields after WouldBlock, providing a separate
diagnostic for dispatch/I/O overhead. It is not the recommended application wait
strategy. `--probe-calls` measures connected idle receives using calling-thread CPU,
without payload or retry scheduling; it does not predict loaded I/O cost.

## Analysis and controls

A block contains all three paths for each provider. Path orders cycle through
shuffled permutations; provider order is shuffled per block, with a reproducible
seed. Ratios pair each wrapped path with its own provider’s direct trial in that
block. Reports contain geometric mean ratios and percentile 95% bootstrap
intervals from 10,000 resamples of paired log ratios. No trials are discarded.
Eight blocks are a minimum, not a guarantee of sufficiently narrow intervals.

The measured profile passes only when:

- At least eight paired trials are available and every trial lasts at least five seconds.
- Direct goodput’s sample coefficient of variation is at most 5%.
- The throughput ratio’s lower 95% bound is at least 0.98.
- The whole-process CPU/byte ratio’s upper 95% bound is at most 1.05.

A confidence interval entirely outside a budget is a regression. Overlapping
bounds or inadequate controls are inconclusive. These are engineering gates for
this measured workload, not a guarantee for other deployments. CPU seconds are
not hardware cycles. Intervals describe observed trial variation; shared-host
interference, thermal drift and systematic bias remain possible. Linux `--cpus`
sets inherited affinity for benchmark and vendor threads; it does not reserve
those CPUs. Other platforms do not emulate affinity.

JSON checkpoints preserve raw samples after every trial and retain partial data
on failure. Reports record configuration, host information, binary/module SHA-256
hashes and provider fingerprints. Direct and wrapped vendor fingerprints must
match. A completed runner exits successfully even if the statistical decision is
regression or inconclusive: inspect `summary` and `status`, not just its exit code.
Small smoke runs intentionally cannot qualify performance. Simultaneous-provider,
close, cancellation, encryption and sanitizer tests remain separate correctness
checks so their load cannot contaminate the throughput measurements.

## Live and encrypted File profiles

```sh
python3 benchmarks/compare.py --build build-real --mode live --mib 1024 \
  --warmup-mib 64 --probe-calls 20000 --runs 8 --output live.json
python3 benchmarks/compare.py --build build-real --mode live --encryption aes-128-ctr \
  --mib 1024 --warmup-mib 64 --probe-calls 20000 --runs 8 --output live-aes128.json
python3 benchmarks/compare.py --build build-real --mode file --encryption aes-128-ctr \
  --mib 2048 --warmup-mib 256 --probe-calls 20000 --runs 8 --output file-aes128.json
```

Each command keeps both providers and all three paths in the matrix; compare paths
within a provider, not one provider’s speed against the other’s. Profiles are
analyzed separately using the same 2%/5% budgets and minimum-duration/stability
controls. These settings do not represent WAN loss, encryption key-size variants,
forced key rotation, many connections or a specified real-time source rate.
The three `benchmark_*` profile tests in CTest are short functional smoke checks,
not performance qualification.

## Versioned profile setup

Pass `--profile-setup` to construct module and facade endpoints through the
immutable profile extension. The direct path keeps the identical native Live/File
configuration. Setup is outside timing, profiles are released before transfer,
and ordinary send/receive/poll operations remain unchanged. Samples record the
actual setup path, and the runner rejects mismatches. Use this for regression
qualification of the extension-created objects; omit it to measure original setup.
For example:

```sh
python3 benchmarks/compare.py --build build-real --profile-setup --mode live \
  --mib 1024 --warmup-mib 64 --probe-calls 20000 --runs 8 --output profiles-live.json
```

No implicit telemetry is sampled in these trials. Applications that continuously
sample native statistics should separately qualify their chosen polling cadence.
The same 2% throughput and 5% process CPU/byte limits and statistical controls apply.


## Boundary diagnostics and failure checks

Native sent-loss, receiver-loss and retransmission totals are sampled without
clearing, immediately before and after the measured transfer. These boundary
deltas help distinguish recovery activity from adapter CPU overhead; asynchronous
native work can straddle the boundary. Native calls, C telemetry and C++ telemetry
read the same vendor counters. No metric lookup or snapshot runs in a payload loop
or inside the timed region. On POSIX, resource snapshots outside timing also record
OS-reported context-switch and major-page-fault deltas; Windows marks these
unavailable. Counts alone do not establish a scheduling or thermal root cause.

`benchmark_worker_failure` injects a sender or receiver failure before its loop on
all six native/module/facade/provider combinations. Every process must report a
controlled failure and join its worker within the deadline. Qualification clears
the injection setting explicitly. Ordinary provider close and cancellation code
is unchanged by these benchmark controls.
