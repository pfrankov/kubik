# Collision sound auditions

Generate the local comparison page with no device, agent or paid API:

```sh
python3 tools/sound-lab/render.py
python3 -m http.server 8767 --bind 127.0.0.1 --directory dist/sound-lab
```

Open `http://127.0.0.1:8767/`. Each of the 20 numbered proposals has
**One particle**, **Rolling cluster** and **Favourite** controls. Only one
clip plays at a time; **Stop audio** stops it. Favourites persist in this
browser and can be filtered. Tell the developer the chosen numbers.

Variant **07 Celesta high** is the selected direction. Its pitches are now
F♯6, A6, B6, D7 and E7: B minor pentatonic, also compatible with the
existing D major pentatonic UI palette. Detuning is disabled. Firmware extends
this timbre one octave lower (F♯5..E7): stronger impacts select higher pentatonic
steps, weaker ones select deeper tones. The page is the original timbre audition.

These are additive timbre proposals, not recordings of installed firmware.
All use the same contact sequence, stable per-particle pitches, mono 24 kHz
PCM and modes below 5.5 kHz. A shared 500 Hz high-pass RMS proxy matches
listening levels; this is not certified LUFS measurement. The master level
only changes preview playback. It does not change Kubik's saved settings.

Generated WAV files and the page are in ignored `dist/sound-lab`; source
parameters are in `tools/sound-lab/render.py`. No clips or cloud dependencies
are embedded in firmware. After selection, implement the chosen resonances
in the bounded device synthesizer and verify the physical speaker, density,
Interface mute, voice isolation and silence at rest before acceptance.
