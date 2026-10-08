# Native Tess games and growth

Tess grows as you discover six touch tricks through two small games. They run
directly on the home character: there is no game menu, title screen, HUD,
start button or exit button. The games run locally on the device and do not
need the agent to score taps or generate game content.

## Start Echo

On the awake, connected home screen while Tess is idle, tap the same area three
times. Keep each tap within 45 pixels of the previous one and leave 0.16–0.65
seconds between taps. After the third tap, pause for 1.4 seconds. Tess shows a
rhythm as two, three or four pulses. Tap to repeat it, keeping each interval
within 0.24 seconds of the interval Tess showed. Echo advances through its
three finite tiers as each one is discovered. The three invitation taps are
ordinary Tess taps until the pause ends and the game begins.

After the last pulse, wait another 0.6 seconds for the demonstration to finish.
Then start your answer within five seconds. Earlier taps are ignored. The intervals are 0.60 s
at tier 1, 0.45 / 0.75 s at tier 2, and 0.45 / 0.45 / 0.90 s at tier 3.

## Start Catch

Draw one continuous circle around the center at panel point (240, 255). Keep the
stroke 55–175 pixels from that center, take 0.6–4 seconds, travel at least 5.5
radians in either direction, and finish within 65 pixels of where you started.
Lift your finger and wait about 0.6 seconds; Tess folds into a moving point.
After the one-second opening motion, tap that point within 44 pixels. Let at least
0.6 seconds pass between catches, and let the target move at least 48 pixels
from its previous position. Catch has three finite tiers, asking for three,
four or five successful hits.

A miss ends Catch. Six seconds without an accepted catch also ends it. Early
repeated taps or taps before the point has moved far enough do not count.

## Growth and interruptions

Each of the six game-and-tier combinations can add progress once. Echo uses the
first three discoveries; Catch uses the next three. Tess grows from a point at
zero discoveries to a square at one or two, a cube at three to five, and a
tesseract at six. Replaying a mastered tier does not add progress. Progress
does not decay and has no streak, timer or repeat-grinding bonus. A full factory
reset clears it.

| Discovery | Learned visual trick |
|---|---|
| Echo, tier 1 | Two pulses |
| Echo, tier 2 | Three pulses and a small lift |
| Echo, tier 3 | Four pulses and a partial fold |
| Catch, tier 1 | A small orbit |
| Catch, tier 2 | A figure-eight motion |
| Catch, tier 3 | Fold to a point and open again |

New installations and upgrades without saved discoveries begin at a point;
existing network and voice settings remain. Wins are saved after the app has
finished active capture or agent work. Let it return to idle before powering
off; an unfinished flash write is not a confirmed durable save.

A wrong answer, an idle timeout, or a held touch ends the current game without
a game-specific failure sound, penalty or message. A pet returns to the usual
petting response and sound. Pressing KEY, starting voice capture or Live,
receiving text or speech, going offline, opening Setup or a menu, or turning the
screen fully dark gives the agent and device state priority; the game ends and
does not resume. Recording keeps its existing sphere animation. During agent
thinking or speech, the point cloud temporarily uses the complete tesseract.
If a game tap and a priority event are processed together, the tap is discarded:
it cannot earn a win, dismiss the arriving reply or stop the new recording.
An interrupted tap is never replayed into another round.

After 40 seconds of quiet idle, Tess can replay a discovered trick as a short
animation. Ordinary touch interrupts it. Game prompts are never started by a
background timer. Screen Lab has local game previews for development; its
progress is temporary and never changes the device's saved discoveries. See
[the Screen Lab guide](screen-lab.md).
