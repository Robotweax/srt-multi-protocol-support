"""Finite Live smoke checks accept only an intact prefix and an explicit missing tail."""
import argparse
import json
import os
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--encrypted', action='store_true')
parser.add_argument('--faults', action='store_true')
parser.add_argument('--api', choices=['direct', 'module', 'facade'])
parser.add_argument('artifacts', nargs=6)
args = parser.parse_args()
rw, hv, module, facade, rw_module, hv_module = args.artifacts
commands = [[rw, '1' if args.faults else '4'], [hv, '1' if args.faults else '4']]
for binary in [module, facade]:
    commands += [[binary, '1' if args.faults else '4', rw_module, 'robotweax', '0.3.5'],
                 [binary, '1' if args.faults else '4', hv_module, 'haivision', '1.5.7']]
if args.api:
    first = {'direct': 0, 'module': 2, 'facade': 4}[args.api]
    commands = commands[first:first + 2]

for command in commands:
    # Run both creation APIs without changing the timed benchmark or its protocol.
    for profile in ([0] if args.faults or len(command) == 2 else [0, 1]):
        cases = [(0, 0, None)]
        if args.faults:
            cases = [(3, 0, None), (0, 1, 'internal gap'), (0, 2, 'payload corruption'),
                     (0, 3, 'delivered no messages'), (0, 4, 'sender failure'),
                     (0, 5, 'duplicate or reordering'), (0, 6, 'duplicate or reordering')]
        for tail, fault, failure in cases:
            env = dict(os.environ, SRTMUX_LIVE_ENCRYPTED=str(int(args.encrypted)),
                       SRTMUX_LIVE_PROFILE_SETUP=str(profile),
                       SRTMUX_LIVE_TEST_DROP_TAIL=str(tail), SRTMUX_LIVE_TEST_FAULT=str(fault))
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=30)
            if failure:
                assert result.returncode == 1 and failure in result.stderr, (command, fault, result.stderr)
            else:
                assert result.returncode == 0, (command, tail, result.stderr)
                row = json.loads(result.stdout)
                assert row['functional_only'] and row['qualification_sample'] is False
                assert 0 < row['messages_received'] <= row['messages_sent']
                assert row['missing_tail_messages'] == row['messages_sent'] - row['messages_received']
                if tail:
                    assert row['missing_tail_messages'] >= tail, row
                print(command[0], 'profile', profile, json.dumps(row), flush=True)
print('Live tail-only contract checks passed')
