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
}

static void vis_register (const visualizer_t *vis) {
    if (vis_count >= MAX_VISUALIZERS) return;
    visualizers[vis_count++] = vis;
    if (vis->init) vis->init();
}

int vis_get_count (void) { return vis_count; }

void vis_next (void) {
    if (vis_count <= 1) return;
    int index = (current_vis + 1) % vis_count;
    if (index == current_vis || transitioning) return;
    next_vis       = index;
    trans_progress = 0.0f;
    transitioning  = true;
}

void vis_prev (void) {
    if (vis_count <= 1) return;
    int index = (current_vis - 1 + vis_count) % vis_count;
    if (index == current_vis || transitioning) return;
    next_vis       = index;
    trans_progress = 0.0f;
    transitioning  = true;
}

void vis_update (const vis_audio_t *audio) {
    if (vis_count == 0) return;

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

/* --- Auto-registration via linker section -------------------------------- */

/* These symbols bracket the vis_registry section.
 * Declared as arrays (not scalars) to prevent MIPS GP-relative addressing. */
extern const visualizer_t * const __start_vis_registry[];
extern const visualizer_t * const __stop_vis_registry[];

void vis_register_all (void) {
    const visualizer_t * const *p = __start_vis_registry;
    while (p < __stop_vis_registry) {
        vis_register(*p);
        p++;
    }
}
