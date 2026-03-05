/**
 * @file visualizer.h
 * @brief Visualizer plugin framework
 * @ingroup visualizer
 *
 * Manages multiple visualizers with dip-to-black transitions between them.
 * Each visualizer registers init/update/render/cleanup callbacks.
 */

#ifndef VISUALIZER_H__
#define VISUALIZER_H__

#include <stdbool.h>
#include <stdint.h>
#include "fft.h"

/*===========================================================================
 * Audio data passed to visualizers each frame
 *===========================================================================*/

typedef struct {
    float bass, mid, treb;           /* Smoothed frequency levels (0.0 – 1.0) */
    float bands[FFT_NUM_BANDS];      /* Per-band spectrum (0.0 – 1.0) */
    bool  beat;                      /* Beat detected this frame */
    float beat_intensity;            /* Beat strength (0.0 – 1.0) */
    int16_t *waveform;               /* Raw PCM samples (may be NULL) */
    int      waveform_len;
    float time;                      /* Accumulated time (pauses when paused) */
    float dt;                        /* Delta time this frame */
    bool  paused;
    uint32_t buttons_pressed;
    uint32_t buttons_held;
} vis_audio_t;

/*===========================================================================
 * Visualizer interface
 *===========================================================================*/

typedef struct {
    const char *name;
    void (*init)(void);
    void (*update)(const vis_audio_t *audio);
    void (*render)(const vis_audio_t *audio);
    void (*cleanup)(void);
} visualizer_t;

/*===========================================================================
 * Auto-registration macro
 *
 * Place VIS_REGISTER(ptr) at file scope in any visualizer .c file.
 * vis_register_all() will discover and register it automatically —
 * no manual includes or registration calls needed.
 *===========================================================================*/

#define VIS_REGISTER(vis_ptr) \
    __attribute__((used, section("vis_registry"))) \
    static const visualizer_t * const _vis_entry_##vis_ptr = &(vis_ptr)

/*===========================================================================
 * Framework API
 *===========================================================================*/

void vis_init(void);
int  vis_get_count(void);
void vis_next(void);
void vis_prev(void);

/**
 * @brief Update framework state — call once per frame.
 */
void vis_update(const vis_audio_t *audio);

/**
 * @brief Render current visualizer with dip-to-black transitions.
 *
 * Transition: fade to black over ~0.3 s, swap visualizer at midpoint,
 * fade back in. Only one visualizer renders at a time.
 */
void vis_render(const vis_audio_t *audio);

/**
 * @brief Register all built-in visualizers. Call once after vis_init().
 */
void vis_register_all(void);

#endif /* VISUALIZER_H__ */
