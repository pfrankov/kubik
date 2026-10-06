#!/usr/bin/env python3
"""Kubik sprite pipeline: animation clips (mp4) -> firmware sprite pack.

Every clip is an 8 s image-to-video render of the plush character on pure
black that starts and ends on the same hero pose, so any clip can follow any
other. clips.json picks the useful sub-range of each clip (and an optional
hold loop inside it). Frames are resampled to the playback rate, scaled into
the panel, stored at HALF resolution (240x240 grid, the firmware doubles it
with midpoint interpolation; at 314 ppi the plush fuzz is invisible anyway)
as 8-bit indices into one shared 256-colour palette, row by row as spans.
Black (index 0) is never stored: the panel background is black, so the rim
glow fades into it and the blank face screen costs nothing.

For every frame we also locate the face screen (the dark hole enclosed by
the plush head): centre, half sizes, tilt and how visible it is. The firmware
draws the glowing eyes and mouth there, so the face follows the body. On top of
that the screen is tracked with perspective: a homography from the neutral
screen (idle frame 0) to every frame, fitted on the screen masks (OpenCV ECC,
seeded by the previous frame). The pack stores where the four corners of the
face's design rectangle land, so the face tilts and foreshortens with the head.
Hand-tuned corrections: clips.json "screen_fix": {anim: {frame: [dx, dy, scale,
rot_deg]}} moves that frame's quad (panel px, about its centre).

Image processing routines used by tools/sprites/build.py.

Usage:
  uv run --with numpy --with pillow --with scipy --with opencv-python-headless \
      tools/sprites/build.py [--preview] [--new-palette] [anim...]
Review: sh firmware/sim/track.sh [anim...] -> firmware/sim/out/track/<anim>.png
"""
import json
import math
import os
import struct
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(ROOT, "..", ".."))
CLIPS = os.path.join(REPO, "art", "clips")
OUT = os.path.join(ROOT, "out")
PACK = os.path.join(REPO, "firmware", "assets", "sprites.bin")
PAL_PATH = os.path.join(ROOT, "palette.json")

W = H = 480  # panel
G = 240  # storage grid
BLACK_T = 12  # luminance (0..255) below which a pixel is background
VERSION = 5
FACE_HW, FACE_HH = 185.0, 135.0  # the face design rectangle (face.c draws into it)


def load_cfg():
    with open(os.path.join(ROOT, "clips.json")) as f:
        return json.load(f)


