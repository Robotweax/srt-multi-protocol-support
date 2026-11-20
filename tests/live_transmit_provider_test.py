"""Real-process UDP/SRT relays, encryption, per-endpoint provider choice and reconnect."""
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

app, rw, hv = map(lambda value: str(Path(value).resolve()), sys.argv[1:4])
prefix = [app, '--robotweax-module', rw, '--haivision-module', hv, '-v', '-t:20']
secret = 'srtmux-public-app-test-secret'


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
        handle.bind(('127.0.0.1', 0))
        return handle.getsockname()[1]


class Processes:
    def __init__(self, root):
        self.root = root; self.processes = []; self.stack = ExitStack()

    def start(self, name, source, target, *extra):
        log = self.root / (name + '.log')
        stream = self.stack.enter_context(log.open('wb'))
        process = subprocess.Popen([*prefix, *extra, source, target], stdout=subprocess.DEVNULL, stderr=stream)
        self.processes.append((process, log))
        return process, log

    def ready(self, item, text, count=1):
        process, log = item; deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            if log.read_text().count(text) >= count: return
            assert process.poll() is None, log.read_text().replace(secret, '<fixture>')
            time.sleep(.02)
        raise AssertionError(log.read_text().replace(secret, '<fixture>'))

    def close(self, check=True):
        failures = []
        for process, _ in self.processes:
            if process.poll() is None: process.send_signal(signal.SIGINT)
        for process, _ in self.processes:
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(); failures.append('Process cleanup timeout')
        self.stack.close()
        if check:
            failures += [log.read_text().replace(secret, '<fixture>') for process, log in self.processes if process.returncode != 0]
        assert not failures, failures


def feed_and_capture(input_port, sink, marker):
    expected = [index.to_bytes(4, 'big') + marker + bytes([index])*503 for index in range(24)]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as feed:
        for message in expected:
            feed.sendto(message, ('127.0.0.1', input_port)); time.sleep(.01)
    actual = []; sink.settimeout(.5)
    while True:
        try: actual.append(sink.recv(65536))
        except socket.timeout: break
    # Explicit finite-Live rule: only an intact nonempty prefix; no inner gaps.
    assert actual and actual == expected[:len(actual)], (len(actual), len(expected))
    return len(actual), len(expected) - len(actual)


