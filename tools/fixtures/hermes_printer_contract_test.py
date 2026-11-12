"""Printer dimensions and inputs fail closed before the physical device boundary."""
from copy import deepcopy
import unittest

from hermes_test_support import Peer, DeviceSocket
from hermes_printer_support import PRINTER, reply
from hermes_kubik import printer_contract as contract


class PrinterContract(unittest.TestCase):
    def test_valid_inputs_and_multibyte_limits(self):
        self.assertEqual(contract.parameters({'action': 'status'}), {'action': 'status'})
        self.assertEqual(contract.parameters({'action': 'print', 'text': 'Список\nМолоко\tХлеб'})['format'], 'text')
        self.assertIsNotNone(contract.parameters({'action': 'print', 'format': 'qr', 'text': 'я' * 128}))
        self.assertIsNone(contract.parameters({'action': 'print', 'format': 'qr', 'text': 'я' * 129}))
        self.assertIsNotNone(contract.parameters({'action': 'print', 'text': 'я' * 512}))
        self.assertIsNone(contract.parameters({'action': 'print', 'text': 'я' * 513}))

    def test_untrusted_inputs_and_dimension_bounds(self):
        bad = [None, [], {}, {'action': 'status', 'width_mm': 10}, {'action': 'print', 'text': '  '},
               {'action': 'print', 'text': '\x1b@'}, {'action': 'print', 'text': '\ud800'},
               {'action': 'print', 'text': 'X', 'url': 'file:///tmp/private'},
               {'action': 'print', 'text': 'X', 'format': 'qr', 'font_mm': 2}]
        bad += [{'action': 'print', 'text': 'X', key: value} for key, value in
                [('width_mm', float('nan')), ('width_mm', 10 ** 1000), ('width_mm', True), ('height_mm', 151), ('font_mm', 1.4)]]
        for value in bad:
            with self.subTest(value=repr(value)):
                self.assertIsNone(contract.parameters(value))

    def test_model_dimensions_are_complete_and_mathematically_consistent(self):
        self.assertTrue(contract.valid_printer(PRINTER))
        for key, value in [('dpi', True), ('feed_dpi', True), ('feed_dpi', 99), ('feed_dpi', 601),
                           ('charging', True), ('charging', False), ('charging', 0),
                           ('width_dots', 0), ('printable_width_mm', 56),
                           ('battery_pct', 101), ('formats', ['text']), ('media_height_mm', 30),
                           ('name', 'X' * 65), ('max_height_mm', float('nan'))]:
            with self.subTest(key=key):
                self.assertFalse(contract.valid_printer({**PRINTER, key: value}))
        for missing in ('feed_dpi', 'charging'):
            old = deepcopy(PRINTER); old.pop(missing)
            self.assertFalse(contract.valid_printer(old))

    def test_native_profile_reports_matching_raster_axes(self):
        request = {'action': 'print', 'rid': 'a' * 32, 'text': 'X', 'width_mm': 20, 'height_mm': 10}
        value = reply(request)
        self.assertEqual(value['layout']['width_dots'], 239)
        self.assertEqual(value['layout']['height_dots'], 120)
        self.assertAlmostEqual(value['layout']['height_mm'], 120 * 25.4 / 304)
        self.assertTrue(contract.result(value, 'print'))
        old_math = deepcopy(value)
        old_math['layout']['height_mm'] = 120 * 25.4 / 152
        self.assertFalse(contract.result(old_math, 'print'))
        too_tall = deepcopy(value)
        too_tall['layout'].update(height_dots=1796, height_mm=1796 * 25.4 / 304)
        self.assertFalse(contract.result(too_tall, 'print'))

    def test_requested_height_and_font_tolerances_use_feed_rows(self):
        request = {'action': 'print', 'rid': 'a' * 32, 'text': 'X',
                   'width_mm': 20.1, 'height_mm': 10.1, 'font_mm': 3}
        value = reply(request)
        value['layout'].update(height_dots=121, height_mm=121 * 25.4 / 304, font_mm=3.06)
        self.assertTrue(contract.result(value, 'print'))
        self.assertTrue(contract.requested_layout(value, request))
        wide = deepcopy(value)
        wide['layout'].update(width_dots=242, width_mm=242 * 25.4 / 304)
        self.assertTrue(contract.result(wide, 'print'))
        self.assertFalse(contract.requested_layout(wide, request))
        tall = deepcopy(value); tall['layout'].update(height_dots=122, height_mm=122 * 25.4 / 304)
        self.assertFalse(contract.requested_layout(tall, request))
        large_font = deepcopy(value); large_font['layout']['font_mm'] = 3.18
        self.assertFalse(contract.requested_layout(large_font, request))

    def test_nominal_square_qr_uses_equal_head_dots_and_feed_rows(self):
        value = reply({'action': 'print', 'rid': 'a' * 32, 'text': 'https://example.com',
                       'format': 'qr', 'width_mm': 240 * 25.4 / 304})
        self.assertEqual(value['layout']['width_dots'], 240)
        self.assertEqual(value['layout']['height_dots'], 240)
        self.assertEqual(value['layout']['width_mm'], value['layout']['height_mm'])
        self.assertTrue(contract.result(value, 'print'))

    def test_result_requires_actual_capabilities_and_layout(self):
        request = {'action': 'print', 'rid': 'a' * 32, 'text': 'X', 'width_mm': 20, 'height_mm': 10}
        value = reply(request)
        self.assertTrue(contract.result(value, 'print'))
        self.assertTrue(contract.requested_layout(value, request))
        for key in ('printer', 'layout', 'outcome'):
            wrong = deepcopy(value); wrong.pop(key)
            self.assertFalse(contract.result(wrong, 'print'))
        wrong = deepcopy(value); wrong['layout']['width_mm'] = 56
        self.assertFalse(contract.result(wrong, 'print'))
        wrong = reply({**request, 'width_mm': 40})
        self.assertFalse(contract.requested_layout(wrong, request))
        wrong = deepcopy(value); wrong['layout']['format'] = 'qr'
        self.assertFalse(contract.requested_layout(wrong, request))

    def test_error_outcomes_and_action_separation(self):
        value = {'t': 'printer_result', 'rid': 'a' * 32, 'status': 'error', 'code': 'printer_not_found'}
        self.assertTrue(contract.result(value, 'status'))
        self.assertFalse(contract.result(value, 'print'))
        self.assertTrue(contract.result({**value, 'outcome': 'not_sent'}, 'print'))
        self.assertTrue(contract.result({**value, 'outcome': 'unknown'}, 'print'))
        self.assertFalse(contract.result({**value, 'outcome': 'submitted'}, 'print'))
        self.assertFalse(contract.result({**value, 'status': 'not_configured'}, 'status'))
        self.assertFalse(contract.result({**value, 'code': {}}, 'status'))
        self.assertFalse(contract.result(reply({'action': 'status', 'rid': 'a' * 32}), 'print'))

    def test_capability_version_does_not_break_unrelated_voice(self):
        for version in (None, 1, 2, 255):
            self.assertEqual(Peer(DeviceSocket(), 'id', {'printer': version}).printer_version, version)
        for version in (True, 0, -1, 256, '2'):
            with self.assertRaises(ValueError):
                Peer(DeviceSocket(), 'id', {'printer': version})


if __name__ == '__main__':
    unittest.main()
