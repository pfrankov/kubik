#!/usr/bin/env python3
"""Compress an embedded web asset reproducibly; the browser does the decoding."""
import argparse
import gzip
import io
from pathlib import Path


def compress(data):
    output = io.BytesIO()
    with gzip.GzipFile(filename='', mode='wb', fileobj=output, compresslevel=9, mtime=0) as stream:
        stream.write(data)
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_bytes(compress(args.source.read_bytes()))


if __name__ == '__main__':
    main()
