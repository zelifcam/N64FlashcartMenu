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
#include "../fonts.h"
#include "../ui_components/constants.h"
#include "../mp3_player.h"

/* Provided by music_player.c — returns the currently loaded cover art surface */
surface_t *music_player_get_cover_art(void);

/* PCM ping-pong buffer — written by the MP3 decoder, read here */
vis_pcm_buffer_t vis_pcm = {0};

/* Session state */
static bool            active        = false;
static bool            paused        = false;
static bool            skip_one_frame = false; /* ignore buttons on enter frame */
static bool            show_help     = false;   /* keybind overlay toggle */
static bool            show_fps      = false;   /* FPS counter toggle */
static rspq_block_t   *help_block    = NULL;    /* pre-recorded overlay draw commands */
static rspq_block_t   *fps_block     = NULL;    /* pre-recorded FPS text draw commands */
static int             fps_frame_count = 0;
static float           fps_time_acc    = 0.0f;
static int             fps_display     = 0;
static bool            show_banner   = false;   /* manual banner toggle (L button) */
static float           time_acc   = 0.0f;
static uint32_t        last_ticks = 0;

/* Banner fade state — driven by auto-show on track change and manual L toggle */
#define BANNER_FADE_IN_TIME   0.4f   /* seconds to fully appear */
#define BANNER_HOLD_TIME      6.0f   /* seconds at full opacity (auto-show) */
#define BANNER_FADE_OUT_TIME  0.6f   /* seconds to fully disappear */
static float           banner_alpha      = 0.0f;  /* 0=invisible, 1=fully visible */
static float           banner_auto_timer = 0.0f;  /* countdown for auto-show hold+fade */
static bool            banner_auto_active = false; /* auto-show in progress */
static surface_t      *banner_thumb      = NULL;   /* pre-scaled cover art thumbnail */
static bool            banner_thumb_dirty = false; /* rebuild thumbnail next frame */
static char            banner_filename[256] = "";  /* fallback filename for no-metadata tracks */

/* Seek ramp — mirrors the music player's progressive seek logic */
#define SEEK_SECONDS_MIN        2
#define SEEK_SECONDS_MAX        60
#define SEEK_RAMP_TICKS         120     /* ticks held before reaching max seek rate */
#define SEEK_MAX_DURATION_FRAC  0.10f   /* max seek = 10% of song duration */

/* Callbacks into the music player */
static vis_toggle_cb   cb_toggle  = NULL;
static vis_skip_cb     cb_skip    = NULL;
static vis_seek_cb     cb_seek    = NULL;

/* Seek hold state */
static int  seek_hold_ticks = 0;
static bool seek_inhibit    = false;

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

void vis_session_init (vis_toggle_cb toggle_cb, vis_skip_cb skip_cb, vis_seek_cb seek_cb, const char *filename) {
    cb_toggle = toggle_cb;
    cb_skip   = skip_cb;
    cb_seek   = seek_cb;
    if (filename && filename[0])
        snprintf(banner_filename, sizeof(banner_filename), "%s", filename);
    else
        banner_filename[0] = '\0';

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

    debugf("[VIS] session_init done, vis_count=%d\n", vis_get_count());
}

void vis_session_deinit (void) {
    vis_pcm.enabled = false;
    active          = false;
    show_banner     = false;
    banner_alpha    = 0.0f;
    banner_auto_active = false;
    banner_auto_timer  = 0.0f;
    banner_filename[0] = '\0';
    if (help_block)   { rspq_block_free(help_block); help_block = NULL; }
    if (fps_block)    { rspq_block_free(fps_block);  fps_block  = NULL; }
    if (banner_thumb) { surface_free(banner_thumb); free(banner_thumb); banner_thumb = NULL; }
    fft_cleanup();
    t3d_destroy();
}

void vis_session_enter (void) {
    debugf("[VIS] enter (vis_count=%d)\n", vis_get_count());
    active             = true;
    skip_one_frame     = true;
    show_help          = false;
    show_fps           = false;
    show_banner        = false;
    banner_alpha       = 0.0f;
    banner_auto_active = true;
    banner_auto_timer  = BANNER_HOLD_TIME + BANNER_FADE_OUT_TIME;
    if (help_block)   { rspq_block_free(help_block); help_block = NULL; }
    if (fps_block)    { rspq_block_free(fps_block);  fps_block  = NULL; }
    if (banner_thumb) { surface_free(banner_thumb); free(banner_thumb); banner_thumb = NULL; }
    banner_thumb_dirty = true;  /* build thumbnail from current cover art on first frame */
    fps_frame_count = 0;
    fps_time_acc    = 0.0f;
    fps_display     = 0;
    last_ticks      = 0;
    time_acc        = 0.0f;
}

