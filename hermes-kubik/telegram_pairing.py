"""Profile-local Telegram approval; no polling bot, agent turn or provider call."""
import asyncio
import logging
import re
import time

from .security import Security


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
        self.adapter, self.bot = adapter, application.bot
        application.add_handler(CallbackQueryHandler(self.handle, pattern=r'^kubik_pair:'))

    async def notify(self, identity, code):
        async with self.lock:
            try:
                return await self._notify(identity, code)
            except Exception:
                logging.getLogger(__name__).warning("Telegram pairing unavailable; use server approval")
                return False

    def prune(self, now):
        live = self.security.read_pending()
        codes = {value['code'] for value in live.values() if value['expires'] > now}
        self.delivered = {key: value for key, value in self.delivered.items() if key in codes and value[0] > now}
        self.attempts = {key: value for key, value in self.attempts.items() if value > now and key in codes}
        return live, codes

    async def _notify(self, identity, code):
        owner = self.owner()
        if not owner or not self.bot:
            return False
        now = time.time()
        live, _ = self.prune(now)
        if code in self.delivered:
            return self.delivered[code][1] == owner
        pending = live.get(identity)
        if not pending or pending['code'] != code or pending['expires'] <= now:
            return False
        if code in self.attempts:
            return False
        self.attempts[code] = now + 60
        return await self.send(identity, code, owner)

    async def send(self, identity, code, owner):
        from telegram import InlineKeyboardButton, InlineKeyboardMarkup
        text = f'Connect {identity.split(":", 1)[0]}?\nMatch this code on Kubik: {code}\nExpires in 10 minutes.'
        try:
            message = await asyncio.wait_for(self.bot.send_message(chat_id=owner, text=text,
                reply_markup=InlineKeyboardMarkup([[InlineKeyboardButton('Allow', callback_data=f'kubik_pair:{code}')]])), 5)
        except Exception:
            return False
        pending = self.security.read_pending().get(identity)
        if not pending or pending['code'] != code:
            return False
        self.delivered[code] = (pending['expires'], owner, message.message_id)
        return True

    async def handle(self, update, context):
        query = update.callback_query
        code = (query.data or '').removeprefix('kubik_pair:')
        record = self.delivered.get(code)
        message = query.message
        owner = self.owner()
        valid = (record and record[0] > time.time() and owner == record[1]
                 and str(query.from_user.id) == owner and message is not None
                 and str(message.chat.id) == owner and message.chat.type == 'private'
                 and message.message_id == record[2])
        if not valid:
            await query.answer('This request is expired or is not yours.', show_alert=True)
            return
        try:
            self.security.approve(code)
        except ValueError:
            await query.answer('This pairing request has expired.', show_alert=True)
            return
        self.delivered.pop(code, None)
        await query.answer('Kubik approved')
        await query.edit_message_text(f'Kubik approved. Code: {code}')
