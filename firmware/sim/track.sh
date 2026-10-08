#!/bin/sh
# Frame-by-frame face alignment review: sim/out/track/<anim>.png, frames in reading order
# (face drawn on each frame, tracked screen outline in yellow). Usage: sh track.sh [anim...]
set -e
cd "$(dirname "$0")"
mkdir -p out/track
cc -O2 -w -o out/sim sim.c sim_sheet.c sim_motion.c sim_dialogue.c sim_mood.c sim_rub.c sim_feel.c sim_play.c sim_text.c ../main/face.c ../main/face_params.c ../main/face_body.c ../main/face_card.c ../main/face_update.c ../main/face_setup.c ../main/face_menu.c ../main/face_plush.c ../main/face_rub.c ../main/tess_geometry.c ../main/tess_growth.c ../main/tess_games.c ../main/tess_motion.c ../main/tess_rub.c ../main/rub.c ../main/tess_mood.c ../main/mood.c ../main/tess_feel.c ../main/tess_gaze.c ../main/tess_play.c ../main/tess_touch.c ../main/tess_scatter.c ../main/tess_fall.c ../main/tess_draw.c ../main/render.c ../main/render_dots.c ../main/render_glass.c ../main/render_output.c ../main/render_pipeline.c ../main/render_raster.c ../main/render_scene.c ../main/render_text.c ../main/sprite.c ../main/qr.c ../main/font.c ../main/canvas.c ../main/font_data.c ../main/ui_text.c ../managed_components/espressif__qrcode/qrcodegen.c -I../managed_components/espressif__qrcode -lm
anims=${*:-$(python3 -c "import json;print(' '.join(a['name'] for a in json.load(open('../../tools/sprites/clips.json'))['anims']))")}
for a in $anims; do
  n=$(./out/sim track "$a" 2>&1 >/dev/null | sed -n "s/^$a: \([0-9]*\) frames/\1/p")
  ./out/sim track "$a" 2>/dev/null | uv run -q --with pillow ../../tools/sprites/track_sheet.py "out/track/$a.png"
  echo "$a: $n frames"
done
