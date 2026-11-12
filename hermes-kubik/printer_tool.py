"""Profile-local tool registration with a trusted inbound device binding."""
import asyncio
from contextlib import contextmanager
from contextvars import ContextVar
from concurrent.futures import TimeoutError as FutureTimeout
import hashlib
import json
import math
from pathlib import Path

from .printer_contract import PARAMETERS
from .printer_jobs import PrinterFailure
from .printer_image_origin import ImageOrigin
from .printer_image_raster import RasterError, prepare

CURRENT_PRINTER = ContextVar('kubik_printer_origin', default=None)
DESCRIPTION = ('Inspect or print text, QR codes, and images on this conversation\'s Kubik printer. '
               'To print an image, first generate it with image_generate and pass its returned kubik_image_ref as image_ref. '
               'Use status to obtain DPI and printable dimensions before selecting a size. '
               'For images, decide from the conversation whether a physical print is useful, even without '
               'an explicit print command. Print text and QR at the user\'s request. '
               'Dimensions are millimetres and text wraps without truncation. '
               'The reported width comes from the verified printer model, not sensing the loaded paper; '
               'dpi describes horizontal head dots; feed_dpi describes vertical rows and font height. '
               'charging=null means unknown; battery_pct is a level, not proof of charging. '
               'Personal Apple emoji need the local atlas; unsupported sequences must not be split or substituted. '
               'never guess a label height. A submitted result is not proof of paper output. '
               'Never retry a print with an unknown outcome. This tool only works in an authenticated Kubik turn.')


@contextmanager
def printer_origin(adapter, peer, event):
    origin = getattr(peer, 'printer_images', None)
    if origin is None:
        origin = ImageOrigin(loop=adapter.printer_loop)
        peer.printer_images = origin
    event._kubik_images = origin
    binding = (adapter, peer, event.message_id, adapter._event_session_key(event), origin)
    token = CURRENT_PRINTER.set(binding)
    try:
        yield
    finally:
        CURRENT_PRINTER.reset(token)


async def process_bound(adapter, event, session_key, run):
    peer = getattr(event, '_kubik_peer', None)
    if peer is None:
        return await run(event, session_key)
    with printer_origin(adapter, peer, event):
        return await run(event, session_key)


def verified_origin(adapter):
    from gateway.session_context import get_session_env
    from hermes_constants import get_hermes_home
    # Hermes 0.22.2 binds this ID around registry dispatch, but does not pass it
    # to plugin handlers. No invented ID permits an ambiguous print retry.
    binding = CURRENT_PRINTER.get()
    if not binding or binding[0] is not adapter:
        raise PrinterFailure('unauthorized')
    _, peer, event_id, session_key, images = binding
    expected = {'HERMES_SESSION_PLATFORM': 'kubik', 'HERMES_SESSION_CHAT_ID': peer.identity,
                'HERMES_SESSION_MESSAGE_ID': event_id, 'HERMES_SESSION_KEY': session_key}
    if any(get_session_env(key, '') != value for key, value in expected.items()):
        raise PrinterFailure('unauthorized')
    if Path(get_hermes_home()).resolve() != Path(adapter.home).resolve():
        raise PrinterFailure('unauthorized')
    if adapter._ready_peer(peer.identity) is not peer:
        raise PrinterFailure('device_replaced')
    return peer, event_id, images


def call_origin(adapter):
    from tools.approval_context import _approval_tool_call_id
    peer, event_id, images = verified_origin(adapter)
    call_id = _approval_tool_call_id.get()
    if not call_id:
        raise PrinterFailure('invalid_input')
    return peer, call_id, event_id, images


class PrinterTool:
    def __init__(self):
        self.adapter = None

    def execute_sync(self, arguments, **_):
        # Hermes interrupts the registry worker thread, not its async bridge.
        from tools.interrupt import is_interrupted
        adapter = self.adapter
        if adapter is None or adapter.printer_loop is None or not adapter.printer_loop.is_running():
            return json.dumps(PrinterFailure('device_offline').value())
        if is_interrupted():
            return json.dumps(PrinterFailure('cancelled').value())
        pending = asyncio.run_coroutine_threadsafe(self.execute(arguments), adapter.printer_loop)
        while True:
            if is_interrupted():
                printing = isinstance(arguments, dict) and arguments.get('action') == 'print'
                pending.cancel()
                return json.dumps(PrinterFailure('cancelled', outcome='unknown' if printing else 'not_sent').value())
            try:
                return pending.result(timeout=.1)
            except FutureTimeout:
                continue

    async def execute(self, arguments, **_):
        try:
            adapter = self.adapter
            if adapter is None or adapter.printer_loop is None or not adapter.printer_loop.is_running():
                raise PrinterFailure('device_offline')
            peer, call_id, event_id, images = call_origin(adapter)
            raster = await image_for_print(adapter, peer, call_id, arguments, event_id, images)
            value = await on_gateway(adapter, peer, call_id, arguments, event_id, raster)
        except PrinterFailure as error:
            value = error.value()
        except (ImportError, AttributeError, RuntimeError):
            value = PrinterFailure('unauthorized').value()
        if value.get('status') == 'error':
            value = {**value, 'error': value['code']}
        return json.dumps(value, ensure_ascii=False, allow_nan=False)



