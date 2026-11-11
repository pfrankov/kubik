"""Deterministic v2 printer replies, with no device or paper effects."""
from copy import deepcopy

PRINTER = {'name': 'PeriPage+TEST', 'model': 'PeriPage A6+', 'firmware': 'V1.12_304dpi',
           'hardware': 'v3.38.21_AY', 'dpi': 304, 'width_dots': 576,
           'printable_width_mm': 576 * 25.4 / 304, 'battery_pct': 100,
           'formats': ['text', 'qr'], 'max_height_mm': 150, 'media_height_mm': None,
           'size_source': 'model_profile'}


def reply(request, status=None):
    printing = request['action'] == 'print'
    value = {'t': 'printer_result', 'rid': request['rid'], 'status': status or ('sent' if printing else 'ready'),
             'printer': deepcopy(PRINTER)}
    if printing:
        width = round(request.get('width_mm', PRINTER['printable_width_mm']) * 304 / 25.4)
        height = round(request.get('height_mm', 10) * 304 / 25.4)
        value.update(outcome='submitted', layout={'format': request.get('format', 'text'),
                     'width_dots': width, 'height_dots': height,
                     'width_mm': width * 25.4 / 304, 'height_mm': height * 25.4 / 304})
    return value
