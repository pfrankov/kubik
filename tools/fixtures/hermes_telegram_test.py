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
        await self.service.handle(NS(callback_query=query), None)
        self.assertTrue(self.service.security.approved(self.identity))
        atomic_json(self.service.security.allow_path, [])
        await self.service.handle(NS(callback_query=query), None)
        self.assertFalse(self.service.security.approved(self.identity))
        with self.assertRaises(ValueError):
            self.service.security.approve(self.code)
        self.assertNotEqual(self.service.security.pair(self.identity), self.code)

    async def test_foreign_forwarded_group_and_wrong_message_rejected(self):
        await self.service.notify(self.identity, self.code)
        for query in (self.query(user=456), self.query(chat=456), self.query(kind='group'), self.query(message=8)):
            await self.service.handle(NS(callback_query=query), None)
            self.assertFalse(self.service.security.approved(self.identity))
            query.edit_message_text.assert_not_awaited()
        self.adapter._is_callback_user_authorized = lambda *a, **kw: False
        await self.service.handle(NS(callback_query=self.query()), None)
        self.assertFalse(self.service.security.approved(self.identity))

    async def test_expired_and_already_cli_consumed_rejected(self):
        await self.service.notify(self.identity, self.code)
        self.service.security.approve(self.code)
        atomic_json(self.service.security.allow_path, [])
        await self.service.handle(NS(callback_query=self.query()), None)
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
        adapter = object.__new__(KubikAdapter)
        checks = iter([False, True])
        adapter.security = NS(approved=lambda identity: next(checks))
        adapter.telegram_pairing = NS(notify=AsyncMock(return_value=True))
        socket = NS(send_json=AsyncMock(), receive=AsyncMock(side_effect=asyncio.TimeoutError))
        ticks = iter([0, 0, 16, 16, 16])
        with patch('hermes_kubik.adapter.time', NS(monotonic=lambda: next(ticks))):
            await adapter.wait_pair(socket, self.identity, self.code, False)
        adapter.telegram_pairing.notify.assert_awaited_once_with(self.identity, self.code)
        socket.send_json.assert_awaited_once_with({'t': 'pair', 'code': self.code, 'approval': 'telegram'})
