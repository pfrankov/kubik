#!/usr/bin/env python3
"""Captures of Tess's moods for a look: python3 tools/tess-mood-captures.py [OUTDIR] (default /tmp/kubik-tess).

Builds the simulator, then writes OUTDIR/mood-<name>.gif for each of the 8 moods, mood-sheet.png
(all 8 moods in the same pose) and mood-transitions.png (a row of frames for each change of mood). Needs ffmpeg.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MOODS = ["calm", "curious", "playful", "loved", "scared", "grumpy", "sad", "sleepy"]
SIZE = 480
ROW = 6
FFMPEG = ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{SIZE}x{SIZE}", "-r", "30"]


def sim_frames(sim, *args):
    return subprocess.run([str(sim), "feel", *args], check=True, capture_output=True).stdout


def ffmpeg(frames, *args):
    subprocess.run(FFMPEG + ["-i", "-", *args], input=frames, check=True)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("/tmp/kubik-tess")
    out.mkdir(parents=True, exist_ok=True)
    sim = out / "sim-feel"
    sources = sorted((ROOT / "firmware/sim/run.sh").read_text().split("-o out/sim ")[1].split(" -I")[0].split())
    subprocess.run(["cc", "-O2", "-w", "-o", str(sim), *[str(ROOT / "firmware/sim" / s) for s in sources],
                    "-I" + str(ROOT / "firmware/managed_components/espressif__qrcode"), "-lm"], check=True, cwd=ROOT / "firmware/sim")
    for mood in MOODS:
        ffmpeg(sim_frames(sim, mood), "-vf", "fps=15,scale=240:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse", str(out / f"mood-{mood}.gif"))
    ffmpeg(sim_frames(sim, "sheet"), "-vf", f"scale=240:-1,tile=4x2", "-frames:v", "1", str(out / "mood-sheet.png"))
    ffmpeg(sim_frames(sim, "transitions"), "-vf", f"scale=240:-1,tile={ROW}x{len(MOODS) - 1}", "-frames:v", "1", str(out / "mood-transitions.png"))
    print("ok:", out)


if __name__ == "__main__":
    main()
