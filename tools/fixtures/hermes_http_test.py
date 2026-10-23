"""The WebSocket listener never decodes HTTP bodies before device authentication."""
import hashlib
import ssl
from types import SimpleNamespace
from unittest.mock import patch

from aiohttp import ClientSession, ClientTimeout, Fingerprint

from hermes_test_support import KubikAdapter, ProtocolFixture


class HttpBodies(ProtocolFixture):
    async def test_compressed_http_bodies_do_not_reach_decoders(self):
        adapter = KubikAdapter(SimpleNamespace(extra={'port': 19793, 'bind': '127.0.0.1'}, enabled=True))
        adapter.security = self.store
        await adapter.connect()
        self.addAsyncCleanup(adapter.disconnect)
        certificate = ssl.PEM_cert_to_DER_cert((self.store.home / 'cert.pem').read_text())
        fingerprint = Fingerprint(hashlib.sha256(certificate).digest())
        # A decoder is optional in the real host. Instrument its entry point without
        # feeding a malformed native payload or requiring an installed Brotli extra.
        with patch('aiohttp.http_parser.HAS_BROTLI', True), \
                patch('aiohttp.http_parser.BrotliDecompressor', side_effect=AssertionError('decoder reached')) as brotli, \
                patch('aiohttp.http_parser.ZLibDecompressor', side_effect=AssertionError('decoder reached')) as zlib:
            async with ClientSession(timeout=ClientTimeout(total=3)) as session:
                for encoding in ('br', 'gzip', 'deflate'):
                    for path, status in (('/kubik/v1', 405), ('/unknown', 404)):
                        with self.subTest(encoding=encoding, path=path):
                            async with session.post('https://127.0.0.1:19793' + path,
                                                    data=b'not a compressed stream', ssl=fingerprint,
                                                    headers={'Content-Encoding': encoding}) as response:
                                self.assertEqual(response.status, status)
                                await response.read()
                            brotli.assert_not_called()
                            zlib.assert_not_called()
            self.assertFalse(adapter.peers)
            self.assertFalse(adapter.sockets)
