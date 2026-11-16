"""Blocked, paired native/module/facade measurements. Never discard slow trials."""
import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time

PATHS = ['direct', 'module', 'facade']
PROVIDERS = ['robotweax', 'haivision']


def interval(values, seed=157033):
    """Geometric mean paired ratio and percentile bootstrap 95% interval."""
    logs = [math.log(v) for v in values]
    rng = random.Random(seed)
    draws = sorted(math.exp(statistics.mean(rng.choices(logs, k=len(logs)))) for _ in range(10000))
    return {'ratio': math.exp(statistics.mean(logs)), 'low95': draws[249], 'high95': draws[9749]}


def summarize(samples, args):
    result = {}
    for provider in PROVIDERS:
        rows = [s for s in samples if s['provider'] == provider]
        baseline = [s for s in rows if s['path'] == 'direct']
        if not baseline:
            continue
        cv = (statistics.stdev(s['mbps'] for s in baseline) / statistics.mean(s['mbps'] for s in baseline)
              if len(baseline) > 1 else None)
        result[provider] = {'direct_cv': cv, 'paths': {}}
        for path in PATHS[1:]:
            pairs = []
            for direct in baseline:
                wrapped = next((s for s in rows if s['block'] == direct['block'] and s['path'] == path), None)
                if wrapped:
                    pairs.append((direct, wrapped))
            if not pairs:
                continue
            throughput = interval([w['mbps']/d['mbps'] for d, w in pairs])
            cpu = interval([(w['cpu_seconds']/w['bytes'])/(d['cpu_seconds']/d['bytes']) for d, w in pairs])
            app_cpu = interval([(w['application_cpu_seconds']/w['bytes'])/(d['application_cpu_seconds']/d['bytes']) for d, w in pairs])
            long_enough = all(s['seconds'] >= args.min_seconds for pair in pairs for s in pair)
            precise = len(pairs) >= 8 and long_enough and cv is not None and cv <= args.max_baseline_cv
            if not precise:
                decision = 'inconclusive_controls'
            elif throughput['high95'] < 1-args.throughput_budget/100 or cpu['low95'] > 1+args.cpu_budget/100:
                decision = 'regression'
            elif throughput['low95'] >= 1-args.throughput_budget/100 and cpu['high95'] <= 1+args.cpu_budget/100:
                decision = 'pass_measured_profile'
            else:
                decision = 'inconclusive_interval'
            result[provider]['paths'][path] = {'pairs': len(pairs), 'throughput': throughput,
                'process_cpu_per_byte': cpu, 'application_cpu_per_byte': app_cpu,
                'minimum_duration_met': long_enough, 'decision': decision}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--mib', type=int, default=2048)
    parser.add_argument('--runs', type=int, default=8, help='paired blocks per provider')
    parser.add_argument('--warmup-mib', type=int, default=128)
    parser.add_argument('--mode', choices=['file', 'live'], default='file')
    parser.add_argument('--encryption', choices=['none', 'aes-128-ctr'], default='none')
    parser.add_argument('--chunk-bytes', type=int, help='default: 32768 File, 1316 Live')
    parser.add_argument('--wait', choices=['yield', 'poll'], default='poll')
    parser.add_argument('--direct-poll-api', choices=['uwait', 'legacy'], default='uwait')
    parser.add_argument('--probe-calls', type=int, default=100000)
    parser.add_argument('--min-seconds', type=float, default=5)
    parser.add_argument('--max-baseline-cv', type=float, default=.05)
    parser.add_argument('--throughput-budget', type=float, default=2)
    parser.add_argument('--cpu-budget', type=float, default=5)
    parser.add_argument('--cpus', help='Linux CPU set for all benchmark and vendor threads, e.g. 2,3,4,5')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.chunk_bytes is None:
        args.chunk_bytes = 1316 if args.mode == 'live' else 32768
    if args.mode == 'live' and not 8 <= args.chunk_bytes <= 1456:
        parser.error('Live message payload must be 8..1456 bytes')
    expected_bytes = args.mib*1024*1024
    if args.mode == 'live':
        expected_bytes = expected_bytes//args.chunk_bytes*args.chunk_bytes
    if not 1 <= args.mib <= 4096 or args.runs < 1 or not 0 <= args.warmup_mib <= 4096:
        parser.error('invalid trial count/size')
    if not 1 <= args.chunk_bytes <= 1048576 or not 0 <= args.probe_calls <= 10000000:
        parser.error('invalid chunk size/probe count')
    if args.min_seconds <= 0 or not 0 < args.max_baseline_cv < 1 or not 0 <= args.throughput_budget < 100 or args.cpu_budget < 0:
        parser.error('invalid analysis controls')
    if args.cpus:
        if not hasattr(os, 'sched_setaffinity'):
            parser.error('--cpus requires Linux sched_setaffinity; no affinity emulation')
        selected = {int(cpu) for cpu in args.cpus.split(',')}
        if not selected or not selected <= os.sched_getaffinity(0):
            parser.error('CPU set is not available')
        os.sched_setaffinity(0, selected)  # Inherited by children and vendor workers.
    root = args.build.resolve() / 'providers'

    def artifact(name, module=False):
        variants = ([name+'.dll', 'lib'+name+'.so', 'lib'+name+'.dylib'] if module else [name, name+'.exe'])
        for directory in [root, root/'Release']:
            for variant in variants:
                candidate = directory/variant
                if candidate.is_file():
                    return str(candidate)
        raise FileNotFoundError(name)

    files = {p: artifact('srtmux_'+p, True) for p in PROVIDERS}
    binaries = {p: artifact('srtmux_direct_'+p) for p in PROVIDERS}
    binaries.update({p: artifact('srtmux_'+p+'_benchmark') for p in PATHS[1:]})
    env = dict(os.environ, SRTMUX_BENCH_POLL=str(int(args.wait == 'poll')),
        SRTMUX_BENCH_WARMUP_MIB=str(args.warmup_mib), SRTMUX_BENCH_CHUNK=str(args.chunk_bytes),
        SRTMUX_BENCH_PROBES=str(args.probe_calls),
        SRTMUX_BENCH_NATIVE_LEGACY=str(int(args.direct_poll_api == 'legacy')),
        SRTMUX_BENCH_LIVE=str(int(args.mode == 'live')), SRTMUX_BENCH_ENCRYPTED=str(int(args.encryption != 'none')))
    rng = random.Random(157033)
    orders = list(itertools.permutations(PATHS))
    rng.shuffle(orders)
    schedule = []
    for block in range(args.runs):
        providers = PROVIDERS.copy(); rng.shuffle(providers)
        for provider in providers:
            order = orders[(block+PROVIDERS.index(provider)*3) % len(orders)]
            schedule.extend((block, provider, path) for path in order)
    report = {'schema': 2, 'environment': {'platform': platform.platform(), 'machine': platform.machine(),
        'cpu_count': os.cpu_count(), 'affinity': sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None,
        'isolation': 'No exclusive-host or thermal-control claim; no other task-owned workloads during capture'},
        'profile': {'mode': args.mode+'/IPv4/'+args.encryption, 'bytes': expected_bytes,
            'encryption': args.encryption, 'latency_ms': 120 if args.mode == 'live' else 0, 'mib': args.mib, 'warmup_mib': args.warmup_mib,
            'chunk_bytes': args.chunk_bytes, 'wait': args.wait, 'probe_calls': args.probe_calls,
            'direct_poll_api': args.direct_poll_api,
            'accounting': 'disabled', 'payload': 'all received bytes verified; Live sequence and boundaries checked',
            'offered_load': 'unpaced, native nonblocking backpressure; no application rate cap'},
        'controls': {'blocks': args.runs, 'min_seconds': args.min_seconds, 'max_baseline_cv': args.max_baseline_cv,
            'throughput_budget_percent': args.throughput_budget, 'cpu_budget_percent': args.cpu_budget,
            'statistic': 'paired geometric mean ratios; 10000 paired bootstrap draws, percentile 95% CI; no exclusions',
            'scope': 'Provisional measured profile only, not deployment qualification; CPU seconds are not hardware cycles'},
        'artifacts': {name: {'path': path, 'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}
            for name, path in {**{'module_'+k: v for k, v in files.items()}, **binaries}.items()},
        'samples': [], 'summary': {}, 'status': 'running'}
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        temporary = args.output.with_suffix(args.output.suffix+'.tmp')
        temporary.write_text(json.dumps(report, indent=2)+'\n')
        temporary.replace(args.output)

    save()
    try:
        for block, provider, path in schedule:
            command = [binaries[provider if path == 'direct' else path], str(args.mib)]
            if path != 'direct':
                command += [files[provider], provider, '0.3.3' if provider == 'robotweax' else '1.5.7']
            started = time.time()
            completed = subprocess.run(command, text=True, capture_output=True, timeout=450, env=env)
            if completed.returncode:
                raise RuntimeError(f'{provider}/{path}: '+completed.stderr[-4000:])
            sample = json.loads(completed.stdout.splitlines()[-1])
            if sample['provider'] != provider or sample['path'] != path or sample['bytes'] != expected_bytes or sample['mode'] != args.mode or sample['encryption'] != args.encryption:
                raise RuntimeError('benchmark result identity/byte count mismatch')
            sample.update(block=block, started_unix=started)
            report['samples'].append(sample); save()
            print(f"block {block} {provider} {path}: {sample['mbps']:.1f} Mbps, {sample['seconds']:.2f}s", file=sys.stderr, flush=True)
        for provider in PROVIDERS:
            if len({s['build_fingerprint'] for s in report['samples'] if s['provider'] == provider}) != 1:
                raise RuntimeError('direct and wrapped vendor build fingerprints differ')
        report['summary'] = summarize(report['samples'], args)
        report['status'] = 'complete'; save()
        print(json.dumps(report['summary'], indent=2))
    except BaseException as error:
        report['status'] = 'failed'; report['failure'] = str(error); save(); raise


if __name__ == '__main__':
    main()
