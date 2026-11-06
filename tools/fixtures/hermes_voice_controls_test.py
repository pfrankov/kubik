"""Profile/key-isolated passive native voice catalogues and persistent selections."""
import asyncio
import contextlib
import json
import os
from pathlib import Path
from types import ModuleType, SimpleNamespace
import sys
from unittest.mock import patch

from hermes_test_support import (DeviceSocket, KubikAdapter, Peer, ProtocolFixture,
                                atomic_json)
from hermes_kubik.runtime import dependencies_available
from hermes_kubik.voice_settings import VoicePreferences, native_settings, native_status


class VoiceControlsContract(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        self.adapter.speech_available = lambda: ({'available': True}, {'available': False})
        self.identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [self.identity])
        self.peer = Peer(DeviceSocket(), self.identity, self.hello)
        self.peer.ready = True
        self.adapter.peers[self.identity] = self.peer
        status = lambda home, mode, selected=None: {'available': True, 'model':
            'gpt-live-1' if mode == 'live' else 'gpt-realtime-2.1', 'provider': 'openai'}
        patcher = patch('hermes_kubik.voice_controls.native_status', side_effect=status)
        patcher.start()
        self.addCleanup(patcher.stop)

    async def test_catalogue_selection_order_persists_only_for_profile_device_key(self):
        controls = self.adapter.voice_controls
        self.assertEqual(controls.mode(self.peer), 'classic')
        await controls.handle(self.peer, {'t': 'agent_options', 'target': 'mode', 'rid': 1})
        row = self.peer.socket.messages[-1]
        self.assertEqual([item['id'] for item in row['models']], ['classic', 'realtime', 'live'])
        self.assertEqual(row['model'], 'classic')
        await controls.handle(self.peer, {'t': 'agent_model', 'target': 'mode', 'rid': 2, 'id': 'live'})
        self.assertEqual([value['t'] for value in self.peer.socket.messages[-2:]],
                         ['agent_options', 'capabilities'])
        await controls.handle(self.peer, {'t': 'agent_model', 'target': 'voice', 'rid': 3,
                                         'id': 'openai/gpt-live-1'})
        state = VoicePreferences(self.store.home).lookup(self.identity)
        self.assertEqual(state, {'mode': 'live', 'live': 'openai/gpt-live-1'})
        self.assertEqual(os.stat(self.store.home / 'voice.json').st_mode & 0o777, 0o600)
        other_key = self.identity.split(':')[0] + ':' + '0' * 32
        self.assertEqual(VoicePreferences(self.store.home).lookup(other_key), {})
        other_profile = Path(self.temp.name) / 'other-profile'
        other_profile.mkdir()
        self.assertEqual(VoicePreferences(other_profile).lookup(self.identity), {})
        reconnect = Peer(DeviceSocket(), self.identity, self.hello)
        self.assertEqual(controls.mode(reconnect), 'live')

    async def test_busy_invalid_unavailable_and_unsupported_do_not_mutate_preferences(self):
        controls = self.adapter.voice_controls
        request = {'t': 'agent_model', 'target': 'mode', 'rid': 2, 'id': 'live'}
        self.peer.turn = 4
        await controls.handle(self.peer, request)
        self.assertEqual(self.peer.socket.messages[-1]['error'], 'busy')
        self.peer.turn = None
        await controls.handle(self.peer, {**request, 'id': 'made-up'})
        self.assertEqual(self.peer.socket.messages[-1]['error'], 'invalid_model')
        with patch('hermes_kubik.voice_controls.native_status', return_value={'available': False}):
            await controls.handle(self.peer, request)
        self.assertEqual(self.peer.socket.messages[-1]['error'], 'unavailable')
        await controls.handle(self.peer, {**request, 'target': 'agent'})
        self.assertEqual(self.peer.socket.messages[-1]['error'], 'unsupported')
        self.assertEqual(controls.mode(self.peer), 'classic')
        self.assertFalse(controls.store.path.exists())

    async def test_revocation_during_catalogue_probe_cannot_write_selection(self):
        controls = self.adapter.voice_controls
        original = controls.catalogue
        async def revoke(peer, target):
            result = await original(peer, target)
            atomic_json(self.store.allow_path, [])
            return result
        with patch.object(controls, 'catalogue', side_effect=revoke):
            await controls.handle(self.peer, {'t': 'agent_model', 'target': 'mode', 'rid': 2, 'id': 'live'})
        self.assertFalse(controls.store.path.exists())
        self.assertEqual(self.peer.socket.messages, [])

    async def test_input_validation_rejects_ambiguous_ids_and_boolean_request_numbers(self):
        controls = self.adapter.voice_controls
        for value in [{'t': 'agent_options', 'rid': True, 'target': 'mode'},
                      {'t': 'agent_options', 'rid': 1, 'cursor': -1, 'target': 'mode'},
                      {'t': 'agent_model', 'rid': 1, 'target': 'voice', 'id': 'bad\0id'}]:
            with self.assertRaises(ValueError):
                await controls.handle(self.peer, value)
        self.assertEqual(self.peer.socket.messages, [])

    def test_corrupt_symlink_and_special_preference_files_fail_closed(self):
        store = VoicePreferences(self.store.home)
        store.path.write_text('{')
        with self.assertRaises(ValueError):
            store.read()
        store.path.unlink()
        target = self.store.home / 'other.json'
        target.write_text(json.dumps({'version': 1, 'entries': {}}))
        store.path.symlink_to(target)
        with self.assertRaises(OSError):
            store.read()
        store.path.unlink()
        os.mkfifo(store.path)
        with self.assertRaises(ValueError):
            store.read()
        store.path.unlink()
        (self.store.home / 'voice.lock').symlink_to(target)
        with self.assertRaises(OSError):
            store.select(self.identity, 'mode', 'live')
        self.assertEqual(json.loads(target.read_text()), {'version': 1, 'entries': {}})


