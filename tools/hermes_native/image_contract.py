"""Exercise the real Hermes result-hook registry without a paid image API call."""
import asyncio
import importlib
import json
import threading
from pathlib import Path


def transform(result):
    from model_tools import _CallIds, _apply_transform_tool_result_hook
    return _apply_transform_tool_result_hook('image_generate', {}, result, 0,
                                             _CallIds(tool_call_id='generated-image-test'))


async def check_image_hook(adapter, device, home):
    from gateway.session_context import set_session_vars, clear_session_vars
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from PIL import Image
    from agent.system_prompt import _default_platform_hint
    hint = _default_platform_hint('kubik')
    assert 'decide from' in hint and 'Do not automatically print every response or image' in hint
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool')
    peer = next(peer for peer in adapter.peers.values() if peer.identity.startswith('kubik-printer:'))
    event = adapter._authorized_voice_event(peer, 'image-hook-test')
    path = Path(home) / 'cache/generated/images/fixture.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new('RGB', (8, 8), 'black').save(path)
    result = json.dumps({'success': True, 'provider': 'openai', 'image': str(path)})
    tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                              session_key=adapter._event_session_key(event))
    profile = set_hermes_home_override(home)
    try:
        with module.printer_origin(adapter, peer, event):
            value = json.loads(await asyncio.to_thread(transform, result))
            ref = value['kubik_image_ref']
            assert event._kubik_images.get(ref) == path.read_bytes()
            from model_tools import handle_function_call
            arguments = {'action': 'print', 'format': 'image', 'image_ref': ref, 'width_mm': 10}
            printed = await asyncio.to_thread(handle_function_call, 'kubik_printer', arguments,
                                              tool_call_id='print-image-test', enabled_tools=['kubik_printer'])
            printed = json.loads(printed)
            assert printed['status'] == 'sent' and printed['layout']['format'] == 'image', printed
            assert printed['paperConfirmed'] is False and device.image_bands
            before = len(device.image_bands)
            too_wide = {**arguments, 'width_mm': 100}
            rejected = await asyncio.to_thread(handle_function_call, 'kubik_printer', too_wide,
                                               tool_call_id='image-too-wide', enabled_tools=['kubik_printer'])
            assert json.loads(rejected)['code'] == 'content_does_not_fit'
            assert len(device.image_bands) == before
        await adapter.on_processing_complete(event, 'success')
        assert event._kubik_images.get(ref) == path.read_bytes()
        await check_document(adapter, device, peer, ref)
        await check_next_turn(adapter, device, peer, ref)
        event._kubik_images.close()
        assert event._kubik_images.get(ref) is None
        assert 'kubik_image_ref' not in json.loads(await asyncio.to_thread(transform, result))
    finally:
        reset_hermes_home_override(profile)
        clear_session_vars(tokens)
        path.unlink(missing_ok=True)
    print('PASS real Hermes image result hook, authenticated turn, snapshot, streamed tool call and cleanup', flush=True)


async def check_interrupt(adapter, device, ref):
    from model_tools import handle_function_call
    from tools.interrupt import set_interrupt
    from voice_fixture import eventually
    device.image, device.image_bands, device.hold_image_after = None, [], 1
    device.hold_abort_reply = True
    owner = {}
    def invoke():
        owner['thread'] = threading.get_ident()
        try:
            args = {'action': 'print', 'format': 'image', 'image_ref': ref, 'width_mm': 10}
            return json.loads(handle_function_call('kubik_printer', args, tool_call_id='image-interrupted',
                                                   enabled_tools=['kubik_printer']))
        finally:
            set_interrupt(False)
    task = asyncio.create_task(asyncio.to_thread(invoke))
    await eventually(lambda: 'thread' in owner and len(device.image_bands) == 1)
    set_interrupt(True, thread_id=owner['thread'])
    result = await asyncio.wait_for(task, 3)
    assert result['code'] == 'cancelled' and result['outcome'] == 'unknown', result
    await eventually(lambda: device.image_aborts == 1)
    peer = next(peer for peer in adapter.peers.values() if peer.identity.startswith('kubik-printer:'))
    assert peer.output_lock.locked() and peer.identity in adapter.printer_jobs.pending
    assert len(device.image_bands) == 1
    await device.send(t='printer_result', rid=device.image['rid'], status='error',
                      code='printer_disconnected', outcome='unknown')
    await eventually(lambda: not adapter.printer_jobs.pending)
    assert not peer.output_lock.locked() and len(device.image_bands) == 1
    print('PASS real Hermes thread interrupt aborts image stream without replay', flush=True)


async def check_next_turn(adapter, device, peer, ref):
    from gateway.session_context import set_session_vars, clear_session_vars
    from model_tools import handle_function_call
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool')
    event = adapter._authorized_voice_event(peer, 'print the previous image')
    device.image, device.image_bands = None, []
    device.hold_image_after, device.hold_abort_reply = None, False
    tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                              session_key=adapter._event_session_key(event))
    try:
        with module.printer_origin(adapter, peer, event):
            args = {'action': 'print', 'format': 'image', 'image_ref': ref, 'width_mm': 10}
            printed = await asyncio.to_thread(handle_function_call, 'kubik_printer', args,
                                               tool_call_id='next-turn-print', enabled_tools=['kubik_printer'])
            assert json.loads(printed)['status'] == 'sent'
            assert device.image_bands
            await check_interrupt(adapter, device, ref)
            before = len(device.image_bands)
            blocked = await asyncio.to_thread(handle_function_call, 'kubik_printer', args,
                                               tool_call_id='unknown-outcome-retry', enabled_tools=['kubik_printer'])
            assert json.loads(blocked)['code'] == 'outcome_unknown'
            assert len(device.image_bands) == before
    finally:
        clear_session_vars(tokens)
    print('PASS previously generated image prints in the next authenticated turn', flush=True)


async def check_document(adapter, device, peer, ref):
    from gateway.session_context import set_session_vars, clear_session_vars
    from model_tools import handle_function_call
    module = importlib.import_module(type(adapter).__module__.rsplit('.', 1)[0] + '.printer_tool')
    event = adapter._authorized_voice_event(peer, 'mixed document')
    device.image, device.image_bands = None, []
    tokens = set_session_vars(platform='kubik', chat_id=peer.identity, message_id=event.message_id,
                              session_key=adapter._event_session_key(event))
    try:
        with module.printer_origin(adapter, peer, event):
            before = len(device.all_image_jobs)
            args = {'action': 'print', 'format': 'document', 'width_mm': 30, 'blocks': [
                {'type': 'text', 'text': 'Заголовок и текст. ' * 40}, {'type': 'image', 'image_ref': ref, 'width_mm': 20},
                {'type': 'text', 'text': 'Текст после рисунка'}]}
            result = await asyncio.to_thread(handle_function_call, 'kubik_printer', args,
                                             tool_call_id='mixed-document', enabled_tools=['kubik_printer'])
            result = json.loads(result)
            assert result['status'] == 'sent' and result['layout']['format'] == 'document', result
            assert result['pagesSubmitted'] >= 2 and result['pagesSubmitted'] == len(device.all_image_jobs) - before
            assert device.image_bands and result['paperConfirmed'] is False
    finally:
        clear_session_vars(tokens)
    print('PASS multipage mixed text/image document through Hermes and the printer stream', flush=True)
