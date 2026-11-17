"""Installed binaries locate both bundled modules independently of the working directory."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
app = root / 'bin/srt-live-transmit'


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
        handle.bind(('127.0.0.1', 0)); return handle.getsockname()[1]


assert subprocess.run([str(app), '-version'], capture_output=True, timeout=5).returncode == 0
assert root.joinpath('share/srtmux/haivision/LICENSE').is_file()
with tempfile.TemporaryDirectory(prefix='srtmux-bundle-cwd-') as temporary:
    for provider in ('haivision', 'robotweax'):
        Path(temporary, f'libsrtmux_{provider}.so').write_bytes(b'not a provider')
        result = subprocess.run([str(app), '-t:1', '-v', '--provider', provider,
                                 f'udp://127.0.0.1:{port()}', f'srt://127.0.0.1:{port()}'],
                                cwd=temporary, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0 and f'Loaded target provider={provider}' in result.stderr, result.stderr
print('Installed bundle resolves both providers without current-directory module lookup')
