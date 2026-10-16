"""Run as the Hermes service owner: python pair.py CODE (never approves by device ID alone)."""
import argparse
import os
from pathlib import Path
import time
from security import Security, atomic_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('code')
    parser.add_argument('--home', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    store = Security(args.home)
    import json
    pending = json.loads(store.pending_path.read_text())
    matches = [identity for identity, item in pending.items()
               if item['code'] == args.code and item['expires'] > time.time()]
    if len(matches) != 1:
        parser.error('Unknown or expired pairing code')
    approved = json.loads(store.allow_path.read_text()) if store.allow_path.exists() else []
    if not isinstance(approved, list) or any(not isinstance(item, str) for item in approved):
        parser.error('Invalid approval store; no approvals changed')
    atomic_json(store.allow_path, sorted(set(approved + matches)))
    print('Kubik approved. The connected device will complete pairing automatically.')


if __name__ == '__main__':
    main()
