#!/usr/bin/env python3
"""Render a local, unpaid palette of additive collision-sound proposals.

These are timbre auditions at the device's 24 kHz output rate, not firmware
recordings. All proposals share contacts and pitch identities. The selected
proposal must still be implemented and checked on the physical speaker.
"""
import array
import json
import math
from pathlib import Path
import random
import shutil
import wave

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'dist/sound-lab'
RATE = 24000
# name, family, modes (ratio, amplitude, decay multiplier), attack, decay, pitch range
VARIANTS = [
    ('Tiny glass', 'Glass', [(1,1,1),(1.49,.32,.55),(2.17,.12,.3)], 4, 45, 1450, 2600),
    ('Glass beads', 'Glass', [(1,1,1),(1.37,.48,.7),(1.93,.2,.42)], 5, 75, 1200, 2400),
    ('Crystal dust', 'Glass', [(1,.7,1),(1.71,.6,.65),(2.43,.2,.4)], 3, 55, 1400, 2200),
    ('Soft crystal', 'Glass', [(1,1,1),(1.26,.26,.8),(1.89,.15,.55)], 7, 90, 1600, 2700),
    ('Glass rain', 'Glass', [(1,1,1),(2.76,.22,.45),(4.12,.08,.25)], 4, 65, 800, 1300),
    ('Celesta light', 'Celesta', [(1,1,1),(2,.28,.6),(3,.08,.3)], 4, 90, 1100, 1750),
    ('Celesta high', 'Celesta', [(1,1,1),(2,.18,.55)], 5, 100, 1900, 2700),
    ('Soft music box', 'Celesta', [(1,1,1),(2.01,.25,.7),(3.98,.06,.3)], 6, 110, 950, 1350),
    ('Pearl keys', 'Celesta', [(1,1,1),(2,.12,.65),(2.98,.1,.45)], 8, 70, 1450, 1800),
    ('Porcelain', 'Celesta', [(1,1,1),(1.96,.3,.45),(3.15,.12,.3)], 3, 60, 1050, 1650),
    ('Silver chimes', 'Chimes', [(1,1,1),(1.414,.4,.7),(2.31,.16,.4)], 5, 120, 1400, 2250),
    ('Wind crystals', 'Chimes', [(1,1,1),(1.67,.35,.65),(2.09,.24,.45)], 8, 150, 1250, 2500),
    ('Fine bells', 'Chimes', [(1,.9,1),(2.71,.28,.65),(4.05,.12,.3)], 5, 110, 950, 1300),
    ('Icy chimes', 'Chimes', [(1,1,1),(1.59,.34,.55),(2.52,.18,.4)], 3, 85, 1500, 2150),
    ('Distant glints', 'Chimes', [(1,1,1),(1.99,.15,.7)], 12, 180, 1750, 2650),
    ('Air shimmer', 'Shimmer', [(1,.6,1),(1.008,.4,.9),(1.5,.15,.55)], 10, 110, 1900, 2800),
    ('Stardust', 'Shimmer', [(1,1,1),(1.003,.25,.7),(2.21,.1,.4)], 5, 60, 2050, 2450),
    ('Frost flakes', 'Shimmer', [(1,.8,1),(1.33,.45,.75),(1.78,.25,.55)], 7, 85, 2000, 2950),
    ('Prism', 'Shimmer', [(1,.7,1),(1.25,.45,.7),(1.875,.2,.5)], 6, 100, 1500, 2750),
    ('Feather glints', 'Shimmer', [(1,1,1),(1.006,.18,.7),(2,.08,.4)], 14, 65, 2350, 2700),
]


def grain(variant, identity, strength):
    _, _, modes, attack, decay, low, high = variant
    rng = random.Random(identity * 65537 + 19)
    frequency = low * (high / low) ** rng.random()
    if variant[0] == 'Celesta high':
        # F#6 A6 B6 D7 E7: B minor pentatonic, same pitch set as D major UI.
        frequency = [1479.97769, 1760., 1975.53321, 2349.31814, 2637.02046][
            ((identity * 73 + 19) % 112) % 5]
    decay *= .85 + .3 * rng.random()
    count = int(RATE * (attack + decay * 5) / 1000)
    pcm = [0.] * count
    for ratio, amplitude, duration in modes:
        hz = frequency * ratio
        if hz > 5500:  # keep every mode inside the device's half-rate synth band
            continue
        phase = rng.random() * 2 * math.pi
        for i in range(count):
            t = i / RATE
            onset = 1 - math.exp(-t / (attack / 1000))
            envelope = onset * onset * math.exp(-t / (decay * duration / 1000))
            # Last 5 ms fade to exact silence; never truncate an oscillator.
            end = min(1., (count - 1 - i) / (RATE * .005))
            pcm[i] += amplitude * strength * envelope * end * math.sin(2 * math.pi * hz * t + phase)
    return pcm


def contacts():
    rng = random.Random(73)
    # Same sparse contacts followed by a denser roll for every proposal.
    return [(t, i % 112, .4 + .5 * rng.random()) for i, t in enumerate(
        [.3, .85, 1.5] + sorted(2 + rng.random() * 2.7 for _ in range(48)))]


def perceived_rms(pcm):
    # A common lightweight high-pass loudness proxy, not certified LUFS.
    previous = filtered = total = 0.
    coefficient = math.exp(-2 * math.pi * 500 / RATE)
    for sample in pcm:
        filtered = coefficient * (filtered + sample - previous)
        previous = sample
        total += filtered * filtered
    return math.sqrt(total / len(pcm))


def write_wav(path, pcm, gain):
    values = array.array('h', [round(max(-.95, min(.95, x * gain)) * 32767) for x in pcm])
    if __import__('sys').byteorder != 'little':
        values.byteswap()
    with wave.open(str(path), 'wb') as output:
        output.setparams((1, 2, RATE, len(values), 'NONE', 'not compressed'))
        output.writeframes(values.tobytes())


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = []
    for number, variant in enumerate(VARIANTS, 1):
        pcm = [0.] * (RATE * 6)
        for time, identity, strength in contacts():
            start = round(time * RATE)
            for offset, value in enumerate(grain(variant, identity, strength)):
                if start + offset < len(pcm):
                    pcm[start + offset] += value
        # Match the weighted RMS; retain headroom for dense contacts.
        gain = .018 / perceived_rms(pcm)
        peak = max(abs(x * gain) for x in pcm)
        if peak > .35:
            raise RuntimeError(f'{number}: excessive crest factor {peak}')
        write_wav(OUT / f'{number:02}-roll.wav', pcm, gain)
        single = [0.] * round(RATE * .1) + grain(variant, 23, .8)
        single += [0.] * round(RATE * .15)
        write_wav(OUT / f'{number:02}-single.wav', single, gain)
        manifest.append({'id': number, 'name': variant[0], 'family': variant[1],
                         'scale': 'B minor pentatonic' if number == 7 else '',
                         'peak': round(peak, 4), 'weightedRms': .018})
    (OUT / 'variants.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for name in ['index.html', 'app.js', 'style.css']:
        shutil.copyfile(Path(__file__).parent / name, OUT / name)
    print(f'{len(manifest)} variants, 40 WAV files at {RATE} Hz → {OUT}')


if __name__ == '__main__':
    main()
