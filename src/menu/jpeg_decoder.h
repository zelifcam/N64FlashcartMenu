/**
 * @file jpeg_decoder.h
 * @brief JPEG decoder component — wraps IJG libjpeg with shrink-on-decode support.
 * @ingroup ui_components
 */

#ifndef JPEG_DECODER_H__
#define JPEG_DECODER_H__

#include <surface.h>

/** @brief JPEG decoder error codes. */
typedef enum {
    JPEG_OK             =  0,
    JPEG_ERR_INT        = -1,
    JPEG_ERR_BUSY       = -2,
    JPEG_ERR_OUT_OF_MEM = -3,
    JPEG_ERR_NO_FILE    = -4,
    JPEG_ERR_BAD_FILE   = -5,
} jpeg_err_t;

/** @brief Callback invoked when decoding completes (or fails).
 *  On success, @p decoded_image is a heap-allocated surface_t (caller must surface_free + free).
 *  On failure, @p decoded_image is NULL. */
typedef void (jpeg_callback_t)(jpeg_err_t err, surface_t *decoded_image, void *callback_data);

/**
 * @brief Start decoding a JPEG file.
 *
 * Uses libjpeg's DCT-domain scaling to decode at the smallest scale factor
 * that produces an image >= max_width x max_height, then scales to fit exactly.
 * This avoids allocating a full-resolution pixel buffer for large source images.
 *
 * @param path           Path to the JPEG file.
 * @param max_width      Maximum output width in pixels.
 * @param max_height     Maximum output height in pixels.
 * @param callback       Called when decoding completes.
 * @param callback_data  Passed through to callback.
 * @return JPEG_OK if decoding started. Call jpeg_decoder_poll() each frame
 *         until the callback fires with the result.
 */
jpeg_err_t jpeg_decoder_start (char *path, int max_width, int max_height,
                               jpeg_callback_t *callback, void *callback_data);

/**
 * @brief Start decoding JPEG from an in-memory buffer.
 *
 * Same as jpeg_decoder_start but reads from a heap buffer instead of a file.
 * The decoder takes ownership of @p buf and frees it when done.
 *
 * @param buf            Heap-allocated JPEG data (ownership transferred).
 * @param buf_size       Size of the buffer in bytes.
 * @param max_width      Maximum output width in pixels.
 * @param max_height     Maximum output height in pixels.
 * @param callback       Called when decoding completes.
 * @param callback_data  Passed through to callback.
 * @return JPEG_OK if decoding started.
 */
jpeg_err_t jpeg_decoder_start_mem (void *buf, size_t buf_size,
                                   int max_width, int max_height,
                                   jpeg_callback_t *callback, void *callback_data);

/** @brief Abort and free all resources if a decode is in progress. */
void jpeg_decoder_abort (void);

/** @brief Returns decode progress (0.0–1.0). Provided for API parity with png_decoder. */
float jpeg_decoder_get_progress (void);

/** @brief Decode a batch of scanlines. Call each frame until the callback fires. */
void jpeg_decoder_poll (void);

#endif /* JPEG_DECODER_H__ */
