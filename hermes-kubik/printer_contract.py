"""The v2 printer wire contract; dimensions describe the print head, not sensed paper."""
import math
import re
import unicodedata

VERSION = 2
TEXT_BYTES = 1024
QR_BYTES = 256
RESULT_CODES = {'invalid_request', 'printer_busy', 'printer_not_found', 'printer_ambiguous',
                'printer_unsupported', 'printer_failed', 'printer_disconnected',
                'content_does_not_fit', 'unsupported_character'}
PARAMETERS = {
    'type': 'object', 'additionalProperties': False, 'required': ['action'],
    'properties': {
        'action': {'type': 'string', 'enum': ['status', 'print']},
        'text': {'type': 'string', 'minLength': 1, 'maxLength': TEXT_BYTES},
        'format': {'type': 'string', 'enum': ['text', 'qr', 'image', 'document']},
        'image_ref': {'type': 'string', 'pattern': '^[0-9a-f]{32}$'},
        'blocks': {'type': 'array', 'minItems': 1, 'maxItems': 20, 'items': {
            'type': 'object', 'additionalProperties': False, 'required': ['type'],
            'properties': {'type': {'type': 'string', 'enum': ['text', 'image']},
                           'text': {'type': 'string', 'minLength': 1, 'maxLength': 4096},
                           'image_ref': {'type': 'string', 'pattern': '^[0-9a-f]{32}$'},
                           'width_mm': {'type': 'number', 'minimum': 1, 'maximum': 216},
                           'font_mm': {'type': 'number', 'minimum': 1.5, 'maximum': 8}}}},
        'width_mm': {'type': 'number', 'minimum': 1, 'maximum': 216},
        'height_mm': {'type': 'number', 'minimum': 1, 'maximum': 150},
        'font_mm': {'type': 'number', 'minimum': 1.5, 'maximum': 8},
    },
}


