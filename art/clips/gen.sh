#!/bin/bash
# Generate animation clips with Veo 3.1 Lite first/last-frame (hero -> hero).
# Usage: gen.sh name [name...]   (reads prompts.json; skips existing mp4)
cd "$(dirname "$0")"
H=https://v3b.fal.media/files/b/0aac12a4/baldc8m6Ps5ocHCUMBqtH_nNcoebQc.png
PRE='The cream plush bear toy with a black face screen comes to life. '
POST=' Static locked-off camera, no camera movement, no zoom, no cuts, the character stays centered at the same size and fully in frame. The face screen stays completely blank black glass the whole time: no eyes, no face, no symbols, no light ever appear on it. Pure black background, no floor, no shadow, no other objects. Smooth soft plush animation with nice squash and stretch, cute and gentle.'
for n in "$@"; do
  [ -f "$n.mp4" ] && continue
  P="$PRE$(python3 -c "import json,sys;print(json.load(open('prompts.json'))[sys.argv[1]])" "$n")$POST"
  ( ~/.local/bin/genmedia run fal-ai/veo3.1/lite/first-last-frame-to-video --prompt "$P" --first_frame_url "$H" --last_frame_url "$H" \
      --duration 8s --resolution 720p --generate_audio false --aspect_ratio 9:16 > "/tmp/gen_$n.out" 2>&1
    U=$(grep -oE 'https://[^"]+\.mp4' "/tmp/gen_$n.out" | head -1)
    if [ -n "$U" ]; then curl -s -o "$n.mp4" "$U" && echo "$n ok"; else echo "$n FAILED"; tail -c 300 "/tmp/gen_$n.out"; fi ) &
  sleep 2
done
wait