void vis_session_exit (void) {
    debugf("[VIS] exit\n");
    active = false;
}

bool vis_session_is_active (void) {
    return active;
}

void vis_session_set_paused (bool is_paused) {
    paused = is_paused;
}

void vis_session_notify_track_changed (const char *filename) {
    /* Trigger auto-show: fade in, hold for BANNER_HOLD_TIME, then fade out */
    banner_auto_active = true;
    banner_auto_timer  = BANNER_HOLD_TIME + BANNER_FADE_OUT_TIME;
    /* Thumbnail is stale — rebuild at start of next frame */
    banner_thumb_dirty = true;
    if (filename && filename[0]) {
        snprintf(banner_filename, sizeof(banner_filename), "%s", filename);
    } else {
        banner_filename[0] = '\0';
    }
    /* Prevent accidental seek on the frame after a track change */
    seek_hold_ticks = 0;
    seek_inhibit    = true;
    debugf("[VIS] track changed — banner auto-show triggered\n");
}

void vis_session_process (void) {
    if (skip_one_frame) {
        skip_one_frame = false;
        return;
    }

    joypad_buttons_t pressed = {0}, held = {0};
    JOYPAD_PORT_FOREACH (i) {
        pressed = joypad_get_buttons_pressed(i);
        held    = joypad_get_buttons_held(i);
        if (pressed.raw || held.raw) break;
    }

    if (pressed.start) {
        show_help = !show_help;
        return;
    }
    if (pressed.r) {
        show_fps = !show_fps;
        return;
    }
    if (pressed.l) {
        show_banner = !show_banner;
        if (show_banner) {
            /* Fade in immediately when manually shown */
            banner_auto_active = false;
            banner_auto_timer  = 0.0f;
        }
        return;
    }
    if (pressed.b || pressed.z) {
        active = false;
        return;
    }
    if (pressed.a && cb_toggle) {
        cb_toggle();
    }

    /* C-up / C-down: prev / next track */
    if (pressed.c_up   && cb_skip) cb_skip(-1);
    if (pressed.c_down && cb_skip) cb_skip(+1);

    /* C-left / C-right: seek (held, progressive ramp — mirrors music player) */
    bool seeking = (held.c_left || held.c_right) && !seek_inhibit;
    if (seeking && cb_seek) {
        if (seek_hold_ticks < SEEK_RAMP_TICKS) seek_hold_ticks++;
        float duration = mp3player_get_duration();
        float seek_max = duration * SEEK_MAX_DURATION_FRAC;
        if (seek_max > SEEK_SECONDS_MAX) seek_max = SEEK_SECONDS_MAX;
        if (seek_max < SEEK_SECONDS_MIN) seek_max = SEEK_SECONDS_MIN;
        float t = seek_hold_ticks / (float)SEEK_RAMP_TICKS;
        int seconds = (int)(SEEK_SECONDS_MIN + t * (seek_max - SEEK_SECONDS_MIN));
        cb_seek(held.c_left ? -seconds : seconds);
    } else if (!held.c_left && !held.c_right) {
        seek_hold_ticks = 0;
        seek_inhibit    = false;
    }

    if (pressed.d_left)  vis_prev();
    if (pressed.d_right) vis_next();
}

/*---------------------------------------------------------------------------
 * Banner helpers
 *---------------------------------------------------------------------------*/

/* Build (or rebuild) the pre-scaled banner thumbnail from current cover art.
 * Called once per track change, before rdpq_attach. */
static void rebuild_banner_thumb (void) {
    if (banner_thumb) {
        surface_free(banner_thumb);
        free(banner_thumb);
        banner_thumb = NULL;
    }

    surface_t *cover = music_player_get_cover_art();
    if (!cover) {
        /* Cover art still loading — retry next frame */
        banner_thumb_dirty = true;
        return;
    }

    int bh    = DISPLAY_HEIGHT / 3;
    int thumb = bh - 16;   /* must match draw_banner() */

    banner_thumb = malloc(sizeof(surface_t));
    if (!banner_thumb) return;
    *banner_thumb = surface_alloc(FMT_RGBA16, thumb, thumb);
    if (!banner_thumb->buffer) { free(banner_thumb); banner_thumb = NULL; return; }

    int crop = (cover->width < cover->height) ? cover->width : cover->height;
    int cs0  = (cover->width  - crop) / 2;
    int ct0  = (cover->height - crop) / 2;
    float scale = (float)thumb / crop;

    rdpq_attach(banner_thumb, NULL);
    rdpq_set_mode_standard();
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_tex_blit(cover, 0, 0, &(rdpq_blitparms_t){
        .s0 = cs0, .t0 = ct0,
        .width = crop, .height = crop,
        .scale_x = scale, .scale_y = scale,
        .filtering = true,
    });
    rdpq_detach_wait();  /* flush RDP before we use the surface as a source */
}

