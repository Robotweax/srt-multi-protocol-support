"""Optional black-box interoperability with unmodified Haivision srt-live-transmit."""
from contextlib import ExitStack
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

app, reference, rw, hv = [str(Path(value).resolve()) for value in sys.argv[1:5]]
secret = 'srtmux-public-reference-test-secret'


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0)); return sock.getsockname()[1]


def ready(process, log, text):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        content = log.read_text()
        if text in content: return
        assert process.poll() is None, content.replace(secret, '<fixture>')
        time.sleep(.02)
    raise AssertionError(log.read_text().replace(secret, '<fixture>'))


with tempfile.TemporaryDirectory(prefix='srtmux-reference-app-') as temporary:
    root = Path(temporary)
    for provider in ('robotweax', 'haivision'):
        for encrypted in (False, True):
            for app_sends in (False, True):
                processes = []
                with ExitStack() as stack, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
                    sink.bind(('127.0.0.1', 0)); sink.settimeout(.5)
                    srt_port, udp_port = port(), port()
                    security = f'&passphrase={secret}&pbkeylen=16' if encrypted else ''
                    new_prefix = [app, '--robotweax-module', rw, '--haivision-module', hv, '-a:no', '-t:20']
                    old_prefix = [reference, '-a:no', '-t:20']
                    receiver_args = (old_prefix if app_sends else new_prefix) + [f'srt://127.0.0.1:{srt_port}?mode=listener{security}' + ('' if app_sends else '&provider=' + provider), f'udp://127.0.0.1:{sink.getsockname()[1]}']
                    sender_args = (new_prefix if app_sends else old_prefix) + [f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{srt_port}?mode=caller{security}' + ('&provider=' + provider if app_sends else '')]
                    try:
                        receiver_log = root / 'receiver.log'; sender_log = root / 'sender.log'
                        receiver = subprocess.Popen(receiver_args, stdout=subprocess.DEVNULL, stderr=stack.enter_context(receiver_log.open('wb'))); processes.append(receiver)
                        if not app_sends: ready(receiver, receiver_log, 'Listening source')
                        else: time.sleep(.2)
                        sender = subprocess.Popen(sender_args, stdout=subprocess.DEVNULL, stderr=stack.enter_context(sender_log.open('wb'))); processes.append(sender)
                        ready(sender, sender_log, 'Connected target' if app_sends else 'SRT target connected')
                        ready(receiver, receiver_log, 'Accepted SRT source connection' if app_sends else 'Connected source')
                        expected = [index.to_bytes(4, 'big') + bytes([index]) * 508 for index in range(24)]
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as feed:
                            for packet in expected:
                                feed.sendto(packet, ('127.0.0.1', udp_port)); time.sleep(.01)
                        actual = []
                        while True:
                            try: actual.append(sink.recv(65536))
                            except socket.timeout: break
                        assert actual and actual == expected[:len(actual)], (len(actual), len(expected))
                        print(provider, 'to-upstream' if app_sends else 'from-upstream', 'aes' if encrypted else 'plain', 'received', len(actual), 'tail', len(expected)-len(actual), flush=True)
                    finally:
                        failures = []
                        for process in processes:
                            if process.poll() is None: process.send_signal(signal.SIGINT)
                        for process in processes:
                            try: process.wait(timeout=5)
                            except subprocess.TimeoutExpired: process.kill(); process.wait(); failures.append('Reference cleanup timeout')
                        assert not failures and all(process.returncode == 0 for process in processes), failures
print('Unmodified Haivision application interoperability passes for both providers and directions')
