/**
 * @file vis_audio.h
 * @brief Shared PCM capture buffer for audio visualizer
 * @ingroup visualizer
 */

#ifndef VIS_AUDIO_H__
#define VIS_AUDIO_H__

#include <stdint.h>
#include <stdbool.h>

#define VIS_PCM_BUFFER_SIZE 2048

/**
 * @brief Ping-pong PCM buffer shared between MP3 decoder and visualizer.
 *
 * The decoder writes into buffers[write_idx]. The visualizer claims that
 * buffer and flips write_idx atomically, so reads are never torn.
 *
 * Volatile flags ensure safe cross-thread access if decoder runs on a
 * separate execution context in the future. N64 is currently single-threaded,
 * but this pattern future-proofs the code.
 */
typedef struct {
    int16_t buffers[2][VIS_PCM_BUFFER_SIZE];
    volatile int write_idx;      /* decoder writes, visualizer reads */
    volatile int sample_count;   /* decoder writes, visualizer reads */
    int channels;                /* set once at init, never changed */
    volatile bool new_data;      /* decoder writes, visualizer reads */
    volatile bool enabled;       /* decoder writes, visualizer reads */
} vis_pcm_buffer_t;

extern vis_pcm_buffer_t vis_pcm;

#endif /* VIS_AUDIO_H__ */
