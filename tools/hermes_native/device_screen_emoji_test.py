"""Screen fixture controls/metadata only; no physical display, voice or print simulation claims."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

from device import ROOT, arguments, copy_private_color
from device_screen_emoji import MARKER, ONLY_MARKER, ONLY_TEXT, Observations, SEQUENCES, ScreenConnection, TEXT, pages, screen_answer


class ScreenFixtureTests(unittest.IsolatedAsyncioTestCase):
    def test_actual_model_response_preserves_multi_page_and_emoji_only_text(self):
        self.assertEqual(screen_answer([{'role': 'user', 'content': MARKER + 'new'}])[0]['content'], TEXT)
        self.assertEqual(screen_answer([{'role': 'user', 'content': ONLY_MARKER + 'new'}])[0]['content'], ONLY_TEXT)
        self.assertGreater(len(SEQUENCES), 8)
        self.assertIsNone(screen_answer([{'role': 'user', 'content': 'unrelated'}]))

    async def test_live_model_commentary_roundtrip_emits_one_complete_assistant_transcript(self):
        socket = SimpleNamespace(send_json=AsyncMock())
        connection = ScreenConnection('live', socket)
        await connection.respond({'type': 'session.commentary.append', 'event_id': 'a', 'content': TEXT[:20]})
        self.assertFalse(connection.transcript_sent)
        await connection.respond({'type': 'session.commentary.append', 'event_id': 'b', 'content': TEXT[20:]})
        events = [row.args[0] for row in socket.send_json.await_args_list]
        transcripts = [event for event in events if event['type'] == 'session.output_transcript.delta']
        self.assertEqual([event['delta'] for event in transcripts], [TEXT])

    async def test_fixture_blocks_every_printer_rpc_before_forwarding(self):
        adapter = SimpleNamespace(command=AsyncMock(), authenticate=AsyncMock())
        observation = Observations(adapter)
        socket = SimpleNamespace(send_json=AsyncMock())
        observation.watch_socket(socket)
        with self.assertRaisesRegex(AssertionError, 'forbids every printer'):
            await socket.send_json({'t': 'printer', 'action': 'print', 'text': 'not allowed'})
        self.assertEqual(observation.senders[0][1].await_count, 0)

    def test_private_color_state_stays_on_same_host_with_owner_only_permissions(self):
        specification = importlib.util.spec_from_file_location('public_color_fixture', ROOT / 'tools/fixtures/hermes_color_emoji_support.py')
        fixture = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(fixture)
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            source, target = evidence / 'owner/kubik', evidence / 'fixture/kubik'
            source.mkdir(parents=True); target.mkdir(parents=True)
            data = fixture.atlas(SEQUENCES)
            (source / 'apple-emoji-color.bin').write_bytes(data)
            copy_private_color(SimpleNamespace(tls_identity=source / 'tls.json', evidence=evidence), target)
            self.assertEqual((target / 'apple-emoji-color.bin').read_bytes(), data)
            self.assertEqual((target / 'apple-emoji-color.bin').stat().st_mode & 0o777, 0o600)
            self.assertTrue(json.loads((evidence / 'apple-color-atlas.json').read_text())['private_state'])

    def test_screen_mode_is_explicit_and_excludes_all_print_modes(self):
        argv = ['device.py', '--source', '/tmp/hermes', '--tls-identity', '/tmp/tls.json',
                '--control', 'http://127.0.0.1:18792', '--evidence', '/tmp/new', '--screen-emoji-check']
        with patch('sys.argv', argv):
            self.assertTrue(arguments().screen_emoji_check)
        for mode in ('--printer', '--printer-live-status', '--printer-stream-check', '--printer-quality-check'):
            with patch('sys.argv', argv + [mode]), patch('sys.stderr'):
                with self.assertRaises(SystemExit):
                    arguments()

    async def test_all_four_actual_pages_and_tenth_glyph_are_required(self):
        observation = SimpleNamespace(states=[], frames=[{'page': page} for page in range(4)],
                                      wire=[{'value': {'t': 'text', 'text': TEXT}}], persist_wire=lambda: None)
        visited = []
        async def info():
            index = len(visited)
            page = (0, 0, 1, 2, 3)[min(index, 4)]
            visited.append(page)
            count = 10 if page == 3 else min(9, 3 * (page + 1))
            observation.wire = observation.wire[:1] + [{'value': {'t': 'emoji', 'seq': sequence, 'data': 'present'}}
                                                      for sequence in SEQUENCES[:count]]
            return {'emoji_id': page + 1, 'emoji_count': 1, 'emoji_ready': 1, 'emoji_missing': 0,
                    'card_page': page, 'card_pages': 4, 'heap_internal': 10000, 'heap_failures': 0,
                    'live_active': True, 'mic_open': True, 'mic_rx_enabled': True}
        ready = await pages(SimpleNamespace(info=info), observation)
        self.assertEqual(ready, {0, 1, 2, 3})
        self.assertEqual(visited, [0, 0, 1, 2, 3])


if __name__ == '__main__':
    unittest.main()