/* Update banner_alpha toward its target based on show_banner + auto-timer */
static void update_banner_alpha (float dt) {
    float target = 0.0f;

    if (show_banner) {
        target = 1.0f;          /* manually shown — stay fully visible */
    } else if (banner_auto_active) {
        /* Auto-show: hold at 1.0 until timer runs out, then fade to 0 */
        banner_auto_timer -= dt;
        if (banner_auto_timer > BANNER_FADE_OUT_TIME) {
            target = 1.0f;
        } else if (banner_auto_timer > 0.0f) {
            target = banner_auto_timer / BANNER_FADE_OUT_TIME;
        } else {
            banner_auto_active = false;
            target = 0.0f;
        }
    }

    /* Lerp alpha toward target */
    float rate = (target > banner_alpha) ? (1.0f / BANNER_FADE_IN_TIME)
                                         : (1.0f / BANNER_FADE_OUT_TIME);
    float delta = target - banner_alpha;
    if (delta > 0.0f) {
        banner_alpha += rate * dt;
        if (banner_alpha > target) banner_alpha = target;
    } else if (delta < 0.0f) {
        banner_alpha -= rate * dt;
        if (banner_alpha < target) banner_alpha = target;
    }
}

/* Draw banner at current banner_alpha (called after rdpq_attach, inside frame) */
static void draw_banner (void) {
    if (banner_alpha <= 0.0f) return;

    int bh    = DISPLAY_HEIGHT / 3;                  /* ~160px — tall enough for 3 lines */
    int by    = DISPLAY_HEIGHT - bh - OVERSCAN_HEIGHT;
    int bx    = OVERSCAN_WIDTH;
    int bw    = DISPLAY_WIDTH - OVERSCAN_WIDTH * 2;
    int thumb = bh - 16;                             /* 8px padding top + bottom */

    uint8_t panel_a = (uint8_t)(160 * banner_alpha);

    const mp3_metadata_t *meta = mp3player_get_metadata();

    /* Semi-transparent dark panel — full available width */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(RGBA32(0, 0, 0, panel_a));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_fill_rectangle(bx, by, bx + bw, by + bh);

    uint8_t content_a = (uint8_t)(255 * banner_alpha);

    /* Cover art thumbnail — blit pre-scaled surface with alpha fade */
    if (banner_thumb) {
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0,0,PRIM,0), (TEX0,0,PRIM,0)));
        rdpq_set_prim_color(RGBA32(255, 255, 255, content_a));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_tex_blit(banner_thumb, bx + 8, by + 8, NULL);
    }

    /* Metadata text */
    {
        int tx = bx + (banner_thumb ? thumb + 16 : 12);
        int tw = bw - (tx - bx) - 12;
        int ty = by + 8;
        int th = bh - 16;

        /* Build text: prefer metadata, fall back to filename */
        char text_buf[512];
        bool has_title  = meta && meta->has_metadata && meta->title[0];
        bool has_artist = meta && meta->has_metadata && meta->artist[0];
        bool has_album  = meta && meta->has_metadata && meta->album[0];

        if (has_title || has_artist || has_album) {
            char title_line[128];
            if (meta->track_number > 0)
                snprintf(title_line, sizeof(title_line), "%d. %s", meta->track_number, meta->title);
            else
                snprintf(title_line, sizeof(title_line), "%s", has_title ? meta->title : "");
            snprintf(text_buf, sizeof(text_buf), "%s%s%s%s%s",
                title_line,
                has_artist ? "\n" : "",
                has_artist ? meta->artist : "",
                (has_artist && has_album) ? "\n" : "",
                has_album  ? meta->album  : "");
        } else {
            /* No metadata — show bare filename passed in at track change */
            snprintf(text_buf, sizeof(text_buf), "%s",
                banner_filename[0] ? banner_filename : "Unknown");
        }

        /* Temporarily set the font style alpha so text fades with the banner */
        rdpq_font_t *fnt = (rdpq_font_t *)rdpq_text_get_font(FNT_DEFAULT);
        rdpq_font_style(fnt, STL_DEFAULT,
            &(rdpq_fontstyle_t){ .color = RGBA32(0xFF, 0xFF, 0xFF, content_a) });

        rdpq_text_printn(
            &(rdpq_textparms_t){
                .style_id = STL_DEFAULT,
                .width = tw, .height = th,
                .align = ALIGN_LEFT, .valign = VALIGN_CENTER,
                .wrap = WRAP_WORD,
                .line_spacing = -2,
            },
            FNT_DEFAULT, tx, ty, text_buf, strlen(text_buf));

        /* Restore full-opacity style */
        rdpq_font_style(fnt, STL_DEFAULT,
            &(rdpq_fontstyle_t){ .color = RGBA32(0xFF, 0xFF, 0xFF, 0xFF) });
    }
}

