#!/usr/bin/env python3
"""Assemble the configured clips into a Kubik sprite pack."""
import json
import math
import os
import struct
import sys

import cv2
import numpy as np
from PIL import Image

from sprite_pipeline import (
    BLACK_T, CLIPS, G, GLASS_N, OUT, PACK, PAL_PATH, REPO, VERSION, W,
    apply_h, corners_of, decode_frame, encode_frame, extract, face_rect,
    fit_inside, fix_quad, glass_of, hull_of, kmeans_palette, load_cfg,
    lum, plush, quantise, rgb565, screen_of, seamless_segs, similarity_h, soft,
    upscale565,
)

def fit_ecc(ref_soft, target_soft, init):
    """ECC homography mapping reference screen pixels onto this frame's screen."""
    crit = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 150, 1e-6)
    try:
        _, h = cv2.findTransformECC(ref_soft, target_soft, init.astype(np.float32).copy(), cv2.MOTION_HOMOGRAPHY,
                                    crit, None, 1)
        return h / h[2, 2]
    except cv2.error:
        return None


def fit_score(ref_hull, h, hull, body):
    """How well the reference screen, moved by h, explains this frame (0..1). Missing visible screen
    is an error; covering plush is only a small one when that plush sits inside the head outline
    (a paw in front of the glass), a full one outside it."""
    warped = cv2.warpPerspective(ref_hull.astype(np.uint8), h, hull.shape[::-1], flags=cv2.INTER_NEAREST) > 0
    hit = (warped & hull).sum()
    miss = (hull & ~warped).sum()
    extra = warped & ~hull
    extra_cost = 0.5 * (extra & body).sum() + (extra & ~body).sum()
    return hit / max(1.0, hit + miss + extra_cost)


def sane(h, rect0):
    """A usable face quad: convex, not folded, no side shrunk or grown out of proportion."""
    q = apply_h(h, rect0)
    if not np.all(np.isfinite(q)) or not cv2.isContourConvex(q.reshape(-1, 1, 2)):
        return False
    side = np.linalg.norm(q - np.roll(q, -1, axis=0), axis=1)
    side0 = np.linalg.norm(rect0 - np.roll(rect0, -1, axis=0), axis=1)
    r = side / side0
    return r.min() > 0.25 and r.max() < 2.5 and r[0] / r[2] < 2.2 and r[2] / r[0] < 2.2 and r[1] / r[3] < 2.2 \
        and r[3] / r[1] < 2.2


def quad_angle(h, rect0):
    q = apply_h(h, rect0)
    return math.atan2(q[1, 1] - q[0, 1], q[1, 0] - q[0, 0])


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def tracking_candidates(ref, sc, points, target_soft, dirs, prev):
    corner_fit = cv2.getPerspectiveTransform(ref["corners"], corners_of(points, sc, dirs))
    candidates = [corner_fit]
    for seed in (corner_fit, prev):
        if seed is not None:
            fitted = fit_ecc(ref["soft"], target_soft, seed)
            if fitted is not None:
                candidates.append(fitted)
    candidates.append(prev if prev is not None else similarity_h(ref["sc"], sc))
    return candidates


def select_tracking_candidate(candidates, ref, hull, body, prev, previous_quad):
    best, score, best_fit = None, -1e9, -1
    for h in candidates:
        if not sane(h, ref["rect"]):
            continue
        fit = fit_score(ref["hull"], h, hull, body)
        value = fit
        if previous_quad is not None:
            quad = apply_h(h, ref["rect"])
            turn = abs(math.degrees(wrap(quad_angle(h, ref["rect"]) - quad_angle(prev, ref["rect"]))))
            value -= 0.004 * turn + 0.0005 * float(np.linalg.norm(quad - previous_quad, axis=1).mean())
        if value > score:
            best, score, best_fit = h, value, fit
    return best, best_fit


def track_h(ref, sc, mask, prev, img, prev_fit=0.0):
    """Best of: corner fit, ECC seeded by it and by the previous frame, the previous frame itself, and
    the plain moment fit. Scored by how well they explain the screen, minus a penalty for turning or
    jumping away from the previous frame (the video is smooth; ambiguous shapes must not drift)."""
    body = plush(img)  # paws included
    dirs = None
    if prev is not None and prev_fit > 0.85:
        # Plush deep inside where the glass surely just was is a paw in front of it: the glass goes on behind.
        was = cv2.warpPerspective(ref["hull"].astype(np.uint8), prev, mask.shape[::-1], flags=cv2.INTER_NEAREST)
        was = cv2.erode(was, np.ones((25, 25), np.uint8)) > 0
        mask = mask | (was & body)
    if prev is not None:
        pc = apply_h(prev, ref["corners"])
        dirs = [tuple(v) for v in (pc - pc.mean(axis=0))]
    hull, pts = hull_of(mask)
    candidates = tracking_candidates(ref, sc, pts, soft(hull), dirs, prev)
    previous_quad = apply_h(prev, ref["rect"]) if prev is not None else None
    best, best_fit = select_tracking_candidate(candidates, ref, hull, body, prev, previous_quad)
    return (best, best_fit) if best is not None else (similarity_h(ref["sc"], sc), -1)


