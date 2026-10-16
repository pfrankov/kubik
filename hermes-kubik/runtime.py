"""Native Hermes profile scope and passive capability checks; no process-global mutation."""

import contextlib
import importlib.util
import ipaddress
import logging
from pathlib import Path
import shutil

LOGGER = logging.getLogger(__name__)


def dependencies_available():
    return all(importlib.util.find_spec(name) is not None for name in ('aiohttp', 'cryptography'))


@contextlib.contextmanager
def profile_scope(home):
    from hermes_constants import set_hermes_home_override, reset_hermes_home_override
    from agent.secret_scope import build_profile_secret_scope, set_secret_scope, reset_secret_scope
    home_token = set_hermes_home_override(home)
    secret_token = None
    try:
        secret_token = set_secret_scope(build_profile_secret_scope(Path(home)), profile_home=str(home))
        yield
    finally:
        if secret_token is not None:
            reset_secret_scope(secret_token)
        reset_hermes_home_override(home_token)


def listener_options(config):
    from agent.secret_scope import get_secret
    extra = config.extra or {}
    port = get_secret('KUBIK_PORT')
    if port is None:
        port = extra.get('port', 18793)
    elif isinstance(port, str) and port.isascii() and port.isdecimal():
        port = int(port)
    if type(port) is not int or not 1024 <= port <= 65535:
        raise ValueError('Kubik port must be an integer from 1024 to 65535')
    bind = extra.get('bind', '0.0.0.0')
    if not isinstance(bind, str):
        raise ValueError('Kubik bind must be a listener IP address')
    ipaddress.ip_address(bind)
    return bind, port


def speech_available(home):
    from tools.transcription_tools import is_stt_enabled, _load_stt_config, _get_provider
    from tools.tts_tool import check_tts_requirements
    with profile_scope(home):
        try:
            config = _load_stt_config()
            stt = bool(is_stt_enabled(config) and _get_provider(config) != 'none')
        except Exception as error:
            LOGGER.warning('Hermes STT readiness failed: %s', type(error).__name__)
            stt = False
        try:
            tts = bool(shutil.which('ffmpeg') and check_tts_requirements())
        except Exception as error:
            LOGGER.warning('Hermes TTS readiness failed: %s', type(error).__name__)
            tts = False
    return {'available': stt}, {'available': tts}
