# Profile and telemetry foundation validation, 2026-11-16–17

Both pinned modules implement the Live/File subset of the versioned profile interface and
all 82 compatible native statistics fields. The C++ facade exposes immutable Profile
ownership and caller-buffer telemetry. See [supported contracts](../../providers/EXTENSIONS.md).

A [subsequent File investigation](2026-11-17-file-qualification.md) uses corrected
steady-state timing and a separate fixed campaign. It remains inconclusive; the
historical results below are unchanged and are not pooled with that protocol.

## Functional validation

- Release: **14/14 tests passed**.
- Optimized ASan/UBSan: **14/14 tests passed**.
- Both providers loaded in both orders; all four provider pairings through profile setup, Live/File and plaintext/AES-CTR, plus simultaneous opposite-direction transfers.
- Profiles released before transfer; caller secret storage overwritten after creation; foreign/stale handles and unsupported channel/version requests rejected.
- Complete 82-field snapshots, non-clearing counters, undersized output, unknown IDs and future output tails checked; native snapshots race abort-close safely.
- C++ profile transfers exercise existing concurrent send/receive/close, provider-local polling, sticky cancellation and shutdown tests.
- C11/C++20 header checks and exact pinned native inventory/metric coverage checks pass.

No ordinary payload operation, Entry layout, close gate or poller implementation changed.
Profile maps are used during setup only. Native statistics are sampled on explicit calls,
not implicitly per message. The native Haivision KM/close patch remains in both native and
wrapped benchmark builds. This milestone does not claim a new cross-platform CI run.

## Controlled local performance

Captures started on November 16 and continued on November 17 (Europe/Berlin).
Filenames retain the start date; each raw sample records its start timestamp.

Environment: `macOS-27.2.1-arm64-arm-64bit`, `arm64`, 8 logical CPUs; AppleClang Release and OpenSSL.
One IPv4 loopback connection per trial, 8 MiB SRT buffers, accounting disabled.
File: 1,536 MiB measured after 256 MiB warmup, 32 KiB calls. Live: 640 MiB
(rounded to complete 1,316-byte messages) after 64 MiB warmup, 120 ms latency.
Each fresh process also performs 20,000 separately timed idle receive probes.
All delivered bytes and Live sequence/boundaries are verified. AES-128-CTR state
and key size are checked before and after measurement. Setup is outside timing;
profile handles are released before transfer. Both providers and native/C/C++ paths
are included in every capture. No task-owned builds, tests or profiling overlap
measured transfers. This is not an exclusive-host, affinity or thermal-control claim.

Unchanged gates: lower paired 95% throughput ratio >=0.98; upper paired 95%
whole-process CPU/byte ratio <=1.05; at least eight pairs, every trial >=5 seconds,
native goodput CV <=5%. The existing runner uses geometric paired ratios and
10,000 percentile-bootstrap draws. Worker CPU is included; CPU seconds are not
hardware cycles. No slow observation is excluded.

Five initial eight-block captures were retained. Three configurations were
inconclusive, so one fixed twelve-block follow-up was run for each, including
both providers and all paths rather than selecting individual failed paths.
The combined analysis retains all original eight and follow-up twelve blocks,
renumbering follow-up block IDs to avoid collisions. It asserts identical settings,
environment descriptors and binary hashes before using the same analysis function.
The follow-up was selected after inspecting initial captures; this is provisional
local evidence, not a preregistered independent deployment certification. No further
sequential sampling or threshold adjustment was performed.

Measured transfers: **456**, totaling **537.00 GiB** excluding warmup. Combined files contain derived data, not additional trials.

Primary comparisons: **17/20 pass, 3 inconclusive, 0 confirmed regressions**.
**Full performance qualification remains incomplete.** Inconclusive results are not passes.
No comparison establishes a significant regression under the stated rule; no speculative I/O or lifecycle optimization was applied.

The table reports the limiting confidence bounds, not observed mean changes.

