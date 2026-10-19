#include "tess_sky_play.h"

// A complete touch frame owns all temporary fields. No star, source or permanent
// black hole captures the gesture; each finger always bends the passing light.
#define APPEAR_S .10f
#define RELEASE_S .25f
_Static_assert(SKY_TOUCHES == TOUCH_POINTS, "Skies must cover every panel contact");

static bool on_glass(const touch_point_t *point) {
    return point->down && point->x >= 0 && point->x < SKY_W && point->y >= 0 && point->y < SKY_W;
}

bool sky_touch(const face_t *f, tess_sky_t *k, sky_force_t *force, float dt) {
    if (!isfinite(dt) || dt < 0) dt = 0;
    bool touching = false;
    force->count = SKY_TOUCHES;
    for (unsigned i = 0; i < SKY_TOUCHES; i++) {
        const touch_point_t *point = &f->touch_in.point[i];
        sky_touch_t *hole = &k->touch[i];
        bool down = on_glass(point);
        if (down) {
            if (!hole->down || hole->id != point->id) {
                hole->shown = 0;
                hole->spin = 1;
                hole->id = point->id;
            }
            hole->x = point->x; hole->y = point->y;
            touching = true;
        }
        hole->down = down;
        hole->shown = down ? fminf(1, hole->shown + dt / APPEAR_S) : fmaxf(0, hole->shown - dt / RELEASE_S);
        force->touch[i] = *hole;
    }
    k->finger_down = touching;
    return touching;
}
