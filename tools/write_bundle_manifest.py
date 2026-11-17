"""Write a relocatable SHA-256 inventory for a staged application bundle."""
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

root = Path(sys.argv[1]).resolve(strict=True)
manifest = root / 'SHA256SUMS'
repository = Path(__file__).resolve().parents[1]
try:
    commit = subprocess.check_output(['git', '-C', str(repository), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(repository), 'status', '--porcelain'], text=True).strip())
except (OSError, subprocess.CalledProcessError):
    commit, dirty = None, None
root.joinpath('BUILD-INFO.json').write_text(json.dumps({
    'source_repository': 'https://github.com/Robotweax/srt-multi-protocol-support',
    'source_commit': commit, 'source_tree_dirty': dirty,
    'system': platform.system(), 'machine': platform.machine(),
}, indent=2) + '\n')
files = sorted(path for path in root.rglob('*') if path.is_file() and path != manifest)
assert root.joinpath('bin/srt-live-transmit').is_file(), 'Bundle executable missing'
manifest.write_text(''.join(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(root).as_posix() + '\n' for path in files))
print(f'Sealed {len(files)} bundle files')
