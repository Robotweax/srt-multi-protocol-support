# Controlled local File/IPv4 qualification

Scope: macOS 27.2.1 arm64 (eight logical CPUs), AppleClang 21 Release, OpenSSL 3,
one plaintext File/IPv4
loopback stream, 32 KiB calls and 8 MiB SRT buffers, accounting disabled. This is
the local comparison requested by the user. It does not qualify Live, encryption,
loss, full duplex, multiple connections or other operating systems/hardware.

## Protocol and provenance

Each measured transfer delivers and verifies 2 GiB after 256 MiB warmup. Each
fresh process first executes 20,000 connected idle receive calls, separately
timed with calling-thread CPU. The receiver checks every byte against the test
pattern and requires complete delivery. Both providers and all three paths are
included in every matrix. Tests/builds/profiling do not overlap recorded trials.
The host is not exclusively reserved and affinity/thermal control is not claimed.

The [runner protocol](../../benchmarks/README.md) defines the fixed 2% throughput
and 5% process-CPU/byte budgets, paired geometric means, 10,000-resample percentile
95% intervals, minimum five-second trial duration, minimum eight blocks and
maximum 5% direct-throughput coefficient of variation. CPU seconds include vendor
threads; they are a proxy, not hardware cycles. Application-thread CPU and idle
probes are diagnostic and do not replace the whole-process CPU gate.

Raw reports preserve all trials, binary SHA-256 hashes and build fingerprints:

- [Before optimization, eight blocks](2026-11-16-controlled-poll-before.json).
- [Optimized, first eight blocks](2026-11-16-controlled-poll-after.json).
- [Fixed eight-block replication](2026-11-16-controlled-poll-replication.json).

Before runtime code is `42b5b5b`; optimized runtime and harness are `dae72f2`.
The before harness already has warmup, polling and paired analysis; its direct
path uses legacy `srt_epoll_wait`. Optimized direct uses `srt_epoll_uwait`, avoiding
a weak native baseline. Therefore before/after percentages are relative to their
own native controls, not a matched estimate of the isolated optimization effect.
Both paths within each matrix use identical vendor static targets, including the
existing Haivision KM/close lifetime fix. Neither is an unmodified upstream binary.
The separate short pilot and profiler captures were diagnostic, not gate inputs.

## Identified regression and correction

Before optimization, Haivision’s C module lost 13.86% goodput (95% interval
−14.85% to −12.78%) and increased process CPU/byte 7.59%. The C++ facade lost
13.17% (−14.64% to −11.38%) and increased CPU/byte 7.75%. Robotweax passed both
budgets. This was a repeatable Haivision polling regression, not merely the noisy
subsecond result from the previous milestone.

Native-source inspection and a separate macOS stack sample identified the
combined SRT/system-descriptor wait: Haivision checks the OS wake descriptor
through `kevent` under its epoll lock and constructs temporary result storage even
when SRT readiness exists. Stack samples are wall-time observations, not CPU
percentages. Native `uwait` avoids that path but rejects OS descriptors.

The adapter now uses a private, unbound INIT-state SRT socket as a notification
identity. A small bridge compiled inside the pinned native target sets its epoll
readiness and signals the native event. The wait uses `srt_epoll_uwait`, directly
translates native ready flags, and hides the notification identity. There is no
UDP transport, payload queue or extra worker. The bridge is private and hashed
into provider metadata. Robotweax retains its OS wake path with fewer unnecessary
drain syscalls and ordinary-readiness state queries. Both paths initialize only
the needed portion of the bounded event merge table. WouldBlock avoids fetching
unused native OS-error detail.

Atomic close gates, generation validation, sticky cancellation, one waiter per
poller and the native crypto-close fix remain enabled. No payload copies, busy
retry loops, wrapper timeout slicing or cancellation relaxation were introduced.

## Results

All four wrapped paths pass the measured-profile gates in the fixed replication
and in the combined 16-block analysis. The first eight-block Haivision C ABI
result was inconclusive (−1.76%, interval −2.88% to −0.53%); exactly one additional
eight-block matrix was selected to assess precision. No trials were excluded or
replaced. The combined intervals below are nominal 95% intervals, not adjusted
for that sequential selection. Both constituent reports remain available.

[Combined report with all 96 optimized trials](2026-11-16-controlled-poll-combined.json)
includes source-report hashes and original raw samples with distinct block IDs.
Changes below are relative to each provider’s paired direct-native baseline.

| Provider/path | Goodput change [95% interval] | Process CPU/byte change [95% interval] | Decision |
| --- | ---: | ---: | --- |
| Robotweax C ABI | +4.40% [+3.41%, +5.35%] | -16.61% [-17.00%, -16.20%] | Pass, local profile |
| Robotweax C++ facade | +4.71% [+3.88%, +5.65%] | -16.71% [-17.03%, -16.38%] | Pass, local profile |
| Haivision C ABI | -1.00% [-1.92%, -0.09%] | +2.53% [+2.00%, +3.08%] | Pass, local profile |
| Haivision C++ facade | -0.44% [-1.36%, +0.49%] | +2.44% [+1.82%, +3.01%] | Pass, local profile |

