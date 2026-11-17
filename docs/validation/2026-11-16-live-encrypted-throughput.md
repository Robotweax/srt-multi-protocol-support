# Controlled local Live and encrypted File throughput

Scope: local IPv4 loopback on macOS arm64, AppleClang 21 Release, OpenSSL 3,
Robotweax SRT 0.3.3 and Haivision SRT 1.5.7 with the existing KM/close patch and
private native poller wake bridge. This extends the earlier
[plaintext File qualification](2026-11-16-controlled-throughput.md).

## Workloads and controls

Each profile contains eight paired blocks, with both providers and direct-native,
C ABI module and C++ facade paths in every block. Trials run sequentially in
fresh processes. No task-owned builds, tests or profilers overlap capture. The
host is shared; CPU affinity, exclusive cores and thermal control are not claimed.

| Profile | Measured payload | Warmup | Call size | Latency | Encryption |
| --- | ---: | ---: | ---: | ---: | --- |
| Live plaintext | 1 GiB rounded down to whole messages | 64 MiB rounded likewise | 1,316 bytes | 120 ms receive/peer | Disabled |
| Live encrypted | Same | Same | 1,316 bytes | 120 ms receive/peer | AES-128-CTR required |
| File encrypted | 2 GiB | 256 MiB | 32 KiB | Native File mode | AES-128-CTR required |

All paths use 8 MiB SRT buffers, 1,500-byte MSS, disabled application accounting,
nonblocking I/O and real polling after WouldBlock. Direct uses native `uwait`;
wrapped paths retain their full provider pollers. The sender has no application
rate cap. Native pacing and late-packet-drop defaults remain unchanged, so these
results describe this profile rather than unlimited packet-processing capacity.
Setup, 20,000 idle receive probes, warmup and encryption readback are outside the
measured interval. CPU includes the whole process and native worker threads.

Live uses real message send/receive calls. Every message has an eight-byte sequence
number followed by the test pattern. Sequence continuity, message boundaries,
remaining payload bytes and complete delivery are required. Dropped, duplicated,
reordered or truncated messages fail the trial rather than inflate goodput.
File verifies every received byte and requires the complete byte count. Encrypted
profiles check both endpoints’ negotiated key state and 128-bit key size before
and after measurement. The passphrase is a public benchmark fixture.

Each wrapped path is compared with its own provider’s direct baseline using the
same mode, encryption and vendor static target. The gates measure layer overhead,
not the cost of encryption relative to plaintext. The unchanged
[analysis protocol](../../benchmarks/README.md) requires a lower 95% throughput
ratio bound of at least 0.98, upper 95% process-CPU/byte ratio bound of at most
1.05, at least eight pairs, all trials at least five seconds and native goodput
CV at most 5%. Ratios are paired geometric means; intervals use 10,000 bootstrap
resamples. No observations are excluded. CPU seconds are not hardware cycles.

## Results

All twelve wrapped-path comparisons pass the unchanged 2% throughput / 5%
process-CPU gates. All three fixed eight-block matrices are complete: 144 measured transfers,
approximately 192 GiB of verified payload plus warmup. No observations were
excluded or replaced. Each row uses its own provider’s paired direct baseline
with matching mode/encryption. Brackets show per-comparison 95% intervals.

- [Plaintext Live, eight blocks](2026-11-16-live-plain.json).
- [AES-128-CTR Live, eight blocks](2026-11-16-live-aes128.json).
- [AES-128-CTR File, eight blocks](2026-11-16-file-aes128.json).