async def on_gateway(adapter, peer, call_id, arguments, event_id, raster=None):
    operation = (adapter.printer_jobs.request(peer, call_id, arguments, event_id, raster) if raster is not None
                 else adapter.printer_jobs.request(peer, call_id, arguments, event_id))
    if asyncio.get_running_loop() is adapter.printer_loop:
        return await operation
    try:
        pending = asyncio.run_coroutine_threadsafe(operation, adapter.printer_loop)
    except BaseException:
        operation.close()
        raise
    action = 'print' if isinstance(arguments, dict) and arguments.get('action') == 'print' else 'status'
    try:
        return await asyncio.wait_for(asyncio.wrap_future(pending), adapter.printer_jobs.timeouts[action] + 1)
    except TimeoutError:
        raise PrinterFailure('timeout', outcome='unknown' if action == 'print' else 'not_sent') from None



async def image_for_print(adapter, peer, call_id, arguments, event_id, images):
    if not isinstance(arguments, dict) or arguments.get('format') != 'image':
        return None
    from . import printer_contract as contract
    if contract.parameters(arguments) is None:
        raise PrinterFailure('invalid_input')
    if images.is_uncertain(arguments['image_ref']):
        raise PrinterFailure('outcome_unknown', outcome='unknown')
    data = images.get(arguments['image_ref'])
    if data is None:
        raise PrinterFailure('invalid_image_ref')
    if getattr(peer, 'printer_image_version', None) != 1:
        raise PrinterFailure('printer_unsupported')
    status = await on_gateway(adapter, peer, call_id + ':image-status', {'action': 'status'}, event_id)
    if status.get('status') != 'ready':
        raise PrinterFailure(status.get('code', 'printer_failed'))
    profile = status['printer']
    if arguments.get('width_mm', 0) > profile['printable_width_mm'] + .01:
        raise PrinterFailure('content_does_not_fit')
    width_mm = min(arguments.get('width_mm', profile['printable_width_mm']), profile['printable_width_mm'])
    height_mm = min(arguments.get('height_mm', profile['max_height_mm']), profile['max_height_mm'])
    try:
        return await asyncio.to_thread(prepare, data, head_width=profile['width_dots'],
            width_dots=math.floor(width_mm * profile['dpi'] / 25.4),
            height_limit=math.floor(height_mm * profile['feed_dpi'] / 25.4),
            dpi=profile['dpi'], feed_dpi=profile['feed_dpi'])
    except RasterError as error:
        raise PrinterFailure(str(error)) from None


def image_hook(tool, **kwargs):
    if kwargs.get('tool_name') != 'image_generate' or tool.adapter is None:
        return None
    try:
        _, event_id, images = verified_origin(tool.adapter)
        call_id = kwargs.get('tool_call_id')
        if not isinstance(call_id, str) or not call_id:
            return None
        scoped_call = hashlib.sha256((event_id + '\n' + call_id).encode()).hexdigest()
        ref = images.capture(tool.adapter.home, scoped_call, kwargs.get('result'))
        if not ref:
            return None
        result = kwargs['result']
        value = json.loads(result) if isinstance(result, str) else dict(result)
        value['kubik_image_ref'] = ref
        value['kubik_print_instruction'] = ('Decide from the conversation whether a physical copy is useful. '
            'If it is, call kubik_printer with action=print, format=image and image_ref=kubik_image_ref. '
            'The print is a separate tool call; generation alone did not print anything.')
        return json.dumps(value, ensure_ascii=False)
    except (PrinterFailure, ImportError, AttributeError, RuntimeError, TypeError, ValueError):
        return None


def register_printer(ctx):
    tool = PrinterTool()
    ctx.register_hook('transform_tool_result', lambda **kwargs: image_hook(tool, **kwargs))
    ctx.register_tool(name='kubik_printer', toolset='kubik',
                      schema={'name': 'kubik_printer', 'description': DESCRIPTION, 'parameters': PARAMETERS},
                      handler=tool.execute_sync, is_async=False, description='Print with this Kubik', emoji='🖨️')
    return tool
