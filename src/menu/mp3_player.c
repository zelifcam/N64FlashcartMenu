/**
 * @file mp3_player.c
 * @brief MP3 Player with seamless playback support
 * @ingroup ui_components
 *
 * Supports preloading the next track so the mixer never stops between
 * tracks. When the current track's audio data is exhausted, the decoder
 * seamlessly switches to the preloaded track inside the waveform callback.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <libdragon.h>

#include "mp3_player.h"
#include "id3_parser.h"
#include "sound.h"
#include "utils/fs.h"
#include "utils/utils.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include <minimp3/minimp3_ex.h>
#include <minimp3/minimp3.h>

#define SEEK_PREDECODE_FRAMES   (5)

/** @brief Per-track file and decoder state. */
typedef struct {
    bool loaded;
    FILE *f;
    size_t file_size;
    size_t data_start;
    uint8_t buffer[16 * 1024];
    uint8_t *buffer_ptr;
    size_t buffer_left;

    mp3dec_t dec;
    mp3dec_frame_info_t info;

    int seek_predecode_frames;
    float duration;
    float bitrate;

    id3_metadata_t metadata;
} mp3_track_t;

/** @brief Player state with current + preloaded next track. */
typedef struct {
    mp3_track_t current;
    mp3_track_t next;
    bool next_ready;
    bool track_advanced;  /* set when track crossover occurs */

    waveform_t wave;
} mp3player_t;

static mp3player_t *p = NULL;


static void track_reset_decoder (mp3_track_t *t) {
    mp3dec_init(&t->dec);
    t->seek_predecode_frames = 0;
    t->buffer_ptr = t->buffer;
    t->buffer_left = 0;
}

static void track_fill_buffer (mp3_track_t *t) {
    if (feof(t->f)) return;

    if (t->buffer_left >= ALIGN(MAX_FREE_FORMAT_FRAME_SIZE, FS_SECTOR_SIZE)) return;

    if ((t->buffer_ptr != t->buffer) && (t->buffer_left > 0)) {
        memmove(t->buffer, t->buffer_ptr, t->buffer_left);
        t->buffer_ptr = t->buffer;
    }

    t->buffer_left += fread(t->buffer + t->buffer_left, 1, sizeof(t->buffer) - t->buffer_left, t->f);
}

static void track_calculate_duration (mp3_track_t *t, int samples) {
    uint32_t frames;
    int delay, padding;

    long data_size = (t->file_size - t->data_start);
    if (mp3dec_check_vbrtag((const uint8_t *)(t->buffer_ptr), t->info.frame_bytes, &frames, &delay, &padding) > 0) {
        t->duration = (frames * samples) / (float)(t->info.hz);
        t->bitrate = (data_size * 8) / t->duration;
    } else {
        t->bitrate = t->info.bitrate_kbps * 1000;
        t->duration = data_size / (t->bitrate / 8);
    }
}

static void track_unload (mp3_track_t *t) {
    if (t->loaded) {
        t->loaded = false;
        fclose(t->f);
    }
}

/** Load a track's file and find the first audio frame. Does not start playback.
 *  id3_flags controls what id3_parse extracts (e.g. ID3_FLAG_EXTRACT_ART). */
static mp3player_err_t track_load (mp3_track_t *t, char *path, int id3_flags) {
    if (t->loaded) track_unload(t);

    memset(t, 0, sizeof(*t));

    if ((t->f = fopen(path, "rb")) == NULL) {
        return MP3PLAYER_ERR_IO;
    }
    setbuf(t->f, NULL);

    struct stat st;
    if (fstat(fileno(t->f), &st)) {
        fclose(t->f);
        return MP3PLAYER_ERR_IO;
    }
    t->file_size = st.st_size;

    id3_parse(t->f, t->file_size, &t->metadata, id3_flags);

    track_reset_decoder(t);

    while (!(feof(t->f) && t->buffer_left == 0)) {
        track_fill_buffer(t);

        if (ferror(t->f)) {
            fclose(t->f);
            return MP3PLAYER_ERR_IO;
        }

        size_t id3v2_skip = mp3dec_skip_id3v2((const uint8_t *)(t->buffer_ptr), t->buffer_left);
        if (id3v2_skip > 0) {
            if (fseek(t->f, (-t->buffer_left) + id3v2_skip, SEEK_CUR)) {
                fclose(t->f);
                return MP3PLAYER_ERR_IO;
            }
            track_reset_decoder(t);
            continue;
        }

        int samples = mp3dec_decode_frame(&t->dec, t->buffer_ptr, t->buffer_left, NULL, &t->info);
        if (samples > 0) {
            t->loaded = true;
            t->data_start = ftell(t->f) - t->buffer_left + t->info.frame_offset;

            t->buffer_ptr += t->info.frame_offset;
            t->buffer_left -= t->info.frame_offset;

            track_calculate_duration(t, samples);

            return MP3PLAYER_OK;
        }

        t->buffer_ptr += t->info.frame_bytes;
        t->buffer_left -= t->info.frame_bytes;
    }

    fclose(t->f);
    return MP3PLAYER_ERR_INVALID_FILE;
}

