"""Private, short-lived TLS identity trusted only by the native contract subprocess."""
from datetime import datetime, timedelta, timezone
import ipaddress
from pathlib import Path
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID


def create_identity(directory):
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Kubik local contract')])
    now = datetime.now(timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(minutes=1)).not_valid_after(now + timedelta(hours=1))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1')),
                                                        x509.DNSName('localhost')]), critical=False)
            .sign(key, hashes.SHA256()))
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    (directory / 'provider-cert.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    private = directory / 'provider-key.pem'
    private.touch(mode=0o600)
    private.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                        serialization.NoEncryption()))


if __name__ == '__main__':
    create_identity(Path(sys.argv[1]))
