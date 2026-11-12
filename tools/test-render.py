#!/usr/bin/env python3
"""Run native renderer regressions with memory/undefined-behavior sanitizers."""
from pathlib import Path
import os
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
RENDER = ['render', 'render_dots', 'render_glass', 'render_output', 'render_pipeline', 'render_raster', 'render_projection', 'render_scene', 'render_text', 'render_emoji', 'render_emoji_store', 'emoji_cluster', 'render_overlay_s3', 'sprite', 'font', 'font_data']
FACE = ['event_journal', 'event_journal_draw', 'face', 'face_params', 'face_body', 'face_card', 'face_emoji', 'face_update', 'face_setup', 'face_menu', 'sound_levels', 'face_status', 'ui_kit', 'face_plush', 'face_rub', 'tess_geometry', 'tess_growth', 'tess_games', 'tess_gather', 'tess_sky', 'tess_sky_stream', 'tess_sky_compose', 'tess_sky_plan', 'tess_sky_melody', 'tess_sky_touch', 'tess_sky_summon', 'tess_sky_sound', 'tess_sky_draw', 'tess_play_shape', 'tess_stretch', 'tess_temper', 'tess_motion', 'tess_rub', 'rub', 'tess_mood', 'mood', 'tess_feel', 'tess_gaze', 'tess_play', 'tess_touch', 'tess_scatter', 'tess_fall', 'tess_draw']

def face_sources(extra=(), omit=()):
    # Tests that include a .c for a private reference omit that translation unit.
    return [source for source in FACE + RENDER if source not in omit] + list(extra)

TESTS = {
    'render_workspace': RENDER + ['canvas'],
    'render_sqrt': [source for source in RENDER if source != 'render_raster'],
    'render_bounds': [source for source in RENDER if source != 'render_raster'],
    'face_math': [],
    'tess_scene_capacity': face_sources(),
    'tess_sky_draw': face_sources(),
    'tess_sky_transition': face_sources(),
    'palette_blend': [],
    'output_flat': ['render_output'],
    'overlay_alpha': [source for source in RENDER if source != 'render_text'],
    'present_merge': RENDER + ['frame_store', 'canvas'],
    'window_plan': ['render_output'],
    'dot_alpha': [],
    'tess_palette': face_sources(omit=['tess_draw']),
    'tess_ripple_cache': face_sources(),
    'tess_target_cache': face_sources(omit=['tess_geometry']),
    'tess_scatter_cache': face_sources(omit=['tess_scatter']),
    'event_journal': ['event_journal'],
    'lift': ['lift'],
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
    'tess_animation_sound': face_sources(extra=['tess_sound', 'tess_sound_compose', 'tess_sound_body', 'tess_sound_sky', 'tess_sound_rules']),
    'tess_growth': face_sources(extra=[], omit=[]),
    'tess_play': face_sources(extra=[], omit=[]),
    'tess_tremble': face_sources(extra=[], omit=[]),
    'tess_ripple': face_sources(extra=[], omit=[]),
    'tess_gaze': face_sources(extra=[], omit=[]),
    'menu_layout': face_sources(extra=[], omit=[]),
    'text_layout': face_sources(extra=[], omit=[]),
    'face_card_emoji': face_sources(extra=[], omit=[]),
    'tess_fall': face_sources(extra=[], omit=['tess_fall']),
    'tess_shimmer': face_sources(extra=['canvas'], omit=[]),
    'present': face_sources(extra=['present', 'frame_store', 'canvas'], omit=[]),
    'edge_overlay': ['render', 'render_dots', 'render_glass', 'render_output', 'render_pipeline', 'render_raster', 'render_projection', 'render_scene', 'render_text', 'render_emoji', 'render_emoji_store', 'emoji_cluster', 'render_overlay_s3', 'sprite', 'font', 'font_data'],
    'icons': ['render_scene', 'render_text', 'render_emoji', 'render_emoji_store', 'emoji_cluster', 'render_overlay_s3', 'font', 'font_data'],
    'tess_conversation': face_sources(extra=[], omit=['tess_draw']),
    'stroke_fast': RENDER,
}


