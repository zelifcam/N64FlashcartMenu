/**
 * @file vis_session.h
 * @brief Visualizer session coordinator
 * @ingroup visualizer
 *
 * Owns the full runtime pipeline: t3d lifecycle, FFT, and per-frame
 * orchestration. The visualizer runs at the menu's native 640x480 resolution —
 * no display mode switch is needed. The music player view calls into this
 * module to enter/exit visualizer mode and drive each frame.
 */

#ifndef VIS_SESSION_H__
#define VIS_SESSION_H__

#include <libdragon.h>
#include <stdbool.h>

/** @brief Callback: toggle MP3 playback (A button) */
typedef void (*vis_toggle_cb)(void);

/** @brief Callback: skip track, direction +1 or -1 */
typedef void (*vis_skip_cb)(int direction);

/** @brief Callback: seek track by signed seconds */
typedef void (*vis_seek_cb)(int seconds);

/**
 * @brief Initialize the visualizer session.
 *
 * Allocates FFT, initializes t3d, registers visualizers.
 * Call once when the music player view is ready to support visualizer mode.
 *
 * @param toggle_cb  Called when the user presses A (play/pause)
 * @param skip_cb    Called when the user presses C-up or C-down (prev/next track)
 * @param seek_cb    Called when the user holds C-left or C-right (rewind/fast-forward)
 * @param filename   Bare filename of the current track (fallback for no-metadata banner)
 */
void vis_session_init(vis_toggle_cb toggle_cb, vis_skip_cb skip_cb, vis_seek_cb seek_cb, const char *filename);

/** @brief Tear down the session and free all resources. */
void vis_session_deinit(void);

/**
 * @brief Enter visualizer mode immediately.
 *
 * Resets the frame timer and activates the session. The visualizer runs
 * at the menu's native 640x480 — no display reinit required.
 */
void vis_session_enter(void);

/** @brief Exit visualizer mode immediately. */
void vis_session_exit(void);

/** @brief Returns true while in visualizer mode. */
bool vis_session_is_active(void);

/**
 * @brief Process input for the visualizer.
 *
 * Call each frame while vis_session_is_active().
 * B/Z=exit, A=play/pause, C-up/down=skip track, C-left/right=seek, D-pad=switch visualizer.
 */
void vis_session_process(void);

/**
 * @brief Render one visualizer frame.
 *
 * Runs FFT on latest PCM, builds vis_audio_t, calls vis_update + vis_render.
 * Attaches and detaches the display surface internally.
 *
 * @param display  Current display surface
 */
void vis_session_frame(surface_t *display);

/** @brief Inform the session whether audio is currently paused. */
void vis_session_set_paused(bool paused);

/**
 * @brief Notify the session that the current track has changed.
 *
 * Triggers an auto-show of the track info banner: fades in, holds for
 * ~6 seconds, then fades out. Call this from the music player whenever
 * the track advances (auto-advance or manual skip).
 *
 * @param filename  Bare filename (no path) — used as fallback when no ID3 metadata.
 */
void vis_session_notify_track_changed(const char *filename);

#endif /* VIS_SESSION_H__ */
