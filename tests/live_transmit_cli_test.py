"""Exercise application parsing, byte preservation, cancellation and UDP truncation."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

app = str(Path(sys.argv[1]).resolve())


def run(*args, **kwargs):
    return subprocess.run([app, *args], capture_output=True, timeout=10, **kwargs)


def wait_text(path, text, process, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if text in path.read_text():
            return
        assert process.poll() is None, path.read_text()
        time.sleep(.02)
    raise AssertionError(path.read_text())


assert run('-h').returncode == 0
assert b'multi-SRT' in run('-version').stdout
secret = 'public-cli-test-secret'
bad = [
    ['-c:-1'], ['-buffering:0'], ['-t:abc'], ['--provider', 'unknown'], ['-st:yes'], ['--unknown', 'value'],
    ['file://con', f'srt://127.0.0.1:9000?passphrase={secret}&fec=1'],
    ['file://con', 'srt://127.0.0.1:9000?mode=rendezvous'],
    ['file://con', 'srt://127.0.0.1:9000?transtype=file'],
    ['file://con', 'srt://127.0.0.1:9000?passphrase=%ZZ'],
    ['file://con', 'srt://127.0.0.1:9000?passphrase=%A'],
    ['file://con', 'srt://127.0.0.1:9000?passphrase=%+A'],
    ['file://con', 'srt://127.0.0.1:9000?passphrase=%00'],
    ['file://con', 'srt://127.0.0.1:9000?provider=haivision&provider=robotweax'],
    ['file://con', 'srt://127.0.0.1:9000?missing-value'],
    ['file://con', 'srt://127.0.0.1:9000junk'],
    ['file://con', 'srt://127.0.0.1:9000?pbkeylen=16'],
]
for arguments in bad:
    result = run(*arguments)
    assert result.returncode != 0, arguments
    assert secret.encode() not in result.stderr, result.stderr

with tempfile.TemporaryDirectory(prefix='srtmux-transmit-cli-') as temporary:
    root = Path(temporary)
    source = root / 'input.bin'; target = root / 'output.bin'
    payload = bytes(range(256)) * 4096
    source.write_bytes(payload)
    result = run(str(source), str(target))
    assert result.returncode == 0 and target.read_bytes() == payload, result.stderr
    spaced = root / 'with space.bin'; spaced.write_bytes(payload)
    result = run(spaced.as_uri(), target.as_uri())
    assert result.returncode == 0 and target.read_bytes() == payload, result.stderr
    result = run(str(source), str(source))
    assert result.returncode != 0 and source.read_bytes() == payload
    result = run('-statsout', str(source), str(source), str(target))
    assert result.returncode != 0 and source.read_bytes() == payload
    result = run('-pf:json', '-s:1', 'file://con', 'file://con', input=payload)
    assert result.returncode == 0 and result.stdout == payload, result.stderr[-1000:]
    assert b'"written_bytes"' in result.stderr

    with (root / 'idle.log').open('wb') as log:
        process = subprocess.Popen([app, '-t:1', 'file://con', str(target)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        try:
            started = time.monotonic(); process.wait(timeout=4)
            assert process.returncode == 0 and .8 < time.monotonic() - started < 4
        finally:
            if process.poll() is None: process.kill(); process.wait()
            process.stdin.close(); process.stdout.close()
    with (root / 'signal.log').open('wb') as log:
        process = subprocess.Popen([app, 'file://con', str(target)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        try:
            wait_text(root / 'signal.log', 'Relay ready', process)
            process.send_signal(signal.SIGTERM); process.wait(timeout=3)
            assert process.returncode == 0
        finally:
            if process.poll() is None: process.kill(); process.wait()
            process.stdin.close(); process.stdout.close()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(('127.0.0.1', 0)); port = reservation.getsockname()[1]
    with (root / 'udp.log').open('wb') as log:
        process = subprocess.Popen([app, '-c:8', f'udp://127.0.0.1:{port}', str(target)], stdout=subprocess.DEVNULL, stderr=log)
        try:
            wait_text(root / 'udp.log', 'Relay ready', process)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                sender.sendto(b'oversize-datagram', ('127.0.0.1', port))
            process.wait(timeout=4)
            assert process.returncode != 0 and target.read_bytes() == b''
            assert 'exceeds chunk size' in (root / 'udp.log').read_text()
        finally:
            if process.poll() is None: process.kill(); process.wait()
    if socket.has_ipv6:
        try:
            reservation = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            reservation.bind(('::1', 0)); ipv6_port = reservation.getsockname()[1]; reservation.close()
            sink = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM); sink.bind(('::1', 0))
        except OSError:
            print('IPv6 UDP unavailable on this host')
        else:
            with sink, (root / 'ipv6.log').open('wb') as log:
                process = subprocess.Popen([app, f'udp://[::1]:{ipv6_port}', f'udp://[::1]:{sink.getsockname()[1]}'], stdout=subprocess.DEVNULL, stderr=log)
                try:
                    wait_text(root / 'ipv6.log', 'Relay ready', process)
                    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as feed:
                        for packet in [b'ipv6-data', b'']:
                            feed.sendto(packet, ('::1', ipv6_port)); sink.settimeout(2)
                            assert sink.recv(4096) == packet
                    process.send_signal(signal.SIGINT); process.wait(timeout=3)
                    assert process.returncode == 0
                finally:
                    if process.poll() is None: process.kill(); process.wait()
print('Application CLI, files/console, signal/timer cancellation and UDP truncation checks pass')
