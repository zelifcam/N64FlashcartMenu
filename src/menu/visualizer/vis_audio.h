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
 */
typedef struct {
    int16_t buffers[2][VIS_PCM_BUFFER_SIZE];
    int write_idx;
    int sample_count;
    int channels;
    bool new_data;
    bool enabled;
} vis_pcm_buffer_t;

extern vis_pcm_buffer_t vis_pcm;

#endif /* VIS_AUDIO_H__ */
