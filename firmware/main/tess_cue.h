// What Tess's behaviour asks the speaker for. The face (portable, also run by the simulator) only names the cue;
// the app takes them once a frame (face_take_cue) and hands them to audio_tess_cue (tess_sound.c).
#pragma once

typedef enum {
    TC_TOUCH,    // a finger lands on the cloud (strength 0, position: which side)
    TC_FLING,    // a swipe flings its spin (strength: how hard, 0..1; position: the way it goes)
    TC_SWING,    // it swings back for more
    TC_EXCITE,   // taps in a run build up (strength: 0..1 with the count)
    TC_RUB,      // the rubbing reached a stage (strength: stage 1..4 over 4)
    TC_GLANCE,   // it looks at what touched it
    TC_PEEK,     // a quarter turn through the 4th dimension
    TC_INVITE,   // it has been ignored a while and asks for attention
    TC_DODGE,    // it ducks away from a finger, playfully
    TC_STARTLE,  // a sudden shake or lift
    TC_CONTENT,  // the sigh after being petted
    TC_MISCHIEF, // bored: a small trick
    TC_LOVE,     // the heart: the rubbing's reward, or a loving word
    TC_JOY,      // glad: a high rub stage, or a happy word
    TC_SAD,      // a sad word
    TC_TINKLE,   // a light sparkling word
    TC_SNAP,     // a hard shake's dots are pulled home: the magnet closes on them
    // The words of the moods (tess_feel.c): played as it enters one, when the trigger has not said it already.
    TC_CURIOUS,  // something caught its attention: a rising question
    TC_PLAYFUL,  // it feels like playing: a bounce up and back
    TC_SCARED,   // frightened: a fast high flutter
    TC_GRUMPY,   // fed up: a low "hm-hm"
    TC_LONELY,   // ignored: a sparse bell, falling
    TC_SETTLE,   // back to calm after a strong mood: a barely audible sigh
    TC_IMPACT,   // a real fallen-particle collision; position encodes its persistent tone
    TC_COUNT
} tess_cue_t;
