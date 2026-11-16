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
with one local IPv4 File stream, plaintext, accounting disabled, 32 KiB calls,
8 MiB transport buffers, and identical transport options. Every received byte is
checked against the constant test pattern and the complete byte count is required.
Connection setup, idle probes and warmup are outside the timed transfer. Timing
ends after receiver completion and sender join. SRT worker CPU is included in
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
