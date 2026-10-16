"""Profile-local Telegram approval; no polling bot, agent turn or provider call."""
import asyncio
from dataclasses import dataclass
import logging
import re
import time

from .security import Security


@dataclass(frozen=True)
class Delivery:
    identity: str
    expires: float
    owner: str
    message_id: int
    bot: object


class TelegramPairing:
    def __init__(self, home):
        self.home = home
        self._security = None
        self.adapter = None
        self.bot = None
        self.lock = asyncio.Lock()
        self.attempts = {}
        self.delivered = {}  # bounded by the three live security requests

    @property
    def security(self):
        if self._security is None:
            self._security = Security(self.home)
        return self._security

    def owner(self):
        adapter = self.adapter
        if adapter is None or not adapter.is_connected:
            return None
        home = getattr(getattr(adapter, 'config', None), 'home_channel', None)
        chat = str(getattr(home, 'chat_id', '') or '')
        if not re.fullmatch('[1-9][0-9]{0,19}', chat):
            return None
        if not adapter._is_callback_user_authorized(chat, chat_id=chat, chat_type='private'):
            return None
        return chat

    def install(self, application, adapter):
        from telegram.ext import CallbackQueryHandler
        if self.bot is not application.bot:
            self.attempts.clear()
            self.delivered.clear()
        self.adapter, self.bot = adapter, application.bot
        application.add_handler(CallbackQueryHandler(self.handle, pattern=r'^kubik_pair:'))

    async def notify(self, identity, code):
        async with self.lock:
            try:
                return await self._notify(identity, code)
            except Exception:
                logging.getLogger(__name__).warning('Telegram pairing unavailable; use server approval')
                return False

    def prune(self, now):
        live = self.security.read_pending()
        codes = {value['code'] for value in live.values() if value['expires'] > now}
        self.delivered = {key: value for key, value in self.delivered.items() if key in codes and value.expires > now}
        self.attempts = {key: value for key, value in self.attempts.items() if value > now and key in codes}
        return live

    async def _notify(self, identity, code):
        owner = self.owner()
        if not owner or not self.bot:
            return False
        now = time.time()
        pending = self.prune(now).get(identity)
        if not pending or pending['code'] != code or pending['expires'] <= now:
            return False
        record = self.delivered.get(code)
        if record:
            return record.identity == identity and record.owner == owner and record.bot is self.bot
        if code in self.attempts:
            return False
        self.attempts[code] = now + 60
        return await self.send(identity, code, owner, pending)

    async def send(self, identity, code, owner, pending):
        from telegram import InlineKeyboardButton, InlineKeyboardMarkup
        bot = self.bot
        text = f'Connect {identity.split(":", 1)[0]}?\nMatch this code on Kubik: {code}\nExpires within 10 minutes.'
        try:
            message = await asyncio.wait_for(bot.send_message(chat_id=owner, text=text,
                reply_markup=InlineKeyboardMarkup([[InlineKeyboardButton('Allow', callback_data=f'kubik_pair:{code}')]])), 5)
        except Exception:
            return False
        # Re-check everything that may have changed while Telegram was awaited.
        if self.security.read_pending().get(identity) != pending or pending['expires'] <= time.time():
            return False
        if self.bot is not bot or self.owner() != owner:
            return False
        if type(message.message_id) is not int or message.message_id <= 0:
            return False
        self.delivered[code] = Delivery(identity, pending['expires'], owner, message.message_id, bot)
        return True

    def valid_callback(self, query, context, record):
        if not record or record.expires <= time.time():
            return False
        if self.bot is not record.bot or getattr(context, 'bot', None) is not record.bot:
            return False
        if self.owner() != record.owner or str(query.from_user.id) != record.owner:
            return False
        message = query.message
        return (message is not None and str(message.chat.id) == record.owner
                and message.chat.type == 'private' and message.message_id == record.message_id)

    async def feedback(self, action, *args, **kwargs):
        try:
            await asyncio.wait_for(action(*args, **kwargs), 5)
        except Exception:
            logging.getLogger(__name__).warning('Telegram pairing feedback unavailable')

    async def handle(self, update, context):
        query = update.callback_query
        data = query.data
        code = data.removeprefix('kubik_pair:') if isinstance(data, str) else ''
        record = self.delivered.get(code)
        if not self.valid_callback(query, context, record):
            await self.feedback(query.answer, 'This request is expired or is not yours.', show_alert=True)
            return
        try:
            self.security.approve(code, identity=record.identity)
        except (ValueError, OSError):
            await self.feedback(query.answer, 'Pairing was not approved. Try again or use server approval.', show_alert=True)
            return
        self.delivered.pop(code, None)
        await self.feedback(query.answer, 'Kubik approved')
        await self.feedback(query.edit_message_text, f'Kubik approved. Code: {code}')
