#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "input_motion_target.h"
#include "viewpoint.h"

static bool s_imu = true;
static uint8_t registers[12];
static bool imu_rd(uint8_t reg, uint8_t *out, size_t n) {
    assert(reg == 0x35 && n == sizeof registers);
    memcpy(out, registers, n);
    return true;
}
/* PRODUCTION_IMU_READ */

typedef struct {
    float gravity[3], gyro_bias[3];
    bool gravity_initialized;
    int sample_ms;
} input_state_t;
typedef struct {
    float acceleration[3], rotation_rate[3];
    float dynamic, rotation;
    bool still;
} motion_sample_t;
static float s_grav_x, s_grav_y;
static int32_t s_jolt_x, s_jolt_y;
/* PRODUCTION_PULL_SIGNS */
/* PRODUCTION_FUSION */

static void near(float actual, float expected) { assert(fabsf(actual - expected) < 1e-6f); }
static void same_vector(const float actual[3], const float expected[3]) {
    for (int i = 0; i < 3; ++i) near(actual[i], expected[i]);
}
static float dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void known_board_axes(void) {
    float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i) input_motion_to_screen(axes[i]);
#if CONFIG_IDF_TARGET_ESP32S3
    same_vector(axes[0], (float[]){0, 1, 0});
    same_vector(axes[1], (float[]){-1, 0, 0});
#else
    same_vector(axes[0], (float[]){1, 0, 0});
    same_vector(axes[1], (float[]){0, 1, 0});
#endif
    same_vector(axes[2], (float[]){0, 0, 1});
    float handed[3]; cross(axes[0], axes[1], handed);
    same_vector(handed, axes[2]);
}
static void rigid_rotation(void) {
    float a[3] = {.3f, -.4f, .7f}, b[3] = {-.2f, .1f, .5f};
    float product = dot(a, b), norm = dot(a, a), c[3], rotated[3];
    cross(a, b, c); memcpy(rotated, c, sizeof c);
    input_motion_to_screen(a); input_motion_to_screen(b); input_motion_to_screen(rotated);
    near(dot(a, b), product); near(dot(a, a), norm);
    cross(a, b, c); same_vector(c, rotated);
#if !CONFIG_IDF_TARGET_ESP32S3
    uint32_t bits[3] = {0x80000000u, 0x7fc01234u, 0x3f800000u};
    memcpy(a, bits, sizeof a); input_motion_to_screen(a);
    assert(!memcmp(a, bits, sizeof a));  // Preserve -0, NaN payload and every finite value.
#endif
}
static void set_axis(int offset, int16_t value) {
    registers[offset] = (uint16_t)value & 255;
    registers[offset + 1] = (uint16_t)value >> 8;
}
static void actual_sensor_reader(void) {
    set_axis(0, 8192); set_axis(2, 4096); set_axis(4, -16384);
    set_axis(6, 64); set_axis(8, -128); set_axis(10, 192);
    float a[3], w[3], raw_a[3] = {.5f, .25f, -1};
    const float dps = 3.14159265f / 180.f;
    float raw_w[3] = {dps, -2 * dps, 3 * dps};
    input_motion_to_screen(raw_a); input_motion_to_screen(raw_w);
    assert(imu_read(a, w)); same_vector(a, raw_a); same_vector(w, raw_w);
    s_imu = false; assert(!imu_read(a, w)); s_imu = true;
}
static void fusion_and_jolt_share_frame(void) {
    memset(registers, 0, sizeof registers); set_axis(4, -16384);
    input_state_t s = {.sample_ms = 20}; motion_sample_t sample;
    assert(read_motion_sample(&s, &sample));
    same_vector(s.gravity, (float[]){0, 0, -1});
    set_axis(0, 4096); assert(read_motion_sample(&s, &sample));
#if CONFIG_IDF_TARGET_ESP32S3
    near(s_grav_x, 0); assert(s_grav_y > 0);
    assert(s_jolt_x == 0 && s_jolt_y > 0);
#else
    assert(s_grav_x > 0); near(s_grav_y, 0);
    assert(s_jolt_x > 0 && s_jolt_y == 0);
#endif
}
static void fusion_rotates_consistently(void) {
    input_state_t raw = {.gravity = {.2f, -.3f, -.9f}, .gravity_initialized = true, .sample_ms = 20};
    input_state_t screen = raw;
    motion_sample_t a = {.acceleration = {.21f, -.28f, -.88f}, .rotation_rate = {.2f, -.3f, .1f}};
    motion_sample_t b = a;
    input_motion_to_screen(screen.gravity);
    input_motion_to_screen(b.acceleration); input_motion_to_screen(b.rotation_rate);
    near(update_gravity(&raw, &a), update_gravity(&screen, &b));
    input_motion_to_screen(raw.gravity); same_vector(raw.gravity, screen.gravity);
}
static void angular_viewpoint_shares_frame(void) {
    viewpoint_t raw, screen; viewpoint_init(&raw); viewpoint_init(&screen);
    float rate[3] = {.2f, -.3f, .1f}, rotated[3]; memcpy(rotated, rate, sizeof rate);
    input_motion_to_screen(rotated);
    for (int i = 0; i < 20; ++i) {
        viewpoint_step(&raw, rate, false, .02f);
        viewpoint_step(&screen, rotated, false, .02f);
    }
    float vector[3] = {raw.p[1], raw.p[2], raw.p[3]};
    input_motion_to_screen(vector); same_vector(screen.p + 1, vector); near(screen.p[0], raw.p[0]);
}
int main(void) {
    known_board_axes(); rigid_rotation(); actual_sensor_reader();
    fusion_and_jolt_share_frame(); fusion_rotates_consistently(); angular_viewpoint_shares_frame();
    puts("motion target: axes, handedness, byte reader, fusion, gravity, jolts, viewpoint and C6 identity passed");
}
