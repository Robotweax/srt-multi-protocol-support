"""Interleave comparable runs; emit raw JSON. This is not release qualification."""
import argparse
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--mib', type=int, default=128)
parser.add_argument('--runs', type=int, default=3)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
if args.runs < 1:
    parser.error('--runs must be positive')
root = args.build.resolve() / 'providers'

def artifact(name, module=False):
    variants = ([name + '.dll', 'lib' + name + '.so', 'lib' + name + '.dylib'] if module
                else [name, name + '.exe'])
    for directory in [root, root / 'Release']:
        for variant in variants:
            candidate = directory / variant
            if candidate.is_file():
                return str(candidate)
    raise FileNotFoundError(name)

schedule = [(provider, path) for _ in range(args.runs)
            for provider in ['robotweax', 'haivision'] for path in ['direct', 'module', 'facade']]
random.Random(157033).shuffle(schedule)
results = []
for provider, path in schedule:
    if path == 'direct':
        command = [artifact('srtmux_direct_' + provider), str(args.mib)]
    else:
        command = [artifact('srtmux_' + path + '_benchmark'), str(args.mib),
                   artifact('srtmux_' + provider, module=True), provider,
                   '0.3.3' if provider == 'robotweax' else '1.5.7']
    completed = subprocess.run(command, text=True, capture_output=True, timeout=150)
    if completed.returncode:
        raise RuntimeError(completed.stderr[-4000:])
    sample = json.loads(completed.stdout.splitlines()[-1])
    results.append(sample)
    print(f"{provider} {path}: {sample['mbps']:.1f} Mbps", file=sys.stderr)
summary = {}
for provider in ['robotweax', 'haivision']:
    direct = [r for r in results if r['provider'] == provider and r['path'] == 'direct']
    summary[provider] = {}
    for path in ['module', 'facade']:
        wrapped = [r for r in results if r['provider'] == provider and r['path'] == path]
        ratio = statistics.median(r['mbps'] for r in wrapped) / statistics.median(r['mbps'] for r in direct)
        cpu_ratio = statistics.median(r['cpu_seconds']/r['bytes'] for r in wrapped) / statistics.median(r['cpu_seconds']/r['bytes'] for r in direct)
        summary[provider][path] = {'median_throughput_change_percent': (ratio-1)*100,
                                 'median_cpu_per_byte_change_percent': (cpu_ratio-1)*100}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps({'platform': sys.platform,
    'profile': 'File stream, IPv4 loopback, plaintext, 32 KiB calls, accounting off, 8 MiB warmup; payload verified',
    'qualification': 'Exploratory only: shared host, no affinity, no confidence bounds or latency measurements',
    'samples': results, 'summary': summary}, indent=2) + '\n')
print(json.dumps(summary, indent=2))