class ProfileConfigurationContract(ProtocolFixture):
    def setUp(self):
        super().setUp()
        self.home = Path(self.temp.name)
        self.config = {}
        self.profile = None
        self.calls = []
        config = ModuleType('hermes_cli.config')
        config.load_config = lambda: self.config
        backend = ModuleType('tools.tool_backend_helpers')
        def resolve():
            self.calls.append(self.profile)
            return 'profile-audio-key' if self.profile == self.home else None
        backend.resolve_openai_audio_api_key = resolve
        @contextlib.contextmanager
        def scope(home):
            self.profile = home
            try:
                yield
            finally:
                self.profile = None
        for patcher in [patch.dict(sys.modules, {'hermes_cli.config': config,
                                                'tools.tool_backend_helpers': backend}),
                        patch('hermes_kubik.voice_settings.profile_scope', scope)]:
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_scoped_audio_key_and_live_config_without_global_voice_mode_changes(self):
        self.config = {'voice': {'voice_chat_mode': 'text', 'gpt_live': {
            'model': 'gpt-live-1-custom', 'voice': 'cedar', 'base_url': 'https://audio.test/v1',
            'instructions': 'Use the agent'}}}
        settings = native_settings(self.home, 'live')
        self.assertEqual(settings['model'], 'gpt-live-1-custom')
        self.assertEqual(settings['voice'], 'cedar')
        self.assertEqual(settings['api_key'], 'profile-audio-key')
        self.assertEqual(self.calls, [self.home])
        self.assertIsNone(self.profile)
        self.assertEqual(native_settings(self.home, 'realtime')['model'], 'gpt-realtime-2.1')
        self.assertEqual(self.config['voice']['voice_chat_mode'], 'text')
        self.assertFalse(native_status(Path('/other-profile'), 'live')['available'])
        self.assertFalse(native_status(self.home, 'live', 'openai/gpt-live-old')['available'])

    def test_explicit_live_key_and_invalid_configuration(self):
        self.config = {'voice': {'gpt_live': {'api_key': 'explicit-fixture'}}}
        self.assertTrue(native_status(self.home, 'realtime')['available'])
        self.assertEqual(self.calls, [])
        for value in ('http://audio.test/v1', 'https://user:pass@audio.test', 'https://audio.test/?key=secret'):
            self.config['voice']['gpt_live']['base_url'] = value
            self.assertFalse(native_status(self.home, 'live')['available'])
        self.config = {'voice': {'gpt_live': []}}
        self.assertFalse(native_status(self.home, 'live')['available'])

    def test_missing_echo_dependency_never_advertises_live(self):
        with patch('hermes_kubik.voice_settings.importlib.util.find_spec', return_value=None):
            self.assertFalse(native_status(self.home, 'live')['available'])
            self.assertFalse(dependencies_available())
