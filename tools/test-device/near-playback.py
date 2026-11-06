#!/usr/bin/env python3
"""Play one fixed WAV on this Mac for a remote, SSH-forwarded acoustic fixture.

Set KUBIK_TEST_NEAR_TOKEN to 64 random hex characters on both hosts. Run:
  python3 tools/test-device/near-playback.py --wav /private/near.wav --port 18998
Forward that loopback port with SSH -R 127.0.0.1:18998:127.0.0.1:18998.
On the fixture host set KUBIK_TEST_NEAR_PLAY_URL=http://127.0.0.1:18998/play
and KUBIK_TEST_NEAR_WAV to an exact copy of the WAV. No TLS keys leave that host.
The helper exits after 15 minutes; Ctrl-C/SIGTERM cancels active playback.
"""
import argparse
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit
import wave

MAX_WAV_BYTES = 1024 * 1024


class PlaybackError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def wav_payload(path):
    with path.open('rb') as stream:
        data = stream.read(MAX_WAV_BYTES + 1)
    if len(data) > MAX_WAV_BYTES:
        raise ValueError('Near-end WAV exceeds 1 MiB')
    with wave.open(io.BytesIO(data), 'rb') as audio:
        if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getcomptype()) != (1, 2, 24000, 'NONE'):
            raise ValueError('Near-end WAV must be mono PCM16 at 24 kHz')
        frames = audio.getnframes()
        if not 24000 <= frames <= 20 * 24000:
            raise ValueError('Near-end WAV must last between 1 and 20 seconds')
        if len(audio.readframes(frames)) != frames * 2:
            raise ValueError('Near-end WAV is truncated')
    return data


class FixedPlayback:
    def __init__(self, source, *, popen=subprocess.Popen, timeout=25, max_plays=8):
        data = wav_payload(Path(source))
        self.digest = hashlib.sha256(data).hexdigest()
        self.directory = Path(tempfile.mkdtemp(prefix='kubik-near-play-'))
        self.source = self.directory / 'source.wav'
        self.source.write_bytes(data)
        self.source.chmod(0o400)
        self.popen, self.timeout, self.max_plays = popen, timeout, max_plays
        self.plays = 0
        self.closed = False
        self.process = None
        self.guard = threading.Lock()
        self.play_lock = threading.Lock()

    def _run(self):
        with self.guard:
            if self.closed:
                raise PlaybackError(503, 'Playback helper stopped')
            try:
                self.process = self.popen(['/usr/bin/afplay', str(self.source)],
                                          stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
            except OSError as error:
                raise PlaybackError(502, 'Playback could not start') from error
            process = self.process
        try:
            process.wait(timeout=self.timeout)
            if process.returncode != 0:
                raise PlaybackError(502, 'Playback failed')
        except subprocess.TimeoutExpired as error:
            process.kill()
            process.wait(timeout=2)
            raise PlaybackError(504, 'Playback timed out') from error
        finally:
            with self.guard:
                self.process = None

    def play(self, digest):
        if not hmac.compare_digest(self.digest, digest):
            raise PlaybackError(409, 'WAV digest differs')
        if not self.play_lock.acquire(blocking=False):
            raise PlaybackError(409, 'Playback already active')
        try:
            if self.plays >= self.max_plays:
                raise PlaybackError(429, 'Playback limit reached')
            self.plays += 1
            self._run()
            return {'ok': True, 'sha256': self.digest}
        finally:
            self.play_lock.release()

    def close(self):
        with self.guard:
            self.closed = True
            process = self.process
            if process and process.poll() is None:
                process.kill()
        if process:
            process.wait(timeout=2)
        shutil.rmtree(self.directory, ignore_errors=True)


class PlaybackServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 4

    def __init__(self, port, playback, token):
        if not re.fullmatch(r'[a-f0-9]{64}', token):
            raise ValueError('KUBIK_TEST_NEAR_TOKEN must contain 64 random lowercase hex characters')
        self.playback, self.token = playback, token
        self.slots = threading.BoundedSemaphore(4)
        super().__init__(('127.0.0.1', port), PlaybackHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.close_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class PlaybackHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(2)

    def log_message(self, *_args):
        pass  # Neither authorization headers nor paths belong in the log.

    def _validate(self):
        if self.path != '/play' or self.headers.get('Origin'):
            raise PlaybackError(403, 'Unsupported playback request')
        host = urlsplit('http://' + self.headers.get('Host', '')).hostname
        if host not in ('127.0.0.1', 'localhost'):
            raise PlaybackError(403, 'Loopback Host required')
        authorization = self.headers.get('Authorization', '')
        if not hmac.compare_digest(authorization.encode(), ('Bearer ' + self.server.token).encode()):
            raise PlaybackError(401, 'Playback authorization required')
        if self.headers.get('Transfer-Encoding') or self.headers.get('Content-Type') != 'application/json':
            raise PlaybackError(400, 'JSON body required')
        length = int(self.headers.get('Content-Length', '0'))
        if not 1 <= length <= 128:
            raise PlaybackError(413, 'Playback body exceeds bound')
        value = json.loads(self.rfile.read(length))
        if not isinstance(value, dict) or set(value) != {'sha256'}:
            raise PlaybackError(400, 'Only a WAV digest is accepted')
        if not isinstance(value['sha256'], str) or not re.fullmatch('[a-f0-9]{64}', value['sha256']):
            raise PlaybackError(400, 'Invalid WAV digest')
        return value['sha256']

    def do_POST(self):
        try:
            digest = self._validate()
            result = self.server.playback.play(digest)
            status = 200
        except PlaybackError as error:
            status, result = error.status, {'ok': False, 'error': str(error)}
        except (ValueError, OSError):
            status, result = 400, {'ok': False, 'error': 'Invalid playback request'}
        data = json.dumps(result).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except OSError:
            pass  # Playback is never retried after an ambiguous client disconnect.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wav', required=True, type=Path)
    parser.add_argument('--port', type=int, default=18998)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('Physical playback requires macOS afplay')
    playback = FixedPlayback(args.wav)
    server = timer = None
    try:
        server = PlaybackServer(args.port, playback, os.environ.get('KUBIK_TEST_NEAR_TOKEN', ''))
        stop = lambda *_: threading.Thread(target=server.shutdown, daemon=True).start()
        signal.signal(signal.SIGINT, stop)
        signal.signal(signal.SIGTERM, stop)
        timer = threading.Timer(900, server.shutdown)
        timer.daemon = True
        timer.start()
        print(f'Ready http://127.0.0.1:{server.server_port}/play sha256={playback.digest}', flush=True)
        server.serve_forever(poll_interval=0.1)
    finally:
        playback.close()
        if timer:
            timer.cancel()
        if server:
            server.server_close()


if __name__ == '__main__':
    main()
