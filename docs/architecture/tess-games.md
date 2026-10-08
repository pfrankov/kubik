# Native Tess games and growth architecture

## Runtime contract

`tess_games.c` owns two bounded per-face games, three tiers each, and six durable
discovery bits. There is no game screen or dedicated button route. One ordinary
tap followed by 0.55 seconds of quiet produces an invitation wave, bow and local
cue. The next tap within six seconds joins Duet; this accepting tap and active
game taps are local, without agent pokes. Rapid taps before the invitation keep
ordinary character reactions, including the existing six-tap scatter.

Duet answers the player's touch location and pace. A WAIT tap schedules a reply
0.24–0.48 seconds later, derived from the preceding interval. The wave, gold tint
and bow accompany the reply. Taps during that brief SHOW phase are ignored;
there is no exact rhythm test. Four, six or eight exchanges open bits 0–2.
The final exchange earns its bit when Tess answers, not before the reply.

An ordinary short swipe (at least 72 px in 0.08–0.6 seconds) invites Chase after
0.55 seconds from release. Raw pointer samples feed the existing two-axis
rotation/inertia before entry. A game drag of 18 px or hold of 0.65 seconds yields
its original coordinates and age to ordinary touch. That release cannot start
another game. Long petting gestures do not invite Chase.

Chase folds the same body into a bright 13 px core and visits seven irregular
waypoints, with variable flights and pauses. A six-position trail samples the
actual projected centre at no more than 18 Hz. Taps within 68 px of that centre
count, at least 0.45 seconds apart and after 48 px of displacement since the
last catch. Three, four or five catches open bits 3–5. A miss does not count;
Tess responds and moves towards the hand for another attempt.

Both games allow six seconds without an accepted exchange and twenty seconds
per round. Celebration returns to normal after three seconds. Progress is a
finite set of unique wins: repeating a tier never changes its bit. The original
six-bit mask and growth milestones remain; no saved settings need migration.

A hold, pet, shake, KEY/voice/Live, agent text/speech, menu/Setup, offline or dark
state ends play without resumption. Recording keeps its existing sphere;
thinking and speech temporarily target the full tesseract. Model, cues and trail
use fixed arrays; no per-frame allocation, flash or network operations.

The app keeps its existing limit of eight ordinary events followed by ten
reserved events per batch. Only taps captured while the face still identifies
an active Tess game are deferred until the batch's other events have run.
They occupy eight fixed `{x, y, round}` records (96 stack bytes), with no new
queue, allocation or I/O. After each other event, loss of app or face eligibility
cancels the game even if a later event restores availability in the same batch.
Existing handlers still own session, text revision and replacement-route checks;
stale events and empty ordinary text therefore do not create a second preemption
policy. Notifications retain their normal priority, including an empty notification.
Normal non-game event order, including KEY press/release, stays unchanged.

Deferred taps are consumed only by the game model after another eligibility
check under the face mutex. They cannot dismiss a new card, stop newly started
speech/capture, become agent pokes or wake a screen that the batch turned off.
Capture deliberately checks face-level gameplay first: speech may have begun
before its reserved event is handled. A monotonically advanced nonzero round
token also prevents an old tap from reaching a round that the display task
started while an event handler was running; restore advances the token too.
This is batch-level game preemption, not a global FIFO order between the two
event channels. Events arriving after the bounded drain belong to a later batch.

## Growth geometry

The production renderer keeps the same 112 point slots and 16 signed 4D vertex
slots. `tess_growth_vertices` scales those axes in order and passes them through
the existing 4D turn and edge sampler: the first two axes make the square, the
third opens the cube, and the fourth completes the tesseract. At collapsed
stages, the unchanged sampler still emits its 112 slots; coincident projected
dots are suppressed by the growth renderer. Animation eases between form
targets. There is no second 2D renderer or separate point lattice.

The adult renderer remains the default for `face_init` fixtures. The production
bootstrap calls `tess_games_restore`, after settings load and after `face_init`,
before the display task starts. Game and growth Lab fixtures restore their own
temporary masks. The dormant path and the fully
grown, unfolded path call the original `tess_vertices4d` exactly, preserving
adult projection behavior.

## Persistence and resource bounds

