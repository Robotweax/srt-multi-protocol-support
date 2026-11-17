# Linux Release performance results — 17 November 2026

20 of 32 comparisons meet the qualification gates; overall qualification is incomplete. 2 case(s) have incomplete captures and remain failed.

Measured source: [`2e185a983e9c`](https://github.com/Robotweax/srt-multi-protocol-support/commit/2e185a983e9c36b8ae478831f121df939c629a37).
Protocol: `persistent-workers-memcmp-v2`. Eight workload combinations, both providers,
native/C/C++ paths, eight paired blocks: 384 scheduled samples and 32 comparisons.
6 complete captures provide 288 validated published samples.
Every complete-capture sample and every case outcome is retained. Incomplete
captures remain failed and supply no qualification samples. Earlier timing-protocol
data are not pooled.

These are Linux Release IPv4-loopback measurements in virtual machines. They
qualify only the observed source builds and profiles; they do not establish
physical-NIC or WAN performance. Observed architecture/logical-CPU-count combinations:
`x86_64` / 2, `x86_64` / 4.
Each JSON file records CPU affinity, public artifact digests, source fingerprints,
all numeric samples and its complete acceptance summary.
The Haivision build uses the pinned project's `USE_ENCLIB=openssl` selection;
native and wrapped paths share each provider's build fingerprint.

For an incomplete case, `failed_capture` marks unavailable qualification data
for the whole case. It does not assert a failure in every listed provider/API.
No ratios are inferred for these entries.

The unchanged gates are at least eight pairs, every trial at least five seconds,
native throughput CV at most 5%, throughput lower 95% ratio at least 0.98 and
process CPU/byte upper 95% ratio at most 1.05. Intervals use the existing
[public comparison script](https://github.com/Robotweax/srt-multi-protocol-support/blob/2e185a983e9c36b8ae478831f121df939c629a37/benchmarks/compare.py):
paired geometric ratios and 10,000 bootstrap draws. Each C/C++ result is paired
with its own vendor's native baseline. A result marked inconclusive does not
establish a regression and does not meet the qualification gate.

| Case | Provider | API | Native CV | Min seconds | Throughput low95 | CPU/byte high95 | Decision |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| file-none-core | robotweax | module | 3.073% | 12.697 | 0.99793844 | 1.01043444 | pass_measured_profile |
| file-none-core | robotweax | facade | 3.073% | 13.346 | 0.98550781 | 1.01475471 | pass_measured_profile |
| file-none-core | haivision | module | 1.819% | 17.589 | 0.98428400 | 1.02276968 | pass_measured_profile |
| file-none-core | haivision | facade | 1.819% | 17.589 | 0.97924785 | 1.02709418 | inconclusive_interval |
| file-none-profiles | robotweax | module | 5.686% | 12.614 | 0.91957169 | 1.05810690 | inconclusive_controls |
| file-none-profiles | robotweax | facade | 5.686% | 12.614 | 0.94797030 | 1.04442790 | inconclusive_controls |
| file-none-profiles | haivision | module | 1.339% | 15.774 | 0.98934530 | 1.01754900 | pass_measured_profile |
| file-none-profiles | haivision | facade | 1.339% | 15.772 | 0.98764990 | 1.01642416 | pass_measured_profile |
| file-aes128-core | robotweax | module | — | — | — | — | failed_capture |
| file-aes128-core | robotweax | facade | — | — | — | — | failed_capture |
| file-aes128-core | haivision | module | — | — | — | — | failed_capture |
| file-aes128-core | haivision | facade | — | — | — | — | failed_capture |
| file-aes128-profiles | robotweax | module | — | — | — | — | failed_capture |
| file-aes128-profiles | robotweax | facade | — | — | — | — | failed_capture |
| file-aes128-profiles | haivision | module | — | — | — | — | failed_capture |
| file-aes128-profiles | haivision | facade | — | — | — | — | failed_capture |
| live-none-core | robotweax | module | 0.133% | 10.242 | 0.99973434 | 1.03236207 | pass_measured_profile |
| live-none-core | robotweax | facade | 0.133% | 10.254 | 0.99951516 | 1.03182689 | pass_measured_profile |
| live-none-core | haivision | module | 0.194% | 9.783 | 0.99715133 | 1.01545909 | pass_measured_profile |
| live-none-core | haivision | facade | 0.194% | 9.783 | 0.99669373 | 1.01717701 | pass_measured_profile |
| live-none-profiles | robotweax | module | 0.118% | 10.240 | 0.99917605 | 1.03285440 | pass_measured_profile |
| live-none-profiles | robotweax | facade | 0.118% | 10.240 | 0.99970073 | 1.04551134 | pass_measured_profile |
| live-none-profiles | haivision | module | 0.157% | 9.783 | 0.99907297 | 1.02371822 | pass_measured_profile |
| live-none-profiles | haivision | facade | 0.157% | 9.782 | 0.99867876 | 1.02795850 | pass_measured_profile |
| live-aes128-core | robotweax | module | 0.190% | 10.336 | 0.99764995 | 1.03829023 | pass_measured_profile |
| live-aes128-core | robotweax | facade | 0.190% | 10.354 | 0.99867690 | 1.05075661 | inconclusive_interval |
| live-aes128-core | haivision | module | 0.610% | 22.204 | 0.99585721 | 1.00364210 | pass_measured_profile |
| live-aes128-core | haivision | facade | 0.610% | 22.107 | 0.98986988 | 1.00871429 | pass_measured_profile |
| live-aes128-profiles | robotweax | module | 0.105% | 10.383 | 0.99907319 | 1.03353491 | pass_measured_profile |
| live-aes128-profiles | robotweax | facade | 0.105% | 10.383 | 0.99880154 | 1.03029985 | pass_measured_profile |
| live-aes128-profiles | haivision | module | 1.805% | 21.786 | 0.98858800 | 1.00997314 | pass_measured_profile |
| live-aes128-profiles | haivision | facade | 1.805% | 21.786 | 0.99144726 | 1.01160942 | pass_measured_profile |

The per-case JSON files contain the full sanitized numerical results. `summary.json`
contains the combined matrix; `SHA256SUMS` seals this results directory.
