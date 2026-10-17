"""Native plugin wiring and PTB dispatch; only the Bot API transport is synthetic."""
import json
import time

from telegram import Update
from telegram.ext import Application, ExtBot
from telegram.request import BaseRequest


class BotTransport(BaseRequest):
    """No sockets or credentials: exercise PTB's actual request/response conversion."""
    def __init__(self):
        self.calls = []

    @property
    def read_timeout(self):
        return 5

    async def initialize(self):
        pass

    async def shutdown(self):
        pass

    async def do_request(self, url, method, request_data=None, **kwargs):
        action = url.rsplit('/', 1)[-1]
        data = request_data.parameters if request_data else {}
        self.calls.append((action, data))
        if action == 'getMe':
            result = {'id': 999, 'is_bot': True, 'first_name': 'Local', 'username': 'local_test_bot'}
        elif action in ('sendMessage', 'editMessageText'):
            result = {'message_id': 7, 'date': int(time.time()),
                      'chat': {'id': int(data['chat_id']), 'type': 'private'}, 'text': data['text']}
        elif action in ('answerCallbackQuery', 'deleteWebhook', 'setWebhook'):
            result = True
        else:
            raise AssertionError(f'Unexpected Bot API method: {action}')
        return 200, json.dumps({'ok': True, 'result': result}).encode()


async def check_telegram_callbacks(kubik, gateway, providers):
    from gateway.config import HomeChannel, Platform, PlatformConfig
    from plugins.platforms.telegram.adapter import TelegramAdapter
    from plugins.platforms.telegram.update_admission import TelegramApplication

    transport = BotTransport()
    # PTB requires token syntax; this deliberately invalid value never leaves this process.
    bot = ExtBot('999:local-test-only', request=transport, get_updates_request=BotTransport())
    config = PlatformConfig(enabled=True, home_channel=HomeChannel(Platform.TELEGRAM, '123', 'Owner'))
    telegram = TelegramAdapter(config)
    turns, errors = [], []
    primary = gateway._primary_message_handler()

    async def observe_turn(event):
        turns.append(event)
        return await primary(event)

    async def observe_error(update, context):
        errors.append(context.error)

    gateway._wire_adapter_handlers(telegram, message_handler=observe_turn)
    application = Application.builder().bot(bot).application_class(
        TelegramApplication, {'adapter': telegram}).build()
    application.add_error_handler(observe_error)
    # These are the same native registration boundaries used by TelegramAdapter.connect().
    # The manager must supply Kubik's real register_telegram_handler factory.
    telegram._wire_plugin_handlers(application)
    telegram._register_handlers(application)
    count = sum(map(len, application.handlers.values()))
    telegram._wire_plugin_handlers(application)
    assert sum(map(len, application.handlers.values())) == count, 'Duplicate native plugin handlers'
    await application.initialize()
    telegram._running = True
    pairing = kubik.telegram_pairing
    before = len(providers.calls)
    identity = 'kubik-telegram:native-key'
    code = kubik.security.pair(identity)

    async def click(number, *, user=123, chat=123, kind='private', message=7, data=None):
        update = Update.de_json({'update_id': number, 'callback_query': {
            'id': str(number), 'from': {'id': user, 'is_bot': False, 'first_name': 'Owner'},
            'chat_instance': 'local', 'data': data or f'kubik_pair:{code}',
            'message': {'message_id': message, 'date': int(time.time()),
                        'chat': {'id': chat, 'type': kind}, 'text': 'Pairing'}}}, bot)
        await application.process_update(update)
        assert not errors, errors
        assert not turns and len(providers.calls) == before, 'Callback started an agent turn'

    try:
        assert pairing.adapter is telegram and pairing.bot is bot, 'Native registration was not wired'
        assert await pairing.notify(identity, code), 'Native authorization rejected the owner'
        sent = next(data for method, data in transport.calls if method == 'sendMessage')
        assert sent['reply_markup']['inline_keyboard'][0][0]['callback_data'] == f'kubik_pair:{code}'
        for number, invalid in enumerate(({'user': 456}, {'chat': 456}, {'kind': 'group'}, {'message': 8}), 1):
            await click(number, **invalid)
            assert not kubik.security.approved(identity)
            assert transport.calls[-1][0] == 'answerCallbackQuery'
            assert transport.calls[-1][1]['show_alert'] is True
        await click(5)
        assert kubik.security.approved(identity), 'Owner callback did not approve'
        assert transport.calls[-1][0] == 'editMessageText'
        edits = sum(method == 'editMessageText' for method, _ in transport.calls)
        await click(6)
        assert sum(method == 'editMessageText' for method, _ in transport.calls) == edits
        assert transport.calls[-1][1]['show_alert'] is True, 'Consumed callback was accepted'
        # A scoped plugin must leave Hermes's own callback namespace reachable.
        await click(7, data='ea:deny:424242')
        assert transport.calls[-1][0] == 'answerCallbackQuery'
        assert not transport.calls[-1][1].get('show_alert'), 'Kubik swallowed a core callback'
        from webhook_contract import check_webhook
        await check_webhook(application, pairing, kubik.security)
        assert not errors and not turns and len(providers.calls) == before, 'Webhook started an agent turn'
        print('PASS: native Hermes plugin registration, TelegramApplication/PTB callback routing, '
              'owner approval, foreign/group/message/replay rejection, core callback and no agent turn', flush=True)
    finally:
        telegram._running = False
        if telegram._update_receipt_flush is not None:
            await telegram._update_receipt_flush
        await application.shutdown()