def number(value, low, high):
    return type(value) in (int, float) and low <= value <= high and math.isfinite(value)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def request_id(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{32}', value) is not None


def safe_string(value, maximum=64):
    if not isinstance(value, str) or not value.strip():
        return False
    try:
        size = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        return False
    return size <= maximum and not any(unicodedata.category(c).startswith('C') for c in value)


def valid_text(value, maximum):
    if not isinstance(value, str) or not value.strip():
        return False
    try:
        size = len(value.encode('utf-8'))
    except UnicodeEncodeError:
        return False
    return size <= maximum and not any((ord(c) < 32 and c not in '\n\t') or 127 <= ord(c) <= 159 for c in value)


def parameters(value):
    if not isinstance(value, dict) or value.keys() - PARAMETERS['properties'].keys():
        return None
    if value.get('action') == 'status':
        return {'action': 'status'} if value.keys() == {'action'} else None
    kind = value.get('format', 'text')
    if value.get('action') != 'print' or kind not in ('text', 'qr', 'image', 'document'):
        return None
    if not valid_content(value, kind):
        return None
    if not print_dimensions(value, kind):
        return None
    return {**value, 'format': kind}


def valid_content(value, kind):
    if kind == 'document':
        return 'text' not in value and 'image_ref' not in value and valid_blocks(value.get('blocks'))
    if 'blocks' in value:
        return False
    if kind == 'image':
        return request_id(value.get('image_ref')) and 'text' not in value and 'font_mm' not in value
    return 'image_ref' not in value and valid_text(value.get('text'), QR_BYTES if kind == 'qr' else TEXT_BYTES)


def valid_blocks(blocks):
    if not isinstance(blocks, list) or not 1 <= len(blocks) <= 20:
        return False
    if not all(valid_block(block) for block in blocks):
        return False
    return sum(len(block.get('text', '').encode('utf-8')) for block in blocks) <= 8192


def valid_block(block):
    if not isinstance(block, dict):
        return False
    if block.get('type') == 'text':
        return (not block.keys() - {'type', 'text', 'font_mm'} and valid_text(block.get('text'), 4096)
                and ('font_mm' not in block or number(block['font_mm'], 1.5, 8)))
    if block.get('type') == 'image':
        return (not block.keys() - {'type', 'image_ref', 'width_mm'} and request_id(block.get('image_ref'))
                and ('width_mm' not in block or number(block['width_mm'], 1, 216)))
    return False


def image_refs(value):
    if value.get('format') == 'image':
        return [value['image_ref']]
    if value.get('format') == 'document':
        return list(dict.fromkeys(block['image_ref'] for block in value['blocks'] if block['type'] == 'image'))
    return []


def print_dimensions(value, kind):
    bounds = {'width_mm': (1, 216), 'height_mm': (1, 150), 'font_mm': (1.5, 8)}
    if any(key in value and not number(value[key], *limit) for key, limit in bounds.items()):
        return False
    return kind != 'qr' or 'font_mm' not in value


def valid_printer(value):
    fields = {'name', 'model', 'firmware', 'hardware', 'dpi', 'feed_dpi', 'width_dots', 'printable_width_mm',
              'battery_pct', 'charging', 'formats', 'max_height_mm', 'media_height_mm', 'size_source'}
    if not isinstance(value, dict) or value.keys() != fields:
        return False
    strings = all(safe_string(value[key]) for key in ('name', 'model', 'firmware', 'hardware'))
    dimensions = integer(value['dpi'], 100, 600) and integer(value['feed_dpi'], 100, 600) and integer(value['width_dots'], 1, 4096)
    if not strings or not dimensions or not number(value['printable_width_mm'], 0, 216):
        return False
    width = value['width_dots'] * 25.4 / value['dpi']
    exact = abs(value['printable_width_mm'] - width) <= .01
    return exact and _printer_profile(value)


def _printer_profile(value):
    formats = isinstance(value['formats'], list) and value['formats'] in (['text', 'qr'], ['qr', 'text'])
    return integer(value['battery_pct'], 0, 100) and value['charging'] is None and formats and number(value['max_height_mm'], 1, 150) and value['media_height_mm'] is None and value['size_source'] == 'model_profile'


def valid_layout(value, printer, image=False):
    required = {'format', 'width_dots', 'height_dots', 'width_mm', 'height_mm'}
    if not isinstance(value, dict) or not required <= value.keys() or value.keys() - required - {'font_mm'}:
        return False
    if value['format'] not in (['image'] if image else printer['formats']) or not integer(value['width_dots'], 1, printer['width_dots']):
        return False
    maximum = math.floor(printer['max_height_mm'] * printer['feed_dpi'] / 25.4)
    if not integer(value['height_dots'], 1, maximum):
        return False
    for key, resolution in (('width', printer['dpi']), ('height', printer['feed_dpi'])):
        expected = value[key + '_dots'] * 25.4 / resolution
        if not number(value[key + '_mm'], 0, 216) or abs(value[key + '_mm'] - expected) > .01:
            return False
    if 'font_mm' in value:
        return value['format'] == 'text' and number(value['font_mm'], 1.5, 8)
    return True


def result(value, action, image=False):
    return _result_header(value, action) and _outcome(value, action) and _result_payload(value, image)


def _result_header(value, action):
    fields = {'t', 'rid', 'status', 'code', 'outcome', 'printer', 'layout'}
    if not isinstance(value, dict) or value.keys() - fields or value.get('t') != 'printer_result' or not request_id(value.get('rid')):
        return False
    state = value.get('status')
    if state not in ('ready', 'sent', 'error') or (state == 'ready' and action != 'status') or (state == 'sent' and action != 'print'):
        return False
    return True


def _result_payload(value, image=False):
    state = value['status']
    printer = value.get('printer')
    if state != 'error' and printer is None:
        return False
    if printer is not None and not valid_printer(printer):
        return False
    layout = value.get('layout')
    if state == 'ready' and layout is not None:
        return False
    if state == 'sent' and layout is None:
        return False
    return layout is None or printer is not None and valid_layout(layout, printer, image)


def _outcome(value, action):
    if value['status'] == 'error':
        if not isinstance(value.get('code'), str) or value['code'] not in RESULT_CODES:
            return False
    elif 'code' in value:
        return False
    if action == 'status':
        return 'outcome' not in value
    expected = ('submitted',) if value['status'] == 'sent' else ('not_sent', 'unknown')
    return value.get('outcome') in expected


def requested_layout(value, request):
    layout, printer = value.get('layout'), value.get('printer')
    if layout is None:
        return True
    expected = 'image' if request.get('format') == 'document' else request.get('format', 'text')
    if layout['format'] != expected:
        return False
    for key, resolution in (('width_mm', printer['dpi']), ('height_mm', printer['feed_dpi'])):
        tolerance = 25.4 / resolution
        if key in request and layout[key] > request[key] + tolerance:
            return False
    return request.get('format') == 'document' or 'font_mm' not in request or 'font_mm' in layout and layout['font_mm'] <= request['font_mm'] + 25.4 / printer['feed_dpi']