static bool track_is_finished (mp3_track_t *t) {
    return t->loaded && feof(t->f) && (t->buffer_left == 0);
}


/**
 * @brief Waveform read callback. Called by the mixer to get audio samples.
 *
 * When the current track runs out of data and a next track is preloaded,
 * the crossover happens here: the next track becomes current and decoding
 * continues without the mixer ever stopping.
 */
/* Decode into a cached stack buffer first, then memcpy to the uncached
 * samplebuffer. Writing directly to uncached RDRAM is significantly slower
 * on the VR4300 because each store bypasses the data cache. Decoding to a
 * cached buffer and doing a single memcpy is faster overall. */
static void mp3player_wave_read (void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
    mp3_track_t *t = &p->current;
    int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (wlen > 0) {
        track_fill_buffer(t);

        int samples = mp3dec_decode_frame(&t->dec, t->buffer_ptr, t->buffer_left, pcm_buf, &t->info);

        if (samples > 0) {
            int pcm_bytes = samples * t->info.channels * sizeof(int16_t);
            short *out = (short *)(samplebuffer_append(sbuf, samples));
            memcpy(out, pcm_buf, pcm_bytes);

            if (t->seek_predecode_frames > 0) {
                t->seek_predecode_frames -= 1;
                memset(out, 0, pcm_bytes);
            }

            wlen -= samples;
        }

        t->buffer_ptr += t->info.frame_bytes;
        t->buffer_left -= t->info.frame_bytes;

        if (t->info.frame_bytes == 0) {
            /* Current track exhausted. Try track crossover. */
            if (p->next_ready && p->next.loaded) {
                track_unload(&p->current);
                memcpy(&p->current, &p->next, sizeof(mp3_track_t));
                memset(&p->next, 0, sizeof(mp3_track_t));
                p->next_ready = false;
                p->track_advanced = true;

                /* Update waveform params for new track */
                p->wave.channels = p->current.info.channels;
                p->wave.frequency = p->current.info.hz;

                t = &p->current;
                continue;
            }

            /* No next track available, fill with silence */
            short *buffer = (short *)(samplebuffer_append(sbuf, wlen));
            memset(buffer, 0, wlen * sizeof(short) * t->info.channels);
            wlen = 0;
        }
    }
}


void mp3player_mixer_init (void) {
    mixer_ch_set_limits(SOUND_MP3_PLAYER_CHANNEL, 16, 96000, 0);
}

mp3player_err_t mp3player_init (void) {
    p = calloc(1, sizeof(mp3player_t));
    if (p == NULL) return MP3PLAYER_ERR_OUT_OF_MEM;

    track_reset_decoder(&p->current);

    p->wave = (waveform_t) {
        .name = "mp3player",
        .bits = 16,
        .channels = 2,
        .frequency = 44100,
        .len = WAVEFORM_MAX_LEN - 1,
        .loop_len = WAVEFORM_MAX_LEN - 1,
        .read = mp3player_wave_read,
        .ctx = p,
    };

    return MP3PLAYER_OK;
}

void mp3player_deinit (void) {
    mp3player_unload();
    if (p->next.loaded) track_unload(&p->next);
    p->next_ready = false;
    id3_free_cover_art();
    free(p);
    p = NULL;
}

mp3player_err_t mp3player_load (char *path) {
    if (p->current.loaded) {
        mp3player_stop();
        track_unload(&p->current);
    }
    /* Discard any preloaded next track */
    if (p->next.loaded) track_unload(&p->next);
    p->next_ready = false;
    p->track_advanced = false;

    mp3player_err_t err = track_load(&p->current, path, ID3_FLAG_EXTRACT_ART);
    if (err != MP3PLAYER_OK) return err;

    p->wave.channels = p->current.info.channels;
    p->wave.frequency = p->current.info.hz;

    return MP3PLAYER_OK;
}

