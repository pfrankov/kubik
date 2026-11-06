"""Explicit physical-fixture transport; Wi-Fi reuses the device's pinned endpoint."""
from urllib.parse import urlsplit


def endpoint(value):
    parsed = urlsplit(value)
    if parsed.scheme != 'kubik' or not parsed.hostname or parsed.port is None:
        raise ValueError('Wi-Fi endpoint must be kubik://host:port')
    if parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ('', '/'):
        raise ValueError('Wi-Fi endpoint must contain only its scheme, host and explicit port')
    return parsed.hostname.lower(), parsed.port


def validate_options(args):
    if args.wifi:
        if not args.endpoint or endpoint(args.endpoint)[1] != args.port:
            raise ValueError('Wi-Fi requires --endpoint and a matching --port')
    elif args.endpoint:
        raise ValueError('--endpoint requires the explicit --wifi mode')


def validate_baseline(args, original):
    validate_options(args)
    if not args.wifi:
        return
    if original['server_pinned'] is not True:
        raise ValueError('Wi-Fi fixture requires the existing pinned TLS identity')
    if endpoint(original['url']) != endpoint(args.endpoint):
        raise ValueError('Wi-Fi fixture endpoint does not match the saved device URL')
    if original['online'] or original['via'] != 'none':
        raise ValueError('Stop the idle production listener and observe its disconnect before Wi-Fi fixture startup')


def binding(args):
    return '0.0.0.0' if args.wifi else '127.0.0.1'


async def connect(args, control):
    if not args.wifi:
        await control.sim('wifi', ms=120000)
    route = 'wifi' if args.wifi else 'usb'
    await control.until(lambda state: state['online'] and state['via'] == route,
                        'pinned fixture ' + route + ' link', 40)
