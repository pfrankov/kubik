"""Restore the observed voice mode while the approved fixture transport still exists."""
import asyncio

from device_control import save


def failure(error):
    return {'type': type(error).__name__, 'message': str(error)[:500]}


def peer_state(peer):
    if peer is None:
        return {'present': False}
    return {'present': True, 'ready': peer.ready, 'closed': peer.socket.closed,
            'close_code': getattr(peer.socket, 'close_code', None),
            'native': peer.native is not None}


async def ready_peer(adapter, identity):
    async with asyncio.timeout(12):
        while True:
            peer = adapter._ready_peer(identity)
            if peer:
                return peer
            await asyncio.sleep(.05)


async def stop_capture(peer, control, report):
    await control.sim('ptt_up')
    state = await control.info()
    if state['mic_open'] or state['live_active']:
        # Device-driven cancellation orders capture shutdown before its cancel command.
        if state['screen_dark']:
            await control.sim('pwr')
        if not state['menu']:
            await control.sim('menu')
        await control.until(lambda value: not value['mic_open'] and not value['live_active'],
                            'capture stopped before fixture restoration', timeout=10)
    if peer.native:
        try:
            await asyncio.wait_for(peer.native.close(), 10)
        except (ConnectionError, TimeoutError) as error:
            report['native_close_error'] = failure(error)
    report['after_capture_peer'] = peer_state(peer)


async def restore_mode(adapter, peer, control, original, report):
    identity = peer.identity
    report['initial_peer'] = peer_state(peer)
    # A replacement handshake must publish the baseline, even if the old socket is gone.
    adapter.voice_controls.store.select(identity, 'mode', original['voice_mode'])
    await stop_capture(peer, control, report)
    for attempt in range(2):  # At most one replacement connection; never rewrite device NVS.
        current = await ready_peer(adapter, identity)
        report.setdefault('publish_attempts', []).append(peer_state(current))
        try:
            await adapter.voice_controls.publish(current)
            await control.until(lambda value: value['voice_mode'] == original['voice_mode'] and
                                not value['mic_open'] and not value['live_active'],
                                'original voice mode on fixture transport', timeout=10)
            if adapter._ready_peer(identity) is not current:
                raise ConnectionError('Fixture peer changed while restoring voice mode')
            report['mode_restored_before_release'] = True
            return
        except (ConnectionError, RuntimeError, TimeoutError) as error:
            report['publish_attempts'][-1]['error'] = failure(error)
            if attempt or adapter._ready_peer(identity) is current:
                raise
    raise AssertionError('Voice mode restoration exhausted its bounded reconnect')


async def cleanup(gateway, adapter, peer, control, original, observations, backend, providers, primary,
                  *, release_wifi_pause=True):
    report = {'primary_error': failure(primary) if primary else None, 'steps': []}

    async def step(name, coroutine, timeout):
        item = {'name': name}
        report['steps'].append(item)
        try:
            await asyncio.wait_for(coroutine, timeout)
            item['ok'] = True
        except Exception as error:
            item['error'] = failure(error)

    if adapter and peer and original:
        await step('restore_voice_before_transport_release',
                   restore_mode(adapter, peer, control, original, report), 50)
    if gateway:
        await step('stop_gateway', gateway.stop(), 30)
    if original:
        await step('restore_route_settings_screen',
                   control.restore(original, release_wifi_pause=release_wifi_pause), 55)
    if observations:
        try:
            observations.persist(control.evidence, providers)
        except Exception as error:
            report['steps'].append({'name': 'persist_observations', 'error': failure(error)})
        observations.restore()
    if backend:
        await step('close_provider_fixture', backend.cleanup(), 10)
    await step('close_control', control.http.close(), 5)
    report_cleanup(control, report, primary)


def report_cleanup(control, report, primary):
    errors = [item for item in report['steps'] if 'error' in item]
    report['ok'] = not errors
    save(control.evidence / 'cleanup.json', report)
    if errors:
        detail = '; '.join(item['name'] + ': ' + item['error']['message'] for item in errors)
        if primary is not None:
            primary.add_note('Fixture cleanup also failed: ' + detail)
        else:
            raise RuntimeError('Fixture cleanup failed: ' + detail)