| Profile | Provider/path | Goodput change [95% interval] | Process CPU/byte change [95% interval] | Gate decision |
| --- | --- | ---: | ---: | --- |
| Live plaintext | Robotweax C ABI | -0.89% [-1.61%, -0.00%] | -0.05% [-2.13%, +1.80%] | Pass |
| Live plaintext | Robotweax C++ | -0.42% [-1.40%, +0.43%] | +1.28% [-0.09%, +2.36%] | Pass |
| Live plaintext | Haivision C ABI | +0.27% [-0.56%, +1.10%] | +1.09% [+0.58%, +1.42%] | Pass |
| Live plaintext | Haivision C++ | +0.10% [-0.51%, +0.77%] | +1.32% [+0.33%, +2.22%] | Pass |
| Live AES-128-CTR | Robotweax C ABI | +0.15% [-0.50%, +0.86%] | +1.91% [+1.32%, +2.50%] | Pass |
| Live AES-128-CTR | Robotweax C++ | -0.09% [-0.79%, +0.65%] | +1.70% [+1.08%, +2.28%] | Pass |
| Live AES-128-CTR | Haivision C ABI | +1.04% [+0.32%, +1.86%] | +0.44% [+0.12%, +0.74%] | Pass |
| Live AES-128-CTR | Haivision C++ | +0.49% [-0.33%, +1.36%] | +0.85% [+0.37%, +1.35%] | Pass |
| File AES-128-CTR | Robotweax C ABI | +3.39% [+2.54%, +4.17%] | -13.16% [-13.44%, -12.91%] | Pass |
| File AES-128-CTR | Robotweax C++ | +3.01% [+1.27%, +4.36%] | -12.76% [-13.40%, -11.92%] | Pass |
| File AES-128-CTR | Haivision C ABI | -0.08% [-0.33%, +0.20%] | +1.23% [+1.07%, +1.45%] | Pass |
| File AES-128-CTR | Haivision C++ | -0.61% [-1.12%, -0.05%] | +1.36% [+1.00%, +1.73%] | Pass |

The reports retain exact payload/message counts, whole-process and application
thread CPU, call/WouldBlock counts, idle receive probes, binary/module hashes and
provider fingerprints. CPU includes identical sequence/payload verification on
all paths; these are application-level goodput and CPU/byte measurements, not an
isolated instruction count for dispatch. A passing gate does not imply zero
adapter overhead. Apparent speedups are specific to this workload and scheduling.

The controls were applied independently to each profile/provider:

| Profile | Provider | Direct goodput CV | Trial duration range | Median direct / C ABI / C++ Mbps |
| --- | --- | ---: | ---: | ---: |
| Live plaintext | Robotweax | 0.58% | 10.14–10.63 s | 838.1 / 832.3 / 837.9 |
| Live plaintext | Haivision | 0.71% | 10.43–10.79 s | 807.2 / 809.1 / 806.9 |
| Live AES-128-CTR | Robotweax | 0.71% | 10.32–10.53 s | 831.4 / 831.4 / 828.3 |
| Live AES-128-CTR | Haivision | 1.11% | 10.46–10.86 s | 811.0 / 816.9 / 810.7 |
| File AES-128-CTR | Robotweax | 1.18% | 8.77–9.34 s | 1876.7 / 1935.7 / 1944.1 |
| File AES-128-CTR | Haivision | 0.75% | 17.60–18.01 s | 969.1 / 968.3 / 960.6 |

The harness and functional smoke tests are committed as `b4a56e7`. Provider,
facade, polling and lifecycle code are unchanged from the earlier qualified
milestone. No runtime optimization was applied during these captures. The same
Release artifacts were used for all three matrices, checked by their hashes.

Reproduce each capture using the three commands in the
[benchmark profile instructions](../../benchmarks/README.md#live-and-encrypted-file-profiles).
The short 16 MiB smoke runs and 256 MiB duration pilot are diagnostics, not gate
inputs. No statistical replication was selected after observing these results.

## Safety and validation

This milestone changes the benchmark and its smoke tests only. Provider
close gates, generation checks, sticky cancellation, native wake implementation
and crypto-close lifetime fix are unchanged. Both providers still run together in
the existing correctness matrix, including simultaneous encrypted transfers and
cross-provider connections. Performance trials measure one provider connection at
a time; they do not qualify concurrent mixed-provider throughput.

All 12 local Release tests and all 12 optimized ASan/UBSan tests pass, including
three new profile smoke tests across both providers and all API paths. Short
smoke tests intentionally cannot satisfy performance qualification controls.

All six [CI jobs](https://github.com/Robotweax/srt-multi-protocol-support/actions/runs/43948858992)
pass on the latest attempt. The first Linux sanitizer attempt failed the existing
`real_providers_normal` test during Haivision-to-Haivision encrypted Live/IPv6
connection setup, reporting “Caller setup failed,” without an ASan/UBSan memory
error. The reversed-order test and both lifecycle tests passed in that attempt.
The failed job passed when rerun without code changes. This isolated setup failure
is not root-caused or claimed fixed; it is retained here rather than hidden by
the rerun. The requested throughput captures are IPv4.

Scope excludes IPv6 performance, WAN/loss scenarios, smaller Live messages,
specified real-time source rates, other AES key sizes, forced rekeying, full
duplex, many connections and other hosts. No broader deployment qualification is
claimed from these local measurements.
