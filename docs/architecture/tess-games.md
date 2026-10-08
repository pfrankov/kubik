# Native Tess games and growth architecture

## Runtime contract

`tess_games.c` owns a bounded per-face state machine with two games, three tiers
each, and six durable discovery bits. There is no game screen or dedicated
button route. Ordinary taps invite Echo: three taps within 45 panel pixels and
0.16–0.65 seconds apart create a pending start, then a 1.4-second quiet pause
starts the next unlearned Echo tier. A continuous closed circle invites Catch.
It is sampled from the cursor coordinates available on update frames around
(240, 255), with radius 55–175 px, 0.6–4 seconds, at least 5.5 radians of sweep,
and a final point less than 65 px from the start. Either direction is valid.
One sampled angular step may be as large as 1.6 radians, tolerating a skipped
render interval without interpolating or queueing a synthetic cursor path; a
reversal over 0.12 radians invalidates the circle. Catch starts after the
remaining quiet pause (about 0.6 seconds after release).

Echo presents 2, 3 or 4 pulses and accepts the matching intervals within
±0.24 seconds. Catch follows the rendered cloud center, not a separately
estimated or flattened target. It accepts taps within 44 px, at least 0.6
seconds apart, after the target has moved at least 48 px; the tiers require 3,
4 or 5 hits. Echo accepts answers starting 0.6 seconds after its last sample
pulse, with a 5-second first-answer timeout; Catch is 6 seconds. Every round
has a 20-second total limit, and a successful round celebrates for at most
3 seconds before returning to ordinary Tess.

Each game and tier maps to one bit: Echo bits 0–2, Catch bits 3–5. The bit is
set only on a successful, previously unearned tier. Population count selects
form: 0 is a point, 1–2 a square, 3–5 a cube, and 6 a tesseract. This is a finite
progression with no streak, decay or repeat reward. A discovered trick may play
after 40 seconds of quiet idle and yields immediately to normal touch. Games
are invited by deliberate touch only; no background event forces a new game.

Eligibility is checked at both app and face level. Games require the Tess build,
an available connected session, an awake visible home face in idle mode, and no
active capture, Live turn, agent work, speech, text card, menu or Setup. Priority
input/state changes leave the game; the state machine never resumes it.
Wrong answers and timeouts end silently, with no penalty or failure sound. A
pet exits into the existing pet response and pet sound. Game taps while a game
is active are consumed locally and do not become agent tap pokes; the three
invitation taps remain ordinary taps until the game starts. Recording retains
the existing sphere; thinking and speech temporarily target the full tesseract.

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
The Echo and Catch bit ranges keep the six discoveries independently verifiable.

For geometry, axis scaling reuses Tess's 4D vertex set and renderer while
preserving the topology as each dimension opens. A separate shape renderer
would introduce competing point layouts; flattening the existing tesseract in
2D would lose the staged square-to-cube-to-tesseract structure. Axis growth
keeps one source of points and one adult path.

Games start only after an intentional Echo tap sequence or circle. Randomly
starting a game while the user is idle would interrupt ordinary character use.

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
and PLUSH firmware. Run both levels for a code change; `device` acceptance does
not currently automate game play.

On a physical TESS device, verify the actual touch route: make three close taps
at 0.3 seconds apart, wait 1.4 seconds and repeat Echo's shown intervals; repeat
until 2/3/4-pulse tiers are each completed. Draw Catch in both directions at the
specified center/radius/duration, then catch the moving cloud center 3/4/5 times
with the stated spacing. Check the four growth forms, and verify earned form
after a normal reboot. On a test unit, update from a firmware/NVS state with no
`tess_progress` key and confirm the point form appears while network and user
settings remain. Check wrong input, idle timeout, hold/pet exit, and KEY, voice,
text/speech, offline, Setup, menu and dark-screen preemption; none resumes an
interrupted game or awards a bit. These physical steps supplement host tests;
they are not implied by native rendering or build results.
