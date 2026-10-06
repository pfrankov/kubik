#!/bin/sh
# Builds the simulator and renders sheet.png + face.mp4 + face.gif into sim/out.
set -e
cd "$(dirname "$0")"
mkdir -p out
cc -O2 -Wall -Wextra -Wno-unused-parameter -o out/sim sim.c sim_sheet.c sim_motion.c sim_dialogue.c sim_mood.c sim_rub.c sim_feel.c sim_play.c sim_text.c ../main/face.c ../main/face_params.c ../main/face_body.c ../main/face_card.c ../main/face_update.c ../main/face_setup.c ../main/face_menu.c ../main/face_agent.c ../main/agent_menu.c ../main/face_plush.c ../main/face_rub.c ../main/tess_geometry.c ../main/tess_motion.c ../main/tess_rub.c ../main/rub.c ../main/tess_mood.c ../main/mood.c ../main/tess_feel.c ../main/tess_gaze.c ../main/tess_play.c ../main/tess_touch.c ../main/tess_scatter.c ../main/tess_fall.c ../main/tess_draw.c ../main/render.c ../main/render_dots.c ../main/render_glass.c ../main/render_output.c ../main/render_pipeline.c ../main/render_raster.c ../main/render_scene.c ../main/render_text.c ../main/sprite.c ../main/qr.c ../main/font.c ../main/canvas.c ../main/font_data.c ../main/ui_text.c ../managed_components/espressif__qrcode/qrcodegen.c -I../managed_components/espressif__qrcode -lm
./out/sim sheet | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1920x6720 -i - -vf scale=960:3360 out/sheet.png
./out/sim video | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 480x480 -r 24 -i - -pix_fmt yuv420p out/face.mp4
ffmpeg -loglevel error -y -i out/face.mp4 -vf "fps=15,scale=240:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse" out/face.gif
echo "ok: $(pwd)/out"
