# Tess play architecture

## Runtime contract

Her structure is the toy. `tess_games.c` owns eligibility, touch routing,
discoveries, growth form and self-play scheduling; `tess_play_shape.c` turns play
into displacements of her 16 tesseract vertices; `tess_stretch.c` owns the pull,
flight and self-play acts; `tess_gather.c` owns the burst's hang and sweep. Nothing
is drawn on top of her: the renderer, palette and dot budget are unchanged.

**Structure.** `tess_form` emits each vertex followed by three dots along each of
its upward edges (16 + 32 × 3 = 112 form slots). `tess_play_frame`, called from
`tess_point_targets` once per target pass with the body scale and offset, computes
one displacement per vertex; `tess_play_point` gives each dot the blend of its
edge's two ends, so she always stays connected. The existing per-dot damped
springs supply wobble, overshoot and lag; a frame with any displacement sets
`shaping`, which keeps the rigid fast path off. While she is still folded small
(`form < 3`), displacements use the unfolded tesseract (inner cell 0.55, outer 1)
as their home, so a point flashes the full shape. The same pass stores each
vertex's panel position (`vertex_px`, without perspective) for touch lookup: no
input path projects dots or allocates on its task stack.

**Pluck and Echo.** An ordinary tap within 150 px of her centre, still delivered
to ordinary tap reactions, plucks the nearest vertex. A wave (four slots: a free
one first, else the oldest) rings each hypercube shell `h = popcount(v ^ origin)`
from `0.12 h` s, amplitude `0.7 + 0.3 h`, 3 Hz, decay 3.2/s, outwards by 0.55 units
and towards the viewer by 0.3. Each shell plays `TC_EXCITE` one step higher; the
opposite corner raises `pulse`. A second non-quiet pluck at another corner within
three shells of a wave is Echo; amplitudes simply add where waves overlap.

**Bloom.** A fling keeps 85% of the spin it meets (at most 16 rad/s), and a finger
moving faster than 250 px/s over her drives rather than brakes her. Bloom eases
towards `smoothstep((spin - 3.5) / 6.5)`: the outer cell grows by 1.1, the inner by
0.3, and the two cells counter-rotate by ±0.55 rad. Above 0.8 is the discovery.

**Twang, Slingshot and catch.** A finger that lands at least 120 px from her
centre (and is not a burst sweep) starts a STRETCH round on the nearest vertex:
it is pulled towards the finger (at most 2.6 units), its neighbours by 0.42, 0.17,
0.06 and 0.02 per edge-distance. A release within 12 px and 0.35 s is an ordinary
tap. Beyond her 90 px reach, a release past 0.3 units twangs (Twang); past 0.9
units it slings her (FLY) the opposite way at `3 + 6 × pull` units/s (at most 15).
She flies with drag 0.8/s and reflects off a 1.45-unit rim with restitution 0.6,
flattening the side that hit (`squash`); the first rim impact is Slingshot. At
12 units/s or more the impact bursts her (`tess_scatter_begin`). Slow, she
returns on a spring; a touch within 95 px of her in flight catches her.

**Gather.** The burst stops at `TESS_HANG_S` while `tess_games_holds_burst`
holds (1.8 s with no round, or the whole round). A finger landing at the hang
starts Gather; each frame the finger's segment is tested against each slot's
precomputed panel spot in squared distances (46 px, 96 px once 85% are home);
hanging dots within 120 px lean towards the finger; swept dots fly home in 0.3 s
along alternating curves. All 112 home finishes the burst and is the discovery;
2.2 s without a newly gathered dot ends the round.

**Self-play.** After 16–30 s of quiet (no finger, rub, heart, burst, agitation,
dimming or drowsiness, and eligible), she plays a quiet pluck or a spin kick.
Corner pulls require a finger; there is no autonomous corner-stretch animation.
Each act starts a low-gain body tone. A landing finger ends it.
Dimming releases quiet waves and the flagged
idle-spin impulse, preserving manual drag inertia. Self-play earns nothing.

**Organic burst.** Each burst's spread is best-candidate sampling on the square
glass (16 px margin): every dot takes the farthest of 5–8 random candidates from
the dots already placed (integer distances, a 60 px neighbourhood grid), so there
are no clumps, overlaps or big holes and no grid. It is laid out ahead, 16 dots a
frame while she is whole (`tess_scatter_prepare`). At the burst a throw leans
every dot 22–30 px its way, less the nearer the wall it already is, so nothing
lines up along the edge; a 3 px jitter follows. The host test keeps the spacing
spread 0.1–0.36, the closest pair ≥10 px, the largest hole <70 px and at most five
dots on the edge. Per dot: its own departure (0–0.12 s), flight (0.38–0.7 s),
curve (±0.45) and grip. While the burst hangs, the relative tilt slides every free dot at
900 px/s² × (1 − grip) with linear friction (skipped while level and at rest); dots bounce off the panel edges
(knocks above 120 px/s tick, at most two per frame) and a dot within 60 px of
her centre is poured home (`tess_gather_pour`, which starts the round). Held
tilted, the burst waits up to 8 s and a round does not time out.

