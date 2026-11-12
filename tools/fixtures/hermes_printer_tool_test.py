"""Trusted source context survives Classic and native turns without exposing a target argument."""
import asyncio
from contextlib import contextmanager
from contextvars import ContextVar
import importlib.util
import json
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace
from unittest.mock import patch

from hermes_test_support import (DeviceSocket, KubikAdapter, Peer, ProtocolFixture, ROOT,
                                 atomic_json)
from hermes_printer_support import reply
from hermes_kubik.printer_tool import CURRENT_PRINTER, PrinterTool, printer_origin, image_hook
from hermes_kubik.runtime import profile_scope

SESSION = ContextVar('printer_test_session', default={})
CALL = ContextVar('printer_test_call', default='')


@contextmanager
def source_context(adapter, peer, event, *, call='call-printer', change=None):
    env = {'HERMES_SESSION_PLATFORM': 'kubik', 'HERMES_SESSION_CHAT_ID': peer.identity,
           'HERMES_SESSION_MESSAGE_ID': event.message_id,
           'HERMES_SESSION_KEY': adapter._event_session_key(event)}
    env.update(change or {})
    a, b = SESSION.set(env), CALL.set(call)
    try:
        yield
    finally:
        CALL.reset(b); SESSION.reset(a)


class PrinterTools(ProtocolFixture):
    def setUp(self):
        super().setUp()
        sessions = ModuleType('gateway.session_context')
        sessions.get_session_env = lambda key, default=None: SESSION.get().get(key, default)
        approval = ModuleType('tools.approval_context')
        approval._approval_tool_call_id = CALL
        modules = patch.dict(sys.modules, {'gateway.session_context': sessions, 'tools.approval_context': approval})
        modules.start(); self.addCleanup(modules.stop)
        self.adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        self.adapter.security = self.store
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        self.peer = Peer(DeviceSocket(), identity, {**self.hello, 'printer': 2})
        self.peer.ready = True
        self.adapter.peers[identity] = self.peer
        self.event = self.adapter._authorized_voice_event(self.peer, 'print a list')
        self.tool = PrinterTool(); self.tool.adapter = self.adapter

    async def execute(self, *, change=None, call='call-printer', foreign=False):
        self.adapter.printer_loop = asyncio.get_running_loop()
        async def send(message, **_kwargs):
            self.peer.socket.messages.append(message)
            await self.adapter.command(self.peer, reply(message))
        self.peer.socket.send_json = send
        with printer_origin(self.adapter, self.peer, self.event), source_context(
                self.adapter, self.peer, self.event, call=call, change=change):
            if foreign:
                return json.loads(await asyncio.to_thread(lambda: asyncio.run(self.tool.execute({'action': 'status'}))))
            return json.loads(await self.tool.execute({'action': 'status'}))

    async def test_real_call_identity_reaches_gateway_loop_from_a_tool_worker(self):
        value = await self.execute(foreign=True)
        self.assertEqual(value['status'], 'ready')
        self.assertEqual(value['printer']['dpi'], 304)
        self.assertEqual(len(self.peer.socket.messages), 1)
        self.assertIsNone(CURRENT_PRINTER.get())

    async def test_no_target_or_environment_spoof_can_replace_trusted_inbound_context(self):
        self.adapter.printer_loop = asyncio.get_running_loop()
        with source_context(self.adapter, self.peer, self.event):
            self.assertEqual(json.loads(await self.tool.execute({'action': 'status'}))['code'], 'unauthorized')
        for key, value in [('HERMES_SESSION_PLATFORM', 'telegram'), ('HERMES_SESSION_CHAT_ID', 'other'),
                           ('HERMES_SESSION_MESSAGE_ID', 'previous'), ('HERMES_SESSION_KEY', 'foreign')]:
            with self.subTest(key=key):
                result = await self.execute(change={key: value})
                self.assertEqual(result['code'], 'unauthorized')
        self.assertFalse(self.peer.socket.messages)

    async def test_missing_native_call_id_and_wrong_profile_are_rejected(self):
        self.assertEqual((await self.execute(call=''))['code'], 'invalid_input')
        other = Path(self.temp.name) / 'other'; other.mkdir()
        with profile_scope(other):
            self.assertEqual((await self.execute())['code'], 'unauthorized')
        self.assertFalse(self.peer.socket.messages)

    async def test_generated_image_handle_is_bound_to_authenticated_turn(self):
        directory = self.adapter.home / 'cache/generated/images'
        directory.mkdir(parents=True)
        path = directory / 'generated.png'
        path.write_bytes(b'image')
        result = json.dumps({'success': True, 'provider': 'openai', 'image': str(path)})
        with printer_origin(self.adapter, self.peer, self.event), source_context(self.adapter, self.peer, self.event):
            value = image_hook(self.tool, tool_name='image_generate', tool_call_id='image-call', result=result)
            generated = json.loads(value)
            ref = generated['kubik_image_ref']
            self.assertIn('Decide from the conversation', generated['kubik_print_instruction'])
            self.assertFalse(self.peer.socket.messages)
            self.assertEqual(len(ref), 32)
            self.assertIsNone(image_hook(self.tool, tool_name='other', tool_call_id='image-call', result=result))
        with source_context(self.adapter, self.peer, self.event):
            self.assertIsNone(image_hook(self.tool, tool_name='image_generate', tool_call_id='other', result=result))
        await self.adapter.on_processing_complete(self.event, 'success')
        self.assertEqual(self.event._kubik_images.get(ref), b'image')
        next_event = self.adapter._authorized_voice_event(self.peer, 'print the picture')
        with printer_origin(self.adapter, self.peer, next_event), source_context(self.adapter, self.peer, next_event):
            self.assertEqual(next_event._kubik_images.get(ref), b'image')
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'printer': 2})
        replacement.ready = True
        replacement_event = self.adapter._authorized_voice_event(replacement, 'print the picture')
        with printer_origin(self.adapter, replacement, replacement_event):
            self.assertIsNone(replacement_event._kubik_images.get(ref))
        await self.adapter._remove_peer(self.peer.socket, self.peer)
        self.assertIsNone(next_event._kubik_images.get(ref))

    async def test_old_turn_cannot_redirect_to_a_reconnected_socket(self):
        replacement = Peer(DeviceSocket(), self.peer.identity, {**self.hello, 'printer': 2})
        replacement.ready = True
        self.adapter.peers[self.peer.identity] = replacement
        self.assertEqual((await self.execute())['code'], 'device_replaced')
        self.assertFalse(replacement.socket.messages)

    async def test_classic_and_native_modes_share_the_same_bound_message_helper(self):
        seen = []
        async def handle(event):
            binding = CURRENT_PRINTER.get()
            self.assertEqual(binding[:3], (self.adapter, self.peer, event.message_id))
            seen.append(event.message_id)
            event._gateway_accepted = True
            await self.adapter.on_processing_complete(event, 'success')
        self.adapter.handle_message = handle
        await self.adapter._run_voice_event(self.peer, self.event)
        for mode in ('realtime', 'live'):
            async def answered(*args, **kwargs):
                pass
            voice = SimpleNamespace(peer=self.peer, current=lambda: True, reply=answered, fail=answered)
            job = self.adapter.native_jobs.submit(voice, mode)
            await asyncio.wait_for(job.task, 1)
        self.assertEqual(len(seen), 3)
        self.assertIsNone(CURRENT_PRINTER.get())

    async def test_registration_uses_native_kubik_toolset_and_profile_local_adapter_holder(self):
        spec = importlib.util.spec_from_file_location('hermes_kubik.entrypoint', ROOT / 'hermes-kubik/__init__.py')
        entry = importlib.util.module_from_spec(spec); entry.__package__ = 'hermes_kubik'
        spec.loader.exec_module(entry)
        class Context:
            def register_hook(self, name, callback):
                pass
            def register_telegram_handler(self, handler):
                pass
            def register_platform(self, **values):
                self.platform = values
            def register_tool(self, **values):
                self.tool = values
        first, second = Context(), Context()
        entry.register(first); entry.register(second)
        self.assertEqual(first.tool['toolset'], 'kubik')
        self.assertFalse(first.tool['is_async'])
        one = first.platform['adapter_factory'](SimpleNamespace(extra={}, enabled=True))
        two = second.platform['adapter_factory'](SimpleNamespace(extra={}, enabled=True))
        self.assertIs(first.tool['handler'].__self__.adapter, one)
        self.assertIs(second.tool['handler'].__self__.adapter, two)
        self.assertNotIn('device', first.tool['schema']['parameters']['properties'])
        self.assertIn('decide from', first.platform['platform_hint'])
        self.assertIn('Do not automatically print every response or image', first.platform['platform_hint'])
