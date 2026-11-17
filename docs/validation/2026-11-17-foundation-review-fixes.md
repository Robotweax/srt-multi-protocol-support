# Foundation review fixes — 17 November 2026

These changes address findings from a review of `6507b2e`. They change adapter
and polling internals only. The provider ABI, the C++ facade, the native source
pins and the Haivision patches are unchanged. Build fingerprints change because
adapter sources are part of them, so earlier throughput qualification records
describe the earlier fingerprints and are not re-labelled.

## Robotweax poller wake

The Robotweax adapter woke its poller through an OS socket pair registered as a
system socket. Robotweax SRT 0.3.3 checks system sockets on a 10 ms tick, so
`wake()`, close notification and shutdown reached a blocked waiter late, and an
idle poller woke 100 times a second. Data readiness was not affected.

The wake identity is now an unbound SRT socket registered for the error event.
Closing it is reported by `srt_epoll_uwait` immediately; the waiter re-arms with
a new socket. Only public vendor API is used. Both providers now share one
`srt_epoll_uwait` path, which removed the dual read/write arrays, the per-event
socket-state query for ordinary readiness and the POSIX/Windows wake socket code.

## Per-wait cost

`poller_wait` validated every caller record and cleared a merge table sized by
the output capacity on every wake-up, and resolved each event through two hash
lookups. A registration is reported either by the native set or, once closed, by
the terminal list, never both, so no merge is needed. Records are now checked as
they are written and each native event costs one lookup.

## Close notification

Every close and release scanned all registry slots under the runtime control
lock to find subscribed pollers. Each object now records its subscribed pollers,
and close/release visit only those.

## Handle generations

Generations came from one process-wide 32-bit counter that was never reused, so
object churn alone would eventually make creation fail permanently. Generations
are now per slot, with a module-wide floor so a later runtime cannot alias
handles of an earlier one. Profile handles use their own 48-bit sequence.

## Authentication failures

`connection_info` reported a wrong or missing passphrase as a generic peer
rejection. Native reasons `SRT_REJ_BADSECRET` and `SRT_REJ_UNSECURE` now map to
the authentication category, with the native rejection code still attached.

## Measurements

macOS ARM64, Release, same machine, before (`6507b2e`) and after. Wake latency is
200 wakes of a blocked waiter through the C++ facade; idle cost is one 3 s wait
with nothing ready; close is closing and releasing 4,000 unconnected connections
of which half are registered with one poller.

| Provider | Measure | Before | After |
| --- | --- | ---: | ---: |
| Robotweax | Wake latency p50 / p90 | 4,424 / 10,934 µs | 62 / 91 µs |
| Robotweax | Process CPU for 3 s idle wait | 11.3 ms | 0.6 ms |
| Robotweax | Close and release 4,000 objects | 48.7 ms | 2.7 ms |
| Haivision | Wake latency p50 / p90 | 31 / 46 µs | 31 / 43 µs |
| Haivision | Process CPU for 3 s idle wait | 10.1 ms | 14.1 ms |
| Haivision | Close and release 4,000 objects | 215.2 ms | 37.0 ms |

The Haivision idle figure is the pinned library's own 10 ms internal wait and is
within run-to-run noise; this change does not alter it. These are single local
runs, not qualification samples, and the throughput matrix was not rerun.

## Checks

`real_provider_tests` gains regression checks for both providers: median wake
latency below 2 ms, close notification reaching exactly the subscribed pollers,
per-slot generation advance with stale-handle rejection, and the authentication
category for a wrong passphrase. The full suite passes in macOS ARM64 Release.

## Not changed

The overlap between core setup calls and the profiles extension, the declared
but unimplemented extension surface, and upstreaming of the two Haivision
compatibility patches are design and third-party decisions left to the maintainer.