**Tilt.** `tilt` is gravity on the panel minus a resting pose that starts at the
first reading and follows with a 20 s time constant, with a 0.06 dead band. Her
body (`tess_body_update`) is one point mass for throws, tilt and rest: tilt
accelerates it at 14 units/s² per unit when no round holds her; a home spring of
3.5/s² with damping 1.2/s (underdamped, she sloshes), none during a fast flight,
and 40/s² critically damped while the agent has priority. Rim impacts above
0.8 units/s squash, thud and rattle; only a throw can shatter. Corners sag along
the tilt (outer 0.5, inner 0.3 units per unit, varied per corner).

**Temper.** `tess_temper.c` keeps excitement, annoyance, fear and fondness
(0–1, fading at 0.08, 0.07, 0.1 and 0.02 per second) and the last touch. A pluck
adds excitement, and annoyance when it comes within 1.2 s of the last (more at
the same corner). Above 0.7 annoyance a tap recoils the corner (negative
amplitude, she shrinks back), flinches, grumbles and raises `ME_PESTER`; the same
corner at once while excited giggles (pace 0.55: quicker and an octave up); 30% of
excited plucks are answered from the opposite corner 0.55–0.85 s later and 10%
reflect. Pets soothe; rim hits frighten (`ME_JOLT` above 6 units/s); a full
gather calms and endears (`ME_GLAD`). Fear above 0.5 inverts and weakens a pull;
fondness strengthens how far her other corners follow. Annoyance damps bloom.
Self-play is chosen by temper and mood: none when cross or scared, spin or pluck
when excited, otherwise alternating plucks and spins. After a minute without touch
she only plucks; excited, she asks again within 3–6 s. Knocks (`jolt` above
0.015) rattle up to three corners with quiet plucks and grains.

**Body sound.** `tess_tone` queues up to twelve physical tones per frame beside the
cues (`face_take_tone`), silent when cues are; the display task plays them after
the face lock through `audio_tess_tone`, which skips them while the microphone is
open or a reply plays. `tess_sound_body.c` turns each into one synth note shaped by
kind, always from the established ensembles (no bell, no tune): a ring (GLOW at
her body pitch, the corner choosing the octave or twelfth overtone and the beating,
sinking 1.5% as it settles; a quick wave a fifth up, a recoil a fourth down), creak
(SWEEP rising with tension, beating faster), twang (falling to rest), thud (HUM,
200–290 Hz by size, a third harmonic for the small speaker), grain (a short NUDGE
puff gliding down 20%), whoosh (a launch accent), chirp
(a glide up or down) and wobble (CLUSTER beating). `tess_tone_hz` is continuous,
not a scale: a 0–1 height lifts her resting pitch up to a fifth; the rest is D4,
B3 when grumpy, sad or sleepy, F#4 when scared, a fifth up for a Tess smaller than
a cube. Body notes are 150–1000 Hz, at most three at once, each ducked by those
sounding. Full-strength levels measure −35 to −44 dB on the speaker weighting,
under her words. `python3 tools/render-particles.py --play OUT.wav` renders a
scripted play session through the real face and synth and prints its peak,
loudest 100 ms and biggest rise.

`tess_motion_sound.c` observes the existing rendered projection, without a second
geometry pass. Every 50 ms it measures all 112 points, smooths their speed and
maps changing extent to resonance. This covers rigid turns (zero solver velocity),
springs, growth and inertia during an explicit animation. `TT_MOTION` updates coalesce in the queue;
`tess_sound_motion.c` sustains one quiet body voice at a fixed carrier pitch,
varying its level and partials without restarting phases or envelope. The material varies once per gesture,
using the synth's prime cycles, independently of visual randomness.
The voice shares the three-body-voice limit, never steals, and retries if full.
A zero-gain release passes through full/quiet queues; missing updates release
after 0.83 s. Menu, text, dim, sleep, agent processing, capture, speech, Live, Skies and fallen
particles stop this layer. Screen Lab uses its own face and preserves cleanup
across screen replacement. Only a touch gesture, finite game/reaction, carried
momentum owns it, with a bounded 0.6 s spring tail.
Device tilt, rocking and the camera quaternion's automatic recentering cannot
start or prolong it. Static sag, idle pose rates and
a stationary finger without a recognized purr cannot sustain sound. Returning to ordinary idle drift
releases it, leaving real silence for the local listener.

