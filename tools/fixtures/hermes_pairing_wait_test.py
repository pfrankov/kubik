"""Pairing socket lifetime is independent of optional Telegram delivery."""
import asyncio
import json
import time
from types import SimpleNamespace as NS
import unittest
from unittest.mock import AsyncMock

from aiohttp import WSMsgType
from hermes_test_support import KubikAdapter


class PairingWaitTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.identity, self.code = 'kubik-test:fingerprint', 'ABCD2345'
        self.approved = False
        self.expires = time.time() + 600
        self.adapter = object.__new__(KubikAdapter)
        self.adapter.security = NS(
            approved=lambda identity: self.approved,
            read_pending=lambda: {self.identity: {'code': self.code, 'expires': self.expires}})
        self.incoming = asyncio.Queue()
        self.socket = NS(closed=False, send_json=AsyncMock(), receive=self.receive)
        self.started, self.cancelled = asyncio.Event(), asyncio.Event()
        self.adapter.telegram_pairing = NS(notify=self.hung_notification)

    async def receive(self, timeout):
        return await asyncio.wait_for(self.incoming.get(), timeout)

    async def hung_notification(self, *args):
        self.started.set()
        try:
            await asyncio.Future()
        finally:
            self.cancelled.set()

    def wait(self):
        return self.adapter.wait_pair(self.socket, self.identity, self.code, False)

    async def ping(self, value=None):
        await self.incoming.put(NS(type=WSMsgType.TEXT, data=json.dumps(value or {'t': 'ping', 'ts': 42})))

    async def test_ping_and_cli_approval_do_not_wait_for_telegram(self):
        task = asyncio.create_task(self.wait())
        await asyncio.wait_for(self.started.wait(), 1)
        await self.ping()
        for _ in range(20):
            if self.socket.send_json.await_count:
                break
            await asyncio.sleep(0)
        self.socket.send_json.assert_awaited_once_with({'t': 'pong', 'ts': 42})
        self.approved = True
        await self.ping()
        await asyncio.wait_for(task, 1)
        self.assertTrue(self.cancelled.is_set())
        self.assertFalse(any(call.args[0].get('approval') for call in self.socket.send_json.call_args_list))

    async def test_disconnect_cancels_and_joins_notification(self):
        task = asyncio.create_task(self.wait())
        await asyncio.wait_for(self.started.wait(), 1)
        await self.incoming.put(NS(type=WSMsgType.CLOSE))
        with self.assertRaisesRegex(ValueError, 'Pairing closed'):
            await asyncio.wait_for(task, 1)
        self.assertTrue(self.cancelled.is_set())
        self.socket.send_json.assert_not_awaited()

    async def test_cancellation_cleans_up_notification(self):
        task = asyncio.create_task(self.wait())
        await asyncio.wait_for(self.started.wait(), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(self.cancelled.is_set())

    async def test_reconnect_uses_remaining_persisted_expiry(self):
        self.expires = time.time() + 0.03
        task = asyncio.create_task(self.wait())
        await asyncio.wait_for(self.started.wait(), 1)
        done, _ = await asyncio.wait({task}, timeout=0.5)
        expired = task in done
        if not expired:
            task.cancel()
        with self.assertRaises((TimeoutError, asyncio.CancelledError)):
            await task
        self.assertTrue(expired, 'Reconnect must not grant a fresh ten-minute wait')
        self.assertTrue(self.cancelled.is_set())

    async def test_delayed_delivery_updates_the_waiting_socket(self):
        release = asyncio.Event()
        async def notify(*args):
            self.started.set()
            await release.wait()
            return True
        self.adapter.telegram_pairing.notify = notify
        task = asyncio.create_task(self.wait())
        await asyncio.wait_for(self.started.wait(), 1)
        release.set()
        for _ in range(20):
            if self.socket.send_json.await_count:
                break
            await asyncio.sleep(0)
        self.socket.send_json.assert_awaited_once_with({'t': 'pair', 'code': self.code, 'approval': 'telegram'})
        self.approved = True
        await self.ping()
        await asyncio.wait_for(task, 1)

    async def test_non_object_json_is_rejected_as_protocol_error(self):
        await self.incoming.put(NS(type=WSMsgType.TEXT, data='[]'))
        with self.assertRaisesRegex(ValueError, 'Pairing only accepts ping'):
            await self.wait()