The settings key `tess_progress` is a six-bit NVS mask. A missing key, including
on upgrade, or a value containing unsupported bits loads as zero; other saved
settings are independent. Successful wins update in-memory progress first. The
app task later copies the mask under the face mutex, releases it, then writes
through the NVS lock only when agent, speech and capture state allow it. Failed
writes retry after 10 seconds. NVS merges the incoming mask with the stored
mask, so stale snapshots cannot erase earlier wins. An invalid stored mask is
replaced with a valid mask on the next earned discovery. Full factory reset clears
the key and resets the in-memory game state before queueing reboot, so a pending
app flush cannot restore erased wins. A latch under the same NVS mutex rejects
older snapshots from a concurrent USB reset until the next startup load.
Screen Lab fixtures pass temporary masks to their own face and never
write settings.

This uses the existing ESP-IDF 5.5.1 [NVS integer and commit contract](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c6/api-reference/storage/nvs_flash.html).
RAM publishes the durable mask only after a successful commit. Power loss during
an unfinished write can lose that new milestone; it is not a confirmed save.

The per-frame model is fixed-size and uses no heap allocation, network call or
flash write. Persistence occurs in the app task outside the display frame and
face mutex. Frame-sensitive shape work is bounded to the existing vertex and
point arrays; updates use elapsed time capped at 100 ms.

## Design decisions

Progress counts **unique wins**, not total play time, streaks or an open-ended
score. This provides six visible milestones, survives reboots, prevents
grinding from manufacturing growth and cannot decay when the device is unused.
The Duet and Chase bit ranges keep the six discoveries independently verifiable.

For geometry, axis scaling reuses Tess's 4D vertex set and renderer while
preserving the topology as each dimension opens. A separate shape renderer
would introduce competing point layouts; flattening the existing tesseract in
2D would lose the staged square-to-cube-to-tesseract structure. Axis growth
keeps one source of points and one adult path.

Games grow from a normal touch or swipe, with an observable invitation rather
than a secret gesture puzzle. Unattended timers never start a game.

For queued game taps, draining the whole reserved inbox first would reorder
ordinary KEY-down and reserved KEY-up. A shared global event order would change
both channels and their latency bounds. A separate pending-event fence would
duplicate the handlers' session, revision and route checks. Bounded deferral of
only active-game taps keeps those handlers and normal input order intact, while
letting every effective preemption in their batch win over game scoring.

For comparison, Flipper's [Dolphin service](https://github.com/flipperdevices/flipperzero-firmware/blob/dev/applications/services/dolphin/dolphin.c)
uses deed events, deferred persistence and separate daily/mood timers. Tess keeps
the separation of gameplay state from storage, but its finite discoveries do
not need calendar timers, an additional service queue or absence penalties.

## Verification

`python3 tools/test-tess-games.py` exercises the model and production tap route
with ASan/UBSan. The renderer, runtime eligibility/bootstrap, and NVS storage
regressions are covered by `python3 tools/test-render.py`,
`python3 tools/test-state.py`, and `python3 tools/test-settings.py`; all run in
`python3 tools/accept.py host`. `python3 tools/accept.py build` builds both TESS
and PLUSH firmware. Run both levels for a code change. Device acceptance runs
`node tools/test-device-tess-games.mjs`: native touch sampling, four swipe
directions, all six temporary Lab tiers, varied pacing, miss feedback, hold/drag
handoff and timeout. Its optional `--learn` earns missing real discoveries and
checks durability after reboot; it never resets user settings.

USB `sim:pointer` supplies one validated controller sample (integer x/y 0–479),
expiring after 350 ms. It enters the same input task and gesture/rub handlers as
the touch controller; it cannot set game state or award victories. USB
`sim:game-state` reports a mutex-protected snapshot and saved mask, without
credentials. These verify execution on the ESP32, not finger sensitivity,
subjective audio or panel appearance.

Screen Lab's growth/game fixtures can play only the local TOUCH, FLING, SWING,
EXCITE, DODGE and JOY cues; offline remains IMPACT-only. Synthetic agent/voice
screens cannot play provider speech or start capture. The same permission
predicate is rechecked under the face lock immediately before physical playback.

For physical acceptance, tap once and observe the invitation, then play at
varied tempos and positions. Swipe horizontally and vertically; check the
usual inertia before folding, target visibility, pauses, catches and misses.
Hold or drag in play and confirm ordinary pet/rotation returns. Check KEY,
reply, offline, menu and dark preemption, then restore user settings. No acoustic
wake-word or paid provider run is required for this change.
