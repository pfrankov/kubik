#!/usr/bin/env python3
"""Run native renderer regressions with memory/undefined-behavior sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
RENDER = ['render', 'render_dots', 'render_glass', 'render_output', 'render_pipeline', 'render_raster', 'render_scene', 'render_text', 'sprite', 'font', 'font_data']
FACE = ['event_journal', 'event_journal_draw', 'face', 'face_params', 'face_body', 'face_card', 'face_update', 'face_setup', 'face_menu', 'face_plush', 'face_rub', 'tess_geometry', 'tess_growth', 'tess_games', 'tess_motion', 'tess_rub', 'rub', 'tess_mood', 'mood', 'tess_feel', 'tess_gaze', 'tess_play', 'tess_touch', 'tess_scatter', 'tess_fall', 'tess_draw']

def face_sources(extra=(), omit=()):
    # Tests that include a .c for a private reference omit that translation unit.
    return [source for source in FACE + RENDER if source not in omit] + list(extra)

TESTS = {
    'event_journal': ['event_journal'],
    'viewpoint': ['viewpoint'],
    'pose_graph': face_sources(extra=[], omit=['face_body']),
    'sprite_interpolation': face_sources(extra=[], omit=['sprite']),
    'face_timing': face_sources(extra=[], omit=[]),
    'render_regression': face_sources(extra=['canvas'], omit=[]),
    'canvas': ['canvas'],
    'tess_projection': face_sources(extra=['viewpoint'], omit=['tess_draw']),
    'tess_rest': face_sources(extra=['viewpoint'], omit=['tess_geometry']),
    'tess_mood': face_sources(extra=['viewpoint'], omit=['tess_draw']),
    'rub': face_sources(extra=[], omit=[]),
    'tess_rigid': face_sources(extra=[], omit=[]),
    'tess_feel': face_sources(extra=[], omit=['tess_draw']),
    'tess_scatter': face_sources(extra=[], omit=[]),
    'tess_games_integration': face_sources(extra=[], omit=[]),
    'tess_growth': face_sources(extra=[], omit=[]),
    'tess_play': face_sources(extra=[], omit=[]),
    'tess_tremble': face_sources(extra=[], omit=[]),
    'tess_ripple': face_sources(extra=[], omit=[]),
    'tess_gaze': face_sources(extra=[], omit=[]),
    'menu_layout': face_sources(extra=[], omit=[]),
    'text_layout': face_sources(extra=[], omit=[]),
    'tess_fall': face_sources(extra=[], omit=['tess_fall']),
    'tess_shimmer': face_sources(extra=['canvas'], omit=[]),
    'present': face_sources(extra=['present', 'frame_store', 'canvas'], omit=[]),
    'edge_overlay': ['render', 'render_dots', 'render_glass', 'render_output', 'render_pipeline', 'render_raster', 'render_scene', 'render_text', 'sprite', 'font', 'font_data'],
    'icons': ['render_scene', 'render_text', 'font', 'font_data'],
    'tess_conversation': face_sources(extra=[], omit=['tess_draw']),
    'stroke_fast': RENDER,
}

def main():
    with tempfile.TemporaryDirectory(prefix="kubik-render-") as build:
        runs = [(name, sources, []) for name, sources in TESTS.items()]
        # Force the lossless store's split path too: an independent capacity boundary.
        runs.append(("present", TESTS["present"], ["-DFRAME_STORE_BYTES=12288", "-DPRESENT_SPLIT=1"]))
        runs.append(("present", TESTS["present"], ["-DFRAME_STORE_BYTES=8"]))
        for name, sources, defines in runs:
            if "face" in sources:
                sources = [*sources, "agent_menu", "face_agent", "ui_text"]
            exe = str(Path(build) / (name + "".join(defines)))
            subprocess.run([
                os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
                "-Wno-unused-parameter", *defines, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-Ifirmware/sim/present_stubs", f"firmware/sim/{name}_test.c",
                *[f"firmware/main/{s}.c" for s in sources], "-lm", "-o", exe,
            ], cwd=ROOT, check=True)
            args = ["firmware/assets/sprites.bin"] if name == "canvas" else []
            subprocess.run([exe, *args], cwd=ROOT, check=True)

if __name__ == "__main__":
    main()
