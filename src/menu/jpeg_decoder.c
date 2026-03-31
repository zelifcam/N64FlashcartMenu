/**
 * @file jpeg_decoder.c
 * @brief JPEG decoder — uses IJG libjpeg with DCT-domain shrink-on-decode.
 * @ingroup ui_components
 *
 * Decodes a batch of scanlines per poll() call for non-blocking operation.
 * The callback fires only on completion (success or decode error during poll).
 * Start-time errors (file not found, OOM) are communicated via return value only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <libdragon.h>
#include <jpeglib.h>
#include "jpeg_decoder.h"

#define SCANLINES_PER_POLL  (1)

/* libjpeg error handler that longjmps instead of calling exit(). */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buf;
} jpeg_error_mgr_ex_t;

typedef struct {
    struct jpeg_decompress_struct cinfo;
    jpeg_error_mgr_ex_t jerr;
    FILE            *f;
    uint8_t         *mem_buf;       /* heap buffer for in-memory decode (freed on deinit) */
    size_t           mem_buf_size;
    JSAMPROW         row_buf;
    surface_t       *image;
    int              src_w, src_h, dst_w, dst_h, comps;
    int              scan_y;
    jpeg_callback_t *callback;
    void            *callback_data;
    bool             started;
} jpeg_decoder_t;

static jpeg_decoder_t *decoder;


static void jpeg_error_exit_ex (j_common_ptr cinfo) {
    jpeg_error_mgr_ex_t *err = (jpeg_error_mgr_ex_t *)cinfo->err;
    longjmp(err->setjmp_buf, 1);
}

static void jpeg_decoder_deinit (bool free_image) {
    if (!decoder) return;

    free(decoder->row_buf);
    if (decoder->started) {
        jpeg_abort_decompress(&decoder->cinfo);
        jpeg_destroy_decompress(&decoder->cinfo);
    }
    if (decoder->f) fclose(decoder->f);
    free(decoder->mem_buf);
    if (free_image && decoder->image) {
        surface_free(decoder->image);
        free(decoder->image);
    }
    free(decoder);
    decoder = NULL;
}


/** Shared setup after the JPEG source has been configured.
 *  Reads the header, picks a scale factor, allocates the output surface. */
static jpeg_err_t jpeg_decoder_setup (int max_width, int max_height) {
    if (setjmp(decoder->jerr.setjmp_buf)) {
        jpeg_decoder_deinit(false);
        return JPEG_ERR_BAD_FILE;
    }

    jpeg_create_decompress(&decoder->cinfo);
    decoder->started = true;

    if (decoder->f) {
        jpeg_stdio_src(&decoder->cinfo, decoder->f);
    } else {
        jpeg_mem_src(&decoder->cinfo, decoder->mem_buf, decoder->mem_buf_size);
    }

    jpeg_read_header(&decoder->cinfo, TRUE);

    /* Pick the best DCT scale that fits within max dimensions AND
     * available heap. Try the largest scale first, fall back to smaller
     * until the output surface + row buffer + headroom fits in memory.
     * Progressive JPEGs also need a coefficient buffer inside
     * jpeg_start_decompress — account for that cost here. */
    bool is_progressive = jpeg_has_multiple_scans(&decoder->cinfo);
    size_t coeff_cost = 0;
    if (is_progressive) {
        coeff_cost = (size_t)decoder->cinfo.image_width
                   * decoder->cinfo.image_height
                   * decoder->cinfo.num_components * sizeof(JCOEF);
    }

    decoder->cinfo.dct_method = JDCT_IFAST;
    bool fits = false;
    /* Progressive JPEGs need large internal coefficient buffers.
     * Start at denom=2 to halve their memory footprint. */
    int start_denom = is_progressive ? 2 : 1;
    for (int denom = start_denom; denom <= 8; denom *= 2) {
        decoder->cinfo.scale_num   = 1;
        decoder->cinfo.scale_denom = denom;
        jpeg_calc_output_dimensions(&decoder->cinfo);

        int out_w = (int)decoder->cinfo.output_width;
        int out_h = (int)decoder->cinfo.output_height;

        /* Clamp to caller's max dimensions */
        int dst_w = out_w, dst_h = out_h;
        if (dst_w > max_width || dst_h > max_height) {
            if (out_w * max_height >= out_h * max_width) {
                dst_w = max_width;
                dst_h = (out_h * max_width) / out_w;
            } else {
                dst_h = max_height;
                dst_w = (out_w * max_height) / out_h;
            }
        }
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;

        int comps = decoder->cinfo.output_components;
        /* Our row buffer + libjpeg internal buffers (color convert,
         * upsample, ~10 rows of working memory at output width). */
        size_t jpeg_cost = (size_t)out_w * comps * 12;
        size_t surf_size = (size_t)dst_w * dst_h * 2;
        /* Progressive: coefficient buffer is constant regardless of scale
         * (it's based on source dimensions, not output). */
        size_t total_cost = jpeg_cost + surf_size + coeff_cost + 64 * 1024;

        heap_stats_t heap;
        sys_get_heap_stats(&heap);
        size_t available = heap.total - heap.used;

        if (total_cost <= available) {
            fits = true;
            break;
        }
    }

    if (!fits) {
        jpeg_decoder_deinit(false);
        return JPEG_ERR_OUT_OF_MEM;
    }

    jpeg_start_decompress(&decoder->cinfo);

    decoder->src_w = (int)decoder->cinfo.output_width;
    decoder->src_h = (int)decoder->cinfo.output_height;
    decoder->comps = (int)decoder->cinfo.output_components;

    /* Scale to fit, preserving aspect ratio */
    decoder->dst_w = decoder->src_w;
    decoder->dst_h = decoder->src_h;
    if (decoder->dst_w > max_width || decoder->dst_h > max_height) {
        if (decoder->src_w * max_height >= decoder->src_h * max_width) {
            decoder->dst_w = max_width;
            decoder->dst_h = (decoder->src_h * max_width) / decoder->src_w;
        } else {
            decoder->dst_h = max_height;
            decoder->dst_w = (decoder->src_w * max_height) / decoder->src_h;
        }
    }
    if (decoder->dst_w < 1) decoder->dst_w = 1;
    if (decoder->dst_h < 1) decoder->dst_h = 1;

    decoder->image = calloc(1, sizeof(surface_t));
    if (!decoder->image) {
        jpeg_decoder_deinit(false);
        return JPEG_ERR_OUT_OF_MEM;
    }

    *decoder->image = surface_alloc(FMT_RGBA16, decoder->dst_w, decoder->dst_h);
    if (!decoder->image->buffer) {
        jpeg_decoder_deinit(true);
        return JPEG_ERR_OUT_OF_MEM;
    }

    decoder->row_buf = malloc((size_t)decoder->src_w * decoder->comps);
    if (!decoder->row_buf) {
        jpeg_decoder_deinit(true);
        return JPEG_ERR_OUT_OF_MEM;
    }

    decoder->scan_y = 0;
    return JPEG_OK;
}

