/**
 * @file mp3_player.h
 * @brief Audio Player (MP3/FLAC)
 * @ingroup menu 
 */

#ifndef MP3_PLAYER_H__
#define MP3_PLAYER_H__

#include <stdbool.h>

#include "id3_parser.h"

/**
 * @brief Audio file error enumeration.
 *
 * Enumeration for different types of errors that can occur in the audio player.
 */
typedef enum {
    MP3PLAYER_OK,               /**< No error */
    MP3PLAYER_ERR_OUT_OF_MEM,   /**< Out of memory error */
    MP3PLAYER_ERR_IO,           /**< Input/Output error */
    MP3PLAYER_ERR_NO_FILE,      /**< No file found error */
    MP3PLAYER_ERR_INVALID_FILE, /**< Invalid file error */
} mp3player_err_t;

/**
 * @brief Initialize the audio player mixer.
 *
 * This function initializes the mixer for the audio player.
 */
void mp3player_mixer_init(void);

/**
 * @brief Initialize the audio player.
 *
 * This function initializes the audio player and prepares it for playback.
 * 
 * @return mp3player_err_t Error code indicating the result of the initialization.
 */
mp3player_err_t mp3player_init(void);

/**
 * @brief Deinitialize the audio player.
 *
 * This function deinitializes the audio player and releases any resources.
 */
void mp3player_deinit(void);

/**
 * @brief Load an audio file.
 *
 * This function loads an audio file from the specified path.
 *
 * @param path Path to the audio file.
 * @return mp3player_err_t Error code indicating the result of the load operation.
 */
mp3player_err_t mp3player_load(char *path);

/**
 * @brief Unload the current audio file.
 *
 * This function unloads the currently loaded audio file.
 */
void mp3player_unload(void);

/**
 * @brief Process the audio player.
 *
 * This function processes the audio player, handling playback and other operations.
 * 
 * @return mp3player_err_t Error code indicating the result of the process operation.
 */
mp3player_err_t mp3player_process(void);

/**
 * @brief Check if the audio player is playing.
 *
 * This function checks if the audio player is currently playing.
 *
 * @return true if the audio player is playing, false otherwise.
 */
bool mp3player_is_playing(void);

/**
 * @brief Check if the audio player has finished playing.
 *
 * This function checks if the audio player has finished playing the current file.
 *
 * @return true if the audio player has finished playing, false otherwise.
 */
bool mp3player_is_finished(void);

/**
 * @brief Start playback of the audio file.
 *
 * This function starts playback of the currently loaded audio file.
 * 
 * @return mp3player_err_t Error code indicating the result of the play operation.
 */
mp3player_err_t mp3player_play(void);

/**
 * @brief Stop playback of the audio file.
 *
 * This function stops playback of the currently loaded audio file.
 */
void mp3player_stop(void);

/**
 * @brief Toggle playback of the audio file.
 *
 * This function toggles playback of the currently loaded audio file.
 * 
 * @return mp3player_err_t Error code indicating the result of the toggle operation.
 */
mp3player_err_t mp3player_toggle(void);

/**
 * @brief Mute or unmute the audio player.
 *
 * This function mutes or unmutes the audio player.
 * 
 * @param mute true to mute, false to unmute.
 */
void mp3player_mute(bool mute);

/**
 * @brief Seek to a specific position in the audio file.
 *
 * This function seeks to a specific position in the currently loaded audio file.
 * 
 * @param seconds Number of seconds to seek.
 * @return mp3player_err_t Error code indicating the result of the seek operation.
 */
mp3player_err_t mp3player_seek(int seconds);

/**
 * @brief Get the duration of the audio file.
 *
 * This function gets the duration of the currently loaded audio file.
 *
 * @return float Duration of the audio file in seconds.
 */
float mp3player_get_duration(void);

/**
 * @brief Get the bitrate of the audio file.
 *
 * This function gets the bitrate of the currently loaded audio file.
 *
 * @return float Bitrate of the audio file in kbps.
 */
float mp3player_get_bitrate(void);

/**
 * @brief Get the sample rate of the audio file.
 *
 * This function gets the sample rate of the currently loaded audio file.
 *
 * @return int Sample rate of the audio file in Hz.
 */
int mp3player_get_samplerate(void);

/**
 * @brief Get the native sample rate before downsampling.
 *
 * For hi-res FLAC files that are downsampled for playback, this returns
 * the original rate. For MP3 or non-downsampled FLAC, same as get_samplerate.
 *
 * @return int Native sample rate in Hz.
 */
int mp3player_get_native_samplerate(void);

/**
 * @brief Get the current playback progress.
 *
 * This function gets the current playback progress of the audio file.
 * 
 * @return float Current playback progress as a percentage (0.0 to 100.0).
 */
float mp3player_get_progress(void);

/**
 * @brief Get metadata extracted from ID3 tags.
 *
 * @return const id3_metadata_t* Pointer to metadata (valid while file is loaded).
 */
const id3_metadata_t *mp3player_get_metadata(void);

/**
 * @brief Preload the next track for seamless playback.
 *
 * Opens and parses the file so it's ready for instant crossover when
 * the current track ends. The mixer never stops.
 *
 * @param path Path to the next audio file.
 * @return mp3player_err_t Error code.
 */
mp3player_err_t mp3player_preload_next(char *path);

/**
 * @brief Check if the preloaded track matches a given path.
 *
 * @param path Path to check against.
 * @return true if a track is preloaded and its path matches.
 */
bool mp3player_preload_matches(char *path);

/**
 * @brief Swap the preloaded track into the current slot and start playback.
 *
 * Stops the current track, promotes preloaded to current, and starts
 * the mixer. Much faster than mp3player_load for the next track.
 *
 * @return mp3player_err_t Error code.
 */
mp3player_err_t mp3player_play_preloaded(void);

/**
 * @brief Check if a seamless track advance occurred.
 *
 * Returns true once after the player internally switched from the
 * current track to the preloaded next track. The caller should update
 * UI state (metadata, cover art, selection) when this returns true.
 *
 * @return true if a track advance happened since last check.
 */
bool mp3player_did_advance(void);

#endif /* MP3_PLAYER_H__ */