with tempfile.TemporaryDirectory(prefix='srtmux-app-providers-') as temporary:
    root = Path(temporary)
    wrong = subprocess.run([app, '--provider', 'robotweax', '--robotweax-module', hv, 'file://con', 'srt://127.0.0.1:9000'], capture_output=True, timeout=10)
    assert wrong.returncode != 0, 'Explicit module identity mismatch must fail'
    for encrypted in (False, True):
        security = f'&passphrase={secret}&pbkeylen=16' if encrypted else ''
        for sender_provider in ('robotweax', 'haivision'):
            for receiver_provider in ('robotweax', 'haivision'):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
                    sink.bind(('127.0.0.1', 0)); srt_port, udp_port = port(), port()
                    group = Processes(root)
                    try:
                        receiver = group.start('receiver', f'srt://127.0.0.1:{srt_port}?mode=listener&provider={receiver_provider}{security}', f'udp://127.0.0.1:{sink.getsockname()[1]}', '--provider', 'haivision' if receiver_provider == 'robotweax' else 'robotweax', '-a:no', '-s:1', '-pf:relay-json', '-statsout', str(root / 'stats.jsonl'))
                        group.ready(receiver, 'Listening source')
                        sender_uri = f'srt://127.0.0.1:{srt_port}?mode=caller{security}'
                        default_provider = sender_provider
                        if encrypted:
                            sender_uri += f'&provider={sender_provider}'
                            default_provider = 'haivision' if sender_provider == 'robotweax' else 'robotweax'
                        sender = group.start('sender', f'udp://127.0.0.1:{udp_port}', sender_uri, '--provider', default_provider, '-a:no')
                        group.ready(sender, 'SRT target connected'); group.ready(receiver, 'Accepted SRT source connection')
                        received, tail = feed_and_capture(udp_port, sink, b'matrix')
                        assert f'Loaded target provider={sender_provider}' in sender[1].read_text()
                        assert f'Loaded source provider={receiver_provider}' in receiver[1].read_text()
                        rows = [json.loads(line) for line in (root / 'stats.jsonl').read_text().splitlines()]
                        assert rows and rows[-1]['source']['provider'] == receiver_provider
                        assert all(secret not in log.read_text() for _, log in group.processes)
                        print(sender_provider, '->', receiver_provider, 'aes' if encrypted else 'plain', 'received', received, 'tail', tail, flush=True)
                    finally: group.close()
    for first, second in [('haivision', 'robotweax'), ('robotweax', 'haivision')]:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
            sink.bind(('127.0.0.1', 0)); left, right, udp_port = port(), port(), port()
            group = Processes(root)
            try:
                receiver = group.start('bridge-sink', f'srt://127.0.0.1:{right}?mode=listener&provider={second}', f'udp://127.0.0.1:{sink.getsockname()[1]}')
                group.ready(receiver, 'Listening source')
                bridge = group.start('bridge', f'srt://127.0.0.1:{left}?mode=listener&provider={first}', f'srt://127.0.0.1:{right}?provider={second}')
                group.ready(bridge, 'Listening source'); group.ready(bridge, 'SRT target connected')
                sender = group.start('bridge-feed', f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{left}?provider={first}')
                group.ready(sender, 'SRT target connected'); group.ready(bridge, 'Accepted SRT source connection')
                received, tail = feed_and_capture(udp_port, sink, b'bridge')
                assert f'Loaded source provider={first}' in bridge[1].read_text() and f'Loaded target provider={second}' in bridge[1].read_text()
                print('in-process', first, '->', second, 'received', received, 'tail', tail, flush=True)
            finally: group.close()
    if socket.has_ipv6:
        try:
            reservation = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            reservation.bind(('::1', 0)); listen_port = reservation.getsockname()[1]; reservation.close()
        except OSError:
            print('IPv6 SRT unavailable on this host')
        else:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
                sink.bind(('127.0.0.1', 0)); udp_port = port(); group = Processes(root)
                try:
                    receiver = group.start('ipv6-sink', f'srt://[::1]:{listen_port}?mode=listener&provider=robotweax', f'udp://127.0.0.1:{sink.getsockname()[1]}', '-a:no')
                    group.ready(receiver, 'Listening source')
                    sender = group.start('ipv6-feed', f'udp://127.0.0.1:{udp_port}', f'srt://[::1]:{listen_port}?provider=haivision', '-a:no')
                    group.ready(sender, 'SRT target connected'); group.ready(receiver, 'Accepted SRT source connection')
                    feed_and_capture(udp_port, sink, b'ipv6')
                finally: group.close()
    # A finite file feeds Live messages; accepted peer closure flushes the capture.
    for provider in ('haivision', 'robotweax'):
        source_path = root / 'finite-input.bin'; capture_path = root / 'finite-output.bin'
        payload = b''.join(hashlib.sha256(index.to_bytes(4, 'big')).digest() for index in range(256)) + b'partial-final-message'
        source_path.write_bytes(payload); listen_port = port(); group = Processes(root)
        receiver_provider = 'haivision' if provider == 'robotweax' else 'robotweax'
        try:
            receiver = group.start('finite-sink', f'srt://127.0.0.1:{listen_port}?mode=listener&provider={receiver_provider}', str(capture_path), '-a:no')
            group.ready(receiver, 'Listening source')
            sender = group.start('finite-feed', str(source_path), f'srt://127.0.0.1:{listen_port}?provider={provider}', '-a:no')
            group.ready(sender, 'SRT target connected')
            sender[0].wait(timeout=8); receiver[0].wait(timeout=8)
            captured = capture_path.read_bytes()
            assert captured and captured == payload[:len(captured)], (len(captured), len(payload))
            print('finite Live', provider, 'captured', len(captured), 'missing tail bytes', len(payload)-len(captured), flush=True)
        finally: group.close()
    # One persistent listener must accept a second independent sender session.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(('127.0.0.1', 0)); listen_port = port(); group = Processes(root)
        try:
            receiver = group.start('reconnect-sink', f'srt://127.0.0.1:{listen_port}?mode=listener&provider=haivision', f'udp://127.0.0.1:{sink.getsockname()[1]}')
            group.ready(receiver, 'Listening source')
            for index, provider in enumerate(('robotweax', 'haivision')):
                udp_port = port()
                sender = group.start('reconnect-feed-' + str(index), f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider={provider}', '-a:no')
                group.ready(sender, 'SRT target connected')
                feed_and_capture(udp_port, sink, b'session' + bytes([index]))
                sender[0].send_signal(signal.SIGINT); sender[0].wait(timeout=5)
                if index == 0: group.ready(receiver, 'SRT source disconnected')
        finally: group.close()
    # A persistent caller must reconnect after its listening sender is restarted.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(('127.0.0.1', 0)); listen_port = port(); group = Processes(root)
        try:
            receiver = group.start('caller-reconnect', f'srt://127.0.0.1:{listen_port}?provider=haivision&conntimeo=500', f'udp://127.0.0.1:{sink.getsockname()[1]}')
            for index, provider in enumerate(('robotweax', 'haivision')):
                udp_port = port()
                sender = group.start('listen-feed-' + str(index), f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?mode=listener&provider={provider}', '-a:no')
                group.ready(sender, 'Accepted SRT target connection'); group.ready(receiver, 'SRT source connected', index + 1)
                feed_and_capture(udp_port, sink, b'caller' + bytes([index]))
                sender[0].send_signal(signal.SIGINT); sender[0].wait(timeout=5)
                if index == 0: group.ready(receiver, 'SRT source disconnected')
        finally: group.close()
    # An initial encryption mismatch must fail, not fall back to plaintext.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
        sink.bind(('127.0.0.1', 0)); listen_port, udp_port = port(), port(); group = Processes(root)
        try:
            receiver = group.start('wrong-key-sink', f'srt://127.0.0.1:{listen_port}?mode=listener&provider=haivision&passphrase={secret}&pbkeylen=16', f'udp://127.0.0.1:{sink.getsockname()[1]}', '-a:no')
            group.ready(receiver, 'Listening source')
            sender = group.start('wrong-key-feed', f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider=robotweax&passphrase=different-public-fixture&pbkeylen=16', '-a:no')
            sender[0].wait(timeout=12)
            assert sender[0].returncode != 0 and secret not in sender[1].read_text()
        finally: group.close(check=False)
    # SRT endpoint statistics use the original's three formats on both sides.
    sections = {'window': ['flow', 'congestion', 'flight'], 'link': ['rtt', 'bandwidth', 'maxBandwidth'],
        'send': ['packets', 'packetsUnique', 'packetsLost', 'packetsDropped', 'packetsRetransmitted', 'packetsFilterExtra', 'bytes', 'bytesUnique', 'bytesDropped', 'byteAvailBuf', 'msBuf', 'mbitRate', 'sendPeriod'],
        'recv': ['packets', 'packetsUnique', 'packetsLost', 'packetsDropped', 'packetsRetransmitted', 'packetsBelated', 'packetsFilterExtra', 'packetsFilterSupply', 'packetsFilterLoss', 'bytes', 'bytesUnique', 'bytesLost', 'bytesDropped', 'byteAvailBuf', 'msBuf', 'mbitRate', 'msTsbPdDelay']}
    for provider in ('haivision', 'robotweax'):
        for style, extra in (('json', []), ('csv', []), ('default', []), ('json', ['-f'])):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
                sink.bind(('127.0.0.1', 0)); listen_port, udp_port = port(), port(); group = Processes(root)
                received_stats = root / 'receiver-stats.txt'; sent_stats = root / 'sender-stats.txt'
                try:
                    receiver = group.start('stats-sink', f'srt://127.0.0.1:{listen_port}?mode=listener&provider={provider}', f'udp://127.0.0.1:{sink.getsockname()[1]}', '-s:4', '-pf:' + style, '-statsout', str(received_stats), *extra)
                    group.ready(receiver, 'Listening source')
                    sender = group.start('stats-feed', f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider={provider}', '-s:4', '-r:8', '-pf:' + style, '-statsout', str(sent_stats), *extra)
                    group.ready(sender, 'SRT target connected'); group.ready(receiver, 'Accepted SRT source connection')
                    received, _ = feed_and_capture(udp_port, sink, b'stats')
                finally: group.close()
                sent_text, received_text = sent_stats.read_text(), received_stats.read_text()
                assert ('{"bandwidth":' if style == 'json' else '+++/+++SRT BANDWIDTH: ') in sender[1].read_text()
                if style == 'json':
                    sent_rows = [json.loads(line) for line in sent_text.splitlines() if line]
                    received_rows = [json.loads(line) for line in received_text.splitlines() if line]
                    assert len(sent_rows) == 6 and len(received_rows) == received // 4, (len(sent_rows), len(received_rows), received)
                    for row in sent_rows + received_rows:
                        assert list(row) == ['sid', 'timepoint', 'time', 'window', 'link', 'send', 'recv'], list(row)
                        assert all(list(row[name]) == keys for name, keys in sections.items()), row
                    totals = [row['send']['packets'] for row in sent_rows]
                    # Native counters trail the application by the packets still queued for the wire.
                    if extra: assert totals == sorted(totals) and totals[-1] >= 20, totals       # -f: running totals.
                    else: assert sum(totals) >= 20 and max(totals[1:]) <= 8, totals              # Default: per-report intervals.
                elif style == 'csv':
                    header = 'Timepoint,Time,SocketID,Time,pktFlowWindow,pktCongestionWindow,pktFlightSize,msRTT,mbpsBandwidth,mbpsMaxBW,pktSent,pktSentUnique,pktSndLoss,pktSndDrop,pktRetrans,pktSndFilterExtra,byteSent,byteSentUnique,byteSndDrop,byteAvailSndBuf,msSndBuf,mbpsSendRate,usPktSndPeriod,pktRecv,pktRecvUnique,pktRcvLoss,pktRcvDrop,pktRcvRetrans,pktRcvBelated,pktRcvFilterExtra,pktRcvFilterSupply,pktRcvFilterLoss,byteRecv,byteRecvUnique,byteRcvLoss,byteRcvDrop,byteAvailRcvBuf,msRcvBuf,mbpsRecvRate,msRcvTsbPdDelay'
                    lines = sent_text.splitlines()
                    assert lines[0] == header and len(lines) == 7 and all(len(line.split(',')) == 40 for line in lines), lines[:2]
                else:
                    assert sent_text.count('======= SRT STATS: sid=') == 6 and 'PACKETS     SENT: ' in sent_text and 'BUFFERLEFT:  SND: ' in sent_text, sent_text[:400]
                print('statistics', provider, style, *extra, flush=True)
    # Robotweax null packet deletion: bit-exact transport stream delivery with
    # nulls removed on the wire, unchanged service for a caller without it, and
    # an explicit failure on a provider that lacks it.
    def unit(index, null):
        return b'\x47\x1f\xff\x10' + b'\xff' * 184 if null else b'\x47\x01' + bytes([index % 256, 0x10]) + bytes((index + offset) % 256 for offset in range(184))
    stream = [b''.join(unit(message * 7 + position, position % 2 == 1) for position in range(7)) for message in range(24)]
    for caller_provider, caller_policy, expected in (('robotweax', '&npd=required', 'active'), ('haivision', '', 'not negotiated')):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
            sink.bind(('127.0.0.1', 0)); sink.settimeout(.5); listen_port, udp_port = port(), port(); group = Processes(root)
            sender_stats = root / 'npd-sender.jsonl'
            try:
                receiver = group.start('npd-sink', f'srt://127.0.0.1:{listen_port}?mode=listener&provider=robotweax&npd=if_offered', f'udp://127.0.0.1:{sink.getsockname()[1]}')
                group.ready(receiver, 'Listening source')
                sender = group.start('npd-feed', f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider={caller_provider}{caller_policy}', '-s:1', '-pf:relay-json', '-statsout', str(sender_stats))
                group.ready(sender, 'SRT target connected'); group.ready(receiver, 'Accepted SRT source connection')
                group.ready(receiver, 'Null packet deletion ' + expected + ' on source')
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as feed:
                    for message in stream:
                        feed.sendto(message, ('127.0.0.1', udp_port)); time.sleep(.01)
                actual = []
                while True:
                    try: actual.append(sink.recv(65536))
                    except socket.timeout: break
                assert actual and actual == stream[:len(actual)], (len(actual), len(stream))
                rows = [json.loads(line) for line in sender_stats.read_text().splitlines()]
                if caller_policy:
                    assert rows[-1]['target']['robotweax.npd.active'] == 1 and rows[-1]['target']['robotweax.npd.deleted_units'] == 3 * rows[-1]['written_packets'], rows[-1]
                else:
                    assert 'robotweax.npd.active' not in rows[-1]['target'], rows[-1]
                print('null packet deletion', caller_provider, expected, 'received', len(actual), flush=True)
            finally: group.close()
    unsupported = subprocess.run([*prefix, '-a:no', 'file://con', f'srt://127.0.0.1:{port()}?provider=haivision&npd=required'], input=b'', capture_output=True, timeout=10)
    assert unsupported.returncode != 0 and b'not available in this provider' in unsupported.stderr, unsupported.stderr
    # With reconnect enabled a wrong passphrase is retried as in the original,
    # reported once with its reason, and never spins or falls back to plaintext.
    for provider in ('haivision', 'robotweax'):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
            sink.bind(('127.0.0.1', 0)); listen_port, udp_port = port(), port(); group = Processes(root)
            try:
                receiver = group.start('retry-key-sink-' + provider, f'srt://127.0.0.1:{listen_port}?mode=listener&provider=haivision&passphrase={secret}&pbkeylen=16', f'udp://127.0.0.1:{sink.getsockname()[1]}')
                group.ready(receiver, 'Listening source')
                sender = group.start('retry-key-feed-' + provider, f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider={provider}&passphrase=different-public-fixture&pbkeylen=16')
                group.ready(sender, 'wrong or missing passphrase')
                time.sleep(2.5)
                text = sender[1].read_text()
                assert sender[0].poll() is None and text.count('connection failed') == 1 and 'SRT target connected' not in text, text
            finally: group.close()
    # A listener serves one client at a time, and an idle connected SRT source
    # must wait in the poller rather than spin.
    for provider in ('haivision', 'robotweax'):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sink:
            sink.bind(('127.0.0.1', 0)); listen_port, udp_port, second_port = port(), port(), port(); group = Processes(root)
            try:
                receiver = group.start('single-sink-' + provider, f'srt://127.0.0.1:{listen_port}?mode=listener&provider={provider}', f'udp://127.0.0.1:{sink.getsockname()[1]}')
                group.ready(receiver, 'Listening source')
                sender = group.start('single-feed-' + provider, f'udp://127.0.0.1:{udp_port}', f'srt://127.0.0.1:{listen_port}?provider={provider}')
                group.ready(sender, 'SRT target connected'); group.ready(receiver, 'Accepted SRT source connection')
                extra = group.start('single-extra-' + provider, f'udp://127.0.0.1:{second_port}', f'srt://127.0.0.1:{listen_port}?provider={provider}&conntimeo=500')
                def cpu_seconds(process):
                    minutes, seconds = subprocess.check_output(['ps', '-o', 'time=', '-p', str(process.pid)], text=True).strip().rsplit(':', 1)
                    return sum(int(part) * 60 ** index for index, part in enumerate(reversed(minutes.split(':')), 1)) + float(seconds)
                before = cpu_seconds(receiver[0]); time.sleep(3); used = cpu_seconds(receiver[0]) - before
                assert used < 0.5, f'idle connected SRT source used {used:.2f} s CPU in 3 s'
                assert 'SRT target connected' not in extra[1].read_text(), 'second caller was accepted while the listener was busy'
                feed_and_capture(udp_port, sink, b'single')
                print('single client', provider, 'idle CPU seconds in 3 s:', round(used, 2), flush=True)
            finally: group.close()
print('Provider matrix, AES/rejection, IPv6, native snapshots, mixed-provider bridges and both reconnect roles, retry reporting, single-client listening and idle waiting pass')