jpeg_err_t jpeg_decoder_start (char *path, int max_width, int max_height,
                               jpeg_callback_t *callback, void *callback_data) {
    if (decoder) return JPEG_ERR_BUSY;

    decoder = calloc(1, sizeof(jpeg_decoder_t));
    if (!decoder) return JPEG_ERR_OUT_OF_MEM;
    decoder->callback      = callback;
    decoder->callback_data = callback_data;

    decoder->f = fopen(path, "rb");
    if (!decoder->f) {
        jpeg_decoder_deinit(false);
        return JPEG_ERR_NO_FILE;
    }

    decoder->cinfo.err = jpeg_std_error(&decoder->jerr.pub);
    decoder->jerr.pub.error_exit = jpeg_error_exit_ex;

    return jpeg_decoder_setup(max_width, max_height);
}

jpeg_err_t jpeg_decoder_start_mem (void *buf, size_t buf_size,
                                   int max_width, int max_height,
                                   jpeg_callback_t *callback, void *callback_data) {
    if (decoder) return JPEG_ERR_BUSY;

    decoder = calloc(1, sizeof(jpeg_decoder_t));
    if (!decoder) return JPEG_ERR_OUT_OF_MEM;
    decoder->callback      = callback;
    decoder->callback_data = callback_data;

    decoder->mem_buf      = buf;
    decoder->mem_buf_size = buf_size;

    decoder->cinfo.err = jpeg_std_error(&decoder->jerr.pub);
    decoder->jerr.pub.error_exit = jpeg_error_exit_ex;

    return jpeg_decoder_setup(max_width, max_height);
}

void jpeg_decoder_poll (void) {
    if (!decoder || !decoder->started) return;

    /* Re-establish error handler for this call stack */
    if (setjmp(decoder->jerr.setjmp_buf)) {
        jpeg_callback_t *cb = decoder->callback;
        void *cb_data = decoder->callback_data;
        jpeg_decoder_deinit(false);
        cb(JPEG_ERR_BAD_FILE, NULL, cb_data);
        return;
    }

    int lines_this_frame = 0;
    while ((int)decoder->cinfo.output_scanline < decoder->src_h &&
           lines_this_frame < SCANLINES_PER_POLL) {
        jpeg_read_scanlines(&decoder->cinfo, &decoder->row_buf, 1);
        int dst_y = (decoder->scan_y * decoder->dst_h) / decoder->src_h;
        uint16_t *dst_row = (uint16_t *)((uint8_t *)decoder->image->buffer +
                                         dst_y * decoder->image->stride);
        for (int dst_x = 0; dst_x < decoder->dst_w; dst_x++) {
            int src_x = (dst_x * decoder->src_w) / decoder->dst_w;
            uint8_t *p = decoder->row_buf + src_x * decoder->comps;
            uint8_t r = p[0];
            uint8_t g = (decoder->comps > 1) ? p[1] : r;
            uint8_t b = (decoder->comps > 2) ? p[2] : r;
            dst_row[dst_x] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
        }
        decoder->scan_y++;
        lines_this_frame++;
    }

    /* Check if decode is complete */
    if ((int)decoder->cinfo.output_scanline >= decoder->src_h) {
        jpeg_finish_decompress(&decoder->cinfo);

        surface_t *img = decoder->image;
        jpeg_callback_t *cb = decoder->callback;
        void *cb_data = decoder->callback_data;

        /* Deinit without freeing the image (caller owns it now) */
        decoder->image = NULL;
        jpeg_decoder_deinit(false);
        cb(JPEG_OK, img, cb_data);
    }
}

void jpeg_decoder_abort (void) {
    jpeg_decoder_deinit(true);
}

float jpeg_decoder_get_progress (void) {
    if (!decoder || decoder->src_h == 0) return 0.0f;
    return (float)decoder->scan_y / (float)decoder->src_h;
}
