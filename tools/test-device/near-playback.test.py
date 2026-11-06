#!/usr/bin/env python3
"""Loopback playback contract; subprocesses are fakes and no speaker is used."""
import concurrent.futures
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import wave

SPEC = importlib.util.spec_from_file_location('near_playback', Path(__file__).with_name('near-playback.py'))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
TOKEN = 'a' * 64


def wav_bytes():
    output = io.BytesIO()
    with wave.open(output, 'wb') as stream:
        stream.setparams((1, 2, 24000, 0, 'NONE', 'not compressed'))
        stream.writeframes(b'\0\0' * 24000)
    return output.getvalue()


class FakeProcess:
    def __init__(self, *, blocked=False, timeout=False):
        self.returncode = None
        self.timeout = timeout
        self.started = threading.Event()
        self.finished = threading.Event()
        self.killed = False
        if not blocked:
            self.finished.set()

    def wait(self, timeout):
        self.started.set()
        if self.timeout:
            self.timeout = False
            raise subprocess.TimeoutExpired('afplay', timeout)
        if not self.finished.wait(timeout):
            raise subprocess.TimeoutExpired('afplay', timeout)
        self.returncode = -9 if self.killed else 0
        return self.returncode

    def poll(self):
        return self.returncode

    def kill(self):
        self.killed = True
        self.finished.set()


class PlaybackTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.wav = Path(self.directory.name) / 'near.wav'
        self.wav.write_bytes(wav_bytes())
        self.process = FakeProcess()
        self.commands = []
        def popen(command, **kwargs):
            self.commands.append((command, kwargs))
            return self.process
        self.playback = MODULE.FixedPlayback(self.wav, popen=popen, timeout=0.5)
        self.server = MODULE.PlaybackServer(0, self.playback, TOKEN)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01})
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.playback.close()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())
        self.assertFalse(self.playback.directory.exists())
        self.directory.cleanup()

    def request(self, value=None, *, token=TOKEN, headers=None, path='/play'):
        body = json.dumps({'sha256': self.playback.digest} if value is None else value).encode()
        request = Request(f'http://127.0.0.1:{self.server.server_port}{path}', data=body,
                          headers={'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token,
                                   **(headers or {})})
        try:
            with urlopen(request, timeout=2) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            return error.code, json.load(error)

    def test_fixed_copy_and_completion(self):
        original = self.wav.read_bytes()
        self.wav.write_bytes(b'changed after staging')
        status, result = self.request()
        self.assertEqual(status, 200)
        self.assertEqual(result, {'ok': True, 'sha256': hashlib.sha256(original).hexdigest()})
        self.assertEqual(self.playback.source.read_bytes(), original)
        self.assertEqual(self.commands[0][0], ['/usr/bin/afplay', str(self.playback.source)])
        self.assertEqual(self.playback.source.stat().st_mode & 0o777, 0o400)

    def test_auth_hash_and_request_boundaries(self):
        cases = [({'token': 'b' * 64}, 401), ({'value': {'sha256': '0' * 64}}, 409),
                 ({'token': 'é'}, 401),
                 ({'value': {'sha256': self.playback.digest, 'path': '/etc/passwd'}}, 400),
                 ({'value': {'sha256': 'x' * 129}}, 413), ({'path': '/anything'}, 403),
                 ({'headers': {'Origin': 'http://localhost'}}, 403),
                 ({'headers': {'Host': 'example.com'}}, 403)]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                self.assertEqual(self.request(**arguments)[0], expected)
        self.assertEqual(self.commands, [])

    def test_reentrant_request_rejected_without_queue(self):
        self.process = FakeProcess(blocked=True)
        with concurrent.futures.ThreadPoolExecutor() as executor:
            first = executor.submit(self.request)
            self.assertTrue(self.process.started.wait(1))
            self.assertEqual(self.request()[0], 409)
            self.assertFalse(first.done())
            self.process.finished.set()
            self.assertEqual(first.result(timeout=1)[0], 200)
        self.assertEqual(len(self.commands), 1)

    def test_playback_timeout_is_killed_and_not_retried(self):
        self.process = FakeProcess(timeout=True)
        self.assertEqual(self.request()[0], 504)
        self.assertTrue(self.process.killed)
        self.assertEqual(len(self.commands), 1)

    def test_spawn_failure_is_explicit(self):
        def fail(*_args, **_kwargs):
            raise OSError('test spawn failure')
        self.playback.popen = fail
        self.assertEqual(self.request(), (502, {'ok': False, 'error': 'Playback could not start'}))

    def test_close_cancels_playback_and_cleans_copy(self):
        self.process = FakeProcess(blocked=True)
        with concurrent.futures.ThreadPoolExecutor() as executor:
            first = executor.submit(self.request)
            self.assertTrue(self.process.started.wait(1))
            self.playback.close()
            self.assertEqual(first.result(timeout=1)[0], 502)
        self.assertTrue(self.process.killed)
        self.assertFalse(self.playback.directory.exists())
        self.assertEqual(self.request()[0], 503)

    def test_bounded_play_count(self):
        self.playback.max_plays = 1
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.request()[0], 429)
        self.assertEqual(len(self.commands), 1)

    def test_invalid_source_and_token_fail_closed(self):
        with self.assertRaises(ValueError):
            MODULE.PlaybackServer(0, self.playback, '')
        self.wav.write_bytes(b'\0' * (1024 * 1024 + 1))
        with self.assertRaises(ValueError):
            MODULE.FixedPlayback(self.wav)


if __name__ == '__main__':
    unittest.main()
