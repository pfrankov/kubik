#!/usr/bin/env python3
"""Temporary byte-only relay for the radio fixture; TLS terminates only at the local mock."""
import ipaddress
import json
import os
import select
import signal
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

MAX_CONNECTIONS = 32
MAX_ACCEPTED_CONNECTIONS = 128
MAX_BYTES = 16 * 1024 * 1024
LIFETIME_SECONDS = 600


def pump(peer, tunnel_port, stop):
    try:
        with peer, socket.create_connection(("127.0.0.1", tunnel_port), timeout=5) as target:
            peers = {peer: target, target: peer}
            for endpoint in peers:
                endpoint.settimeout(5)
                endpoint.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            total = 0
            while not stop.is_set() and total <= MAX_BYTES:
                ready, _, _ = select.select(list(peers), [], [], 1)
                for source in ready:
                    data = source.recv(16384)
                    if not data: return
                    total += len(data)
                    if total > MAX_BYTES: return
                    peers[source].sendall(data)
    except OSError:
        pass


def serve_connections(listener, tunnel_port, stop):
    slots = threading.BoundedSemaphore(MAX_CONNECTIONS)
    deadline = time.monotonic() + LIFETIME_SECONDS
    accepted = 0

    def serve(peer):
        try: pump(peer, tunnel_port, stop)
        finally: slots.release()

    while not stop.is_set() and time.monotonic() < deadline and accepted < MAX_ACCEPTED_CONNECTIONS:
        ready, _, _ = select.select([listener, sys.stdin.fileno()], [], [], 1)
        if sys.stdin.fileno() in ready and not os.read(sys.stdin.fileno(), 1024): break
        if listener not in ready: continue
        try: peer, _ = listener.accept()
        except socket.timeout: continue
        accepted += 1
        if not slots.acquire(blocking=False):
            peer.close()
            continue
        threading.Thread(target=serve, args=(peer,), daemon=True).start()


def remove_rule(rule):
    command = ["iptables", "-C", "INPUT", *rule]
    result = subprocess.run(command, timeout=5)
    if result.returncode == 0:
        subprocess.run(["iptables", "-D", "INPUT", *rule], check=True, timeout=5)
    elif result.returncode != 1:
        raise subprocess.CalledProcessError(result.returncode, command)


def main():
    tunnel_port, public_host = int(sys.argv[1]), str(ipaddress.IPv4Address(sys.argv[2]))
    if not 1 <= tunnel_port <= 65535: raise ValueError("Invalid tunnel port")
    stop = threading.Event()
    for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(signum, lambda _signum, _frame: stop.set())
    with socket.socket() as listener:
        listener.bind((public_host, 0))
        listener.listen(MAX_CONNECTIONS)
        listener.settimeout(1)
        port = listener.getsockname()[1]
        expires = datetime.fromtimestamp(time.time() + LIFETIME_SECONDS, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
        rule = ["-d", public_host, "-p", "tcp", "--dport", str(port), "-m", "time", "--datestop", expires,
                "-m", "comment", "--comment", "kubik-radio-fixture", "-j", "ACCEPT"]
        allow_port = len(sys.argv) > 3 and sys.argv[3] == "--allow-port"
        try:
            if allow_port: subprocess.run(["iptables", "-I", "INPUT", "1", *rule], check=True, timeout=5)
            print(json.dumps({"port": port}), flush=True)
            serve_connections(listener, tunnel_port, stop)
        finally:
            stop.set()
            if allow_port: remove_rule(rule)


if __name__ == "__main__": main()
