#!/usr/bin/env python3
"""Pack firmware/assets into one image for the `assets` flash partition.

Layout (little endian):
  'KAST' u32 count, then count x {char name[24]; u32 off; u32 len}
  followed by the files, each 4-byte aligned. Offsets are from the image start.

Contents: sprites.bin (character animation) and sfx/<name>.pcm (sound kit,
24 kHz mono s16le). Kubik has no recorded speech: words go on screen.
Tess maps its local wake model; Plush contains only its animation and sound kit.
Usage: python3 tools/assets/pack.py out --character TESS|PLUSH
"""
import argparse
import hashlib
import json
import os
import struct
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
from firmware_layout import partitions
SRC = os.path.join(REPO, "firmware", "assets")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("out")
parser.add_argument("--character", choices=("TESS", "PLUSH"), required=True)
args = parser.parse_args()
OUT = args.out
# Size of the "assets" partition, read from the partition table.
LIMIT = partitions(args.character)['assets']['size']

files = [("sprites", os.path.join(SRC, "sprites.bin"))] if args.character == "PLUSH" else [("wake/tessa", os.path.join(SRC, "wake", "tessa.tflite"))]
for sub in (("sfx",) if args.character == "PLUSH" else ()):
    for fn in sorted(os.listdir(os.path.join(SRC, sub))):
        if fn.endswith(".pcm"):
            files.append((f"{sub}/{fn[:-4]}", os.path.join(SRC, sub, fn)))

head = 8 + 32 * len(files)
off = (head + 3) & ~3
table, blobs = bytearray(), bytearray()
for name, path in files:
    assert len(name) < 24, name
    data = open(path, "rb").read()
    if name == "wake/tessa":
        model_source = json.loads(open(os.path.join(SRC, "wake", "source.json")).read())
        if hashlib.sha256(data).hexdigest() != model_source["sha256"]:
            sys.exit("Tessa model differs from its pinned source")
    table += name.encode().ljust(24, b"\0") + struct.pack("<II", off + len(blobs), len(data))
    blobs += data
    while len(blobs) % 4:
        blobs += b"\0"
img = b"KAST" + struct.pack("<I", len(files)) + table
img += b"\0" * (off - len(img)) + blobs
if len(img) > LIMIT:
    sys.exit(f"assets image {len(img)} B exceeds the partition ({LIMIT} B)")
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(img)
print(f"{os.path.relpath(OUT, REPO)}: {len(files)} files, {len(img) / 1048576:.2f} MB of {LIMIT / 1048576:.2f} MB")