Haivision’s C ABI lower throughput bound is −1.92%, only 0.08 percentage points
inside the −2% limit. This is a narrow pass, not evidence of zero overhead.
Robotweax’s direct-goodput coefficient of variation is 1.61%; Haivision’s is
1.20%. Every measured optimized trial lasts over eight seconds. Median goodput
is approximately 2.00 Gbit/s direct and 2.09 Gbit/s wrapped for Robotweax;
1.72 Gbit/s direct and 1.71 Gbit/s wrapped for Haivision. The ratio table uses
paired geometric means, not ratios of these medians.

Haivision application-thread CPU/byte rises 6.80% for the C ABI and 7.22% for the
facade. Whole-process CPU/byte, which includes the native transport workers, rises
2.53% and 2.44% respectively and is the stated CPU gate. There is real residual
application overhead even though the end-to-end budgets pass.

Median connected-idle receive probe times across optimized runs:

| Provider | Direct | C ABI | C++ facade |
| --- | ---: | ---: | ---: |
| Robotweax | 147 ns | 156 ns | 157 ns |
| Haivision | 27,733 ns | 28,931 ns | 28,860 ns |

These are CPU times for the complete idle receive boundary, including vendor error
handling, not isolated atomic-gate costs or predictions for successful payload
calls. Differences in retry counts, native wait implementations and batching also
change loaded CPU cost. Robotweax’s apparent speedup is specific to this workload
and wait pattern, not a general claim that dispatch makes SRT faster.

This qualifies the measured local File/IPv4 profile against the stated engineering
gates. It does not establish the broader deployment release gate or hardware-cycle
budget. The separate simultaneous-provider and lifecycle tests establish
correctness under those test scenarios, not concurrent throughput qualification.

## Correctness validation

All nine local Release CTest groups pass, including the benchmark gate analysis.
All nine optimized ASan/UBSan groups pass with host, adapter and vendor code
instrumented. The facade/cancellation suite additionally passes 20 consecutive
runs (ten per load order). The 52-case transport matrix still includes both
providers loaded together and transferring concurrently, encrypted/plain traffic,
cross-provider pairings and independent cleanup.

New regression checks cover a non-power-of-two ready-event batch and remote peer
failure with a read-only subscription. Existing checks retain infinite/finite
waits, wake-before-wait, repeated/coalesced wake, registration changes, safe
concurrent close, stale handles, terminal events and shutdown cancellation.

All six [CI jobs for the code commit](https://github.com/Robotweax/srt-multi-protocol-support/actions/runs/43943213600)
pass: Linux/macOS/Windows host checks, Linux/macOS real providers, and Linux
sanitizers. Windows real-provider performance is not qualified by the host check.

## Reproduce

Run the command below twice, using `after` and then `replication` in the output
filename. Keep binaries and transport settings unchanged between captures.

```sh
python3 benchmarks/compare.py --build build-real --mib 2048 --runs 8 \
  --warmup-mib 256 --probe-calls 20000 --wait poll \
  --output docs/validation/2026-11-16-controlled-poll-after.json
```

The following Python, run from the repository root, reproduces the combined
analysis from the retained reports (without rerunning the transfers):

```python
import copy
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace
sys.path.insert(0, 'benchmarks')
from compare import summarize
paths = [Path('docs/validation/2026-11-16-controlled-poll-' + suffix + '.json')
         for suffix in ['after', 'replication']]
reports = [json.loads(p.read_text()) for p in paths]
assert all(r['status'] == 'complete' for r in reports)
assert reports[0]['profile'] == reports[1]['profile']
assert reports[0]['artifacts'] == reports[1]['artifacts']
assert reports[0]['controls'] == reports[1]['controls']
r = copy.deepcopy(reports[0])
r['samples'] = []
r['inputs'] = []
offset = 0
for path, source in zip(paths, reports):
    r['inputs'].append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    for sample in source['samples']:
        row = dict(sample)
        row['block'] += offset
        r['samples'].append(row)
    offset += source['controls']['blocks']
r['controls']['blocks'] = offset
r['analysis_note'] = ('All 16 blocks from the first optimized run plus one fixed eight-block replication; '
                     'replication selected after an inconclusive Haivision C ABI interval. '
                     'No exclusions; intervals are nominal, not adjusted for sequential selection.')
c = r['controls']
args = SimpleNamespace(min_seconds=c['min_seconds'], max_baseline_cv=c['max_baseline_cv'],
                       throughput_budget=c['throughput_budget_percent'], cpu_budget=c['cpu_budget_percent'])
r['summary'] = summarize(r['samples'], args)
Path('docs/validation/2026-11-16-controlled-poll-combined.json').write_text(json.dumps(r, indent=2)+'\n')
print(json.dumps(r['summary'], indent=2))
```
