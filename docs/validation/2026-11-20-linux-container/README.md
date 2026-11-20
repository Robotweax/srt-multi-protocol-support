# Linux container performance results — 20 November 2026

16 of 32 comparisons meet the qualification gates, 16 are inconclusive and none is a confirmed regression. All 16 Haivision comparisons pass. All 16 Robotweax comparisons are inconclusive because the Robotweax native baseline itself varied between runs, as described below. Overall qualification is therefore still incomplete.

Measured source: [`34ab94b5da84`](https://github.com/Robotweax/srt-multi-protocol-support/commit/34ab94b5da849854ad3b0269c19bf1f4d9dfd513), with Robotweax SRT 0.3.4 and Haivision SRT 1.5.7 plus its two compatibility patches.
Protocol: `persistent-workers-memcmp-v2`. Eight workload combinations, both providers,
native/C/C++ paths, eight paired blocks: 384 samples and 32 comparisons, all retained.
All eight captures completed on the first attempt.

These are Linux ARM64 Release measurements over IPv4 loopback, built with GCC 13.3
in an Ubuntu 24.04 Docker container. The container ran in a 16-CPU Linux 7.0
virtual machine on a macOS ARM64 desktop host. They qualify only the observed
builds and profiles and do not establish bare-metal Linux, x86-64, physical-network
or WAN performance. They are not pooled with the 17 November Linux results, which
used other hosts and Robotweax 0.3.3, or with the macOS results.

**The host was not exclusive.** The load average inside the virtual machine was
sampled every 10 seconds and is stored in each file as `capture_load`; its
median per case was 0.8 to 2.9 and its highest peak 3.6. Load on the macOS host
outside the virtual machine was not recorded.

The unchanged gates are at least eight pairs, every trial at least five seconds,
native throughput CV at most 5%, throughput lower 95% ratio at least 0.98 and
process CPU/byte upper 95% ratio at most 1.05. Intervals use
`benchmarks/compare.py`: paired geometric ratios and 10,000 bootstrap draws.
Each C/C++ result is paired with its own vendor's native baseline. A result
marked inconclusive does not establish a regression and does not meet the gate.

| Case | Provider | API | Native CV | Min seconds | Throughput low95 | CPU/byte high95 | Decision |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| live-none-core | robotweax | module | 0.087% | 10.191 | 0.99910000 | 1.20412322 | inconclusive_interval |
| live-none-core | robotweax | facade | 0.087% | 10.191 | 0.99954043 | 1.24221936 | inconclusive_interval |
| live-none-core | haivision | module | 0.256% | 10.329 | 0.99795703 | 0.99463580 | pass_measured_profile |
| live-none-core | haivision | facade | 0.256% | 10.329 | 1.00045035 | 1.00173097 | pass_measured_profile |
| live-none-profiles | robotweax | module | 0.106% | 10.179 | 0.99837491 | 1.17645830 | inconclusive_interval |
| live-none-profiles | robotweax | facade | 0.106% | 10.179 | 0.99748742 | 1.33494042 | inconclusive_interval |
| live-none-profiles | haivision | module | 0.320% | 10.311 | 0.99691644 | 0.99922903 | pass_measured_profile |
| live-none-profiles | haivision | facade | 0.320% | 10.311 | 0.99877548 | 1.00123308 | pass_measured_profile |
| live-aes128-core | robotweax | module | 0.229% | 10.277 | 0.99484592 | 1.16037989 | inconclusive_interval |
| live-aes128-core | robotweax | facade | 0.229% | 10.277 | 0.99860017 | 1.17803049 | inconclusive_interval |
| live-aes128-core | haivision | module | 0.405% | 10.400 | 0.99632797 | 0.99759104 | pass_measured_profile |
| live-aes128-core | haivision | facade | 0.405% | 10.400 | 0.99919783 | 1.00060719 | pass_measured_profile |
| live-aes128-profiles | robotweax | module | 0.683% | 10.161 | 0.99604562 | 1.05713709 | inconclusive_interval |
| live-aes128-profiles | robotweax | facade | 0.683% | 10.161 | 0.99666575 | 1.11764189 | inconclusive_interval |
| live-aes128-profiles | haivision | module | 0.380% | 10.371 | 0.99912600 | 1.01095449 | pass_measured_profile |
| live-aes128-profiles | haivision | facade | 0.380% | 10.371 | 0.99992394 | 1.01807089 | pass_measured_profile |
| file-none-core | robotweax | module | 16.935% | 6.384 | 0.84379494 | 1.01065928 | inconclusive_controls |
| file-none-core | robotweax | facade | 16.935% | 6.384 | 0.81032941 | 1.01196966 | inconclusive_controls |
| file-none-core | haivision | module | 0.980% | 8.285 | 1.00093378 | 0.99861689 | pass_measured_profile |
| file-none-core | haivision | facade | 0.980% | 8.285 | 0.99961658 | 1.00354421 | pass_measured_profile |
| file-none-profiles | robotweax | module | 14.394% | 6.473 | 0.88382649 | 1.00749031 | inconclusive_controls |
| file-none-profiles | robotweax | facade | 14.394% | 6.473 | 0.87906667 | 1.01642399 | inconclusive_controls |
| file-none-profiles | haivision | module | 0.538% | 8.246 | 0.99455188 | 1.00735050 | pass_measured_profile |
| file-none-profiles | haivision | facade | 0.538% | 8.246 | 0.99363866 | 1.01094682 | pass_measured_profile |
| file-aes128-core | robotweax | module | 10.035% | 6.561 | 0.89132015 | 1.04655463 | inconclusive_controls |
| file-aes128-core | robotweax | facade | 10.035% | 6.561 | 0.99417920 | 1.00444724 | inconclusive_controls |
| file-aes128-core | haivision | module | 0.285% | 22.332 | 0.99912521 | 1.00381050 | pass_measured_profile |
| file-aes128-core | haivision | facade | 0.285% | 22.332 | 1.00148564 | 0.99978992 | pass_measured_profile |
| file-aes128-profiles | robotweax | module | 8.943% | 6.481 | 0.85761715 | 1.07121123 | inconclusive_controls |
| file-aes128-profiles | robotweax | facade | 8.943% | 6.481 | 1.00056515 | 1.01639006 | inconclusive_controls |
| file-aes128-profiles | haivision | module | 0.339% | 22.344 | 0.99975258 | 1.00135990 | pass_measured_profile |
| file-aes128-profiles | haivision | facade | 0.339% | 22.344 | 0.99922037 | 1.00279890 | pass_measured_profile |

## Reading the results

- **Haivision passes everywhere.** Throughput point estimates are 0.998 to
  1.006 of native and process CPU per byte 0.990 to 1.005. The two Haivision
  File comparisons that were below native on macOS are not below native here.
- **Haivision application-thread CPU per byte**, recorded but not gated, is 0.5 to
  3.4% above native. On macOS at the previous source it was 11 to 24% above.
  This run cannot separate the effect of the Live readiness pre-check in this
  source from the platform difference, and File paths have no pre-check.
- **Robotweax Live** throughput is within 0.2% of native on every path, with
  lower bounds of at least 0.994. The results are inconclusive only on process
  CPU: Robotweax's own CPU time per run falls into two groups, about 3.9 s and
  about 5 to 7 s for the same 1 GiB, in the native baseline as well as through
  the abstraction. The intervals are therefore wide (for example 0.78 to 1.20).
- **Robotweax File** native throughput also falls into two groups, near
  3,858 Mbit/s and near 5,200 Mbit/s, across all three paths including native.
  This gives native CVs of 9 to 17%, above the 5% control limit, so every
  Robotweax File comparison is `inconclusive_controls`. The cause lies below
  the abstraction and was not investigated here.
- **Encrypted File completed** in both setups for both providers.

Native throughput per case, eight runs each, in Mbit/s:

| Case | Provider | Min | Median | Max | CV |
| --- | --- | ---: | ---: | ---: | ---: |
| live-none-core | robotweax | 838 | 839 | 840 | 0.09% |
| live-none-core | haivision | 823 | 828 | 829 | 0.26% |
| live-none-profiles | robotweax | 838 | 840 | 841 | 0.11% |
| live-none-profiles | haivision | 825 | 827 | 833 | 0.32% |
| live-aes128-core | robotweax | 828 | 833 | 835 | 0.23% |
| live-aes128-core | haivision | 815 | 820 | 824 | 0.40% |
| live-aes128-profiles | robotweax | 833 | 836 | 845 | 0.68% |
| live-aes128-profiles | haivision | 815 | 818 | 825 | 0.38% |
| file-none-core | robotweax | 3857 | 4565 | 5382 | 16.94% |
| file-none-core | haivision | 4009 | 4109 | 4125 | 0.98% |
| file-none-profiles | robotweax | 3858 | 4331 | 5296 | 14.39% |
| file-none-profiles | haivision | 4081 | 4104 | 4146 | 0.54% |
| file-aes128-core | robotweax | 3857 | 5144 | 5237 | 10.03% |
| file-aes128-core | haivision | 1520 | 1526 | 1532 | 0.29% |
| file-aes128-profiles | robotweax | 3922 | 5126 | 5222 | 8.94% |
| file-aes128-profiles | haivision | 1516 | 1525 | 1535 | 0.34% |

These figures compare each provider with itself across API paths. They are
loopback figures in a virtual machine and are not a ranking of the two
implementations.

Paths in the JSON files are relative to the repository root. `capture_load`,
`source_commit` and the `isolation` text were added after capture; samples and
summaries are as written by the runner. `SHA256SUMS` seals this directory.
