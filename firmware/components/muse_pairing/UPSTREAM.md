# Muse Gadget SDK

Source: https://github.com/facebookincubator/muse-gadget-sdk

Pinned revision: `74a5e2d7fc895f109f83a9a1dbed705dbcd8b1ff`. Apache-2.0.

Pairing cryptography is retained unchanged. IDF 5.5 port: public GCM header;
ESP ECDSA driver includes use their existing manufacturer-auth guard.
This target is community-only: no manufacturer key, epoch zero. Physical
confirmation and encrypted v5 provisioning remain mandatory.
SDK token prefixes are removed from diagnostic logs.
The software handshake harness and platform fakes under tests/ are upstream
Apache-2.0. Kubik runs its community profile against the actual pairing component
with IDF 5.5 Mbed TLS; the harness does not validate BLE or hardware attestation.
