/**
 * @file fft.c
 * @brief FFT-based audio analysis for visualizer
 * @ingroup visualizer
 */

#include <math.h>
#include <libdragon.h>
#include <kissfft/kiss_fftr.h>
#include "fft.h"
#include "../mp3_player.h"

#define FFT_SIZE     256

/* Usable bin range: skip DC (bin 0) and stay below Nyquist */
#define BIN_FIRST    1
#define BIN_LAST     (FFT_SIZE / 2)

/* Frequency range mapped across the bands */
#define FREQ_MIN     60.0f
#define FREQ_MAX     18000.0f

static kiss_fftr_cfg  fft_cfg;
static kiss_fft_scalar fft_in[FFT_SIZE];
static kiss_fft_cpx    fft_out[FFT_SIZE / 2 + 1];

/* Precomputed Hann window — eliminates spectral leakage between bins */
static float hann_window[FFT_SIZE];

/*
 * Per-band running average for imm/avg ratio normalization (MilkDrop style).
 * Tracks the medium-term energy level of each band so relative dynamics are
 * preserved even on hyper-compressed material.
 *
 * Asymmetric update: fast rise so loud sections are tracked quickly, slow
 * fall so quiet sections don't immediately pump the bars up.
 *
 * BAND_FLOOR prevents noise amplification in genuinely silent bands — a band
 * with avg below the floor is treated as inactive rather than boosted to full.
 */
#define BAND_FLOOR      0.05f   /* minimum avg denominator — silent band floor   */
#define BAND_AVG_RISE   0.30f   /* new-value weight when val > avg (fast, ~2 frames) */
#define BAND_AVG_FALL   0.05f   /* new-value weight when val < avg (slow, ~14 frames) */

static float band_avg[FFT_NUM_BANDS];

/*
 * Logarithmically-spaced bin boundaries, computed once in fft_init().
 * band_bin_lo[i] and band_bin_hi[i] are the inclusive FFT bin range
 * for band i.  Each band spans an equal ratio of frequency (one semitone
 * group), so low bands cover narrow Hz ranges and high bands cover wide
 * ones — matching how human hearing perceives pitch.
 */
static int band_bin_lo[FFT_NUM_BANDS];
static int band_bin_hi[FFT_NUM_BANDS];

/*
 * Per-band perceptual gain — computed at runtime in fft_init() so it
 * scales automatically to any FFT_NUM_BANDS value.
 * Exponential curve from 0.04 (sub-bass) to 4.0 (high treble), matching
 * the hand-tuned shape of the original 32-band table.
 */
static float band_gain[FFT_NUM_BANDS];

static float bass = 0, mid = 0, high = 0;
static float bass_avg = 0;
static bool  beat_detected = false;
static float beat_intensity_val = 0;

static float bands[FFT_NUM_BANDS] = {0};

void fft_init (void) {
    fft_cfg = kiss_fftr_alloc(FFT_SIZE, 0, NULL, NULL);

    /* Hann window: w(n) = 0.5 * (1 - cos(2*pi*n / (N-1))) */
    for (int i = 0; i < FFT_SIZE; i++) {
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
    }

    /* Logarithmically-spaced bin boundaries.
     * Use the actual sample rate of the loaded track so bin edges are correct
     * for both 44100 Hz and 48000 Hz files.
     * bin_for_freq(f) = f * FFT_SIZE / sample_rate */
    int sample_rate = mp3player_get_samplerate();
    if (sample_rate <= 0) sample_rate = 44100;
    float bin_scale = (float)FFT_SIZE / sample_rate;
    float log_min = logf(FREQ_MIN);
    float log_max = logf(FREQ_MAX);

    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        band_avg[i] = BAND_FLOOR;

        float f_lo = expf(log_min + (log_max - log_min) * (float)i / FFT_NUM_BANDS);
        float f_hi = expf(log_min + (log_max - log_min) * (float)(i + 1) / FFT_NUM_BANDS);

        int lo = (int)(f_lo * bin_scale);
        int hi = (int)(f_hi * bin_scale);

        if (lo < BIN_FIRST) lo = BIN_FIRST;
        if (hi < lo)        hi = lo;
        if (hi > BIN_LAST)  hi = BIN_LAST;

        band_bin_lo[i] = lo;
        band_bin_hi[i] = hi;

        /* Perceptual gain: exponential from 0.04 (bass) to 4.0 (treble).
         * Above 75% of bands the curve plateaus at 4.0 — high bands need
         * maximum boost to be visible at all. */
        float t = (float)i / (FFT_NUM_BANDS - 1);
        float g = 0.04f * expf(t * logf(4.0f / 0.04f));
        if (g > 4.0f) g = 4.0f;
        band_gain[i] = g;
    }
}

