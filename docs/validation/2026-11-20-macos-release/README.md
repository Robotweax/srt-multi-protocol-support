# macOS Release performance results — 20 November 2026

20 of 32 comparisons meet the qualification gates, 12 are inconclusive and none is a confirmed regression. All eight captures completed on the first attempt. Overall qualification is therefore still incomplete.

Measured source: [`b8a7f2020352`](https://github.com/Robotweax/srt-multi-protocol-support/commit/b8a7f2020352c9e8121acc89dde01e9a991191bf), with Robotweax SRT 0.3.4 and Haivision SRT 1.5.7 plus its two compatibility patches.
Protocol: `persistent-workers-memcmp-v2`. Eight workload combinations, both providers,
native/C/C++ paths, eight paired blocks: 384 samples and 32 comparisons, all retained.
Earlier records describe Robotweax 0.3.3 builds or Linux hosts and are not pooled with these.

These are macOS ARM64 Release measurements over IPv4 loopback on one 16-core
desktop host. They qualify only the observed builds and profiles and do not
establish physical-network, WAN or Linux performance.

**The host was not exclusive.** A macOS media-analysis daemon and an idle Docker
virtual machine ran throughout. Each case started only below a one-minute load
average of 3.5; the load was sampled every 10 seconds and is stored in each
file as `capture_load`. A case above a peak of 9 would have been repeated; none
was. Median load per case was 3.5 to 5.5 and the highest peak was 8.3, in
`file-none-core`, whose Haivision native baseline varied by 11% and is
inconclusive for that reason.

The unchanged gates are at least eight pairs, every trial at least five seconds,
native throughput CV at most 5%, throughput lower 95% ratio at least 0.98 and
process CPU/byte upper 95% ratio at most 1.05. Intervals use
`benchmarks/compare.py`: paired geometric ratios and 10,000 bootstrap draws.
Each C/C++ result is paired with its own vendor's native baseline. A result
marked inconclusive does not establish a regression and does not meet the gate.

| Case | Provider | API | Native CV | Min seconds | Throughput low95 | CPU/byte high95 | Decision |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| live-none-core | robotweax | module | 0.512% | 10.109 | 0.99720157 | 1.09017732 | inconclusive_interval |
| live-none-core | robotweax | facade | 0.512% | 10.109 | 0.99749146 | 1.11976694 | inconclusive_interval |
| live-none-core | haivision | module | 0.670% | 10.354 | 0.99540104 | 1.02285396 | pass_measured_profile |
| live-none-core | haivision | facade | 0.670% | 10.354 | 0.99642864 | 1.05014773 | inconclusive_interval |
| live-none-profiles | robotweax | module | 0.453% | 10.113 | 0.99963490 | 1.05535712 | inconclusive_interval |
| live-none-profiles | robotweax | facade | 0.453% | 10.113 | 0.99935223 | 1.01473896 | pass_measured_profile |
| live-none-profiles | haivision | module | 0.769% | 10.328 | 0.99544444 | 1.03353621 | pass_measured_profile |
| live-none-profiles | haivision | facade | 0.769% | 10.328 | 0.99222299 | 1.03476922 | pass_measured_profile |
| live-aes128-core | robotweax | module | 0.072% | 10.133 | 0.99830472 | 1.03138468 | pass_measured_profile |
| live-aes128-core | robotweax | facade | 0.072% | 10.133 | 0.99858715 | 1.03178691 | pass_measured_profile |
| live-aes128-core | haivision | module | 0.164% | 10.328 | 0.99721081 | 1.01811003 | pass_measured_profile |
| live-aes128-core | haivision | facade | 0.164% | 10.328 | 0.99647327 | 1.01893597 | pass_measured_profile |
| live-aes128-profiles | robotweax | module | 0.165% | 10.118 | 0.99769479 | 1.01558692 | pass_measured_profile |
| live-aes128-profiles | robotweax | facade | 0.165% | 10.118 | 0.99823212 | 1.01001548 | pass_measured_profile |
| live-aes128-profiles | haivision | module | 0.184% | 10.318 | 1.00107223 | 1.01595401 | pass_measured_profile |
| live-aes128-profiles | haivision | facade | 0.184% | 10.318 | 0.99732350 | 1.02043379 | pass_measured_profile |
| file-none-core | robotweax | module | 2.206% | 14.640 | 0.99748929 | 1.00390161 | pass_measured_profile |
| file-none-core | robotweax | facade | 2.206% | 14.640 | 0.98761312 | 1.01044564 | pass_measured_profile |
| file-none-core | haivision | module | 11.127% | 16.278 | 0.97927937 | 1.02755261 | inconclusive_controls |
| file-none-core | haivision | facade | 11.127% | 16.278 | 0.98909575 | 1.01779184 | inconclusive_controls |
| file-none-profiles | robotweax | module | 4.061% | 13.831 | 0.98187028 | 1.01475787 | pass_measured_profile |
| file-none-profiles | robotweax | facade | 4.061% | 13.831 | 0.97143057 | 1.02145807 | inconclusive_interval |
| file-none-profiles | haivision | module | 4.073% | 15.384 | 0.97650500 | 1.03335875 | inconclusive_interval |
| file-none-profiles | haivision | facade | 4.073% | 15.384 | 0.95667625 | 1.05176237 | inconclusive_interval |
| file-aes128-core | robotweax | module | 2.795% | 14.489 | 0.95622704 | 1.03397287 | inconclusive_interval |
| file-aes128-core | robotweax | facade | 2.795% | 14.489 | 1.00022316 | 1.00067222 | pass_measured_profile |
| file-aes128-core | haivision | module | 0.701% | 29.420 | 0.99389383 | 1.01588992 | pass_measured_profile |
| file-aes128-core | haivision | facade | 0.701% | 29.420 | 0.98512317 | 1.01967087 | pass_measured_profile |
| file-aes128-profiles | robotweax | module | 3.363% | 14.460 | 0.97312744 | 1.03368027 | inconclusive_interval |
| file-aes128-profiles | robotweax | facade | 3.363% | 14.460 | 0.98746720 | 1.01605213 | pass_measured_profile |
| file-aes128-profiles | haivision | module | 0.657% | 29.532 | 0.97799695 | 1.02237637 | inconclusive_interval |
| file-aes128-profiles | haivision | facade | 0.657% | 29.532 | 0.98592942 | 1.02444906 | pass_measured_profile |

## Reading the results

- **No confirmed regression.** No interval lies wholly outside a budget.
- **Encrypted File completed.** Both encrypted File cases, which produced no
  qualification data in the 17 November Linux run, completed here with 96
  samples and five of eight comparisons passing.
- **Throughput point estimates** lie between 0.974 and 1.041 of native. Two
  Haivision File comparisons are below native with intervals that exclude
  parity: the facade in `file-none-profiles` (0.974, interval 0.957 to 0.994)
  and the module in `file-aes128-profiles` (0.988, interval 0.978 to 0.996).
  Both are inconclusive against the 2% budget, not passes.
- **Process CPU per byte** point estimates lie between 0.968 and 1.035.
- **Application-thread CPU per byte**, which is recorded but not gated, is
  about 11 to 24% higher than native on every Haivision path, and between 1%
  lower and 7% higher on Robotweax. Whole-process CPU does not show this, because most of the work
  is on vendor threads. The cause was not investigated here.
- The inconclusive Live results come from wide CPU intervals on a shared host,
  not from throughput: every Live throughput lower bound is at least 0.992.

Paths in the JSON files are relative to the repository root. `capture_load`,
`source_commit` and the `isolation` text were added after capture; samples and
summaries are as written by the runner. `SHA256SUMS` seals this directory.
