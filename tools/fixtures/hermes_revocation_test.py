"""Revocation is re-read at input and output operation boundaries."""
import asyncio
import sys
from types import SimpleNamespace
from unittest.mock import patch

from hermes_test_support import (DeviceSocket, KubikAdapter, Peer, ProtocolFixture,
                                atomic_json)


class Revocation(ProtocolFixture):
    def ready_peer(self):
        identity, _ = self.store.identity(self.hello)
        atomic_json(self.store.allow_path, [identity])
        adapter = KubikAdapter(SimpleNamespace(extra={}, enabled=True))
        adapter.security = self.store
        peer = Peer(DeviceSocket(), identity, self.hello)
        peer.ready = True
        adapter.peers[identity] = peer
        return adapter, peer, identity

    async def test_ptt_and_stt_recheck_approval_from_approved_json(self):
        adapter, peer, identity = self.ready_peer()
        await adapter.ptt(peer, {'turn': 1, 'on': True})
        self.assertEqual(peer.turn, 1, 'an approved device can start recording')
        peer.pcm.extend(bytes(1920))
        atomic_json(self.store.allow_path, [])
        await adapter.ptt(peer, {'turn': 2, 'on': True})
        self.assertIsNone(peer.turn)
        self.assertIsNone(peer.input_turn)
        self.assertEqual(peer.pcm, bytearray())
        self.assertIn({'t': 'error', 'code': 'unauthorized'}, peer.socket.messages)

        calls = []
        with patch.object(sys.modules['tools.transcription_tools'], 'transcribe_audio',
                          lambda *args, **kwargs: calls.append(args), create=True):
            await adapter.dispatch(peer, bytes(1920))
        self.assertEqual(calls, [], 'revoked audio must not start STT')
        self.assertFalse(adapter._is_approved(identity))

    async def test_revocation_just_before_stt_prevents_provider_call(self):
        adapter, peer, _ = self.ready_peer()
        calls = []

        async def revoke_after_transcribing_state(message_type, **fields):
            peer.socket.messages.append({'t': message_type, **fields})
            if fields.get('s') == 'transcribing':
                atomic_json(self.store.allow_path, [])

        peer.send = revoke_after_transcribing_state
        with patch.object(sys.modules['tools.transcription_tools'], 'transcribe_audio',
                          lambda *args, **kwargs: calls.append(args), create=True):
            await adapter.dispatch(peer, bytes(1920))
        self.assertEqual(calls, [])
        self.assertFalse(self.store.approved(peer.identity))
        self.assertTrue(any(message.get('s') == 'idle' for message in peer.socket.messages))

    async def test_revocation_during_stt_prevents_agent_handoff(self):
        adapter, peer, _ = self.ready_peer()
        transcribed, handled = [], []

        def revoke_while_transcribing(path, source):
            transcribed.append(source)
            atomic_json(self.store.allow_path, [])
            return {'success': True, 'transcript': 'do not dispatch'}

        async def handle_message(event):
            handled.append(event)

        adapter.handle_message = handle_message
        with patch.object(sys.modules['tools.transcription_tools'], 'transcribe_audio',
                          revoke_while_transcribing, create=True):
            await adapter.dispatch(peer, bytes(1920))
        self.assertEqual(transcribed, ['gateway'])
        self.assertEqual(handled, [])

    async def test_normal_output_works_but_revocation_blocks_new_and_queued_output(self):
        adapter, peer, identity = self.ready_peer()
        text_task = asyncio.create_task(adapter.send(identity, 'trusted reply'))
        async with asyncio.timeout(1):
            while not peer.socket.messages:
                await asyncio.sleep(0)
        message = peer.socket.messages[-1]
        self.assertEqual(message['text'], 'trusted reply')
        await adapter.command(peer, {'t': 'shown', 'receipt': message['receipt']})
        self.assertTrue((await text_task).success)

        played = []
        async def play_file(current_peer, path):
            played.append(path)
            return SimpleNamespace(success=True)

        with patch('hermes_kubik.speech.play_file', play_file):
            await peer.output_lock.acquire()
            queued = asyncio.create_task(adapter.send_voice(identity, 'queued.wav'))
            await asyncio.sleep(0)
            atomic_json(self.store.allow_path, [])
            peer.output_lock.release()
            queued_result = await queued
            self.assertFalse(queued_result.success)
            self.assertTrue(queued_result.retryable)
            self.assertEqual(played, [], 'voice queued before revocation must recheck after lock')

        before = len(peer.socket.messages)
        self.assertIsNone(adapter._ready_peer(identity))
        self.assertFalse((await adapter.send(identity, 'revoked reply')).success)
        await adapter.send_typing(identity)
        self.assertFalse((await adapter.send_voice(identity, 'new.wav')).success)
        self.assertEqual(len(peer.socket.messages), before)

    async def test_revocation_does_not_cancel_already_accepted_agent_work(self):
        adapter, peer, _ = self.ready_peer()
        entered = asyncio.Event()

        def native_stt(path, source):
            return {'success': True, 'transcript': 'accepted request'}

        async def handle_message(event):
            event._gateway_accepted = True
            entered.set()

        adapter.handle_message = handle_message
        with patch.object(sys.modules['tools.transcription_tools'], 'transcribe_audio',
                          native_stt, create=True):
            task = asyncio.create_task(adapter.dispatch(peer, bytes(1920)))
            await asyncio.wait_for(entered.wait(), 1)
            async with asyncio.timeout(1):
                while peer.stage != 'agent':
                    await asyncio.sleep(0)
            atomic_json(self.store.allow_path, [])
            await asyncio.sleep(0)
            self.assertFalse(task.done())
            self.assertFalse(task.cancelled())
            peer.complete.set()
            await asyncio.wait_for(task, 1)
        self.assertFalse(task.cancelled())
