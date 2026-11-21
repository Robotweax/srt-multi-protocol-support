# Linux container performance results — 21 November 2026, second matrix

24 of 32 comparisons meet the qualification gates, 8 are inconclusive and none is a confirmed regression. All eight Robotweax Live comparisons now pass. Robotweax File passes two of eight and Haivision 14 of 16. Overall qualification is therefore still incomplete.

This repeats the [first 21 November matrix](../2026-11-21-linux-container/README.md) with one change: Robotweax SRT is pinned to revision [`de864c4`](https://github.com/Robotweax/srt/commit/de864c4d2397e8ca06dc1c6391bc218bf3abb223), which adds the shorter coarse-timer fallback ([Robotweax/srt#418](https://github.com/Robotweax/srt/pull/418)) to the previously pinned `9b315a7`. The library still reports release 0.3.4. Haivision SRT and the abstraction layer's own code are the same as in the first matrix.

Measured source: [`ed1e8ff02d03`](https://github.com/Robotweax/srt-multi-protocol-support/commit/ed1e8ff02d03bcd7f2875e9b8abf1dfdd5ef368c), with Haivision SRT 1.5.7 plus its two compatibility patches.
Protocol: `persistent-workers-memcmp-v2`. Eight workload combinations, both providers,
native/C/C++ paths, eight paired blocks: 384 samples and 32 comparisons, all retained.

Each case ran on its own while the host checked every 10 seconds for other
containers in the virtual machine, and would have been repeated had one
appeared. None appeared, so all eight captures are from the first attempt.

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
| live-none-core | robotweax | module | 0.181% | 10.086 | 0.99983274 | 1.00674362 | pass_measured_profile |
| live-none-core | robotweax | facade | 0.181% | 10.086 | 0.99854843 | 1.01223449 | pass_measured_profile |
| live-none-core | haivision | module | 0.515% | 10.303 | 0.99567553 | 1.00402007 | pass_measured_profile |
| live-none-core | haivision | facade | 0.515% | 10.303 | 0.99621369 | 0.99875010 | pass_measured_profile |
| live-none-profiles | robotweax | module | 0.155% | 10.087 | 0.99755707 | 0.99890548 | pass_measured_profile |
| live-none-profiles | robotweax | facade | 0.155% | 10.087 | 0.99700057 | 1.00169295 | pass_measured_profile |
| live-none-profiles | haivision | module | 0.246% | 10.242 | 0.99972249 | 1.01615295 | pass_measured_profile |
| live-none-profiles | haivision | facade | 0.246% | 10.242 | 0.99941877 | 1.02422719 | pass_measured_profile |
| live-aes128-core | robotweax | module | 0.231% | 10.138 | 0.99478885 | 1.02301440 | pass_measured_profile |
| live-aes128-core | robotweax | facade | 0.231% | 10.138 | 0.99616199 | 1.04552845 | pass_measured_profile |
| live-aes128-core | haivision | module | 0.443% | 10.340 | 0.99805344 | 1.00419274 | pass_measured_profile |
| live-aes128-core | haivision | facade | 0.443% | 10.340 | 0.99753034 | 1.01512411 | pass_measured_profile |
| live-aes128-profiles | robotweax | module | 0.132% | 10.147 | 0.99442761 | 0.99772940 | pass_measured_profile |
| live-aes128-profiles | robotweax | facade | 0.132% | 10.147 | 0.99561794 | 1.01077568 | pass_measured_profile |
| live-aes128-profiles | haivision | module | 0.372% | 10.373 | 0.96701103 | 1.04274532 | inconclusive_interval |
| live-aes128-profiles | haivision | facade | 0.372% | 10.373 | 0.99647092 | 1.01216108 | pass_measured_profile |
| file-none-core | robotweax | module | 3.153% | 6.202 | 0.89036601 | 1.05014045 | inconclusive_interval |
| file-none-core | robotweax | facade | 3.153% | 6.202 | 0.92092333 | 1.04919249 | inconclusive_interval |
| file-none-core | haivision | module | 1.425% | 8.250 | 0.98114764 | 1.01781551 | pass_measured_profile |
| file-none-core | haivision | facade | 1.425% | 8.250 | 0.99082566 | 1.01028596 | pass_measured_profile |
| file-none-profiles | robotweax | module | 6.468% | 6.268 | 0.93600738 | 1.04651974 | inconclusive_controls |
| file-none-profiles | robotweax | facade | 6.468% | 6.268 | 0.91350187 | 1.03903988 | inconclusive_controls |
| file-none-profiles | haivision | module | 2.739% | 8.191 | 0.99719996 | 1.00245346 | pass_measured_profile |
| file-none-profiles | haivision | facade | 2.739% | 8.191 | 0.94436029 | 1.03807378 | inconclusive_interval |
| file-aes128-core | robotweax | module | 0.874% | 6.332 | 0.86612019 | 1.05659057 | inconclusive_interval |
| file-aes128-core | robotweax | facade | 0.874% | 6.332 | 0.91299370 | 1.06688388 | inconclusive_interval |
| file-aes128-core | haivision | module | 0.686% | 22.411 | 0.99635549 | 1.00236976 | pass_measured_profile |
| file-aes128-core | haivision | facade | 0.686% | 22.411 | 0.99774332 | 1.00404768 | pass_measured_profile |
| file-aes128-profiles | robotweax | module | 2.616% | 6.310 | 0.99700652 | 1.00871438 | pass_measured_profile |
| file-aes128-profiles | robotweax | facade | 2.616% | 6.310 | 0.99476588 | 0.99985706 | pass_measured_profile |
| file-aes128-profiles | haivision | module | 0.436% | 22.443 | 0.99390287 | 1.00833199 | pass_measured_profile |
| file-aes128-profiles | haivision | facade | 0.436% | 22.443 | 0.99462176 | 1.00560584 | pass_measured_profile |

## Reading the results

- **Robotweax Live passes on every path.** In the first matrix all eight
  comparisons were inconclusive on process CPU, because Robotweax's own CPU
  time for the same 1 GiB ranged from 3.9 to 7.7 s in the native baseline.
  Here it ranges from 3.8 to 4.8 s, and from 3.8 to 4.9 s through the
  abstraction. The CPU upper bounds are 0.998 to 1.046, against 1.11 to 1.65
  before. Throughput lower bounds are at least 0.994.
- **Robotweax File through the abstraction is still not qualified.** Both
  `file-aes128-profiles` comparisons pass. The other six are inconclusive, with
  throughput point estimates of 0.95 to 0.98 of native and lower bounds of 0.87
  to 0.94. In these cases one or two of the eight runs on an abstraction path
  were 10 to 30% slower than the rest. Eight pairs do not show whether this is a
  shortfall of the abstraction or the same run-to-run variation the native
  baseline shows in other cases.
- **Robotweax File native variation moved between cases.** `file-aes128-core`,
  at a CV of 13.2% in the first matrix, is at 0.9% here. `file-none-profiles`,
  at 2.3% before, is at 6.5% here with one native run at 4,463 Mbit/s, so its
  two comparisons are `inconclusive_controls`. The coarse-timer change does not
  target File mode, and this record does not attribute either movement to it.
- **Haivision has two inconclusive comparisons** where the first matrix had
  none. Each is caused by a single slow run: 759 Mbit/s against 818 to 828 in
  `live-aes128-profiles` (`module` path), and 3,527 Mbit/s against 3,890 to 4,166 in
  `file-none-profiles` (`facade` path). The other 14 pass.

Native throughput per case, eight runs each, in Mbit/s:

| Case | Provider | Min | Median | Max | CV |
| --- | --- | ---: | ---: | ---: | ---: |
| live-none-core | robotweax | 846 | 850 | 850 | 0.18% |
| live-none-core | haivision | 821 | 830 | 834 | 0.52% |
| live-none-profiles | robotweax | 848 | 850 | 852 | 0.16% |
| live-none-profiles | haivision | 825 | 829 | 832 | 0.25% |
| live-aes128-core | robotweax | 842 | 845 | 847 | 0.23% |
| live-aes128-core | haivision | 820 | 821 | 831 | 0.44% |
| live-aes128-profiles | robotweax | 843 | 845 | 846 | 0.13% |
| live-aes128-profiles | haivision | 817 | 825 | 828 | 0.37% |
| file-none-core | robotweax | 4997 | 5432 | 5526 | 3.15% |
| file-none-core | haivision | 3967 | 4069 | 4121 | 1.43% |
| file-none-profiles | robotweax | 4463 | 5323 | 5482 | 6.47% |
| file-none-profiles | haivision | 3816 | 4035 | 4134 | 2.74% |
| file-aes128-core | robotweax | 5260 | 5325 | 5397 | 0.87% |
| file-aes128-core | haivision | 1502 | 1522 | 1533 | 0.69% |
| file-aes128-profiles | robotweax | 5020 | 5306 | 5445 | 2.62% |
| file-aes128-profiles | haivision | 1509 | 1522 | 1531 | 0.44% |

These figures compare each provider with itself across API paths. They are
loopback figures in a virtual machine and are not a ranking of the two
implementations.

Paths in the JSON files are relative to the repository root. `capture_load`,
`source_commit` and the `isolation` text were added after capture; samples and
summaries are as written by the runner. `SHA256SUMS` seals this directory.
