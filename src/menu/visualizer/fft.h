/**
 * @file fft.h
 * @brief FFT audio analysis for visualizer
 * @ingroup visualizer
 *
 * Performs real-time frequency analysis on audio samples using kissfft.
 * Outputs 16 frequency bands plus bass/mid/high levels for audio-reactive
 * effects, and beat detection.
 *
 * Usage:
 *   1. Call fft_init() once at startup
 *   2. Call fft_process() each frame with fresh PCM samples
 *   3. Use fft_get_levels() for bass/mid/high effect drivers
 *   4. Use fft_get_bands() for per-band bar heights
 *   5. Use fft_get_beat() for beat-triggered effects
 */

#ifndef FFT_H__
#define FFT_H__

#include <stdbool.h>
#include <stdint.h>

#define FFT_NUM_BANDS 32

void fft_init(void);
void fft_cleanup(void);

/**
 * @brief Process interleaved PCM samples and update all frequency data.
 * @param samples  Interleaved signed 16-bit PCM
 * @param len      Total sample count (frames * channels)
 * @param channels Channel count (1 = mono, 2 = stereo)
 */
void fft_process(int16_t *samples, int len, int channels);

/** @brief Get smoothed bass/mid/high energy levels (0.0 – 1.0). */
void fft_get_levels(float *bass, float *mid, float *high);

/** @brief Get all FFT_NUM_BANDS band values into caller-supplied array. */
void fft_get_bands(float *bands);

/** @brief Get beat detection state for this frame. */
void fft_get_beat(bool *detected, float *intensity);

#endif /* FFT_H__ */