def extract(src, fps, t0, t1, geom, size=W):
    """Decode [t0, t1) of `src` at `fps`, scaled and cropped to the panel (size x size)."""
    k = size / W
    scale, ox, oy = geom["scale"] * k, round(geom["ox"] * k), round(geom["oy"] * k)
    sw, sh = round(720 * scale), round(1280 * scale)
    P = size  # pad so the crop window may extend past the video frame
    vf = (f"fps={fps},scale={sw}:{sh}:flags=lanczos,pad={sw + 2 * P}:{sh + 2 * P}:{P}:{P}:black,"
          f"crop={size}:{size}:{ox + P}:{oy + P}")
    cmd = ["ffmpeg", "-v", "error", "-ss", str(t0), "-t", str(t1 - t0), "-i", src, "-vf", vf,
           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    n = len(raw) // (size * size * 3)
    return np.frombuffer(raw[: n * size * size * 3], np.uint8).reshape(n, size, size, 3).astype(np.float32)


def seamless_segs(src, fps, geom, a):
    """Two segments with the cut moved (up to 0.6 s either way) to where the poses match.

    Among cuts that look no worse than an ordinary frame step, takes the one that keeps the
    fewest frames; if there is none, plays the video through without a cut.
    """
    (s0, e0), (s1, e1) = a["segs"]
    lo = max(a["loop"][1] + 0.15 if "loop" in a else s0 + 0.5, e0 - 0.6)
    A = extract(src, fps, s0, e0 + 0.6, geom, 64)  # the same frame grid seg 1 will use
    ta = s0 + np.arange(len(A)) / fps
    B = extract(src, 2 * fps, s1 - 0.6, min(s1 + 0.6, e1 - 0.5), geom, 64)
    tb = s1 - 0.6 + np.arange(len(B)) / (2 * fps)
    step = float(np.median(np.abs(np.diff(A, axis=0)).mean(axis=(1, 2, 3))))
    best = None
    for i in np.nonzero(ta >= lo)[0]:
        d = np.abs(B - A[i]).mean(axis=(1, 2, 3))
        for j in np.nonzero((d <= step * 1.2) & (tb > ta[i] + 0.2))[0]:
            frames = (ta[i] - s0) + (e1 - tb[j])
            if best is None or frames < best[0]:
                best = (frames, i, j, float(d[j]))
    if best is None:
        print(f"{a['name']:>10}: no clean cut near {e0}->{s1} (frame step {step:.1f}): playing through")
        return [[s0, e1]]
    _, i, j, d = best
    segs = [[s0, round(float(ta[i]) + 0.5 / fps, 3)], [round(float(tb[j]), 3), e1]]
    print(f"{a['name']:>10}: cut {ta[i]:.2f}->{tb[j]:.2f} (diff {d:.1f}, frame step {step:.1f})")
    return segs


def lum(img):
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


def plush(img):
    """Warm cream plush. Glare on the glass is as bright but neutral grey (chroma < 9)."""
    return (lum(img) > 40) & (img.max(-1) - img.min(-1) >= 9)


def screen_of(img):
    """Face screen = largest dark region enclosed by the plush (full-res frame)."""
    body = plush(img)
    body = ndimage.binary_closing(body, iterations=2)
    holes = ndimage.binary_fill_holes(body) & ~body
    lab, n = ndimage.label(holes)
    if n == 0:
        return None
    sizes = ndimage.sum(holes, lab, range(1, n + 1))
    k = int(np.argmax(sizes)) + 1
    if sizes[k - 1] < 800:
        return None
    m = ndimage.binary_fill_holes(lab == k)
    ys, xs = np.nonzero(m)
    cx, cy = xs.mean(), ys.mean()
    dx, dy = xs - cx, ys - cy
    cxx, cyy, cxy = (dx * dx).mean(), (dy * dy).mean(), (dx * dy).mean()
    ang = 0.5 * math.atan2(2 * cxy, cxx - cyy)
    if ang > math.pi / 4:
        ang -= math.pi / 2
    elif ang < -math.pi / 4:
        ang += math.pi / 2
    c, s = math.cos(ang), math.sin(ang)
    u = dx * c + dy * s
    v = -dx * s + dy * c
    hw = math.sqrt((u * u).mean() * 3.0) * 0.97
    hh = math.sqrt((v * v).mean() * 3.0) * 0.97
    return [cx, cy, hw, hh, ang, float(m.sum())], m


def soft(mask):
    return cv2.GaussianBlur(mask.astype(np.float32), (0, 0), 4)


def similarity_h(ref, sc):
    """Homography guess from the moment fits alone (reference screen -> this screen)."""
    s = math.sqrt(sc[5] / ref[5])
    a = sc[4] - ref[4]
    c, n = s * math.cos(a), s * math.sin(a)
    return np.array([[c, -n, sc[0] - (c * ref[0] - n * ref[1])], [n, c, sc[1] - (n * ref[0] + c * ref[1])], [0, 0, 1]],
                    np.float32)


def hull_of(mask):
    """Convex hull of the screen: paws and fingers bite concave chunks out of it."""
    cs, _ = cv2.findContours(mask.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    pts = cv2.convexHull(max(cs, key=cv2.contourArea))
    out = np.zeros(mask.shape, np.uint8)
    cv2.fillConvexPoly(out, pts, 1)
    return out.astype(bool), pts.reshape(-1, 2).astype(np.float32)


GLASS_N = 24  # glass outline: radii from its centre along evenly spaced directions


def texture(img):
    """Local fine-detail energy: fur is grainy (paws 11..19), glare on the glass smooth (2..6)."""
    l = lum(img).astype(np.float32)
    hp = l - cv2.GaussianBlur(l, (0, 0), 1.5)
    return np.sqrt(cv2.GaussianBlur(hp * hp, (0, 0), 3))


def glass_of(mask, img):
    """Where the glass shows in this frame, as a centre and GLASS_N radii (panel px): along each
    direction, how far the screen reaches before plush (its rim, or a paw in front) begins.
    The face is only drawn inside it, so a loose track never smears it onto the plush."""
    # Warm glare reads as plush by colour, often right up to the rim: smooth patches touching
    # the screen are glass too (the fur, rim or paw, is grainy).
    smooth = ~mask & (texture(img) < 7)
    n, lab = cv2.connectedComponents(smooth.astype(np.uint8))
    touch = np.unique(lab[(cv2.dilate(mask.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0) & smooth])
    hull, _ = hull_of(mask)
    near = cv2.dilate(hull.astype(np.uint8), np.ones((31, 31), np.uint8)) > 0
    for k in touch[touch > 0]:
        g = lab == k
        if (g & ~near).sum() < 0.1 * g.sum():  # not the dark background around the head
            mask = mask | g
    m = cv2.morphologyEx(mask.astype(np.uint8), cv2.MORPH_OPEN, np.ones((5, 5), np.uint8)) > 0
    lab, n = ndimage.label(m)
    if n > 1:
        m = lab == 1 + int(np.argmax(ndimage.sum(m, lab, range(1, n + 1))))
    ys, xs = np.nonzero(m)
    c = np.array([xs.mean(), ys.mean()])
    return c, rays(m, c), rays(hull_of(m)[0], c)


def rays(m, c):
    """Distance from c to the edge of mask m along GLASS_N directions (panel px, just inside)."""
    h, w = m.shape
    radii = []
    for k in range(GLASS_N):
        u = (math.cos(2 * math.pi * k / GLASS_N), math.sin(2 * math.pi * k / GLASS_N))
        r = 0.0
        while True:  # walk out to the first pixel off the glass
            x, y = int(c[0] + (r + 0.5) * u[0]), int(c[1] + (r + 0.5) * u[1])
            if not (0 <= x < w and 0 <= y < h and m[y, x]):
                break
            r += 0.5
        radii.append(r)
    radii = np.array(radii)
    # A stray hair across one ray must not notch the outline: a ray may not undercut both neighbours by much.
    lo = np.minimum(np.roll(radii, 1), np.roll(radii, -1))
    radii = np.where(radii < lo - 6, lo, radii)
    return [max(0.0, r - 1.0) for r in radii]  # stay just inside the rim's antialiasing


def star_inside(c, radii, pts, margin):
    """Which points lie inside the outline (margin px in from its edge)."""
    d = pts - c
    a = (np.arctan2(d[..., 1], d[..., 0]) % (2 * math.pi)) / (2 * math.pi) * GLASS_N
    k = np.floor(a).astype(int) % GLASS_N
    f = a - np.floor(a)
    r = np.asarray(radii)
    lim = r[k] * (1 - f) + r[(k + 1) % GLASS_N] * f
    return np.hypot(d[..., 0], d[..., 1]) < lim - margin


# The face's features (eyes, mouth) within its design rectangle, in rectangle units (-1..1).
FEATURES = np.array([(u, v) for u in np.linspace(-0.8, 0.8, 9) for v in (-0.8, 0.55)] +
                    [(u, v) for u in (-0.8, 0.8) for v in np.linspace(-0.8, 0.55, 7)], np.float32)


def fit_inside(quad, c, hull_r):
    """A loose track can put an eye past the glass edge (the screen is turned and foreshortened).
    Find the smallest shift and shrink (about the glass centre) that brings the features inside
    the glass hull. The hull, not the visible glass, so that a paw in front covers the face
    rather than squeezes it."""
    if max(hull_r) < 8:
        return quad
    unit = np.float32([(-1, -1), (1, -1), (1, 1), (-1, 1)])
    H = cv2.getPerspectiveTransform(unit, quad.astype(np.float32))
    p = cv2.perspectiveTransform(FEATURES[None], H)[0].astype(np.float64)
    if star_inside(c, hull_r, p, 3).all():
        return quad
    g = np.arange(-40, 41, 2.0)
    t = np.stack(np.meshgrid(g, g), -1).reshape(-1, 2)
    best, best_cost = None, 1e9
    for sc in np.arange(1.0, 0.59, -0.02):
        if 150 * (1 - sc) >= best_cost:
            break
        ps = c + sc * (p - c)
        ok = star_inside(c, hull_r, ps[None] + t[:, None], 3).all(1)
        if ok.any():
            cost = 150 * (1 - sc) + np.hypot(t[ok, 0], t[ok, 1])
            j = int(np.argmin(cost))
            if cost[j] < best_cost:
                best_cost, best = cost[j], (sc, t[ok][j])
    if best is None:
        return quad
    sc, tt = best
    return c + sc * (quad - c) + tt


def corners_of(pts, sc, dirs=None):
    """Four screen corners: the hull points furthest along the diagonals. The diagonals come from
    the previous frame's corners when known (a near-square screen says nothing about its tilt),
    else from the moment fit."""
    cx, cy, hw, hh, ang, _ = sc
    c, s = math.cos(ang), math.sin(ang)
    if dirs is None:
        dirs = [((u * c - v * s), (u * s + v * c)) for u, v in [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh)]]
    ctr = pts.mean(axis=0)
    d = pts - ctr
    return np.array([pts[int(np.argmax(d[:, 0] * dx + d[:, 1] * dy))] for dx, dy in dirs], np.float32)


def face_rect(ref):
    """Corners (TL, TR, BR, BL) of the face design rectangle on the reference screen."""
    cx, cy, hw, hh, ang, _ = ref
    k = min(hw / FACE_HW, hh / FACE_HH)
    c, s = math.cos(ang), math.sin(ang)
    return np.array([[cx + u * c - v * s, cy + u * s + v * c]
                     for u, v in [(-FACE_HW * k, -FACE_HH * k), (FACE_HW * k, -FACE_HH * k),
                                  (FACE_HW * k, FACE_HH * k), (-FACE_HW * k, FACE_HH * k)]], np.float32)


def apply_h(h, pts):
    return cv2.perspectiveTransform(pts.reshape(-1, 1, 2), h).reshape(-1, 2)


def fix_quad(q, fix):
    dx, dy, sc, rot = (list(fix) + [0, 0, 1, 0][len(fix):])[:4]
    c = q.mean(axis=0)
    a = math.radians(rot)
    r = np.array([[math.cos(a), -math.sin(a)], [math.sin(a), math.cos(a)]], np.float32) * sc
    return (q - c) @ r.T + c + np.array([dx, dy], np.float32)


def kmeans_palette(samples, k, rng, iters=20):
    pts = samples[rng.choice(len(samples), min(len(samples), 60000), replace=False)]
    order = np.argsort(lum(pts))
    cent = pts[order[np.linspace(0, len(pts) - 1, k).astype(int)]].copy()
    for _ in range(iters):
        a = nearest(pts, cent)
        for j in range(k):
            sel = pts[a == j]
            if len(sel):
                cent[j] = sel.mean(0)
    cent = cent[np.argsort(lum(cent))]
    return np.vstack([np.zeros((1, 3), np.float32), cent])


def nearest(pts, pal, chunk=8192):
    out = np.empty(len(pts), np.int32)
    p2 = (pal * pal).sum(1)
    for i in range(0, len(pts), chunk):
        q = pts[i : i + chunk]
        d = p2[None, :] - 2 * q @ pal.T
        out[i : i + chunk] = d.argmin(1)
    return out


def rgb565(c):
    r, g, b = [int(v) for v in np.clip(np.round(c), 0, 255)]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def quantise(img, pal):
    """img: GxGx3 float -> uint8 index image (0 = background)."""
    px = img.reshape(-1, 3)
    idx = nearest(px, pal[1:]).astype(np.uint8) + 1
    idx[lum(px) < BLACK_T] = 0
    return idx.reshape(img.shape[:2])


def encode_frame(idx, gap=4):
    """Rows of spans: u8 nspans, {u8 x, u8 n, n bytes}. Zero gaps shorter than `gap` stay inside a span."""
    rows_nz = np.nonzero(idx.any(1))[0]
    if len(rows_nz) == 0:
        return struct.pack("<BB", 0, 0), (0, 0)
    y0, y1 = int(rows_nz[0]), int(rows_nz[-1]) + 1
    cols = np.nonzero(idx.any(0))[0]
    rows = []
    for y in range(y0, y1):
        r = idx[y]
        nz = np.nonzero(r)[0]
        spans = []
        if len(nz):
            s = e = nz[0]
            for x in nz[1:]:
                if x - e > gap:
                    spans.append((s, e + 1))
                    s = x
                e = x
            spans.append((s, e + 1))
        b = bytearray([len(spans)])
        for s, e in spans:
            b += struct.pack("<BB", int(s), int(e - s)) + bytes(r[s:e])
        rows.append(bytes(b))
    offs, data = [], bytearray()
    for r in rows:
        offs.append(len(data))
        data += r
    assert len(data) < 65536
    head = struct.pack("<BB", y0, y1) + b"".join(struct.pack("<H", o) for o in offs)
    return head + bytes(data), (int(cols[0]), int(cols[-1]))


def decode_frame(blob, pal565):
    """Reference decoder -> GxG uint16 RGB565 (0 = transparent)."""
    out = np.zeros((G, G), np.uint16)
    y0, y1 = blob[0], blob[1]
    base = 2 + 2 * (y1 - y0)
    for y in range(y0, y1):
        (o,) = struct.unpack_from("<H", blob, 2 + 2 * (y - y0))
        p = base + o
        ns = blob[p]
        p += 1
        for _ in range(ns):
            x, n = blob[p], blob[p + 1]
            p += 2
            out[y, x : x + n] = pal565[np.frombuffer(blob[p : p + n], np.uint8)]
            p += n
    return out


def upscale565(g):
    """Firmware-equivalent 2x midpoint upscale of an RGB565 grid -> 8-bit RGB image."""
    def avg(a, b):
        return (((a & 0xF7DE).astype(np.uint32) + (b & 0xF7DE)) >> 1).astype(np.uint16)
    h = np.zeros((G, W), np.uint16)
    h[:, 0::2] = g
    h[:, 1:-1:2] = avg(g[:, :-1], g[:, 1:])
    h[:, -1] = avg(g[:, -1], np.zeros(G, np.uint16))
    o = np.zeros((H, W), np.uint16)
    o[0::2] = h
    o[1:-1:2] = avg(h[:-1], h[1:])
    o[-1] = avg(h[-1], np.zeros(W, np.uint16))
    r = ((o >> 11) & 31) * 255 // 31
    gg = ((o >> 5) & 63) * 255 // 63
    b = (o & 31) * 255 // 31
    return np.stack([r, gg, b], -1).astype(np.uint8)


