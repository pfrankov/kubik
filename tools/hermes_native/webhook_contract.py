"""Bounded HTTP checks against PTB's real local Tornado webhook listener."""
import asyncio
import time

from aiohttp import ClientSession


async def check_webhook(application, pairing, security):
    identity = 'kubik-webhook:native-key'
    code = security.pair(identity)
    assert await pairing.notify(identity, code)
    await application.start()
    updater = application.updater
    secret = 'local-test-webhook-secret'
    try:
        await updater.start_webhook(listen='127.0.0.1', port=0, url_path='kubik-test',
            webhook_url='https://localhost/kubik-test', secret_token=secret, bootstrap_retries=0)
        # PTB owns the socket; port=0 avoids a reserve/rebind race in parallel CI.
        sockets = updater._httpd._http_server._sockets
        port = next(iter(sockets.values())).getsockname()[1]
        url = f'http://127.0.0.1:{port}/kubik-test'
        update = {'update_id': 101, 'callback_query': {
            'id': 'webhook', 'from': {'id': 123, 'is_bot': False, 'first_name': 'Owner'},
            'chat_instance': 'local', 'data': f'kubik_pair:{code}',
            'message': {'message_id': 7, 'date': int(time.time()),
                        'chat': {'id': 123, 'type': 'private'}, 'text': 'Pairing'}}}
        async with ClientSession() as client:
            # Parsing precedes PTB's secret-token gate: over-limit input must fail there.
            over_limit = '&'.join(f'k{i}=1' for i in range(1001))
            async with client.post(url + '?' + over_limit, json=update) as response:
                assert response.status == 400, response.status
                await response.read()
            async with client.post(url, json=update) as response:
                assert response.status == 403, response.status
                await response.read()
            assert not security.approved(identity) and application.update_queue.empty()
            # The supported boundary and a valid secret still reach real callback dispatch.
            allowed = '&'.join(f'k{i}=1' for i in range(1000))
            async with client.post(url + '?' + allowed, json=update,
                    headers={'X-Telegram-Bot-Api-Secret-Token': secret}) as response:
                assert response.status == 200, response.status
                await response.read()
            await asyncio.wait_for(application.update_queue.join(), 5)
            assert security.approved(identity), 'Webhook update did not reach the pairing handler'
        print('PASS: local PTB webhook rejects >1000 query fields before auth, preserves secret gating and owner approval', flush=True)
    finally:
        if updater.running:
            await updater.stop()
        await application.stop()
