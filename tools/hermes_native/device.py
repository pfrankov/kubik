#!/usr/bin/env python3
"""Physical Kubik + real Hermes Gateway; loopback model providers, no service-management writes."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Existing Hermes v0.22.2 source (read-only)')
    parser.add_argument('--tls-identity', required=True, type=Path, help='Pinned device identity on this same host')
    parser.add_argument('--control', required=True, help='Loopback USB bridge control URL, e.g. http://127.0.0.1:18792')
    parser.add_argument('--port', type=int, default=18999, help='Loopback fixture listener forwarded to the USB bridge')
    parser.add_argument('--wifi', action='store_true', help='Use the existing pinned Wi-Fi endpoint after the owner stops production')
    parser.add_argument('--endpoint', help='Exact saved kubik://host:port required in --wifi mode')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--printer', action='store_true', help='Real printer acceptance: status and exactly one small text/QR print each')
    modes.add_argument('--printer-live-status', action='store_true', help='Physical Live microphone plus one printer status RPC; printing is blocked')
    modes.add_argument('--printer-stream-check', action='store_true', help='Exactly one NEW BLE 20 ms text print, 30x10 mm at 3 mm font')
    modes.add_argument('--printer-image-check', action='store_true', help='Exactly one synthetic image through the physical printer')
    modes.add_argument('--printer-live-image-check', action='store_true', help='One synthetic image print while GPT Live microphone remains active')
    modes.add_argument('--printer-document-check', action='store_true', help='Exactly one mixed document through the physical printer')
    modes.add_argument('--printer-quality-check', action='store_true', help='Two NEW jobs: personal Apple emoji text and square 20 mm QR')
    modes.add_argument('--screen-emoji-check', action='store_true', help='Real agent Apple emoji cards across pages with Live microphone; zero prints')
    modes.add_argument('--screen-image-check', action='store_true', help='One image attachment shown by C6; no printer commands')
    modes.add_argument('--screen-image-live-check', action='store_true', help='One image attachment while GPT Live microphone continues; no printing')
    parser.add_argument('--screen-image-source', type=Path, help='Previously verified local PNG for the screen image fixture')
    parser.add_argument('--screen-image-source-sha256', help='Verified SHA-256 of that exact local PNG')
    parser.add_argument('--screen-image-source-owner', help='Verified complete current peer identity')
    parser.add_argument('--evidence', required=True, type=Path, help='NEW private evidence directory')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    source_options = (args.screen_image_source, args.screen_image_source_sha256, args.screen_image_source_owner)
    if any(source_options) and (not args.screen_image_check or not all(source_options)):
        parser.error('screen image source requires the screen-image mode, verified SHA-256 and verified owner')
    if args.screen_image_source_sha256 and not __import__('re').fullmatch('[0-9a-f]{64}', args.screen_image_source_sha256):
        parser.error('screen image source SHA-256 must be 64 lowercase hexadecimal characters')
    if args.screen_image_source:
        args.screen_image_source = args.screen_image_source.expanduser().absolute()
    if not 1024 <= args.port <= 65535:
        parser.error('--port must be 1024..65535')
    from device_transport import validate_options
    try:
        validate_options(args)
    except ValueError as error:
        parser.error(str(error))
    for name in ('source', 'tls_identity', 'evidence'):
        setattr(args, name, getattr(args, name).expanduser().resolve())
    return args


def launch(args):
    from tls_fixture import create_identity
    if sys.version_info[:2] != (3, 14):
        raise ValueError('Run with the prepared Hermes Python 3.14 interpreter')
    if not args.tls_identity.is_file():
        raise ValueError('Explicit local pinned TLS identity is required')
    pin = json.loads((ROOT / 'tools/hermes_native/upstream.json').read_text())
    revision = subprocess.check_output(['git', '-c', 'safe.directory=' + str(args.source),
                                       '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pin['commit']:
        raise ValueError('The physical fixture requires the verified Hermes v0.22.2 source revision')
    args.evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='khd-', dir='/tmp') as directory:
        work = Path(directory)
        create_identity(work)
        env = {name: os.environ[name] for name in ('PATH', 'SYSTEMROOT', 'LD_LIBRARY_PATH') if name in os.environ}
        env.update(HOME=str(work), HERMES_HOME=str(work / 'hermes'), PYTHONPATH=str(args.source),
                   XDG_CONFIG_HOME=str(work / 'config'), XDG_CACHE_HOME=str(work / 'cache'),
                   XDG_DATA_HOME=str(work / 'data'), PYTHONDONTWRITEBYTECODE='1', PYTHONUNBUFFERED='1',
                   HERMES_DISABLE_LAZY_INSTALLS='1',
                   SSL_CERT_FILE=str(work / 'provider-cert.pem'), KUBIK_TEST_TLS_DIR=str(work),
                   NO_PROXY='127.0.0.1,localhost,::1', LANG='C.UTF-8')
        command = [sys.executable, __file__, '--worker', '--source', str(args.source),
                   '--tls-identity', str(args.tls_identity), '--control', args.control,
                   '--port', str(args.port), '--evidence', str(args.evidence)]
        if args.wifi:
            command += ['--wifi', '--endpoint', args.endpoint]
        if args.screen_image_source:
            command += ['--screen-image-source', str(args.screen_image_source),
                        '--screen-image-source-sha256', args.screen_image_source_sha256,
                        '--screen-image-source-owner', args.screen_image_source_owner]
        for mode in ('printer', 'printer_live_status', 'printer_stream_check', 'printer_quality_check',
                     'printer_image_check', 'printer_live_image_check', 'printer_document_check', 'screen_emoji_check', 'screen_image_check', 'screen_image_live_check'):
            if getattr(args, mode, False):
                command.append('--' + mode.replace('_', '-'))
        subprocess.run(command, cwd=ROOT, env=env, timeout=720 if long_mode(args) else 600, check=True)


async def prepare(args, original, identity):
    import ssl
    from aiohttp import web
    from contract import Providers, configure, release_reservation, reserve_loopback_ports
    from device_control import save
    from device_transport import binding
    from voice_fixture import register_contract_tool
    home = Path(os.environ['HERMES_HOME'])
    home.mkdir(mode=0o700, parents=True)
    reserved = reserve_loopback_ports(2)
    backend_port, voice_port = [sock.getsockname()[1] for sock in reserved]
    configure(home, backend_port, voice_port, home_identity=identity)
    subprocess.run([sys.executable, str(ROOT / 'tools/install-hermes.py'), '--home', str(home),
                    '--bind', binding(args), '--port', str(args.port)], check=True, timeout=15)
    state = home / 'kubik'
    state.mkdir(mode=0o700, exist_ok=True)
    save(state / 'tls.json', args.tls_identity.read_bytes())  # Never copied off the owning host.
    save(state / 'approved.json', [identity])
    save(state / 'voice.json', {'version': 1, 'entries': {identity: {'mode': original['voice_mode']}}})
    if args.printer_quality_check:
        copy_private_emoji(args, state)
    if args.screen_emoji_check:
        copy_private_color(args, state)
    providers = Providers()
    app = web.Application(client_max_size=5 * 1024 * 1024)
    app.router.add_route('*', '/{tail:.*}', providers.respond)
    backend = web.AppRunner(app, access_log=None)
    await backend.setup()
    tls_dir = Path(os.environ['KUBIK_TEST_TLS_DIR'])
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(tls_dir / 'provider-cert.pem', tls_dir / 'provider-key.pem')
    for sock in reserved:
        release_reservation(sock)
    await web.TCPSite(backend, '127.0.0.1', backend_port).start()
    await web.TCPSite(backend, '127.0.0.1', voice_port, ssl_context=tls).start()
    from hermes_cli.plugins import get_plugin_manager
    get_plugin_manager().discover_and_load()
    register_contract_tool(providers)
    from gateway.run import GatewayRunner
    from gateway.config import load_gateway_config
    return GatewayRunner(load_gateway_config()), backend, providers


def printer_mode(args):
    return args.printer or args.printer_live_status or args.printer_stream_check or args.printer_quality_check or getattr(args, 'printer_image_check', False) or getattr(args, 'printer_live_image_check', False) or getattr(args, 'printer_document_check', False)


def long_mode(args):
    return printer_mode(args) or args.screen_emoji_check or getattr(args, 'screen_image_check', False) or getattr(args, 'screen_image_live_check', False)


def copy_private_color(args, state):
    import hashlib
    import importlib
    from types import ModuleType
    from device_control import save
    from device_screen_emoji import SEQUENCES
    package = ModuleType('kubik_screen_fixture')
    package.__path__ = [str(ROOT / 'hermes-kubik')]
    sys.modules[package.__name__] = package
    module = importlib.import_module(package.__name__ + '.screen_emoji')
    data = module.read_color(args.tls_identity.parent / 'apple-emoji-color.bin')
    entries = module.parse_color(data)
    if not all(sequence in entries for sequence in SEQUENCES):
        raise ValueError('Private color atlas lacks a required screen-test emoji')
    save(state / 'apple-emoji-color.bin', data)
    save(args.evidence / 'apple-color-atlas.json', {'bytes': len(data), 'entries': len(entries),
                    'sha256': hashlib.sha256(data).hexdigest(), 'private_state': True})


def copy_private_emoji(args, state):
    import hashlib
    import importlib.util
    from device_control import save
    module_path = ROOT / 'hermes-kubik/printer_emoji.py'
    specification = importlib.util.spec_from_file_location('kubik_private_emoji', module_path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    source = args.tls_identity.parent / 'apple-emoji.bin'
    data = module.read_regular(source)
    entries = module.parse(data)
    from device_printer import QUALITY_EMOJI
    if not all(sequence in entries for sequence in QUALITY_EMOJI):
        raise ValueError('Private atlas lacks a required quality-test emoji')
    save(state / 'apple-emoji.bin', data)
    save(args.evidence / 'apple-atlas.json', {'bytes': len(data), 'entries': len(entries),
                                           'sha256': hashlib.sha256(data).hexdigest(), 'private_state': True})


def acceptance(args):
    if getattr(args, 'screen_image_live_check', False):
        from device_screen_image import LiveObservations, check_live
        return LiveObservations, check_live, 'one image attachment with continuous Live microphone and no printing'
    if getattr(args, 'screen_image_check', False):
        from device_screen_image import Observations, check
        return Observations, check, 'one image attachment shown without printing'
    if getattr(args, 'printer_live_image_check', False):
        from device_image import LiveObservations, check_live_image
        return LiveObservations, check_live_image, 'one synthetic image with Live microphone and preview'
    if getattr(args, 'printer_document_check', False):
        from device_image import Observations, check_document
        return Observations, check_document, 'one synthetic mixed document submitted to physical printer'
    if getattr(args, 'printer_image_check', False):
        from device_image import Observations, check
        return Observations, check, 'one synthetic image submitted to physical printer'
    if args.screen_emoji_check:
        from device_screen_emoji import Observations, check
        return Observations, check, 'native agent color emoji cards and continuous Live microphone, zero prints'
    if args.printer_quality_check:
        from device_printer import QualityObservations, check_quality
        return QualityObservations, check_quality, 'two new quality jobs with personal Apple emoji submitted'
    if args.printer_stream_check:
        from device_printer import StreamObservations, check_stream
        return StreamObservations, check_stream, 'one new stream transport print submitted'
    if args.printer_live_status:
        from device_printer import LiveObservations, check_live_status
        return LiveObservations, check_live_status, 'Live microphone and real printer status without printing'
    if args.printer:
        from device_printer import Observations, check
        return Observations, check, 'printer tool and two submitted test prints'
    from device_voice import Observations, check
    return Observations, check, 'Realtime/Live, real agent tools and playback'


async def worker(args):
    import logging
    from contract import BLOCKED, network_guard
    from device_control import Control, save
    from device_restore import cleanup
    from device_transport import connect, validate_baseline
    Observations, check, label = acceptance(args)
    from voice_fixture import eventually
    logging.basicConfig(level=logging.WARNING)
    sys.addaudithook(network_guard)
    control = Control(args.control, args.evidence)
    control.screen_image_source = (args.screen_image_source, args.screen_image_source_sha256, args.screen_image_source_owner) if args.screen_image_source else None
    gateway = backend = adapter = peer = observations = providers = None
    original = primary = None
    try:
        original, identity = await control.baseline()
        validate_baseline(args, original)
        gateway, backend, providers = await prepare(args, original, identity)
        assert await asyncio.wait_for(gateway.start(), 60), 'Fixture Gateway failed to start'
        from gateway.config import Platform
        adapter = gateway.adapters[Platform('kubik')]
        observations = Observations(adapter)
        observations.install()
        await connect(args, control)
        if long_mode(args) and not args.wifi:
            await control.sim('wifi', ms=360000)
        await eventually(lambda: identity in adapter.peers and adapter.peers[identity].ready)
        peer = adapter.peers[identity]
        observations.watch_socket(peer.socket)  # Wi-Fi may authenticate before Gateway.start returns.
        if not (getattr(args, 'printer_image_check', False) or getattr(args, 'printer_document_check', False)):
            await control.prepare_screen()
        result = await check(adapter, peer, control, providers, observations)
        result.update(device=original['device'], transport='wifi' if args.wifi else 'usb',
                      model_provider='local fixture', blocked_network_attempts=len(BLOCKED))
        if not args.printer:
            result['voice_provider'] = 'local fixture'
        save(args.evidence / 'result.json', result)
    except BaseException as error:
        primary = error
        raise
    finally:
        await cleanup(gateway, adapter, peer, control, original, observations, backend, providers, primary,
                      release_wifi_pause=not args.wifi)
    print('PASS physical Kubik through native Hermes Gateway, ' + label, flush=True)


if __name__ == '__main__':
    options = arguments()
    if options.worker:
        asyncio.run(worker(options))
    else:
        launch(options)
