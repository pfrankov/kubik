"""Profile-scoped native voice configuration and bounded paired-device preferences."""
import contextlib
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
from urllib.parse import urlsplit

from .runtime import profile_scope
from .security import atomic_json

MODES = ('classic', 'realtime', 'live')
MODE_LABELS = ('STT', 'Realtime', 'GPT Live')
DEFAULT_MODELS = {'realtime': 'gpt-realtime-2.1', 'live': 'gpt-live-1'}
MODEL_PATTERN = re.compile(r'openai/[a-zA-Z0-9][a-zA-Z0-9._-]{0,119}\Z')
IDENTITY_PATTERN = re.compile(r'[a-z0-9][a-z0-9_-]{0,63}:[0-9a-f]{32}\Z')


def native_settings(home, mode, selection=None):
    """Read Hermes's existing OpenAI audio credentials without changing its profile."""
    from hermes_cli.config import load_config
    from tools.tool_backend_helpers import resolve_openai_audio_api_key
    if mode not in DEFAULT_MODELS:
        raise ValueError('Invalid native voice mode')
    with profile_scope(home):
        voice = load_config().get('voice', {})
        live = voice.get('gpt_live', {}) if isinstance(voice, dict) else {}
        if not isinstance(live, dict):
            raise ValueError('Invalid Hermes voice configuration')
        key = live.get('api_key') or resolve_openai_audio_api_key()
        model = live.get('model', DEFAULT_MODELS[mode]) if mode == 'live' else DEFAULT_MODELS[mode]
        settings = {'api_key': key, 'base_url': live.get('base_url', 'https://api.openai.com/v1'),
                    'model': model, 'voice': live.get('voice', 'marin'),
                    'instructions': live.get('instructions', '')}
    validate_settings(settings)
    if selection is not None and selection != f'openai/{model}':
        raise ValueError('Selected native voice model is no longer configured')
    if mode == 'live' and importlib.util.find_spec('wasmtime') is None:
        raise ValueError('Echo cancellation runtime is unavailable')
    return settings


def validate_settings(settings):
    if not isinstance(settings['api_key'], str) or not settings['api_key'].strip():
        raise ValueError('Native voice credentials are unavailable')
    if not MODEL_PATTERN.fullmatch(f"openai/{settings['model']}"):
        raise ValueError('Invalid native voice model')
    for key, limit in [('voice', 80), ('instructions', 8192), ('base_url', 2048)]:
        value = settings[key]
        if not isinstance(value, str) or len(value) > limit or '\0' in value:
            raise ValueError('Invalid native voice configuration')
    validate_provider_url(settings['base_url'])


def validate_provider_url(value):
    url = urlsplit(value)
    if url.scheme != 'https' or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise ValueError('Native voice requires an HTTPS provider URL')


def native_status(home, mode, selection=None):
    try:
        settings = native_settings(home, mode, selection)
        return {'available': True, 'provider': 'openai', 'model': settings['model']}
    except (ImportError, ValueError, OSError, TypeError, KeyError):
        return {'available': False}


def validate_state(state):
    if not isinstance(state, dict) or state.get('version') != 1:
        raise ValueError('Invalid native voice preferences')
    entries = state.get('entries')
    if not isinstance(entries, dict) or len(entries) > 128:
        raise ValueError('Invalid native voice preference entries')
    for identity, choices in entries.items():
        if not IDENTITY_PATTERN.fullmatch(identity) or not isinstance(choices, dict):
            raise ValueError('Invalid native voice preference identity')
        for name, value in choices.items():
            valid = value in MODES if name == 'mode' else (
                name in DEFAULT_MODELS and isinstance(value, str) and MODEL_PATTERN.fullmatch(value))
            if not valid:
                raise ValueError('Invalid native voice preference')
    return entries


class VoicePreferences:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.path = self.directory / 'voice.json'

    def read(self):
        try:
            descriptor = os.open(self.path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        except FileNotFoundError:
            return {}
        with os.fdopen(descriptor, 'rb') as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise ValueError('Invalid native voice preference file')
            data = stream.read(65537)
        if len(data) > 65536:
            raise ValueError('Native voice preference file is too large')
        return validate_state(json.loads(data))

    def lookup(self, identity):
        return dict(self.read().get(identity, {}))

    def select(self, identity, field, value):
        descriptor = os.open(self.directory / 'voice.lock',
                             os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        with os.fdopen(descriptor, 'a') as lock:
            if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
                raise ValueError('Invalid native voice preference lock')
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                entries = self.read()
                entries[identity] = {**entries.get(identity, {}), field: value}
                state = {'version': 1, 'entries': entries}
                validate_state(state)
                atomic_json(self.path, state)
            finally:
                with contextlib.suppress(OSError):
                    fcntl.flock(lock, fcntl.LOCK_UN)
