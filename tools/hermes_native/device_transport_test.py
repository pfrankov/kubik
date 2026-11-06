"""Wi-Fi fixture scope is explicit and cannot change the saved device endpoint."""
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock

from device_transport import binding, connect, validate_baseline, validate_options


class TransportTests(unittest.IsolatedAsyncioTestCase):
    def options(self, **changes):
        return SimpleNamespace(**(dict(wifi=True, endpoint='kubik://152.53.200.27:18793', port=18793) | changes))

    def baseline(self, **changes):
        return dict(server_pinned=True, url='kubik://152.53.200.27:18793', online=False, via='none') | changes

    async def test_wifi_uses_the_exact_saved_endpoint_and_no_wifi_pause_command(self):
        args = self.options()
        validate_baseline(args, self.baseline())
        self.assertEqual(binding(args), '0.0.0.0')
        control = SimpleNamespace(sim=AsyncMock(), until=AsyncMock())
        await connect(args, control)
        control.sim.assert_not_awaited()
        predicate = control.until.await_args.args[0]
        self.assertTrue(predicate(dict(online=True, via='wifi')))
        self.assertFalse(predicate(dict(online=True, via='usb')))

    async def test_usb_default_retains_loopback_listener_and_runtime_pause(self):
        args = self.options(wifi=False, endpoint=None, port=18999)
        validate_baseline(args, self.baseline(server_pinned=False))
        self.assertEqual(binding(args), '127.0.0.1')
        control = SimpleNamespace(sim=AsyncMock(), until=AsyncMock())
        await connect(args, control)
        control.sim.assert_awaited_once_with('wifi', ms=120000)
        self.assertTrue(control.until.await_args.args[0](dict(online=True, via='usb')))

    async def test_wrong_host_port_unpinned_and_still_online_are_rejected(self):
        for changes in (dict(url='kubik://152.53.200.28:18793'), dict(url='kubik://152.53.200.27:18790'),
                        dict(server_pinned=False), dict(online=True, via='wifi'), dict(via='usb')):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_baseline(self.options(), self.baseline(**changes))

    async def test_explicit_endpoint_and_listener_port_are_required(self):
        for changes in (dict(endpoint=None), dict(port=18999), dict(wifi=False),
                        dict(endpoint='https://152.53.200.27:18793'),
                        dict(endpoint='kubik://user:secret@152.53.200.27:18793'),
                        dict(endpoint='kubik://152.53.200.27'),
                        dict(endpoint='kubik://152.53.200.27:18793/other')):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_options(self.options(**changes))


if __name__ == '__main__':
    unittest.main()
