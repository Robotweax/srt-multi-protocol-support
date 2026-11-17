"""Injected transfer failures must stop and join the persistent benchmark worker."""
import os
import subprocess
import sys

rw_direct, hv_direct, module, facade, rw_module, hv_module = sys.argv[1:]
commands = [[rw_direct, '1'], [hv_direct, '1']]
for binary in [module, facade]:
    commands += [[binary, '1', rw_module, 'robotweax', '0.3.3'],
                 [binary, '1', hv_module, 'haivision', '1.5.7']]
for command in commands:
    for side in ['1', '2']:
        env = dict(os.environ, SRTMUX_BENCH_FAIL_SIDE=side, SRTMUX_BENCH_POLL='1',
                   SRTMUX_BENCH_WARMUP_MIB='0', SRTMUX_BENCH_PROBES='0',
                   SRTMUX_BENCH_LIVE='0', SRTMUX_BENCH_ENCRYPTED='0',
                   SRTMUX_BENCH_PROFILE_SETUP='1', SRTMUX_BENCH_CHUNK='32768')
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == 1, (command, side, result.returncode, result.stderr)
        assert any(text in result.stderr for text in ['injected ', 'readiness timeout/failure',
                                                      'incomplete delivery']), result.stderr
print('Sender and receiver failures join cleanly on native/module/facade paths for both providers')
