"""Fixture restoration guards use a real local control server, never a USB device."""
import base64
import asyncio
from pathlib import Path
import tempfile
import unittest

from aiohttp import web
from device_control import Control, screen_restore_action


class PhysicalControlTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.state = {'ok': True, 'device': 'kubik-fixture', 'key': base64.b64encode(b'\4' + bytes(64)).decode(),
                      'character': 'Tess', 'voice_mode': 'classic', 'agent': 'hermes', 'ssid': '', 'url': '',
                      'server_pinned': True, 'volume': 34, 'ui_volume': 34, 'brightness': 200,
                      'guide_done': True, 'online': False, 'via': 'none', 'wifi_connected': False,
                      'screen_dark': False, 'screen_sleeping': False, 'menu': False, 'mic_open': False,
                      'live_active': False, 'auto_recording': False}
        self.requests = []
        app = web.Application()
        app.router.add_post('/config', self.handle)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, '127.0.0.1', 0)
        await self.site.start()
        port = self.site._server.sockets[0].getsockname()[1]
        self.control = Control(f'http://127.0.0.1:{port}', Path(self.directory.name))

    async def asyncTearDown(self):
        await self.control.http.close()
        await self.runner.cleanup()
        self.directory.cleanup()

    async def handle(self, request):
        value = await request.json()
        self.requests.append(value)
        if value['cmd'] == 'info':
            return web.json_response(self.state)
        self.assertEqual(value['cmd'], 'sim', 'Fixture wrote persistent configuration')
        flag = {'pwr': 'screen_dark', 'menu': 'menu'}.get(value['ev'])
        if flag == 'menu' and self.state['screen_dark']:
            pass  # BOOT/menu input is ignored after explicit PWR sleep on the real device.
        elif flag:
            self.state[flag] = not self.state[flag]
            if flag == 'screen_dark':
                self.state['screen_sleeping'] = False
        else:
            self.assertEqual(value, {'cmd': 'sim', 'ev': 'wifi', 'ms': 1})
        return web.json_response({'ok': True})

    async def test_offline_empty_network_is_preserved_and_runtime_screen_restored(self):
        original, identity = await self.control.baseline()
        self.assertTrue(identity.startswith('kubik-fixture:'))
        self.state.update(screen_dark=True, menu=True)
        await self.control.restore(original)
        self.assertEqual(self.state, original)
        self.assertTrue((Path(self.directory.name) / 'after.json').is_file())

    async def test_originally_offline_device_may_recover_its_real_wifi(self):
        original, _ = await self.control.baseline()
        self.state.update(online=True, via='wifi', wifi_connected=True)
        await self.control.restore(original)
        self.assertTrue(self.state['wifi_connected'])
        self.assertEqual(self.state['ssid'], '')

    async def test_wifi_fixture_cleanup_never_changes_runtime_radio_routing(self):
        original, _ = await self.control.baseline()
        await self.control.restore(original, release_wifi_pause=False)
        self.assertTrue(all(item['cmd'] == 'info' for item in self.requests))

    async def test_offline_saved_profile_rotation_does_not_write_network_configuration(self):
        self.state['ssid'] = 'Odin'
        original, _ = await self.control.baseline()
        self.state['ssid'] = 'LESTA_GUEST'
        await self.control.restore(original)
        self.assertEqual(self.state['ssid'], 'LESTA_GUEST')
        self.assertTrue(all(item['cmd'] in ('info', 'sim') for item in self.requests))

    async def test_online_network_change_remains_a_restoration_failure(self):
        self.state.update(online=True, via='wifi', wifi_connected=True, ssid='Odin')
        original, _ = await self.control.baseline()
        self.state['ssid'] = 'LESTA_GUEST'
        with self.assertRaisesRegex(AssertionError, 'setting changed: ssid'):
            await self.control.restore(original)

    async def test_menu_is_restored_before_returning_to_explicit_sleep(self):
        self.state.update(screen_dark=True, menu=True)
        original, _ = await self.control.baseline()
        self.state.update(screen_dark=False, menu=False)
        await self.control.restore(original)
        self.assertEqual(self.state, original)

    async def test_changed_saved_settings_are_reported_without_repair_writes(self):
        original, _ = await self.control.baseline()
        self.state['volume'] = 35
        with self.assertRaisesRegex(AssertionError, 'setting changed: volume'):
            await self.control.restore(original)
        self.assertTrue(all(item['cmd'] in ('info', 'sim') for item in self.requests))

    async def test_active_or_incomplete_baseline_is_rejected_before_mutation(self):
        self.state['mic_open'] = True
        with self.assertRaisesRegex(ValueError, 'active conversation'):
            await self.control.baseline()
        self.state['mic_open'] = False
        del self.state['key']
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            await self.control.baseline()
        self.assertTrue(all(item['cmd'] == 'info' for item in self.requests))

    async def test_control_rejects_remote_or_credential_urls(self):
        for url in ('http://example.com:18792', 'https://127.0.0.1', 'http://user:secret@127.0.0.1'):
            with self.assertRaises(ValueError):
                Control(url, Path(self.directory.name))

    def test_pure_power_plan_distinguishes_auto_sleep_and_manual_off(self):
        flags = ((False, False), (False, True), (True, False), (True, True))
        expected = (('done', 'power', 'power', 'power'),
                    ('wait', 'done', 'power', 'power'),
                    ('power', 'power', 'done', 'power'),
                    ('wait', 'wait', 'power', 'done'))
        for i, target in enumerate(flags):
            original = dict(zip(('screen_dark', 'screen_sleeping'), target))
            for j, actual in enumerate(flags):
                current = dict(zip(('screen_dark', 'screen_sleeping'), actual))
                self.assertEqual(screen_restore_action(original, current), expected[i][j])

    async def test_original_auto_sleep_waits_naturally_without_pressing_power(self):
        self.state.update(screen_dark=True, screen_sleeping=True)
        original, _ = await self.control.baseline()
        self.state.update(screen_dark=False, screen_sleeping=False)
        async def natural_sleep():
            await asyncio.sleep(1)
            self.state.update(screen_dark=True, screen_sleeping=True)
        timer = asyncio.create_task(natural_sleep())
        try:
            await self.control.restore(original)
        finally:
            await timer
        self.assertTrue(self.state['screen_sleeping'])
        self.assertFalse(any(row.get('ev') == 'pwr' for row in self.requests))

    async def test_original_manual_off_stays_manual_and_never_waits_for_auto_sleep(self):
        self.state.update(screen_dark=True, screen_sleeping=False)
        original, _ = await self.control.baseline()
        self.state.update(screen_dark=False, screen_sleeping=False)
        await self.control.restore(original)
        self.assertTrue(self.state['screen_dark'])
        self.assertFalse(self.state['screen_sleeping'])
        self.assertEqual(sum(row.get('ev') == 'pwr' for row in self.requests), 1)


if __name__ == '__main__':
    unittest.main()
