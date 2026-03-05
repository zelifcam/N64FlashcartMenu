/**
 * @file vis_session.c
 * @brief Visualizer session coordinator
 * @ingroup visualizer
 *
 * Owns the t3d lifecycle, FFT pipeline, and per-frame orchestration.
 * The visualizer runs at the menu's native 640x480 resolution — no
 * display mode switch is needed.
 */

#include <libdragon.h>
#include <t3d/t3d.h>

#include "vis_session.h"
#include "vis_audio.h"
#include "fft.h"
#include "visualizer.h"
#include "vis_quality.h"

/* PCM ping-pong buffer — written by the MP3 decoder, read here */
vis_pcm_buffer_t vis_pcm = {0};

/* Session state */
static bool            active     = false;
static bool            paused     = false;
static float           time_acc   = 0.0f;
static uint32_t        last_ticks = 0;

/* Callbacks into the music player */
static vis_toggle_cb   cb_toggle  = NULL;
static vis_skip_cb     cb_skip    = NULL;

/* PCM read side — swapped each frame */
static int pcm_read_idx   = 0;
static int pcm_read_count = 0;

/*===========================================================================
 * Internal helpers
 *===========================================================================*/

static float get_dt (void) {
    uint32_t now = TICKS_READ();
    float dt = 0.0f;
    if (last_ticks != 0)
        dt = TICKS_DISTANCE(last_ticks, now) / (float)TICKS_PER_SECOND;
    last_ticks = now;
    if (dt > 0.1f) dt = 0.1f;  /* clamp runaway frames */
    return dt;
}

static void process_fft (float *bands_out) {
    if (vis_pcm.new_data) {
        pcm_read_idx   = vis_pcm.write_idx;
        vis_pcm.write_idx ^= 1;
        pcm_read_count = vis_pcm.sample_count;
        vis_pcm.new_data = false;
        fft_process(vis_pcm.buffers[pcm_read_idx], pcm_read_count, vis_pcm.channels);
    }
    fft_get_bands(bands_out);
}

static void build_vis_audio (vis_audio_t *a, float dt) {
    process_fft(a->bands);
    fft_get_levels(&a->bass, &a->mid, &a->treb);
    fft_get_beat(&a->beat, &a->beat_intensity);
    a->waveform     = vis_pcm.buffers[pcm_read_idx];
    a->waveform_len = pcm_read_count / (vis_pcm.channels > 0 ? vis_pcm.channels : 1);
    a->paused       = paused;
    a->dt           = paused ? 0.0f : dt;
    if (!paused) time_acc += dt;
    a->time         = time_acc;

    /* Raw controller state for interactive visualizers */
    joypad_buttons_t pressed = {0}, held = {0};
    JOYPAD_PORT_FOREACH (i) {
        pressed = joypad_get_buttons_pressed(i);
        held    = joypad_get_buttons_held(i);
        if (pressed.raw || held.raw) break;
    }
    a->buttons_pressed = pressed.raw;
    a->buttons_held    = held.raw;
}

/*===========================================================================
 * Public API
 *===========================================================================*/

void vis_session_init (vis_toggle_cb toggle_cb, vis_skip_cb skip_cb) {
    cb_toggle = toggle_cb;
    cb_skip   = skip_cb;

    fft_init();
    t3d_init((T3DInitParams){});
    vis_init();
    vis_register_all();

    vis_pcm.enabled   = true;
    vis_pcm.write_idx = 0;
    vis_pcm.new_data  = false;
    time_acc          = 0.0f;
    last_ticks        = 0;
    paused            = false;
}

void vis_session_deinit (void) {
    vis_pcm.enabled = false;
    active = false;
    fft_cleanup();
    t3d_destroy();
}

void vis_session_enter (void) {
    active     = true;
    last_ticks = 0;
    time_acc   = 0.0f;
}

void vis_session_exit (void) {
    active = false;
}

bool vis_session_is_active (void) {
    return active;
}

void vis_session_set_paused (bool is_paused) {
    paused = is_paused;
}

void vis_session_process (void) {
    joypad_buttons_t pressed = {0};
    JOYPAD_PORT_FOREACH (i) {
        pressed = joypad_get_buttons_pressed(i);
        if (pressed.raw) break;
    }

    if (pressed.b || pressed.z) {
        active = false;
        return;
    }
    if (pressed.a && cb_toggle) {
        cb_toggle();
    }
    if (pressed.c_left  && cb_skip) cb_skip(-1);
    if (pressed.c_right && cb_skip) cb_skip(+1);
    if (pressed.d_left  || pressed.d_up)    vis_prev();
    if (pressed.d_right || pressed.d_down)  vis_next();
}

void vis_session_frame (surface_t *display) {
    float dt = get_dt();

    vis_audio_t audio = {0};
    build_vis_audio(&audio, dt);
    vis_update(&audio);

    rdpq_attach(display, NULL);

    if (vis_get_count() > 0) {
        t3d_frame_start();
        rdpq_mode_zbuf(false, false);
        vis_render(&audio);
    } else {
        rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
        rdpq_fill_rectangle(0, 0, display_get_width(), display_get_height());
    }

    rdpq_detach_show();
}
