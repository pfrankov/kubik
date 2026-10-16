"""Run as the Hermes service owner: python pair.py CODE (never approves by device ID alone)."""
import argparse
from pathlib import Path
from security import Security


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('code')
    parser.add_argument('--home', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    store = Security(args.home)
    try:
        store.approve(args.code)
    except ValueError as error:
        parser.error(str(error))
    print('Kubik approved. The connected device will complete pairing automatically.')


if __name__ == '__main__':
    main()