/*===========================================================================*/

void vis_session_frame (surface_t *display) {
    float dt = get_dt();

    vis_audio_t audio = {0};
    build_vis_audio(&audio, dt);
    vis_update(&audio);

    /* Advance banner fade state */
    update_banner_alpha(dt);

    /* FPS counter — update interval 0.5s.
     * Block building happens here, BEFORE rdpq_attach, so rspq_block_begin
     * is never called while a frame is in progress. */
    fps_frame_count++;
    fps_time_acc += dt;
    if (fps_time_acc >= 0.5f) {
        fps_display     = (int)(fps_frame_count / fps_time_acc + 0.5f);
        fps_frame_count = 0;
        fps_time_acc   -= 0.5f;
        if (show_fps) {
            heap_stats_t hs;
            sys_get_heap_stats(&hs);
            char buf[48];
            snprintf(buf, sizeof(buf), "%d fps  %dK/%dK RAM",
                     fps_display, hs.used / 1024, hs.total / 1024);
            rspq_block_begin();
            rdpq_set_mode_standard();
            rdpq_text_print(NULL, FNT_DEFAULT, DISPLAY_CENTER_X - 80, OVERSCAN_HEIGHT + 36, buf);
            rspq_block_t *new_fps = rspq_block_end();
            /* Free old block AFTER building the new one — the RSP may still
             * be executing the old block from last frame. By the time we reach
             * this point next interval, the RSP will be long done with it. */
            if (fps_block) rspq_block_free(fps_block);
            fps_block = new_fps;
        }
    }

    /* Build help block before rdpq_attach if needed */
    if (show_help && !help_block) {
        int pw = 380, ph = 230;
        int px = DISPLAY_CENTER_X - pw / 2;
        int py = DISPLAY_CENTER_Y - ph / 2;

        rspq_block_begin();

        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(RGBA32(0, 0, 0, 160));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_fill_rectangle(px, py, px + pw, py + ph);

        rdpq_set_mode_standard();
        rdpq_textparms_t cp = { .width = pw, .align = ALIGN_CENTER };
        int ty = py + 16;
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty +   0, "Visualizer Controls");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty +  24, BTN_A "  Pause / Play");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty +  44, BTN_CU " Prev Track  " BTN_CD " Next Track");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty +  64, BTN_CL " Rewind  " BTN_CR " Fast Forward");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty +  84, BTN_DL "  Prev Visualizer  " BTN_DR "  Next Visualizer");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty + 104, BTN_DU "  More Bars  " BTN_DD "  Fewer Bars");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty + 124, BTN_R "  Toggle FPS");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty + 144, BTN_L "  Track Info Banner");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty + 164, BTN_B " / " BTN_Z "  Exit Visualizer");
        rdpq_text_print(&cp, FNT_DEFAULT, px, ty + 184, BTN_START "  Hide This Menu");

        help_block = rspq_block_end();
    }

    /* Rebuild thumbnail if cover art changed — offscreen blit before main attach */
    if (banner_thumb_dirty) {
        banner_thumb_dirty = false;
        rebuild_banner_thumb();
    }

    rdpq_attach(display, NULL);  /* no zbuffer — not needed, and saves ~5ms clear at 640x480 */

    if (vis_get_count() > 0) {
        t3d_frame_start();
        /* t3d_frame_start unconditionally enables zbuf and AA — turn both off.
         * No zbuffer attached (saves depth-clear cost at 640x480).
         * AA adds RDP edge processing cost we can't afford at this fill rate. */
        rdpq_mode_zbuf(false, false);
        rdpq_mode_antialias(AA_NONE);
        t3d_screen_clear_color(RGBA32(0, 0, 0, 255));
        vis_render(&audio);
    }

    if (show_fps && fps_block) {
        rdpq_sync_pipe();
        rspq_block_run(fps_block);
    }

    /* Banner — drawn per-frame so alpha can animate */
    if (banner_alpha > 0.0f) {
        rdpq_sync_pipe();
        draw_banner();
    }

    if (show_help && help_block) {
        rdpq_sync_pipe();
        rspq_block_run(help_block);
    }

    rdpq_detach_show();

    /* Drain the RSP/RDP pipeline before returning.  sound_poll() calls
     * rspq_highpri_sync(), which forces an immediate RSP context switch.
     * If t3d triangle commands are still in-flight when that happens the
     * RDP hits a hardware bug and crashes.  rspq_wait() blocks until both
     * the RSP and RDP are idle, so the high-priority mixer runs on a clean
     * pipeline.  The cost is negligible — display_try_get() at the top of
     * the menu loop would stall here anyway waiting for a free framebuffer. */
    rspq_wait();
}
