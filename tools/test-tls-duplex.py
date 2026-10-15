#!/usr/bin/env python3
"""Exercise the real WSS transport with concurrent socket peers and TLS WANT states."""
from pathlib import Path
import subprocess
import hashlib
import runpy
import tempfile

ROOT = Path(__file__).resolve().parent.parent
patcher = runpy.run_path(str(ROOT / 'tools/patch-websocket.py'))
with tempfile.TemporaryDirectory(prefix='kubik-ws-patch-') as tmp:
    sdk = Path(tmp) / 'esp_websocket_client.c'
    sdk.write_bytes((ROOT / 'firmware/managed_components/espressif__esp_websocket_client/esp_websocket_client.c').read_bytes())
    if hashlib.sha256(sdk.read_bytes()).hexdigest() == patcher['PATCHED']:
        subprocess.run(['patch', '--batch', '-R', '-p0', '-i',
                        str(ROOT / 'tools/fixtures/websocket-duplex.patch')], cwd=tmp, check=True)
    assert hashlib.sha256(sdk.read_bytes()).hexdigest() == patcher['SOURCE']
    patcher['apply'](tmp)
    patched = sdk.read_bytes()
    patcher['apply'](tmp)
    assert sdk.read_bytes() == patched
    drifted = patched + b'\n/* unreviewed SDK */\n'
    sdk.write_bytes(drifted)
    try:
        patcher['apply'](tmp)
    except RuntimeError:
        assert sdk.read_bytes() == drifted
    else:
        raise AssertionError('Unreviewed SDK source was patched')

source = '\n'.join(line for line in (ROOT / 'firmware/main/link_tls.c').read_text().splitlines()
                   if not line.startswith('#include'))
fixture = (ROOT / 'tools/fixtures/tls_duplex_test.c').read_text()
with tempfile.TemporaryDirectory(prefix='kubik-tls-duplex-') as tmp:
    src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    src.write_text(fixture.replace('/* ACTUAL_TRANSPORT */', source))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-g', '-O1', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
    for define, diagnostic in (
        ('ESP_IDF_VERSION=0', 'Review the pinned transport'),
        ('CONFIG_MBEDTLS_SSL_RENEGOTIATION=1', 'Duplex TLS interleaving'),
    ):
        rejected = subprocess.run(['cc', '-std=c11', '-fsyntax-only', '-D' + define, str(src)],
                                  capture_output=True, text=True)
        assert rejected.returncode and diagnostic in rejected.stderr, 'Unsafe transport profile compiled'


# Keep the SDK's send/fragment/error behavior while eliminating unused TX bytes.
sdk_source = (ROOT / 'firmware/managed_components/espressif__esp_websocket_client/esp_websocket_client.c').read_text()
parts = {
    'ALLOCATOR': ('static esp_err_t esp_websocket_new_buf(', 'static void esp_websocket_free_buf('),
    'RELEASE': ('static void esp_websocket_free_buf(', 'static esp_err_t esp_websocket_client_dispatch_event('),
    'SEND': ('static int esp_websocket_client_send_with_exact_opcode(', 'esp_websocket_client_handle_t esp_websocket_client_init('),
}
buffer_fixture = (ROOT / 'tools/fixtures/websocket_buffer_test.c').read_text()
for name, (begin, end) in parts.items():
    buffer_fixture = buffer_fixture.replace('/* ACTUAL_' + name + ' */', sdk_source[sdk_source.index(begin):sdk_source.index(end)])
with tempfile.TemporaryDirectory(prefix='kubik-ws-buffers-') as tmp:
    src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    src.write_text(buffer_fixture)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-O1', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
