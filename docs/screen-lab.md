# Screen Lab and native emulator

Screen Lab previews production firmware screens with temporary fixtures. It uses
`face_draw`, the shared native input controller, and the same render pipeline on
ESP32 and in the browser. It does not record audio, call an agent, open a Wi-Fi AP,
save settings or complete the user's onboarding. Incoming real connection events
continue to be handled normally.

On an idle device, hold BOOT to open Settings. Tap the **Settings title five times
within four seconds**, then **hold that title for 0.8 seconds**. This sequence is
independent of the hidden Reset gesture. Closing Settings clears it. An active
conversation prevents entry; an allocation failure leaves Settings unchanged.

Choose a screen in the paginated catalog. BOOT navigates back within Agent/Sound/Events,
then to the catalog; BOOT in the catalog returns to real Settings. Hold BOOT to
close Settings. PWR keeps its normal device function and exits the lab. Two minutes
without interaction closes the lab and frees its fixture memory.

- Connecting, Wi-Fi QR and Setup QR: KEY advances the preview; QR sizes are unchanged.
- Pairing: a dummy code; no account is paired.
- Settings, Sound, Agent, Voice modes, Models and Guide: native controls, local values.
- Recording, Thinking, Speaking and GPT Live: Pause/Signal controls the synthetic
  level, Next advances states, Done returns to the catalog. In Live, Next cycles
  listening/thinking/speaking without leaving Live. KEY ends the Live preview; elsewhere it toggles the signal. Live
  controls sit below its title so the native microphone indicator stays visible.
  Hold the screen for 0.8 seconds to hide/show preview controls and inspect the
  entire unmodified screen.
- Home and Offline points: native gestures, inertia and orientation.
- Text reply and Error: real card and bubble layouts.
- Background work and Reminder: rotating clock and countdown, without creating jobs.
- Agent events: Next or KEY advances a local sequence through connection, thinking,
  tools, emotion, speech, notification, error and background work. Pause/Signal
  changes the simulated voice level. Done returns to the catalog.
- Event log: a sample history using the production viewer. Swipe up/down, use
  Older/Newer, tap an entry for details, and return with BOOT. Its overlay switch
  changes only this preview.

Offline points on the device plays real collision sounds through the saved
Interface volume. Other previews are silent; generated voice levels show visual
reactions, not microphone capture or speech playback. The browser has no audio
playback. QR payloads use `Kubik-DEMO` / `example.invalid` and cannot configure
the real device.

## Run without hardware

From the repository root, with a C compiler and Python 3 available:

```sh
python3 tools/emulator.py
```

Open the printed `http://127.0.0.1:8766/` address. Use `--port 0` for an automatically
assigned port. The first run compiles the native UI worker into ignored `dist`.
The QR component and sprite pack must be present; a clean development checkout
gets dependencies through the ESP-IDF reconfigure step described in
[acceptance](acceptance.md). BOOT, PWR, KEY are arranged left to right. Hold BOOT for
preview Settings; BOOT from the catalog also opens preview Settings. Hold Speech
for Sound, drag the character, or change the simulated
tilt. The character selector is a development control; production builds still
contain one character. Stop with Ctrl+C; the worker stops with the server.

All software acceptance runs without a connected device:

```sh
python3 tools/accept.py
```

For the affected UI controller alone:

```sh
python3 tools/test-screen-lab.py
uv run --with playwright==1.63.0 python tools/test-emulator-browser.py
```

The full acceptance additionally requires the development tools listed in
[acceptance](acceptance.md), including ESP-IDF and Chrome. No provider credentials
are required. `device` remains a separate physical check: native pixels do
not establish panel tearing, radio reliability, battery life or perceived audio.
See [Russian action/result scenarios](scenarios/debug.md).

## Recent agent events on the device

Tap the battery indicator in Settings five times within four seconds to reveal
**Events** and read the bounded local history or enable the home
overlay. See [event history and limits](events.md). Screen Lab keeps its sample
journal separate from the real device's history and saved preferences. The Settings
preview simulates the same hidden gesture; the Event log preset opens its sample
journal directly.