Discovery bits are 0 Pluck, 1 Echo, 2 Bloom, 3 Twang, 4 Slingshot, 5 Gather. A new
bit raises `pulse`, `TR_JOY` and `TC_JOY`. Existing six-bit masks keep their
meaning as milestones; no settings migration is needed.

Eligibility loss (KEY/voice/Live, a card, agent text/speech, menu/Setup, offline,
dark) and the agent's events (`FEV_NOTIFY`, `FEV_NOT_HEARD`, `FEV_FAIL`,
`FEV_TALK_START`) stop everything at once: waves, pull, flight, squash, self-play
and the round; bloom follows a zero target. Wake, volume and shake do not. A
round consumes its finger: its release tap, and the pet events of a held pull or
sweep. Ownership expires 0.5 s after a lift whose tap never arrived. Recording
keeps its sphere; thinking and speech target the full tesseract. All state is
fixed-size; there is no allocation, flash or network work per frame. Per frame a
wave costs five `sin`/`exp` pairs; radial directions are computed once.

The app keeps its existing limit of eight ordinary events followed by ten
reserved events per batch. Only taps captured while the face still identifies
an active Tess game, or an owned finger's release, are deferred until the batch's
other events have run. A deferred tap whose round has ended is dropped.
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

Earlier designs ran small games on top of her: points to catch, overlay rings and
satellites. They read as a device showing a game. Here every behaviour is a
deformation of her own 4D structure, carried by the springs that already animate
her, so the play is physics of her body: waves that follow her edges and converge
at the opposite corner, momentum that opens her, elasticity that stretches and
throws her, impact that shatters her. Each is reached by a touch people already
make (tap, swipe, pull beside her, a hard throw), and self-play shows the same
repertoire to someone who has not found it yet.

Progress counts **unique discoveries**, not play time, streaks or scores: six
visible milestones that survive reboots, cannot be ground out and never decay.
Axis scaling keeps one source of points for every growth stage; Gather reuses the
burst's slots and choreography instead of a second particle system.

For queued game taps, draining the whole reserved inbox first would reorder
ordinary KEY-down and reserved KEY-up. A shared global event order would change
both channels and their latency bounds. Bounded deferral of only owned game taps
keeps those handlers and normal input order intact, while letting every effective
preemption in their batch win over game input.

## Verification

`python3 tools/test-tess-games.py` checks the model and the production tap route
with ASan/UBSan. `tess_games_integration_test.c` (in `python3 tools/test-render.py`)
plays every behaviour through the real face, springs, burst and renderer: plucks
and three overlapping waves, echo, bloom from swipes, twang, slingshot bounce,
catch, shatter and a full gather, the ordinary burst, drag rotation, the whole
self-play repertoire, priority stops and the dot budget. Runtime eligibility and
NVS storage are covered by `python3 tools/test-state.py` and
`python3 tools/test-settings.py`; all run in `python3 tools/accept.py host`.
`python3 tools/accept.py build` builds both TESS and PLUSH firmware.

Device acceptance runs `node tools/test-device-tess-games.mjs` through the USB
bridge; `sim:gravity` (each axis −1..1, at most 10 s) replaces the sensor's tilt
while it lasts. It covers all six Lab previews, then on the home screen a pluck, two overlapping
plucks, a run of swipes to bloom, a twang, a slingshot into the rim, Settings
preempting a pull, a hard throw that shatters her followed by a full sweep, a
tilt that rolls her into the rim and home, a burst poured home by tilting, and
rapid pokes that annoy her, with the body tones they emitted.
It never resets user settings and checks that previews do not change the saved
mask. USB `sim:pointer` samples expire after 350 ms; a lift shorter than a display
frame is not seen, so injected strokes lift for 150 ms like a finger does.
`sim:game-state` reports `game`, `phase`, `streak`, `waves`, `bloom`, `tones`
(one bit per tone kind), `spin`, `tilt_x/y`, `excite`, `annoy`, `fear`, `fond`,
`pull`, `speed`, `squash`, `self`, `shaping`, `burst`, `form`, her centre and the
saved mask. These verify execution on the ESP32, not finger feel, sound or the
look of the panel.

For physical acceptance, tap her and then two places quickly; swipe her several
times; press beside her and pull a little, then far; tap her in flight; pull to
the very edge and sweep the burst home; leave her alone for half a minute. Check
KEY, reply, offline, menu and dark preemption, then restore user settings.