def load_animations(cfg, only, geom, fps_def):
    anims = []
    for a in cfg["anims"]:
        src = os.path.join(CLIPS, a["src"])
        if not os.path.exists(src) or (only and a["name"] not in only):
            continue
        fps = a.get("fps", fps_def)
        fulls, halves, times = [], [], []
        if len(a["segs"]) == 2:
            a["segs"] = seamless_segs(src, fps, geom, a)
        for t0, t1 in a["segs"]:
            fu = extract(src, fps, t0, t1, geom)
            ha = extract(src, fps, t0, t1, geom, G)
            n = min(len(fu), len(ha))
            fulls.append(fu[:n])
            halves.append(ha[:n])
            times += [t0 + i / fps for i in range(n)]
        a["join_idx"] = len(fulls[0]) if len(fulls) > 1 else 0
        full, half = np.concatenate(fulls), np.concatenate(halves)
        if "loop" in a:
            a["loop_idx"] = [int(np.argmin([abs(t - lt) for t in times])) for lt in a["loop"]]
        anims.append((a, fps, full, half))
        print(f"{a['name']:>10}: {len(full)} frames from {a['src']} {a['segs']} @ {fps} fps, loop {a.get('loop_idx')}")
    return anims


def load_palette(anims, rng):
    if os.path.exists(PAL_PATH) and "--new-palette" not in sys.argv:
        return np.array(json.load(open(PAL_PATH)), np.float32)
    samples = []
    for _, _, _, frames in anims:
        for frame in frames[:: max(1, len(frames) // 8)]:
            pixels = frame.reshape(-1, 3)
            samples.append(pixels[lum(pixels) >= BLACK_T])
    palette = kmeans_palette(np.vstack(samples), 255, rng)
    json.dump(palette.round().astype(int).tolist(), open(PAL_PATH, "w"))
    return palette


def initialize_reference(sc, mask, reference):
    if reference["ref"] is None and sc:
        hull, points = hull_of(mask)
        reference["ref"] = {"sc": sc, "hull": hull, "soft": soft(hull), "corners": corners_of(points, sc), "rect": face_rect(sc)}
    if reference["ref_area"] is None and sc:
        reference["ref_area"] = sc[5]


def track_frame_h(a, i, sc, mask, full, reference, h, fit, fixes):
    if sc:
        h, fit = track_h(reference["ref"], sc, mask, h, full, fit if i else 0.0)
        if fit < 0.8:
            print(f"{a['name']:>10}: frame {i} weak screen fit (IoU {fit:.2f})")
    quad = apply_h(h, reference["ref"]["rect"]) if h is not None else None
    fix = fixes.get(a["name"], {}).get(str(i))
    if quad is not None and fix:
        quad = fix_quad(quad, fix)
    return h, fit, quad


def frame_glass(sc, mask, full, last_gc):
    if sc is not None:
        glass = glass_of(mask, full)
        last_gc = glass[0]
    else:
        glass = (last_gc, [0.0] * GLASS_N, [0.0] * GLASS_N)
    return glass, last_gc


def save_preview(a, i, blob, pal565, sc, quad):
    im = upscale565(decode_frame(blob, pal565))
    if sc:
        cx, cy, hw, hh, ang, _ = sc
        for u, v in [(-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh), (0, 0)]:
            x = int(cx + u * math.cos(ang) - v * math.sin(ang))
            y = int(cy + u * math.sin(ang) + v * math.cos(ang))
            im[max(0, y - 2) : y + 3, max(0, x - 2) : x + 3] = (0, 255, 255)
    if quad is not None:
        cv2.polylines(im, [quad.round().astype(np.int32)], True, (255, 255, 0), 1)
    Image.fromarray(im).save(os.path.join(OUT, f"{a['name']}_{i:03d}.png"))


def process_frame(a, i, full, half, pal, pal565, reference, prev, h, fit, last_gc, fixes, preview):
    idx = quantise(half, pal)
    blob, (bx0, bx1) = encode_frame(idx)
    sc, mask = screen_of(full) or (None, None)
    initialize_reference(sc, mask, reference)
    h, fit, quad = track_frame_h(a, i, sc, mask, full, reference, h, fit, fixes)
    vis = 0 if sc is None else min(1.0, sc[5] / reference["ref_area"])
    glass, last_gc = frame_glass(sc, mask, full, last_gc)
    if sc is None:
        sc = prev
    frame = (blob, sc, vis, bx0, bx1, quad, glass)
    if preview:
        save_preview(a, i, blob, pal565, sc, quad)
    return frame, sc, h, fit, last_gc


def pack_animation(a, fps, full, half, pal, pal565, reference, fixes, preview):
    frames = []
    prev, h, fit = None, None, 0.0
    last_gc = np.array([240.0, 240.0])
    for i in range(len(full)):
        frame, prev, h, fit, last_gc = process_frame(
            a, i, full[i], half[i], pal, pal565, reference, prev, h, fit, last_gc, fixes, preview,
        )
        frames.append(frame)
    first = next((frame[1] for frame in frames if frame[1]), None)
    firstq = next((frame[5] for frame in frames if frame[5] is not None), None)
    frames = [(blob, sc or first, vis, x0, x1, quad if quad is not None else firstq, glass)
              for blob, sc, vis, x0, x1, quad, glass in frames]
    frames = smooth_quads(frames, a.get("loop_idx"))
    frames = [frame[:5] + (fit_inside(frame[5], frame[6][0], frame[6][2]),) + frame[6:] for frame in frames]
    total = sum(len(frame[0]) for frame in frames)
    print(f"{a['name']:>10}: avg {total / len(frames) / 1024:.1f} KB/frame, {total / 1024:.0f} KB")
    return a, fps, frames


def pack_animations(anims, pal, pal565, fixes, preview):
    reference = {"ref": None, "ref_area": None}
    return [pack_animation(a, fps, full, half, pal, pal565, reference, fixes, preview)
            for a, fps, full, half in anims]


def main():
    cfg = load_cfg()
    geom, fps_def = cfg["geometry"], cfg["fps"]
    preview = "--preview" in sys.argv
    only = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(1)
    anims = load_animations(cfg, only, geom, fps_def)
    pal = load_palette(anims, rng)
    pal565 = np.array([rgb565(c) for c in pal], np.uint16)
    pal565[1:][pal565[1:] == 0] = 0x0020
    links = pose_links(anims)
    packed = pack_animations(anims, pal, pal565, cfg.get("screen_fix", {}), preview)
    write_pack(pal565, packed, links)


def link_entries(a, n):
    """Frames of `a` it is fine to cut into: the rest pose (the start of idle), a held clip's
    way in and hold, the first fifth of a gesture."""
    if a["name"] == "idle":
        return list(range(min(3, n)))
    la, lb = a.get("loop_idx", [1, 0])
    return list(range(lb + 1)) if lb > la else list(range(max(3, n // 5)))


def pose_links(anims):
    """For every frame and every clip: the frame of that clip closest in pose, and how close.

    The device leaves a clip through the frame that best matches where it goes next, so the
    cut is invisible. Quality q: 100 = 1.5 ordinary frame steps apart (still seamless), 255 = far.
    """
    feats = [half.reshape(len(half), 60, 4, 60, 4, 3).mean((2, 4)).reshape(len(half), -1) for _, _, _, half in anims]
    X = np.concatenate(feats)
    D = np.concatenate([np.abs(X[i:i + 8, None] - X[None]).mean(-1) for i in range(0, len(X), 8)])
    starts = np.cumsum([0] + [len(f) for f in feats])
    # Match motion, not just a still pose: a cut must also agree with the frames either side
    # (else the paw that was going down comes straight back up). Neighbours stay in-clip.
    prev_i = np.arange(len(X)) - 1
    next_i = np.arange(len(X)) + 1
    for s0, s1 in zip(starts[:-1], starts[1:]):
        prev_i[s0] = s0
        next_i[s1 - 1] = s1 - 1
    D_pose = D
    D = D_pose + 0.5 * (D_pose[prev_i][:, prev_i] + D_pose[next_i][:, next_i])
    D /= 2
    step = np.median(np.concatenate([np.diag(D[s0:s1 - 1, s0 + 1:s1]) for s0, s1 in zip(starts[:-1], starts[1:])]))
    thr = 1.5 * step
    out = bytearray()
    best = np.zeros((len(X), len(anims)), np.int32)
    q = np.zeros((len(X), len(anims)), np.int32)
    for t, (a, _, _, _) in enumerate(anims):
        E = starts[t] + np.array(link_entries(a, len(feats[t])))
        sub = D[:, E]
        best[:, t] = sub.argmin(1) + E[0] - starts[t]
        q[:, t] = np.minimum(255, np.round(sub.min(1) / thr * 100))
    for i in range(len(X)):
        for t in range(len(anims)):
            out += struct.pack("<BB", best[i, t], q[i, t])
    names = [a["name"] for a, _, _, _ in anims]
    for s_, (a, _, _, _) in enumerate(anims):
        rows = q[starts[s_]:starts[s_ + 1]]
        print(f"{a['name']:>10} -> " + " ".join(f"{n[:5]}:{int(rows[:, t].min())}" for t, n in enumerate(names)))
    print(f"links: frame step {step:.1f}, seamless below {thr:.1f}")
    return bytes(out)


def smooth_quads(frames, loop):
    """Light [1 2 1] smoothing of the tracked corners: ECC jitters by a fraction of a pixel."""
    q = np.array([f[5] for f in frames], np.float32)
    if len(q) < 3:
        return frames
    s = q.copy()
    s[1:-1] = (q[:-2] + 2 * q[1:-1] + q[2:]) / 4
    return [f[:5] + (s[i],) + f[6:] for i, f in enumerate(frames)]


def write_pack(pal565, anims, links):
    """Sprite pack (little endian), all offsets from the start of the pack:
      header   'KSPR' u16 version u16 nanims u32 nframes_total u16 grid u16 scale  (16 B)
      palette  u16[256] RGB565
      anims    {char name[12]; u16 first; u16 count; u16 loop_a; u16 loop_b; u8 fps; u8 flags; u16 join} (24 B)
               join: first frame after a cut between segments (0 = none)
      frames   {u32 off; u32 len; i16 cx4, cy4, hw4, hh4, sin14; u8 vis; u8 x0; u8 x1; u8 pad[3];
                i16 quad4[8]; i16 gx4, gy4; u8 gr[24]; u8 pad[4]} (72 B)
               quad = TL, TR, BR, BL of the face design rectangle; glass = the visible screen as
               centre (Q4) and radii (panel px / 2) at 15 degree steps from +x, clockwise on screen,
               out to where plush (the rim or a paw) begins
      links    v5: {u8 frame; u8 q} [nframes_total][nanims]: per frame, the best frame to cut to in
               each anim (see pose_links; q <= 100 is seamless)
      data     frame blobs, 4-byte aligned, grouped per anim (contiguous, so one mapping covers an anim)
    Screen metrics are panel pixels in Q4; loop_a..loop_b (inclusive, relative) is the hold loop
    (loop_b < loop_a = no hold loop). Frame blob: u8 y0, y1; u16 rowoff[y1-y0]; rows of spans.
    """
    nf = sum(len(f) for _, _, f in anims)
    hdr = b"KSPR" + struct.pack("<HHIHH", VERSION, len(anims), nf, G, W // G)
    pal = struct.pack("<256H", *[int(v) for v in pal565])
    assert len(links) == nf * len(anims) * 2
    data_off = len(hdr) + len(pal) + 24 * len(anims) + 72 * nf + len(links)
    data_off = (data_off + 3) & ~3
    atab, ftab, data = bytearray(), bytearray(), bytearray()
    first = 0
    for a, fps, frames in anims:
        la, lb = a.get("loop_idx", [1, 0])
        flags = 1 if a.get("cycle") else 0
        atab += a["name"].encode()[:11].ljust(12, b"\0") + struct.pack("<HHHHBBH", first, len(frames), la, lb, fps,
                                                                        flags, a.get("join_idx", 0))
        first += len(frames)
        for blob, sc, vis, x0, x1, quad, glass in frames:
            while len(data) % 4:
                data += b"\0"
            cx, cy, hw, hh, ang, _ = sc if sc else (240, 200, 70, 60, 0, 0)
            ftab += struct.pack("<IIhhhhhBBBxxx", data_off + len(data), len(blob), round(cx * 16), round(cy * 16),
                                round(hw * 16), round(hh * 16), round(math.sin(ang) * 16384), round(vis * 255), x0,
                                x1)
            ftab += struct.pack("<8h", *[round(float(v) * 16) for v in quad.reshape(-1)])
            gc, gr, _ = glass
            ftab += struct.pack("<hh", round(float(gc[0]) * 16), round(float(gc[1]) * 16))
            ftab += bytes(min(255, int(r / 2)) for r in gr) + b"\0" * 4
            data += blob
    body = hdr + pal + atab + ftab + links
    body += b"\0" * (data_off - len(body))
    os.makedirs(os.path.dirname(PACK), exist_ok=True)
    with open(PACK, "wb") as f:
        f.write(body + data)
    print(f"wrote {os.path.relpath(PACK, REPO)}: {os.path.getsize(PACK) / 1024 / 1024:.2f} MB, {nf} frames")


if __name__ == "__main__":
    main()
