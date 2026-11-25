"""Verify the extracted Windows release with UDP -> SRT -> UDP on loopback."""
from contextlib import ExitStack
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve() / "bin/srt-live-transmit.exe"


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start(stack, cwd, name, *args):
    log = cwd / (name + ".log")
    stream = stack.enter_context(log.open("wb"))
    process = subprocess.Popen([app, "-v", "-t:12", *args], cwd=cwd, stderr=stream)
    def cleanup():
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)
    stack.callback(cleanup)
    return process, log


def wait_for(item, message):
    process, log = item
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and process.poll() is None:
        if message in log.read_text(errors="replace"):
            return
        time.sleep(.025)
    raise RuntimeError(log.read_text(errors="replace"))


with tempfile.TemporaryDirectory() as directory:
    cwd = Path(directory)
    payload = bytes(range(256)) * 32
    source = cwd / "sample input.ts"
    capture = cwd / "sample output.ts"
    source.write_bytes(payload)
    capture.write_bytes(payload * 2)
    subprocess.run([app, str(source), str(capture)], cwd=cwd, check=True, timeout=8)
    assert capture.read_bytes() == payload
    result = subprocess.run([app, "file://con", "file://con"], cwd=cwd,
                            input=payload, capture_output=True, check=True, timeout=8)
    assert result.stdout == payload

for sender, receiver in [("haivision", "robotweax"), ("robotweax", "haivision")]:
    for encryption in ["", "&passphrase=windows-test-secret&pbkeylen=16"]:
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            cwd = Path(directory)
            sink = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            sink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 512 * 1024)
            sink.bind(("127.0.0.1", 0))
            sink.settimeout(4)
            port = free_port()
            rx = start(stack, cwd, "receiver", "--provider", receiver,
                       f"srt://127.0.0.1:{port}?mode=listener{encryption}",
                       f"udp://127.0.0.1:{sink.getsockname()[1]}")
            wait_for(rx, "Listening source")
            ingress = free_port()
            tx = start(stack, cwd, "sender", "--provider", sender,
                       f"udp://127.0.0.1:{ingress}", f"srt://127.0.0.1:{port}?mode=caller{encryption}")
            wait_for(tx, "SRT target connected")
            wait_for(rx, "Accepted SRT source connection")
            try:
                feed = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
                expected = [bytes([i]) * 1316 for i in range(32)]
                for packet in expected + [b"end-of-test"] * 16:
                    feed.sendto(packet, ("127.0.0.1", ingress))
                    time.sleep(.012)
                for packet in expected:
                    assert sink.recv(65535) == packet, "Missing, corrupt or reordered payload"
                for item, side, provider in [(tx, "target", sender), (rx, "source", receiver)]:
                    assert f"Loaded {side} provider={provider} " in item[1].read_text()
            except Exception:
                print(tx[1].read_text(), rx[1].read_text(), flush=True)
                raise
            print(sender, "->", receiver, "AES-128" if encryption else "plain",
                  "32/32 identical 1316-byte datagrams", flush=True)
