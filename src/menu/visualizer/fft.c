/**
 * @file fft.c
 * @brief FFT-based audio analysis for visualizer
 * @ingroup visualizer
 */

#include <math.h>
#include <libdragon.h>
#include <kissfft/kiss_fftr.h>
#include "fft.h"

#define FFT_SIZE 256

static kiss_fftr_cfg  fft_cfg;
static kiss_fft_scalar fft_in[FFT_SIZE];
static kiss_fft_cpx    fft_out[FFT_SIZE / 2 + 1];

/* Precomputed Hann window — eliminates spectral leakage between bins */
static float hann_window[FFT_SIZE];

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
    int bpb = (FFT_SIZE / 2) / FFT_NUM_BANDS;

    /* Per-band gain applied to mag² (avoids sqrtf per bin).
     * Bass squashed, highs boosted for perceptual balance. */
    static const float band_gain[FFT_NUM_BANDS] = {
        0.008f / 8,   /* sub-bass  */
        0.012f / 8,
        0.020f / 8,   /* bass      */
        0.040f / 8,
        0.080f / 8,   /* upper bass */
        0.180f / 8,
        0.400f / 8,   /* low mids  */
        0.840f / 8,
        2.000f / 8,   /* mids      */
        3.400f / 8,
        5.200f / 8,   /* upper mids */
        8.000f / 8,
        11.60f / 8,   /* presence  */
        15.60f / 8,
        20.00f / 8,   /* highs     */
        26.00f / 8,
    };

    for (int i = 0; i < FFT_NUM_BANDS; i++) {
        float sum = 0;
        for (int j = 0; j < bpb; j++) {
            int idx = i * bpb + j + 1;  /* +1 skips DC bin */
            float re = fft_out[idx].r;
            float im = fft_out[idx].i;
            sum += re * re + im * im;
        }

        float val = sum * band_gain[i];
        if (val > 1.0f) val = 1.0f;

        /* Instant attack, moderate decay */
        bands[i] = (val > bands[i]) ? val : bands[i] * 0.85f + val * 0.15f;

        if      (i < FFT_NUM_BANDS / 3)     b += sum;
        else if (i < 2 * FFT_NUM_BANDS / 3) m += sum;
        else                                h += sum;
    }

    /* Squash into [0, 1] */
    b *= 0.10f;  if (b > 1.0f) b = 1.0f;
    m *= 0.20f;  if (m > 1.0f) m = 1.0f;
    h *= 0.50f;  if (h > 1.0f) h = 1.0f;

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
