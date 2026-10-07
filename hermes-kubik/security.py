"""Device identity, TLS pinning and explicit local pairing approval (protocol v5)."""
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import ssl
import tempfile
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def atomic_text(path, text):
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def atomic_json(path, data):
    atomic_text(path, json.dumps(data))


class Security:
    def __init__(self, home):
        self.home = Path(home) / 'kubik'
        if self.home.is_symlink():
            raise ValueError('Kubik state directory must not be a symbolic link')
        self.home.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.home.chmod(0o700)
        for name in ('tls.json', 'approved.json', 'pending.json', 'key.pem', 'cert.pem'):
            path = self.home / name
            if path.is_symlink():
                raise ValueError('Kubik state files must not be symbolic links')
            if path.exists():
                if not path.is_file():
                    raise ValueError('Kubik state must contain regular files')
                path.chmod(0o600)
        self.allow_path = self.home / 'approved.json'
        self.pending_path = self.home / 'pending.json'
        self.pending = {}

    def tls(self):
        path = self.home / 'tls.json'
        if not path.exists():
            key = ec.generate_private_key(ec.SECP256R1())
            name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Kubik Hermes')])
            cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                    .public_key(key.public_key()).serial_number(x509.random_serial_number())
                    .not_valid_before(datetime.datetime(2020, 1, 1))
                    .not_valid_after(datetime.datetime(2099, 1, 1)).sign(key, hashes.SHA256()))
            atomic_json(path, {'key': key.private_bytes(serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8, serialization.NoEncryption()).decode(),
                'cert': cert.public_bytes(serialization.Encoding.PEM).decode()})
        data = json.loads(path.read_text())
        cert = x509.load_pem_x509_certificate(data['cert'].encode())
        der = cert.public_key().public_bytes(serialization.Encoding.DER,
                                             serialization.PublicFormat.SubjectPublicKeyInfo)
        self.bind = hashlib.sha256(der).hexdigest()
        for field in ('key', 'cert'):
            atomic_text(self.home / (field + '.pem'), data[field])
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(self.home / 'cert.pem', self.home / 'key.pem')
        return context

    @staticmethod
    def identity(hello):
        device, wire_key = hello.get('device'), hello.get('key')
        if hello.get('t') != 'hello' or hello.get('v') != 5:
            raise ValueError('protocol')
        if not isinstance(device, str) or not re.fullmatch('[a-z0-9][a-z0-9_-]{0,63}', device):
            raise ValueError('device')
        raw = base64.b64decode(wire_key, validate=True)
        if len(raw) != 65 or base64.b64encode(raw).decode() != wire_key:
            raise ValueError('key')
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), raw)
        return device + ':' + hashlib.sha256(raw).hexdigest()[:32], key

    def verify(self, hello, nonce, signature):
        identity, key = self.identity(hello)
        message = '\n'.join(('kubik-auth-v5', nonce, hello['device'], hello['key'], self.bind))
        key.verify(base64.b64decode(signature, validate=True), message.encode(), ec.ECDSA(hashes.SHA256()))
        return identity

    def approved(self, identity):
        if not self.allow_path.exists():
            return False
        return identity in json.loads(self.allow_path.read_text())

    def pair(self, identity):
        self.pending = {key: value for key, value in self.pending.items() if value['expires'] > time.time()}
        if identity not in self.pending:
            if len(self.pending) >= 3:
                raise ValueError('pairing busy')
            code = ''.join(secrets.choice('ABCDEFGHJKLMNPQRSTUVWXYZ23456789') for _ in range(8))
            self.pending[identity] = {'code': code, 'expires': time.time() + 600}
        atomic_json(self.pending_path, self.pending)
        return self.pending[identity]['code']