void mp3player_unload (void) {
    mp3player_stop();
    track_unload(&p->current);
}

mp3player_err_t mp3player_process (void) {
    if (!p || !p->current.loaded) return MP3PLAYER_OK;

    if (ferror(p->current.f)) {
        mp3player_unload();
        return MP3PLAYER_ERR_IO;
    }

    if (mp3player_is_finished()) {
        mp3player_stop();
    }

    return MP3PLAYER_OK;
}

bool mp3player_is_playing (void) {
    return mixer_ch_playing(SOUND_MP3_PLAYER_CHANNEL);
}

bool mp3player_is_finished (void) {
    return p->current.loaded && feof(p->current.f) && (p->current.buffer_left == 0) && !p->next_ready;
}

mp3player_err_t mp3player_play (void) {
    if (!p->current.loaded) return MP3PLAYER_ERR_NO_FILE;

    if (!mp3player_is_playing()) {
        if (track_is_finished(&p->current) && !p->next_ready) {
            if (fseek(p->current.f, p->current.data_start, SEEK_SET)) {
                return MP3PLAYER_ERR_IO;
            }
            track_reset_decoder(&p->current);
        }
        mixer_ch_play(SOUND_MP3_PLAYER_CHANNEL, &p->wave);
    }
    return MP3PLAYER_OK;
}

void mp3player_stop (void) {
    if (mp3player_is_playing()) {
        mixer_ch_stop(SOUND_MP3_PLAYER_CHANNEL);
    }
}

mp3player_err_t mp3player_toggle (void) {
    if (mp3player_is_playing()) {
        mp3player_stop();
    } else {
        return mp3player_play();
    }
    return MP3PLAYER_OK;
}

void mp3player_mute (bool mute) {
    float volume = mute ? 0.0f : 1.0f;
    mixer_ch_set_vol(SOUND_MP3_PLAYER_CHANNEL, volume, volume);
}

mp3player_err_t mp3player_seek (int seconds) {
    if (!p->current.loaded) return MP3PLAYER_ERR_NO_FILE;

    long bytes_to_move = (long)((p->current.bitrate * seconds) / 8);
    if (bytes_to_move == 0) return MP3PLAYER_OK;

    long position = (ftell(p->current.f) - p->current.buffer_left + bytes_to_move);
    if (position < (long)(p->current.data_start)) {
        position = p->current.data_start;
    }

    if (fseek(p->current.f, position, SEEK_SET)) {
        return MP3PLAYER_ERR_IO;
    }

    track_reset_decoder(&p->current);
    track_fill_buffer(&p->current);

    if (ferror(p->current.f)) return MP3PLAYER_ERR_IO;

    p->current.seek_predecode_frames = (position == p->current.data_start) ? 0 : SEEK_PREDECODE_FRAMES;

    return MP3PLAYER_OK;
}

float mp3player_get_duration (void) {
    if (!p->current.loaded) return 0.0f;
    return p->current.duration;
}

float mp3player_get_bitrate (void) {
    if (!p->current.loaded) return 0.0f;
    return p->current.bitrate;
}

int mp3player_get_samplerate (void) {
    if (!p->current.loaded) return 0;
    return p->current.info.hz;
}

float mp3player_get_progress (void) {
    if (!p->current.loaded) return 0.0f;

    long data_size = p->current.file_size - p->current.data_start;
    long data_consumed = ftell(p->current.f) - p->current.buffer_left;
    long data_position = (data_consumed > p->current.data_start) ? (data_consumed - p->current.data_start) : 0;

    return data_position / (float)(data_size);
}

const id3_metadata_t *mp3player_get_metadata (void) {
    static const id3_metadata_t empty = {0};
    if (!p || !p->current.loaded) return &empty;
    return &p->current.metadata;
}

mp3player_err_t mp3player_preload_next (char *path) {
    /* Discard any existing preload */
    if (p->next.loaded) track_unload(&p->next);
    p->next_ready = false;

    mp3player_err_t err = track_load(&p->next, path, 0);  /* no art extraction for preload */
    if (err != MP3PLAYER_OK) return err;

    p->next_ready = true;
    return MP3PLAYER_OK;
}

bool mp3player_did_advance (void) {
    if (p && p->track_advanced) {
        p->track_advanced = false;
        return true;
    }
    return false;
}
