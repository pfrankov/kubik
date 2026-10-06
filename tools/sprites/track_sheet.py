#!/usr/bin/env python3
"""Tiles raw 480x480 rgb24 frames (stdin, from `sim track <anim>`) into a numbered review sheet.

Usage: sim track idle | uv run --with pillow tools/sprites/track_sheet.py out.png
"""
import sys

from PIL import Image, ImageDraw

W = 480
CROP = (90, 40, 390, 340)  # the head and shoulders
TILE, COLS = 220, 8

raw = sys.stdin.buffer.read()
frames = [Image.frombytes("RGB", (W, W), raw[i:i + W * W * 3]) for i in range(0, len(raw) - W * W * 3 + 1, W * W * 3)]
rows = (len(frames) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * TILE, rows * TILE), (40, 40, 40))
for i, fr in enumerate(frames):
    t = fr.crop(CROP).resize((TILE - 4, TILE - 4), Image.LANCZOS)
    ImageDraw.Draw(t).text((5, 3), str(i), fill=(255, 255, 255))
    sheet.paste(t, ((i % COLS) * TILE + 2, (i // COLS) * TILE + 2))
sheet.save(sys.argv[1])
