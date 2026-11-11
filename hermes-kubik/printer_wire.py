"""One compact UTF-8 JSON representation shared by byte validation and socket writes."""
import json


def dumps(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(',', ':'))


def packet(rid, parameters, glyphs):
    value = {'t': 'printer', 'rid': rid, **parameters}
    if parameters['action'] == 'print' and parameters.get('format', 'text') == 'text':
        value['emoji'] = glyphs
    if len(dumps(value).encode('utf-8')) > 4096:
        raise ValueError('Printer JSON exceeds LINK_JSON_MAX')
    return value
