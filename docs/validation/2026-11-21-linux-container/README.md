# Linux container performance results — 21 November 2026

17 of 32 comparisons meet the qualification gates, 15 are inconclusive and none is a confirmed regression. All 16 Haivision comparisons pass. One of the 16 Robotweax comparisons passes and 15 are inconclusive. Overall qualification is therefore still incomplete.

This repeats the [20 November matrix](../2026-11-20-linux-container/README.md) with one change: Robotweax SRT is pinned to revision [`9b315a7`](https://github.com/Robotweax/srt/commit/9b315a721eff0b0e7186fa7f97886dbd94abdb00), which is release 0.3.4 plus the changes merged to its `main` since. Those include the packet pacer and File loss-decrease rounding fixes ([Robotweax/srt#416](https://github.com/Robotweax/srt/pull/416)) and a restructured batched UDP receive. The library still reports release 0.3.4.

Measured source: [`73a58b926fef`](https://github.com/Robotweax/srt-multi-protocol-support/commit/73a58b926fef08b4734ae8e15b6eec9b6338f1ad), with Haivision SRT 1.5.7 plus its two compatibility patches.
Protocol: `persistent-workers-memcmp-v2`. Eight workload combinations, both providers,
native/C/C++ paths, eight paired blocks: 384 samples and 32 comparisons.

**Four cases were captured twice.** During the first pass, containers of an
unrelated workload ran in the same virtual machine for about 30 minutes. The
host checked every 10 seconds for other containers; the four cases that
overlapped (`live-none-profiles`, `live-aes128-core`, `live-aes128-profiles`,
`file-none-core`) were discarded whatever their result and captured again with
no other container running. The discarded captures are not part of this record.
Each affected file carries a `recaptured` note. The other four cases are from the
first pass, during which no other container was seen.

These are Linux ARM64 Release measurements over IPv4 loopback, built with GCC 13.3
in an Ubuntu 24.04 Docker container. The container ran in a 16-CPU Linux 7.0
virtual machine on a macOS ARM64 desktop host. They qualify only the observed
builds and profiles and do not establish bare-metal Linux, x86-64, physical-network
or WAN performance. They are not pooled with earlier results.

**The host was not exclusive.** The load average inside the virtual machine was
sampled every 10 seconds and is stored in each file as `capture_load`. Load on
the macOS host outside the virtual machine was not recorded.

The unchanged gates are at least eight pairs, every trial at least five seconds,
native throughput CV at most 5%, throughput lower 95% ratio at least 0.98 and
process CPU/byte upper 95% ratio at most 1.05. Intervals use
`benchmarks/compare.py`: paired geometric ratios and 10,000 bootstrap draws.
Each C/C++ result is paired with its own vendor's native baseline. A result
marked inconclusive does not establish a regression and does not meet the gate.

| Case | Provider | API | Native CV | Min seconds | Throughput low95 | CPU/byte high95 | Decision |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| live-none-core | robotweax | module | 0.352% | 10.095 | 0.99826533 | 1.10796970 | inconclusive_interval |
| live-none-core | robotweax | facade | 0.352% | 10.095 | 0.99865504 | 1.14520943 | inconclusive_interval |
| live-none-core | haivision | module | 0.390% | 10.290 | 0.99765220 | 1.01540117 | pass_measured_profile |
| live-none-core | haivision | facade | 0.390% | 10.290 | 0.99984688 | 1.03555414 | pass_measured_profile |
| live-none-profiles | robotweax | module | 0.194% | 10.195 | 0.99914342 | 1.42049361 | inconclusive_interval |
| live-none-profiles | robotweax | facade | 0.194% | 10.195 | 0.99817486 | 1.17803573 | inconclusive_interval |
| live-none-profiles | haivision | module | 0.250% | 10.305 | 0.99524399 | 0.99377062 | pass_measured_profile |
| live-none-profiles | haivision | facade | 0.250% | 10.305 | 0.99468939 | 1.00393549 | pass_measured_profile |
| live-aes128-core | robotweax | module | 0.256% | 10.263 | 0.99699473 | 1.28494346 | inconclusive_interval |
| live-aes128-core | robotweax | facade | 0.256% | 10.263 | 0.99528303 | 1.50002361 | inconclusive_interval |
| live-aes128-core | haivision | module | 0.232% | 10.406 | 0.99666374 | 0.99753287 | pass_measured_profile |
| live-aes128-core | haivision | facade | 0.232% | 10.406 | 0.99755026 | 0.99904023 | pass_measured_profile |
| live-aes128-profiles | robotweax | module | 0.284% | 10.254 | 0.99718778 | 1.64786102 | inconclusive_interval |
| live-aes128-profiles | robotweax | facade | 0.284% | 10.254 | 0.99655669 | 1.19410820 | inconclusive_interval |
| live-aes128-profiles | haivision | module | 0.478% | 10.416 | 0.99541610 | 0.99908565 | pass_measured_profile |
| live-aes128-profiles | haivision | facade | 0.478% | 10.416 | 0.99450867 | 1.00306197 | pass_measured_profile |
| file-none-core | robotweax | module | 4.129% | 6.265 | 0.91811872 | 1.03877240 | inconclusive_interval |
| file-none-core | robotweax | facade | 4.129% | 6.265 | 0.84712769 | 1.06979447 | inconclusive_interval |
| file-none-core | haivision | module | 0.437% | 8.242 | 0.99910080 | 0.99947542 | pass_measured_profile |
| file-none-core | haivision | facade | 0.437% | 8.242 | 0.99653324 | 1.00273896 | pass_measured_profile |
| file-none-profiles | robotweax | module | 2.259% | 6.398 | 0.94118830 | 1.04546816 | inconclusive_interval |
| file-none-profiles | robotweax | facade | 2.259% | 6.398 | 0.92373862 | 1.03989852 | inconclusive_interval |
| file-none-profiles | haivision | module | 0.711% | 8.356 | 0.99842207 | 1.00321550 | pass_measured_profile |
| file-none-profiles | haivision | facade | 0.711% | 8.356 | 0.99547899 | 1.00717411 | pass_measured_profile |
| file-aes128-core | robotweax | module | 13.153% | 6.386 | 1.00416564 | 1.00454203 | inconclusive_controls |
| file-aes128-core | robotweax | facade | 13.153% | 6.386 | 1.02164792 | 0.99086431 | inconclusive_controls |
| file-aes128-core | haivision | module | 0.352% | 22.346 | 0.99718493 | 1.00378170 | pass_measured_profile |
| file-aes128-core | haivision | facade | 0.352% | 22.346 | 0.99828699 | 1.00328781 | pass_measured_profile |
| file-aes128-profiles | robotweax | module | 3.750% | 6.379 | 0.99340544 | 1.01402151 | pass_measured_profile |
| file-aes128-profiles | robotweax | facade | 3.750% | 6.379 | 0.92713153 | 1.07249744 | inconclusive_interval |
| file-aes128-profiles | haivision | module | 0.642% | 22.372 | 0.99843845 | 1.00170764 | pass_measured_profile |
| file-aes128-profiles | haivision | facade | 0.642% | 22.372 | 0.99630760 | 1.00338262 | pass_measured_profile |

## Reading the results

- **Haivision passes everywhere**, as on 20 November. Throughput lower bounds are
  at least 0.994 and process CPU upper bounds at most 1.036.
- **Robotweax File native throughput is steadier.** On 20 November every File
  case had a native CV of 9 to 17%, with runs near 3,858 and near 5,200 Mbit/s.
  Here three of the four cases have a native CV of 2.3 to 4.1%, inside the 5%
  control limit, and no run below 4,750 Mbit/s. `file-aes128-core` still has
  three of eight native runs below 4,500 Mbit/s and a CV of 13.2%, so its two
  comparisons remain `inconclusive_controls`.
- **Robotweax File through the abstraction is not yet qualified.** In the three
  cases with a controlled baseline, one comparison passes and five are
  inconclusive: throughput point estimates are 0.92 to 1.01 of native with
  lower bounds of 0.85 to 0.99. Run-to-run variation of 2 to 4% in the native baseline
  is too large for eight pairs to establish a lower bound of 0.98.
- **Robotweax Live is unchanged.** Throughput is within 0.2% of native on every
  path with lower bounds of at least 0.995. All eight comparisons are
  inconclusive on process CPU only: Robotweax's own CPU time for the same
  1 GiB ranges from 3.9 to 7.7 s in the native baseline, so the intervals are
  wide (upper bounds 1.11 to 1.65). The rounding fixes do not address this and
  its cause has not been established.

Native throughput per case, eight runs each, in Mbit/s:

| Case | Provider | Min | Median | Max | CV |
| --- | --- | ---: | ---: | ---: | ---: |
| live-none-core | robotweax | 843 | 850 | 851 | 0.35% |
| live-none-core | haivision | 823 | 828 | 832 | 0.39% |
| live-none-profiles | robotweax | 838 | 841 | 843 | 0.19% |
| live-none-profiles | haivision | 826 | 830 | 832 | 0.25% |
| live-aes128-core | robotweax | 830 | 835 | 837 | 0.26% |
| live-aes128-core | haivision | 819 | 821 | 824 | 0.23% |
| live-aes128-profiles | robotweax | 830 | 835 | 838 | 0.28% |
| live-aes128-profiles | haivision | 814 | 821 | 825 | 0.48% |
| file-none-core | robotweax | 4769 | 5193 | 5485 | 4.13% |
| file-none-core | haivision | 4046 | 4075 | 4102 | 0.44% |
| file-none-profiles | robotweax | 5013 | 5129 | 5363 | 2.26% |
| file-none-profiles | haivision | 4022 | 4051 | 4102 | 0.71% |
| file-aes128-core | robotweax | 3844 | 5140 | 5379 | 13.15% |
| file-aes128-core | haivision | 1518 | 1528 | 1533 | 0.35% |
| file-aes128-profiles | robotweax | 4762 | 5275 | 5358 | 3.75% |
| file-aes128-profiles | haivision | 1505 | 1526 | 1535 | 0.64% |

These figures compare each provider with itself across API paths. They are
loopback figures in a virtual machine and are not a ranking of the two
implementations.

Paths in the JSON files are relative to the repository root. `capture_load`,
`source_commit`, `recaptured` and the `isolation` text were added after capture;
samples and summaries are as written by the runner. `SHA256SUMS` seals this directory.
