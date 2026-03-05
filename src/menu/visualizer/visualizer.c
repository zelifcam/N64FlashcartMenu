/**
 * @file visualizer.c
 * @brief Visualizer plugin framework implementation
 * @ingroup visualizer
 *
 * Manages multiple visualizers with dip-to-black transitions.
 * Only one visualizer renders at a time — no double render pass.
 */

#include <string.h>
#include <libdragon.h>
#include "visualizer.h"
#include "vis_quality.h"

#define MAX_VISUALIZERS     32
#define TRANSITION_TIME     0.3f    /* seconds for full dip-to-black */

static const visualizer_t *visualizers[MAX_VISUALIZERS];
static int   vis_count      = 0;
static int   current_vis    = 0;
static int   next_vis       = -1;
static float trans_progress = 0.0f;  /* 0.0 = start, 1.0 = done */
static bool  transitioning  = false;

void vis_init (void) {
    vis_count      = 0;
    current_vis    = 0;
    next_vis       = -1;
    trans_progress = 0.0f;
    transitioning  = false;
    vis_quality_init();
}

void vis_register (const visualizer_t *vis) {
    if (vis_count >= MAX_VISUALIZERS) return;
    visualizers[vis_count++] = vis;
    if (vis->init) vis->init();
}

int         vis_get_count   (void)        { return vis_count; }
int         vis_get_current (void)        { return current_vis; }
const char *vis_get_name    (int index)   {
    if (index < 0 || index >= vis_count) return "Unknown";
    return visualizers[index]->name;
}
bool        vis_is_transitioning (void)   { return transitioning; }

void vis_next (void) {
    if (vis_count <= 1) return;
    vis_switch_to((current_vis + 1) % vis_count);
}

void vis_prev (void) {
    if (vis_count <= 1) return;
    vis_switch_to((current_vis - 1 + vis_count) % vis_count);
}

void vis_switch_to (int index) {
    if (index < 0 || index >= vis_count) return;
    if (index == current_vis || transitioning) return;
    next_vis       = index;
    trans_progress = 0.0f;
    transitioning  = true;
}

void vis_set_start (int index) {
    if (index >= 0 && index < vis_count) current_vis = index;
}

int vis_find_by_name (const char *name) {
    if (!name) return -1;
    for (int i = 0; i < vis_count; i++) {
        if (visualizers[i]->name && strcmp(visualizers[i]->name, name) == 0)
            return i;
    }
    return -1;
}

void vis_update (const vis_audio_t *audio) {
    if (vis_count == 0) return;
    vis_quality_update();

    if (visualizers[current_vis]->update)
        visualizers[current_vis]->update(audio);

    /* Advance transition timer */
    if (transitioning && !audio->paused) {
        trans_progress += audio->dt / TRANSITION_TIME;
        if (trans_progress >= 1.0f) {
            /* Swap complete */
            if (visualizers[current_vis]->cleanup)
                visualizers[current_vis]->cleanup();
            if (visualizers[next_vis]->init)
                visualizers[next_vis]->init();
            current_vis    = next_vis;
            next_vis       = -1;
            trans_progress = 0.0f;
            transitioning  = false;
        }
    }
}

void vis_render (const vis_audio_t *audio) {
    if (vis_count == 0) return;
    if (vis_quality_should_skip()) return;

    /* Always render the current visualizer */
    if (visualizers[current_vis]->render)
        visualizers[current_vis]->render(audio);

    if (transitioning) {
        /*
         * Dip-to-black transition:
         *   0.0 – 0.5: black fades IN over current vis  (alpha 0 → 255)
         *   0.5 – 1.0: black fades OUT to reveal next   (alpha 255 → 0)
         *
         * The actual visualizer swap happens at progress == 1.0 in vis_update().
         * During the second half we are still rendering current_vis, which gives
         * the illusion of the new one appearing from black.
         */
        float t;
        if (trans_progress < 0.5f) {
            t = trans_progress * 2.0f;           /* 0 → 1 */
        } else {
            t = (1.0f - trans_progress) * 2.0f;  /* 1 → 0 */
        }
        uint8_t alpha = (uint8_t)(t * 255.0f);

        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(0, 0, 0, alpha));
        rdpq_fill_rectangle(0, 0, display_get_width(), display_get_height());
    }
}

/* --- Built-in visualizer registration ------------------------------------ */

/* Forward declarations — add new visualizers here */
/* extern const visualizer_t vis_bars; */

void vis_register_all (void) {
    /* Visualizers will be registered here as they are implemented */
}
