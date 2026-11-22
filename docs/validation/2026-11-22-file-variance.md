# Robotweax File throughput variation — 21 and 22 November 2026

The [second Linux container matrix of 21 November](2026-11-21-linux-container-coarse-timer/README.md)
left six Robotweax File comparisons inconclusive, with throughput point
estimates of 0.95 to 0.98 of native. This note records what a follow-up found:
the abstraction is not measurably slower, and the variation comes from Robotweax
SRT's File congestion control on this host.

All measurements are Linux ARM64 Release builds over IPv4 loopback in the same
Docker container and virtual machine as the matrix, with Robotweax SRT
[`de864c4`](https://github.com/Robotweax/srt/commit/de864c4d2397e8ca06dc1c6391bc218bf3abb223)
and no other container running. They describe this host and are not
qualification data.

## 32 pairs show no shortfall of the abstraction

`file-none-core` was repeated with 32 paired blocks instead of eight
([data](2026-11-22-file-variance-32-pairs.json), 192 samples, all retained).

| Provider | Path | Throughput vs native | 95% interval | Runs with a frequently blocked sender |
| --- | --- | ---: | ---: | ---: |
| robotweax | native | – | – | 9 of 32 |
| robotweax | module | 0.993 | 0.955 to 1.037 | 11 of 32 |
| robotweax | facade | 0.998 | 0.968 to 1.036 | 9 of 32 |
| haivision | module | 0.999 | 0.997 to 1.002 | – |
| haivision | facade | 0.998 | 0.996 to 1.000 | – |

A Robotweax run counts as frequently blocked when the sender met a full send
buffer more than 3,000 times (`tx_would_block`); most other runs stay below 1,000.
These runs are the slow ones, and they occur on the native path as often as
through the abstraction. In the two eight-pair matrices of 21 November they
happened to fall on the abstraction paths more often (15 and 13 of 32 plaintext
runs against 5 native), which produced the low point estimates there.

The Robotweax native CV over 32 runs is 7.6%, above the 5% control limit, so
both Robotweax comparisons remain `inconclusive_controls`. More pairs do not
change that. Both Haivision comparisons pass.

## The slow runs are paced by the rate chosen at the end of slow start

Thirty native Robotweax runs were repeated with a benchmark variant that also
reads the sender's congestion-control state. The instrumentation is not part of
this repository.

- The packet sending period after the 1 GiB warm-up ranged from 1.0 to 3.0
  microseconds.
- In 13 runs it was above 2.15 microseconds. At 1,456 bytes per packet that
  paces the sender to between 3,800 and 5,300 Mbit/s. These runs had a median
  of 4,985 Mbit/s and a median of 10,198 blocked send calls.
- In the other 17 runs the median was 5,326 Mbit/s with 758 blocked send calls.
- The period falls slowly afterwards, for example from 2.36 to 2.14 microseconds
  over the 4 GiB transfer, so a run that starts paced stays slower.
- No run had a retransmission.

A second instrumented build logged the inputs when slow start ends, over 20
runs. Robotweax SRT sets the period from the receive-rate value of the
acknowledgement that ends slow start. That single value ranged from 175,824 to
1,000,000 packets per second and was absent in two runs; the receive rate later
in the same transfers had a per-run median of 462,000 to 710,000. Haivision SRT
1.5.7 uses the same FileCC scheme but takes its smoothed delivery rate at this
point.

## Using the smoothed rate was tried and is not better here

A Robotweax build that takes the smoothed receive rate at the end of slow start
was alternated with the unchanged build, 20 native runs each, plaintext File,
4 GiB.

| Variant | Mean Mbit/s | Median | Min | Frequently blocked runs | CV |
| --- | ---: | ---: | ---: | ---: | ---: |
| Single value (current) | 5,435 | 5,474 | 4,941 | 1 of 20 | 3.1% |
| Smoothed value | 5,252 | 5,302 | 4,887 | 6 of 20 | 3.5% |

The smoothed value varies less, but during slow start it is often below the
rate the path later sustains, so pacing limits the sender more often. No change
to Robotweax SRT was made.

## Limits

- The share of frequently blocked runs differed widely between series (13 of 30,
  9 of 32, 1 of 20 native runs). The variant comparison shows a direction, not a
  reliable size.
- Loopback in a virtual machine only. On a network with loss a single high
  receive-rate value can overestimate the starting rate; that was not measured.
- Encrypted File and macOS were not examined.

## Consequence

On this host Robotweax File does not reliably meet the 5% native-variation
control, whatever the number of pairs. The cause is below the abstraction.