def compile_object(build, source, flags, objects):
    # Reuse only inside this invocation, with identical source and compiler flags.
    key = (source, tuple(flags))
    if key not in objects:
        target = str(Path(build) / f"unit-{len(objects)}.o")
        subprocess.run([*flags, "-c", source, "-o", target], cwd=ROOT, check=True)
        objects[key] = target
    return objects[key]


def blend_attributes():
    source = (ROOT / 'firmware/main/render_text.c').read_text()
    start = source.index('#if defined(ESP_PLATFORM) && CONFIG_IDF_TARGET_ESP32C6')
    policy = source[start:source.index('#endif', start) + len('#endif')]
    assert 'BLEND_PIXEL_FN void blend_pixel(' in source
    probe = policy + '\nBLEND_PIXEL_FN int blend_probe(void){return 1;}\n'
    for platform in (False, True):
        for chip in ('ESP32C6', 'ESP32S3'):
            for character in (0, 1):
                flags = [os.environ.get('CC', 'cc'), '-E', '-P', '-x', 'c', '-',
                         f'-DCONFIG_IDF_TARGET_{chip}=1', f'-DKUBIK_CHARACTER={character}']
                if platform:
                    flags.append('-DESP_PLATFORM=1')
                result = subprocess.run(flags, input=probe, text=True, capture_output=True, check=True).stdout
                size_profile = platform and chip == 'ESP32C6' and character == 0
                assert ('noinline' in result) == size_profile
                assert ('always_inline' in result) == (not size_profile)
    print('blend policy: one body; C6/Plush firmware shares it, other firmware/native profiles inline it')


def main():
    blend_attributes()
    subprocess.run(['python3', 'tools/assets/render-sqrt-table.py', '--check'], cwd=ROOT, check=True)
    subprocess.run(['python3', 'tools/assets/scatter-material-table.py', '--check'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="kubik-render-") as build:
        objects = {}
        runs = [(name, sources, []) for name, sources in TESTS.items()]
        # Force the lossless store's split path too: an independent capacity boundary.
        runs.append(("present", TESTS["present"], ["-DFRAME_STORE_BYTES=12288", "-DPRESENT_SPLIT=1"]))
        runs.append(("present", TESTS["present"], ["-DFRAME_STORE_BYTES=8"]))
        runs.append(("icons", TESTS["icons"], ["-DICON_TEST_COUNT=24"]))
        runs.append(("stroke_fast", TESTS["stroke_fast"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("render_bounds", TESTS["render_bounds"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("output_flat", TESTS["output_flat"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("overlay_alpha", TESTS["overlay_alpha"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("dot_alpha", TESTS["dot_alpha"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("face_card_emoji", TESTS["face_card_emoji"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("rectangle_layout", face_sources(), ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("render_workspace", TESTS["render_workspace"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("render_projection", [source for source in RENDER if source != "sprite"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("tess_scene_capacity", TESTS["tess_scene_capacity"], ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("tess_world_geometry", face_sources(omit=["tess_fall"]), []))
        runs.append(("tess_world_geometry", face_sources(omit=["tess_fall"]), ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        runs.append(("tess_portrait_size", face_sources(), []))
        runs.append(("tess_portrait_size", face_sources(), ["-DCONFIG_IDF_TARGET_ESP32S3=1"]))
        for name, sources, defines in runs:
            if "face" in sources:
                sources = [*sources, "agent_menu", "face_agent", "ui_text"]
            exe = str(Path(build) / (name + "".join(defines)))
            flags = [
                os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra",
                "-Wno-unused-parameter", *defines, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-Ifirmware/sim/present_stubs",
            ]
            units = [f"firmware/sim/{name}_test.c", *[f"firmware/main/{s}.c" for s in sources]]
            compiled = [compile_object(build, source, flags, objects) for source in units]
            subprocess.run([*flags, *compiled, "-lm", "-o", exe], cwd=ROOT, check=True)
            args = ["firmware/assets/sprites.bin"] if name == "canvas" else []
            subprocess.run([exe, *args], cwd=ROOT, check=True)
            if name == "render_bounds":
                rejected = subprocess.run([exe, "--outside-domain"], cwd=ROOT, capture_output=True, text=True)
                assert rejected.returncode == -signal.SIGABRT and "compute_bbox" in rejected.stderr, "Out-of-domain geometry was accepted"

if __name__ == "__main__":
    main()
