#!/usr/bin/env python3
"""Measure rendered Tess sounds: python3 tools/tess-sound-report.py DIR [--plush] [--md FILE].

Reads every 24 kHz mono 16-bit WAV in DIR (see firmware/sim/tess_sound_render.c) and prints a markdown table:
duration (to 35 dB and to 20 dB under the peak), RMS and K-weighted loudness over the sounding part (to 35 dB under the peak), loudest 100 ms, peak, spectral centroid, share of the energy under
200 Hz (the tiny speaker plays nothing there), the strongest partial and the melodic contour. Needs numpy.
--plush measures raw s16le .pcm files of the Plush kit in DIR instead (the reference level).
"""
import argparse
import wave
from pathlib import Path

import numpy as np

RATE = 24000
NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(hz):
    midi = 69 + 12 * np.log2(hz / 440.0)
    number = int(round(midi))
    return f"{NOTES[number % 12]}{number // 12 - 1}", 100 * (midi - number)


def load(path):
    if path.suffix == ".pcm":
        return np.frombuffer(path.read_bytes(), dtype="<i2").astype(np.float64)
    with wave.open(str(path)) as wav:
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)


def biquad(x, b, a):
    y = np.zeros_like(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        out = b[0] * v + z1
        z1 = b[1] * v - a[1] * out + z2
        z2 = b[2] * v - a[2] * out
        y[i] = out
    return y


def k_weight(x):
    """BS.1770 K-weighting: the high-shelf then the 38 Hz high-pass, designed for RATE."""
    g, q, f = 3.999843853973347, 0.7071752369554196, 1681.974450955533
    k = np.tan(np.pi * f / RATE)
    vh = 10 ** (g / 20)
    vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    shelf_b = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    shelf_a = [1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    q2, f2 = 0.5003270373238773, 38.13547087602444
    k2 = np.tan(np.pi * f2 / RATE)
    a2 = 1 + k2 / q2 + k2 * k2
    return biquad(biquad(x, shelf_b, shelf_a), [1, -2, 1], [1, 2 * (k2 * k2 - 1) / a2, (1 - k2 / q2 + k2 * k2) / a2])


def active_span(x, floor_db=-35):
    loud = np.flatnonzero(np.abs(x) > np.abs(x).max() * 10 ** (floor_db / 20))
    return (loud[0], loud[-1] + 1) if len(loud) else (0, len(x))


def audible_ms(x, hot_db, drop_db=20):
    """How long the sound stays within drop_db of its loudest 100 ms (20 ms RMS steps): what a listener calls its length."""
    step = RATE // 50
    levels = [20 * np.log10(np.sqrt(np.mean(x[i:i + step] ** 2)) / 32768 + 1e-9) for i in range(0, len(x) - step + 1, step)]
    loud = [i for i, level in enumerate(levels) if level > hot_db - drop_db]
    return (loud[-1] + 1) * 20 if loud else 0


def band_shares(power, freqs):
    edges = [0, 200, 500, 1000, 2000, 4000, RATE / 2 + 1]
    total = power.sum() or 1
    return [100 * power[(freqs >= lo) & (freqs < hi)].sum() / total for lo, hi in zip(edges, edges[1:])]


def contour(x):
    """Strongest partial per 85 ms frame (hop 30 ms), consecutive repeats merged: the melody as heard."""
    size, hop = 2048, 720
    window = np.hanning(size)
    x = np.concatenate([x, np.zeros(max(0, size - len(x)))])
    frames = [x[i:i + size] * window for i in range(0, max(1, len(x) - size + 1), hop)]
    levels = [np.sqrt(np.mean(f * f)) for f in frames]
    loudest, freqs, names = max(levels) or 1, np.fft.rfftfreq(size, 1 / RATE), []
    for frame, level in zip(frames, levels):
        if level < loudest * 0.1:
            continue
        magnitude = np.abs(np.fft.rfft(frame))
        magnitude[freqs < 100] = 0
        name = note_name(freqs[int(np.argmax(magnitude))])[0]
        if not names or names[-1] != name:
            names.append(name)
    return ">".join(names[:8])


def measure(x, plush):
    start, end = active_span(x)
    body = x[start:end]
    padded = np.concatenate([body, np.zeros(4096)])
    spectrum = np.abs(np.fft.rfft(padded * np.hanning(len(padded)))) ** 2
    freqs = np.fft.rfftfreq(len(padded), 1 / RATE)
    peak_hz = freqs[int(np.argmax(np.where(freqs > 100, spectrum, 0)))]
    rms = np.sqrt(np.mean(body ** 2))
    hot = np.sqrt(np.convolve(x ** 2, np.ones(RATE // 10) / (RATE // 10), "valid").max()) if len(x) >= RATE // 10 else rms
    lufs = -0.691 + 10 * np.log10(np.mean(k_weight(body) ** 2) + 1e-9) - 90.309  # dBFS(s16) of K-weighted power
    return {
        "ms": round(len(x) * 1000 / RATE), "sound_ms": round((end - start) * 1000 / RATE),
        "rms": 20 * np.log10(rms / 32768 + 1e-9), "hot": 20 * np.log10(hot / 32768 + 1e-9), "lufs": lufs, "peak": 20 * np.log10(np.abs(x).max() / 32768 + 1e-9),
        "centroid": float((freqs * spectrum).sum() / (spectrum.sum() or 1)),
        "bands": band_shares(spectrum, freqs), "peak_hz": peak_hz,
        "audible": audible_ms(x, 20 * np.log10(hot / 32768 + 1e-9)), "click": float(np.abs(np.diff(x)).max()), "first": float(abs(x[0])), "last": float(abs(x[-1])),
        "contour": contour(body) if not plush else "",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    parser.add_argument("--plush", action="store_true")
    parser.add_argument("--md")
    args = parser.parse_args()
    files = sorted(Path(args.directory).glob("*.pcm" if args.plush else "*.wav"))
    rows = ["| sound | ms | sounding ms | audible ms | RMS dBFS | loudest 100 ms | LUFS-ish | peak dBFS | centroid Hz | <200 Hz % | strongest | contour |",
            "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for path in files:
        m = measure(load(path), args.plush)
        name, cents = note_name(m["peak_hz"])
        rows.append(f"| {path.stem} | {m['ms']} | {m['sound_ms']} | {m['audible']} | {m['rms']:.1f} | {m['hot']:.1f} | {m['lufs']:.1f} | {m['peak']:.1f} | "
                    f"{m['centroid']:.0f} | {m['bands'][0]:.0f} | {m['peak_hz']:.0f} Hz {name} | {m['contour']} |")
    text = "\n".join(rows)
    print(text)
    if args.md:
        Path(args.md).write_text(text + "\n")


if __name__ == "__main__":
    main()
