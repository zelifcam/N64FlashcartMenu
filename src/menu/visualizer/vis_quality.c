/**
 * @file vis_quality.c
 * @brief Adaptive quality scaling system
 * @ingroup visualizer
 *
 * Monitors frame time and provides a quality scale factor (0.2 – 1.5).
 * Reacts quickly to slowdowns and recovers slowly to avoid oscillation.
 */

#include <libdragon.h>
#include "vis_quality.h"

#define FRAME_TIME_TARGET   0.0333f  /* 30 fps = 33.3 ms */
#define SLOW_THRESHOLD      0.002f   /* 2 ms over budget — start scaling down */
#define FAST_THRESHOLD      0.004f   /* 4 ms under budget — recover cautiously */
#define MIN_QUALITY         0.2f
#define MAX_QUALITY         1.5f

static uint32_t last_ticks  = 0;
static float    frame_avg   = 0.033f;
static float    scale       = 1.0f;
static bool     forced      = false;

void vis_quality_init (void) {
    last_ticks  = 0;
    frame_avg   = 0.033f;
    scale       = 1.0f;
    forced      = false;
}

void vis_quality_update (void) {
    uint32_t now = TICKS_READ();
    if (last_ticks != 0) {
        float instant = TICKS_DISTANCE(last_ticks, now) / (float)TICKS_PER_SECOND;
        /* ~8 frame smoothing window */
        frame_avg = frame_avg * 0.85f + instant * 0.15f;

        if (!forced) {
            float error = frame_avg - FRAME_TIME_TARGET;
            if (error > SLOW_THRESHOLD) {
                float urgency = error / FRAME_TIME_TARGET;
                scale -= 0.02f + urgency * 0.08f;
            } else if (error < -FAST_THRESHOLD) {
                scale += 0.003f;
            }
            if (scale < MIN_QUALITY) scale = MIN_QUALITY;
            if (scale > MAX_QUALITY) scale = MAX_QUALITY;
        }
    }
    last_ticks = now;
}

quality_level_t vis_quality_get_level (void) {
    if (scale >= 1.0f) return QUALITY_MAX;
    if (scale >= 0.7f) return QUALITY_HIGH;
    if (scale >= 0.4f) return QUALITY_MEDIUM;
    return QUALITY_LOW;
}

float vis_quality_get_scale (void) { return scale; }

int vis_quality_scale_int (int base) {
    int v = (int)(base * scale);
    return v < 1 ? 1 : v;
}

bool vis_quality_should_skip (void) { return false; }

float vis_quality_get_frame_time (void) { return frame_avg; }

void vis_quality_force_up (void) {
    forced = true;
    scale += 0.15f;
    if (scale > MAX_QUALITY) scale = MAX_QUALITY;
}

void vis_quality_force_down (void) {
    forced = true;
    scale -= 0.15f;
    if (scale < MIN_QUALITY) scale = MIN_QUALITY;
}

void vis_quality_force_reset (void) { forced = false; }

bool vis_quality_is_forced (void) { return forced; }
