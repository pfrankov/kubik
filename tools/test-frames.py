#!/usr/bin/env python3
"""Compare every scripted simulator frame with the reviewed RGB reference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frame_diff


ROOT = Path(__file__).resolve().parent.parent
REFERENCE = ROOT / "firmware/sim/frame-hashes.json"
TESS_REFERENCE = ROOT / "firmware/sim/tess-motion-hashes.json"
MOOD_REFERENCE = ROOT / "firmware/sim/tess-mood-hashes.json"
SHEET_REFERENCE = ROOT / "firmware/sim/sheet-hashes.json"
SCENE_REFERENCE = ROOT / "firmware/sim/tess-scene-hashes.json"
RUB_REFERENCE = ROOT / "firmware/sim/rub-hashes.json"
CALM_REFERENCE = ROOT / "firmware/sim/tess-calm-hashes.json"
FEEL_REFERENCE = ROOT / "firmware/sim/tess-feel-hashes.json"
DIALOGUE_REFERENCE = ROOT / "firmware/sim/tess-dialogue-hashes.json"
TEXT_REFERENCE = ROOT / "firmware/sim/text-hashes.json"
WIDTH = HEIGHT = 480
FRAME_BYTES = WIDTH * HEIGHT * 3
FRAME_COUNT = 30 * 38
SOURCES = ["sim/sim", "sim/sim_sheet", "sim/sim_motion", "sim/sim_dialogue", "sim/sim_mood", "sim/sim_rub", "sim/sim_feel", "sim/sim_play", "sim/sim_text", "main/event_journal", "main/event_journal_draw", "main/face", "main/face_params", "main/face_body", "main/face_card", "main/face_update", "main/face_setup", "main/face_menu", "main/face_agent", "main/agent_menu", "main/face_plush", "main/face_rub", "main/tess_geometry", "main/tess_motion", "main/tess_rub", "main/rub", "main/tess_mood", "main/mood", "main/tess_feel", "main/tess_gaze", "main/tess_play", "main/tess_touch", "main/tess_scatter", "main/tess_fall", "main/tess_draw", "main/render", "main/render_dots", "main/render_glass", "main/render_output", "main/render_pipeline", "main/render_raster", "main/render_scene", "main/render_text", "main/sprite", "main/qr",
           "main/font", "main/canvas", "main/font_data", "main/ui_text", "managed_components/espressif__qrcode/qrcodegen"]


def env_for(character, extra=None):
    return {**os.environ, "SIM_CHARACTER": character, "KUBIK_SPRITES": str(ROOT / "firmware/assets/sprites.bin"), **(extra or {})}


def hashes(exe, character, command=("video",), frame_count=None, extra=None):
    """Hash every frame the simulator writes; frame_count, when given, is the exact number expected."""
    process = subprocess.Popen([exe, *command], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env_for(character, extra))
    actual = []
    try:
        while frame_count is None or len(actual) < frame_count:
            data = process.stdout.read(FRAME_BYTES)
            if not data and frame_count is None:
                break
            if len(data) != FRAME_BYTES:
                raise AssertionError(f"{character}: frame {len(actual)} has {len(data)} of {FRAME_BYTES} bytes")
            actual.append(hashlib.blake2b(data, digest_size=16).hexdigest())
        if frame_count is not None and process.stdout.read(1):
            raise AssertionError(f"{character}: more than {frame_count} frames")
        stderr = process.stderr.read().decode(errors="replace")
        if process.wait() != 0:
            raise AssertionError(f"{character}: simulator failed: {stderr}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
    return actual


def write_reference(path, frames_by_character):
    lines = ["{\n"]
    for position, (character, frames) in enumerate(frames_by_character.items()):
        lines.append(f'  "{character}": [\n')
        for index in range(0, len(frames), 8):
            chunk = ", ".join(json.dumps(value) for value in frames[index:index + 8])
            lines.append("    " + chunk + ("," if index + 8 < len(frames) else "") + "\n")
        lines.append("  ]" + ("," if position + 1 < len(frames_by_character) else "") + "\n")
    lines.append("}\n")
    path.write_text("".join(lines))


# (reference file, key, character, simulator command, exact frame count or None)
STREAMS = [
    (DIALOGUE_REFERENCE, "dialogue", "tess", ("dialogue",), 18 * 30),
    (REFERENCE, "plush", "plush", ("video",), FRAME_COUNT),
    (REFERENCE, "tess", "tess", ("video",), FRAME_COUNT),
    (TESS_REFERENCE, "tess-motion", "tess", ("tess", "motion"), 25 * 3 * 30),
    (MOOD_REFERENCE, "tess-mood", "tess", ("mood",), 83 * 30),  # idle moves and reactions, see sim_mood.c
    (SHEET_REFERENCE, "plush", "plush", ("sheet", "tiles"), None),
    (SHEET_REFERENCE, "tess", "tess", ("sheet", "tiles"), None),
    (RUB_REFERENCE, "plush", "plush", ("rub",), 30 * 30),  # petting scripts (sim_rub.c): swipe, slow, vigorous, stop
    (RUB_REFERENCE, "tess", "tess", ("rub",), 30 * 30),
    (TEXT_REFERENCE, "plush", "plush", ("text",), 18),  # every screen with text (sim_text.c), one frame each
    # The neutral Tess (SIM_MOOD_OFF: no moods) plays the same scripts as before there were moods: what every mood is a change of.
    (CALM_REFERENCE, "motion", "tess", ("tess", "motion"), 25 * 3 * 30, {"SIM_MOOD_OFF": "1"}),
    (CALM_REFERENCE, "rub", "tess", ("rub",), 30 * 30, {"SIM_MOOD_OFF": "1"}),
    (FEEL_REFERENCE, "tess-feel", "tess", ("feel",), 7 * 10 * 30 + 15 * 30),  # each mood's clip (sim_feel.c): 7 of 10 s, the sleepy one 15 s
    (FEEL_REFERENCE, "tess-feel-sheet", "tess", ("feel", "sheet"), 8),  # all eight moods in the same pose
    (FEEL_REFERENCE, "tess-feel-changes", "tess", ("feel", "transitions"), 7 * 6),  # from each mood to the next
    (SCENE_REFERENCE, "gravity", "tess", ("gravity",), None),
    (SCENE_REFERENCE, "jitter", "tess", ("jitter",), None),
]


def first_change(expected, frames):
    if len(expected) != len(frames):
        return min(len(expected), len(frames))
    return next((index for index, pair in enumerate(zip(expected, frames)) if pair[0] != pair[1]), None)


def check_stream(exe, stream, frames):
    path, key, character, command, _, *extra = stream
    expected = json.loads(path.read_text())[key]
    index = first_change(expected, frames)
    if index is None:
        print(f"{path.stem} {key}: all {len(frames)} frames match")
        return None
    expected_hash = expected[index] if index < len(expected) else ''
    image = frame_diff.report(ROOT, SOURCES, exe, env_for(character, *extra), command, index, f"{path.stem}-{key}", expected_hash)
    return f"{path.stem} {key}: frame {index} changed (of {len(frames)}); diff image {image}"


def references(actual):
    by_file = {}
    for (path, key, *_), frames in zip(STREAMS, actual):
        by_file.setdefault(path, {})[key] = frames
    return by_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", action="store_true", help="replace the reviewed reference after inspecting changed frames")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kubik-frames-") as temp:
        exe = str(Path(temp) / "sim")
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                        "-Wno-unused-parameter", "-o", exe,
                        *[str(ROOT / "firmware" / (source + ".c")) for source in SOURCES],
                        "-I" + str(ROOT / "firmware/managed_components/espressif__qrcode"), "-lm"], check=True)
        actual = [hashes(exe, *stream[2:]) for stream in STREAMS]
        if args.record:
            for path, frames_by_key in references(actual).items():
                write_reference(path, frames_by_key)
            print("Recorded " + ", ".join(f"{stream[1]} {len(frames)}" for stream, frames in zip(STREAMS, actual)))
            return
        failures = [message for message in (check_stream(exe, stream, frames) for stream, frames in zip(STREAMS, actual)) if message]
    if failures:
        raise AssertionError("\n".join(failures) + "\ninspect the images before updating the reference with --record")


if __name__ == "__main__":
    main()
