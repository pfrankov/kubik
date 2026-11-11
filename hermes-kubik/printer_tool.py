"""Profile-local tool registration with a trusted inbound device binding."""
import asyncio
from contextlib import contextmanager
from contextvars import ContextVar
import json
from pathlib import Path

from .printer_contract import PARAMETERS
from .printer_jobs import PrinterFailure

CURRENT_PRINTER = ContextVar('kubik_printer_origin', default=None)
DESCRIPTION = ('Inspect or print text and QR codes on this conversation\'s Kubik printer. '
               'Use status to obtain DPI and printable dimensions before selecting a size. '
               'Print only at the user\'s request; dimensions are millimetres and text wraps without truncation. '
               'The reported width comes from the verified printer model, not sensing the loaded paper; '
               'never guess a label height. A submitted result is not proof of paper output. '
               'Never retry a print with an unknown outcome. This tool only works in an authenticated Kubik turn.')


@contextmanager
def printer_origin(adapter, peer, event):
    binding = (adapter, peer, event.message_id, adapter._event_session_key(event))
    token = CURRENT_PRINTER.set(binding)
    try:
        yield
    finally:
        CURRENT_PRINTER.reset(token)


def call_origin(adapter):
    from gateway.session_context import get_session_env
    from hermes_constants import get_hermes_home
    # Hermes 0.22.2 binds this ID around registry dispatch, but does not pass it
    # to plugin handlers. No invented ID permits an ambiguous print retry.
    from tools.approval_context import _approval_tool_call_id
    binding = CURRENT_PRINTER.get()
    if not binding or binding[0] is not adapter:
        raise PrinterFailure('unauthorized')
    _, peer, event_id, session_key = binding
    expected = {'HERMES_SESSION_PLATFORM': 'kubik', 'HERMES_SESSION_CHAT_ID': peer.identity,
                'HERMES_SESSION_MESSAGE_ID': event_id, 'HERMES_SESSION_KEY': session_key}
    if any(get_session_env(key, '') != value for key, value in expected.items()):
        raise PrinterFailure('unauthorized')
    if Path(get_hermes_home()).resolve() != Path(adapter.home).resolve():
        raise PrinterFailure('unauthorized')
    if adapter._ready_peer(peer.identity) is not peer:
        raise PrinterFailure('device_replaced')
    call_id = _approval_tool_call_id.get()
    if not call_id:
        raise PrinterFailure('invalid_input')
    return peer, call_id, event_id


class PrinterTool:
    def __init__(self):
        self.adapter = None

    async def execute(self, arguments, **_):
        try:
            adapter = self.adapter
            if adapter is None or adapter.printer_loop is None or not adapter.printer_loop.is_running():
                raise PrinterFailure('device_offline')
            peer, call_id, event_id = call_origin(adapter)
            value = await on_gateway(adapter, peer, call_id, arguments, event_id)
        except PrinterFailure as error:
            value = error.value()
        except (ImportError, AttributeError, RuntimeError):
            value = PrinterFailure('unauthorized').value()
        if value.get('status') == 'error':
            value = {**value, 'error': value['code']}
        return json.dumps(value, ensure_ascii=False, allow_nan=False)


async def on_gateway(adapter, peer, call_id, arguments, event_id):
    operation = adapter.printer_jobs.request(peer, call_id, arguments, event_id)
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


def register_printer(ctx):
    tool = PrinterTool()
    ctx.register_tool(name='kubik_printer', toolset='kubik',
                      schema={'name': 'kubik_printer', 'description': DESCRIPTION, 'parameters': PARAMETERS},
                      handler=tool.execute, is_async=True, description='Print with this Kubik', emoji='🖨️')
    return tool
