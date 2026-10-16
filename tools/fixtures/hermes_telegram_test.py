"""Owner-only Telegram pairing with no Telegram network or provider calls."""
import asyncio
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace as NS
import unittest
from unittest.mock import AsyncMock, patch

from hermes_test_support import Security, atomic_json, KubikAdapter
from hermes_kubik.telegram_pairing import TelegramPairing


class TelegramPairingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.service = TelegramPairing(Path(self.tmp.name))
        self.bot = NS(send_message=AsyncMock(return_value=NS(message_id=7)))
        self.adapter = NS(is_connected=True, config=NS(home_channel=NS(chat_id='123')),
                          _is_callback_user_authorized=lambda *a, **kw: True)
        self.service.adapter, self.service.bot = self.adapter, self.bot
        telegram = ModuleType('telegram')
        telegram.InlineKeyboardButton = lambda *a, **kw: NS(args=a, **kw)
        telegram.InlineKeyboardMarkup = lambda rows: rows
        self.patch = patch.dict(sys.modules, {'telegram': telegram})
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.identity = 'kubik-test:fingerprint'
        self.code = self.service.security.pair(self.identity)

    def query(self, user=123, chat=123, kind='private', message=7, code=None):
        return NS(data='kubik_pair:' + (code or self.code), from_user=NS(id=user),
                  message=NS(chat=NS(id=chat, type=kind), message_id=message),
                  answer=AsyncMock(), edit_message_text=AsyncMock())

    async def test_discovery_and_no_telegram_notification_do_not_create_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'profile'
            service = TelegramPairing(root)
            self.assertFalse(root.exists())
            self.assertFalse(await service.notify('kubik:key', 'ABCD2345'))
            self.assertFalse(root.exists())

    async def test_delivery_deduplicated_concurrent_owner_approval_consumed(self):
        self.assertEqual(await asyncio.gather(*(self.service.notify(self.identity, self.code) for _ in range(3))), [True]*3)
        self.bot.send_message.assert_awaited_once()
        query = self.query()
        await self.service.handle(NS(callback_query=query), NS(bot=self.bot))
        self.assertTrue(self.service.security.approved(self.identity))
        atomic_json(self.service.security.allow_path, [])
        await self.service.handle(NS(callback_query=query), NS(bot=self.bot))
        self.assertFalse(self.service.security.approved(self.identity))
        with self.assertRaises(ValueError):
            self.service.security.approve(self.code)
        self.assertNotEqual(self.service.security.pair(self.identity), self.code)

    async def test_foreign_forwarded_group_and_wrong_message_rejected(self):
        await self.service.notify(self.identity, self.code)
        for query in (self.query(user=456), self.query(chat=456), self.query(kind='group'), self.query(message=8)):
            await self.service.handle(NS(callback_query=query), NS(bot=self.bot))
            self.assertFalse(self.service.security.approved(self.identity))
            query.edit_message_text.assert_not_awaited()
        self.adapter._is_callback_user_authorized = lambda *a, **kw: False
        await self.service.handle(NS(callback_query=self.query()), NS(bot=self.bot))
        self.assertFalse(self.service.security.approved(self.identity))

    async def test_expired_and_already_cli_consumed_rejected(self):
        await self.service.notify(self.identity, self.code)
        self.service.security.approve(self.code)
        atomic_json(self.service.security.allow_path, [])
        await self.service.handle(NS(callback_query=self.query()), NS(bot=self.bot))
        self.assertFalse(self.service.security.approved(self.identity))

    async def test_no_telegram_group_home_or_send_failure_uses_server_path(self):
        self.adapter.is_connected = False
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.adapter.is_connected = True
        self.service.bot = None
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.service.bot = self.bot
        self.adapter.config.home_channel.chat_id = '-123'
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.adapter.config.home_channel.chat_id = '123'
        self.bot.send_message.side_effect = RuntimeError('offline')
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.bot.send_message.assert_awaited_once()
        self.service.security.approve(self.code)
        self.assertTrue(self.service.security.approved(self.identity))

    async def test_expired_request_never_sent_or_approved(self):
        pending = self.service.security.read_pending()
        pending[self.identity]['expires'] = 0
        atomic_json(self.service.security.pending_path, pending)
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.bot.send_message.assert_not_awaited()
        with self.assertRaises(ValueError):
            self.service.security.approve(self.code)

    async def test_telegram_connecting_at_first_pair_is_retried_on_same_socket(self):
        from hermes_kubik.pairing_wait import announce_pairing
        adapter = object.__new__(KubikAdapter)
        adapter.security = self.service.security
        adapter.telegram_pairing = NS(notify=AsyncMock(side_effect=[False, True]))
        socket = NS(closed=False, send_json=AsyncMock())
        with patch('hermes_kubik.pairing_wait.asyncio.sleep', new_callable=AsyncMock) as sleep:
            await announce_pairing(adapter, socket, self.identity, self.code)
        self.assertEqual(adapter.telegram_pairing.notify.await_count, 2)
        sleep.assert_awaited_once_with(15)
        socket.send_json.assert_awaited_once_with({'t': 'pair', 'code': self.code, 'approval': 'telegram'})

    async def test_expiry_during_send_is_not_advertised(self):
        async def send(**kwargs):
            pending = self.service.security.read_pending()
            pending[self.identity]['expires'] = 0
            atomic_json(self.service.security.pending_path, pending)
            return NS(message_id=7)
        self.bot.send_message.side_effect = send
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.assertNotIn(self.code, self.service.delivered)

    async def test_revoked_owner_during_send_is_not_advertised(self):
        async def send(**kwargs):
            self.adapter._is_callback_user_authorized = lambda *a, **kw: False
            return NS(message_id=7)
        self.bot.send_message.side_effect = send
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.assertNotIn(self.code, self.service.delivered)

    async def test_bot_replacement_during_send_cannot_publish_old_delivery(self):
        async def send(**kwargs):
            self.service.bot = NS(send_message=AsyncMock())
            return NS(message_id=7)
        self.bot.send_message.side_effect = send
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.assertNotIn(self.code, self.service.delivered)

    async def test_unidentified_delivery_and_foreign_identity_are_not_cached(self):
        self.bot.send_message.return_value = NS(message_id=0)
        self.assertFalse(await self.service.notify(self.identity, self.code))
        self.service.attempts.clear()
        self.bot.send_message.return_value = NS(message_id=7)
        self.assertTrue(await self.service.notify(self.identity, self.code))
        self.assertFalse(await self.service.notify('kubik-other:key', self.code))

    async def test_callback_is_bound_to_the_original_pending_identity(self):
        await self.service.notify(self.identity, self.code)
        pending = self.service.security.read_pending()
        pending['kubik-other:key'] = pending.pop(self.identity)
        atomic_json(self.service.security.pending_path, pending)
        await self.service.handle(NS(callback_query=self.query()), NS(bot=self.bot))
        self.assertFalse(self.service.security.approved('kubik-other:key'))
        self.assertFalse(self.service.security.approved(self.identity))

    async def test_foreign_bot_context_is_rejected(self):
        await self.service.notify(self.identity, self.code)
        await self.service.handle(NS(callback_query=self.query()), NS(bot=NS()))
        self.assertFalse(self.service.security.approved(self.identity))

    async def test_failed_callback_answer_does_not_prevent_prompt_cleanup(self):
        await self.service.notify(self.identity, self.code)
        query = self.query()
        query.answer.side_effect = RuntimeError('Telegram unavailable')
        await self.service.handle(NS(callback_query=query), NS(bot=self.bot))
        self.assertTrue(self.service.security.approved(self.identity))
        query.edit_message_text.assert_awaited_once()
        self.assertNotIn(self.code, self.service.delivered)

    async def test_authentication_sends_code_before_waiting_for_telegram(self):
        adapter = object.__new__(KubikAdapter)
        adapter.security = NS(identity=lambda hello: None, verify=lambda *a: self.identity,
                              approved=lambda identity: False, pair=lambda identity: self.code)
        adapter.receive_json = AsyncMock(side_effect=[{'volume': 0}, {'t': 'auth'}])
        adapter.wait_pair = AsyncMock()
        started, release = asyncio.Event(), asyncio.Event()
        async def notify(*args):
            started.set()
            await release.wait()
            return False
        adapter.telegram_pairing = NS(notify=notify)
        socket = NS(send_json=AsyncMock())
        task = asyncio.create_task(adapter.authenticate(socket))
        # A replacement wait_pair deliberately does not run notification work.
        await asyncio.sleep(0)
        frames = [call.args[0] for call in socket.send_json.call_args_list]
        release.set()
        await task
        self.assertIn({'t': 'pair', 'code': self.code}, frames)
