#!/usr/bin/env python3
"""Actual local TCP relay: byte fidelity, stdin EOF and signal cleanup; no SSH or firewall changes."""
import json
import os
from pathlib import Path
import signal
import select
import socket
import subprocess
import sys
import threading
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parent.parent


def echo(server):
    peer, _ = server.accept()
    with peer:
        while True:
            data = peer.recv(16384)
            if not data: break
            peer.sendall(data)


def start(target):
    process = subprocess.Popen([sys.executable, "-u", str(ROOT / "tools/radio-relay.py"),
                                str(target.getsockname()[1]), "127.0.0.1"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert select.select([process.stdout], [], [], 5)[0], "relay startup timed out"
        return process, json.loads(process.stdout.readline())["port"]
    except BaseException:
        close(process)
        raise


def close(process):
    if process.poll() is None: process.kill(); process.wait(timeout=3)
    for pipe in (process.stdin, process.stdout, process.stderr): pipe.close()


def check(interrupt):
    with socket.socket() as target:
        target.bind(("127.0.0.1", 0))
        target.listen(1)
        worker = threading.Thread(target=echo, args=(target,), daemon=True)
        worker.start()
        process, port = start(target)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as peer:
                payload = bytes(range(256)) * 400
                peer.sendall(payload)
                returned = b""
                while len(returned) < len(payload):
                    chunk = peer.recv(16384)
                    assert chunk, "relay closed before delivery"
                    returned += chunk
                assert returned == payload
            if interrupt: process.send_signal(signal.SIGTERM)
            else: process.stdin.close()
            assert process.wait(timeout=5) == 0, process.stderr.read().decode()
            worker.join(timeout=3)
            assert not worker.is_alive()
            with socket.socket() as probe:
                probe.settimeout(1)
                assert probe.connect_ex(("127.0.0.1", port)) != 0
        finally:
            close(process)


def connection_budget():
    with socket.socket() as target:
        target.bind(("127.0.0.1", 0)); target.listen(128)
        target.settimeout(3)
        def drain():
            for _ in range(127): echo(target)
        worker = threading.Thread(target=drain, daemon=True)
        worker.start()
        process, port = start(target)
        try:
            # Complete each route before opening the next: test lifetime, not active-slot overload.
            for _ in range(127):
                with socket.create_connection(("127.0.0.1", port), timeout=3) as peer:
                    peer.sendall(b"x")
                    assert peer.recv(1) == b"x"
            worker.join(timeout=3)
            assert not worker.is_alive(), "target fixture did not drain"
            # The final accept ends the fixture's lifetime and can reset its peer on shutdown.
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=3): pass
            except ConnectionResetError:
                pass
            assert process.wait(timeout=5) == 0, process.stderr.read().decode()
            with socket.socket() as probe:
                assert probe.connect_ex(("127.0.0.1", port)) != 0
        finally:
            close(process)


def partial_firewall_failure():
    # A failed insertion can still have taken effect. Exercise cleanup without touching a real firewall.
    with TemporaryDirectory(prefix="kubik-relay-firewall-") as directory, socket.socket() as target:
        root = Path(directory); marker = root / "rule"; command = root / "iptables"
        command.write_text(f"#!{sys.executable}\n" + """import os,pathlib,sys
p=pathlib.Path(os.environ['KUBIK_RELAY_TEST_RULE'])
if sys.argv[1]=='-I': p.touch(); sys.exit(1)
if sys.argv[1]=='-C': sys.exit(0 if p.exists() else 1)
if sys.argv[1]=='-D': p.unlink(); sys.exit(0)
sys.exit(2)
""")
        command.chmod(0o700)
        target.bind(("127.0.0.1", 0)); target.listen(1)
        environment = {**os.environ, "PATH": directory + os.pathsep + os.environ["PATH"],
                       "KUBIK_RELAY_TEST_RULE": str(marker)}
        result = subprocess.run([sys.executable, str(ROOT / "tools/radio-relay.py"),
                                 str(target.getsockname()[1]), "127.0.0.1", "--allow-port"],
                                env=environment, capture_output=True, timeout=10)
        assert result.returncode != 0 and b"CalledProcessError" in result.stderr
        assert not marker.exists(), "partial firewall insertion survived failed startup"


check(False)
check(True)
connection_budget()
partial_firewall_failure()
print("radio relay: binary fidelity, EOF/SIGTERM, lifetime cap and partial firewall failure cleanup passed")