| Setup | Provider/path | Pairs | Throughput lower 95% | CPU/byte upper 95% | Native CV | Decision |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Original File | robotweax C | 20 | +3.172% | -16.183% | 4.440% | Pass |
| Original File | robotweax C++ | 20 | +1.431% | -14.971% | 4.440% | Pass |
| Original File | haivision C | 20 | -2.161% | +2.280% | 2.524% | Inconclusive: interval |
| Original File | haivision C++ | 20 | -1.916% | +2.592% | 2.524% | Pass |
| Profile File | robotweax C | 20 | +0.391% | -14.896% | 4.238% | Pass |
| Profile File | robotweax C++ | 20 | -2.648% | -14.074% | 4.238% | Inconclusive: interval |
| Profile File | haivision C | 20 | +0.014% | +1.823% | 3.476% | Pass |
| Profile File | haivision C++ | 20 | -2.519% | +2.559% | 3.476% | Inconclusive: interval |
| Profile Live | robotweax C | 20 | -1.223% | +4.661% | 0.600% | Pass |
| Profile Live | robotweax C++ | 20 | -0.773% | +4.941% | 0.600% | Pass |
| Profile Live | haivision C | 20 | -0.882% | +1.709% | 0.938% | Pass |
| Profile Live | haivision C++ | 20 | -1.626% | +2.323% | 0.938% | Pass |
| Profile File AES-128 | robotweax C | 8 | +1.853% | -12.699% | 1.490% | Pass |
| Profile File AES-128 | robotweax C++ | 8 | -0.211% | -12.152% | 1.490% | Pass |
| Profile File AES-128 | haivision C | 8 | -1.601% | +1.624% | 0.348% | Pass |
| Profile File AES-128 | haivision C++ | 8 | -1.241% | +1.691% | 0.348% | Pass |
| Profile Live AES-128 | robotweax C | 8 | -0.935% | +1.532% | 0.433% | Pass |
| Profile Live AES-128 | robotweax C++ | 8 | -0.632% | +2.122% | 0.433% | Pass |
| Profile Live AES-128 | haivision C | 8 | -0.776% | +1.277% | 0.823% | Pass |
| Profile Live AES-128 | haivision C++ | 8 | -1.140% | +1.214% | 0.823% | Pass |

## Retained captures and provenance

Initial and standalone replication outcomes are retained below; a later capture does
not erase an earlier inconclusive result. Slow clusters occurred in native and wrapped
paths, but their cause was not established. OS thermal-status queries were unavailable.
The primary combined result uses all observations.

| Capture | Robotweax C / C++ | Haivision C / C++ |
| --- | --- | --- |
| [2026-11-16-core-file-extensions](2026-11-16-core-file-extensions.json) | Inconclusive: controls / Inconclusive: controls | Pass / Inconclusive: interval |
| [2026-11-16-core-file-extensions-replication](2026-11-16-core-file-extensions-replication.json) | Pass / Pass | Inconclusive: interval / Pass |
| [2026-11-16-profiles-file-extensions](2026-11-16-profiles-file-extensions.json) | Inconclusive: interval / Inconclusive: interval | Pass / Inconclusive: interval |
| [2026-11-16-profiles-file-extensions-replication](2026-11-16-profiles-file-extensions-replication.json) | Inconclusive: controls / Inconclusive: controls | Pass / Pass |
| [2026-11-16-profiles-live-extensions](2026-11-16-profiles-live-extensions.json) | Pass / Inconclusive: interval | Pass / Pass |
| [2026-11-16-profiles-live-extensions-replication](2026-11-16-profiles-live-extensions-replication.json) | Inconclusive: interval / Inconclusive: interval | Pass / Inconclusive: interval |
| [2026-11-16-profiles-file-aes128-extensions](2026-11-16-profiles-file-aes128-extensions.json) | Pass / Pass | Pass / Pass |
| [2026-11-16-profiles-live-aes128-extensions](2026-11-16-profiles-live-aes128-extensions.json) | Pass / Pass | Pass / Pass |

Combined reports:

- [Original File, all 20 paired blocks](2026-11-16-core-file-extensions-combined.json).
- [Profile File, all 20 paired blocks](2026-11-16-profiles-file-extensions-combined.json).
- [Profile Live, all 20 paired blocks](2026-11-16-profiles-live-extensions-combined.json).

[Runtime source hashes](2026-11-16-profiles-telemetry-sources.json) identify the
frozen working-tree source set. Each capture also stores executable/module SHA-256
hashes, provider build fingerprints, exact counts, CPU breakdowns and call counts.
Pinned sources remain Robotweax `29c741d35f89a412926c374d6ccb15fa1270833c` and
Haivision `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.

Reproduce initial captures with `benchmarks/compare.py --build BUILD --runs 8`,
using the sizes/warmups above and `--probe-calls 20000`. Use `--profile-setup`
except for Original File, `--mode live` for Live and `--encryption aes-128-ctr`
for encrypted cases. Follow-ups use exactly the same arguments with `--runs 12`.
To reproduce combined analysis, verify settings/artifact equality, append follow-up
samples with block IDs increased by eight, and call `compare.summarize` on all
120 samples with the unchanged thresholds.

## Scope limits

These captures do not qualify continuous telemetry sampling at an application-chosen
frequency, nondefault tuning, forced key rotation, AES-192/256, WAN loss, many
connections, IPv6 throughput or another OS/CPU. Telemetry has native sampling cost;
no claim of free high-frequency monitoring is made. Control/Sensor, admission
callbacks, groups, FEC/GCM and Robotweax-specific identity/RTT/hybrid interfaces
remain unsupported in this implementation stage. Feature availability is separate
from qualification; descriptor qualification IDs remain empty.