void fft_cleanup (void) {
    if (fft_cfg) {
        kiss_fftr_free(fft_cfg);
        fft_cfg = NULL;
    }
}

void fft_process (int16_t *samples, int len, int channels) {
    int frames = len / channels;
    int step = frames / FFT_SIZE;
    if (step < 1) step = 1;

    /* Downmix to mono, normalize to [-1,1], apply Hann window */
    const float ch_scale = (1.0f / 32768.0f) / channels;
    for (int i = 0; i < FFT_SIZE; i++) {
        int frame = i * step;
        if (frame < frames) {
            int idx = frame * channels;
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ch++) sum += samples[idx + ch];
            fft_in[i] = sum * ch_scale * hann_window[i];
        } else {
            fft_in[i] = 0.0f;
        }
    }

    kiss_fftr(fft_cfg, fft_in, fft_out);

    float b = 0, m = 0, h = 0;

    /* Band split boundaries — Bass: bottom 26%, Mid: next 50%, High: top 24%.
     * Computed once here; these ratios scale correctly to any FFT_NUM_BANDS. */
    const int bass_top = FFT_NUM_BANDS * 26 / 100;
    const int mid_top  = FFT_NUM_BANDS * 76 / 100;
    const int bass_cnt = bass_top + 1;
    const int mid_cnt  = mid_top - bass_top;
    const int high_cnt = FFT_NUM_BANDS - bass_cnt - mid_cnt;

    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        int lo = band_bin_lo[i];
        int hi = band_bin_hi[i];
        int count = hi - lo + 1;

        /* Average mag² across the band's bins, then apply perceptual gain.
         * Multiply by reciprocal — one FP multiply is faster than a divide. */
        float sum = 0;
        for (int j = lo; j <= hi; j++) {
            float re = fft_out[j].r;
            float im = fft_out[j].i;
            sum += re * re + im * im;
        }
        float val = sum * (1.0f / count) * band_gain[i];

        /* Asymmetric running average — fast rise, slow fall.
         * Tracks the band's medium-term energy level so the ratio below
         * reflects relative dynamics rather than absolute level. */
        float alpha = (val > band_avg[i]) ? BAND_AVG_RISE : BAND_AVG_FALL;
        band_avg[i] = band_avg[i] * (1.0f - alpha) + val * alpha;

        /* Normalise by running average (imm/avg ratio, MilkDrop style).
         * Floor prevents noise amplification in silent bands. */
        float denom = (band_avg[i] > BAND_FLOOR) ? band_avg[i] : BAND_FLOOR;
        val = val / denom;
        if (val > 1.0f) val = 1.0f;

        /* Instant attack, moderate decay */
        bands[i] = (val > bands[i]) ? val : bands[i] * 0.85f + val * 0.15f;

        /* Accumulate raw power for bass/mid/high levels */
        if      (i <= bass_top) b += sum / count;
        else if (i <= mid_top)  m += sum / count;
        else                    h += sum / count;
    }

    /* Normalise accumulated power into [0, 1] */
    b /= (float)bass_cnt;  b *= 0.50f;  if (b > 1.0f) b = 1.0f;
    m /= (float)mid_cnt;   m *= 0.30f;  if (m > 1.0f) m = 1.0f;
    h /= (float)high_cnt;  h *= 0.15f;  if (h > 1.0f) h = 1.0f;

    /* Fast attack, moderate decay */
    bass = (b > bass) ? (bass * 0.5f + b * 0.5f) : (bass * 0.75f + b * 0.25f);
    mid  = (m > mid)  ? (mid  * 0.5f + m * 0.5f) : (mid  * 0.75f + m * 0.25f);
    high = (h > high) ? (high * 0.5f + h * 0.5f) : (high * 0.75f + h * 0.25f);
    if (bass > 1.0f) bass = 1.0f;
    if (mid  > 1.0f) mid  = 1.0f;
    if (high > 1.0f) high = 1.0f;

    /* Beat detection: spike above running bass average */
    bass_avg = bass_avg * 0.95f + b * 0.05f;
    float threshold = bass_avg * 1.8f + 0.08f;
    beat_detected = (b > threshold) && (b > 0.15f);
    beat_intensity_val = (b > bass_avg && bass_avg > 0.01f)
        ? (b - bass_avg) / bass_avg : 0.0f;
    if (beat_intensity_val > 1.0f) beat_intensity_val = 1.0f;
}

void fft_get_levels (float *b, float *m, float *h) {
    *b = bass;
    *m = mid;
    *h = high;
}

void fft_get_bands (float *out) {
    for (int i = 0; i < FFT_NUM_BANDS; i++) out[i] = bands[i];
}

void fft_get_beat (bool *detected, float *intensity) {
    *detected  = beat_detected;
    *intensity = beat_intensity_val;
}
