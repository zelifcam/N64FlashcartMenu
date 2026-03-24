/**
 * @file id3_parser.c
 * @brief ID3 tag metadata parser
 * @ingroup menu
 *
 * Supports ID3v1, ID3v2.2 (3-char frames), ID3v2.3, and ID3v2.4
 * (4-char frames). Handles UTF-16 to UTF-8 conversion for text
 * frames and ID3v2 unsynchronization.
 */

#include <stdlib.h>
#include <string.h>

#include "id3_parser.h"

#define ID3V2_MAX_TAG_SIZE  (1024 * 1024)


/* --- UTF-16 to UTF-8 conversion --- */

/**
 * @brief Convert a UTF-16 buffer to UTF-8.
 *
 * Handles BOM detection, surrogate pairs, and both big/little endian.
 * Non-representable characters are skipped rather than replaced, so
 * CJK and other scripts come through as valid UTF-8 when the display
 * font supports them.
 *
 * @param src       UTF-16 input bytes.
 * @param src_len   Number of input bytes (not code units).
 * @param dst       Output UTF-8 buffer.
 * @param dst_size  Size of output buffer.
 * @param big_endian Default byte order if no BOM is present.
 */
static void utf16_to_utf8(const uint8_t *src, size_t src_len,
                          char *dst, size_t dst_size, bool big_endian) {
    size_t out = 0;

    /* Detect and consume BOM */
    if (src_len >= 2) {
        if (src[0] == 0xFF && src[1] == 0xFE) {
            big_endian = false; src += 2; src_len -= 2;
        } else if (src[0] == 0xFE && src[1] == 0xFF) {
            big_endian = true;  src += 2; src_len -= 2;
        }
    }

    for (size_t i = 0; i + 1 < src_len; i += 2) {
        uint16_t code_unit = big_endian
            ? ((uint16_t)src[i] << 8) | src[i + 1]
            : ((uint16_t)src[i + 1] << 8) | src[i];

        if (code_unit == 0) break;

        uint32_t codepoint;

        /* Handle surrogate pairs for characters above U+FFFF */
        if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
            if (i + 3 >= src_len) break;
            uint16_t low = big_endian
                ? ((uint16_t)src[i + 2] << 8) | src[i + 3]
                : ((uint16_t)src[i + 3] << 8) | src[i + 2];
            if (low < 0xDC00 || low > 0xDFFF) continue;
            codepoint = 0x10000 + ((uint32_t)(code_unit - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        } else if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
            continue; /* stray low surrogate */
        } else {
            codepoint = code_unit;
        }

        /* Encode as UTF-8 */
        if (codepoint < 0x80) {
            if (out + 1 >= dst_size) break;
            dst[out++] = (char)codepoint;
        } else if (codepoint < 0x800) {
            if (out + 2 >= dst_size) break;
            dst[out++] = (char)(0xC0 | (codepoint >> 6));
            dst[out++] = (char)(0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            if (out + 3 >= dst_size) break;
            dst[out++] = (char)(0xE0 | (codepoint >> 12));
            dst[out++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            dst[out++] = (char)(0x80 | (codepoint & 0x3F));
        } else {
            if (out + 4 >= dst_size) break;
            dst[out++] = (char)(0xF0 | (codepoint >> 18));
            dst[out++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            dst[out++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            dst[out++] = (char)(0x80 | (codepoint & 0x3F));
        }
    }

    dst[out] = '\0';
}


/* --- String helpers --- */

/** Copy and trim leading/trailing whitespace and nulls from a tag field. */
static void trim_copy(char *dst, const uint8_t *src, size_t len, size_t dst_size) {
    while (len > 0 && (*src == ' ' || *src == '\0')) { src++; len--; }
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    for (int i = (int)len - 1; i >= 0 && (dst[i] == ' ' || dst[i] == '\0'); i--) {
        dst[i] = '\0';
    }
}


/* --- Unsynchronization --- */

/**
 * @brief Remove ID3v2 unsynchronization in place.
 *
 * The ID3v2 spec allows inserting 0x00 after every 0xFF byte to prevent
 * false sync detection by hardware decoders. This removes those padding
 * bytes so the frame data is the original payload.
 *
 * @param data  Buffer to de-unsynchronize in place.
 * @param len   Length of input data.
 * @return      New length after removing padding bytes.
 */
static size_t de_unsync(uint8_t *data, size_t len) {
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        data[out++] = data[i];
        if (data[i] == 0xFF && i + 1 < len && data[i + 1] == 0x00) {
            i++; /* skip the padding byte */
        }
    }
    return out;
}


/* --- ID3v1 --- */

static bool parse_id3v1(FILE *f, size_t file_size, id3_metadata_t *meta) {
    if (file_size < 128) return false;

    uint8_t tag[128];
    long saved = ftell(f);
    if (fseek(f, -(long)sizeof(tag), SEEK_END)) return false;
    size_t rd = fread(tag, 1, sizeof(tag), f);
    fseek(f, saved, SEEK_SET);
    if (rd < 128 || memcmp(tag, "TAG", 3) != 0) return false;

    trim_copy(meta->title,  &tag[3],  30, ID3_FIELD_MAX);
    trim_copy(meta->artist, &tag[33], 30, ID3_FIELD_MAX);
    trim_copy(meta->album,  &tag[63], 30, ID3_FIELD_MAX);

    /* ID3v1.1: if byte 125 is zero and byte 126 is nonzero, it's the track number */
    if (tag[125] == 0 && tag[126] != 0) {
        meta->track_number = tag[126];
    }

    meta->has_metadata = (meta->title[0] || meta->artist[0] || meta->album[0]);
    return meta->has_metadata;
}


/* --- ID3v2 --- */

/** Read a synchsafe integer (7 bits per byte). */
static size_t read_syncsafe(const uint8_t *b) {
    return ((size_t)(b[0] & 0x7F) << 21) |
           ((size_t)(b[1] & 0x7F) << 14) |
           ((size_t)(b[2] & 0x7F) << 7)  |
            (size_t)(b[3] & 0x7F);
}

/** Read a plain big-endian 32-bit integer. */
static size_t read_be32(const uint8_t *b) {
    return ((size_t)b[0] << 24) | ((size_t)b[1] << 16) |
           ((size_t)b[2] << 8)  |  (size_t)b[3];
}

/** Read a plain big-endian 24-bit integer (ID3v2.2 frame size). */
static size_t read_be24(const uint8_t *b) {
    return ((size_t)b[0] << 16) | ((size_t)b[1] << 8) | (size_t)b[2];
}

/**
 * @brief Decode a text frame payload into a destination string.
 *
 * Handles all four ID3v2 text encodings:
 *   0x00 = ISO-8859-1, 0x01 = UTF-16 with BOM,
 *   0x02 = UTF-16BE, 0x03 = UTF-8.
 */
static void decode_text_frame(const uint8_t *data, size_t data_len,
                              char *dst, size_t dst_size) {
    if (data_len < 1) return;

    uint8_t encoding = data[0];
    const uint8_t *text = data + 1;
    size_t text_len = data_len - 1;

    if (encoding == 0x01 || encoding == 0x02) {
        utf16_to_utf8(text, text_len, dst, dst_size, encoding == 0x02);
    } else {
        /* ISO-8859-1 (0x00) or UTF-8 (0x03), both usable as-is */
        trim_copy(dst, text, text_len, dst_size);
    }
}

/**
 * @brief Match a frame ID to a metadata field, handling both
 *        ID3v2.2 (3-char) and ID3v2.3/v2.4 (4-char) frame IDs.
 *
 * @return Pointer to the target field in meta, or NULL if not a field we care about.
 */
static char *match_frame_id(const uint8_t *id, int id_len, id3_metadata_t *meta) {
    if (id_len == 4) {
        if (memcmp(id, "TIT2", 4) == 0) return meta->title;
        if (memcmp(id, "TPE1", 4) == 0) return meta->artist;
        if (memcmp(id, "TALB", 4) == 0) return meta->album;
    } else if (id_len == 3) {
        if (memcmp(id, "TT2", 3) == 0) return meta->title;
        if (memcmp(id, "TP1", 3) == 0) return meta->artist;
        if (memcmp(id, "TAL", 3) == 0) return meta->album;
    }
    return NULL;
}

/** Check if a frame ID looks like a track number frame. */
static bool is_track_frame(const uint8_t *id, int id_len) {
    if (id_len == 4) return memcmp(id, "TRCK", 4) == 0;
    if (id_len == 3) return memcmp(id, "TRK", 3) == 0;
    return false;
}

static bool parse_id3v2(const uint8_t *buf, size_t buf_size, id3_metadata_t *meta) {
    if (buf_size < 10 || memcmp(buf, "ID3", 3) != 0) return false;

    uint8_t version = buf[3];
    if (version < 2 || version > 4) return false;

    uint8_t flags = buf[5];
    bool tag_unsync = (flags & 0x80) != 0;

    size_t tag_size = read_syncsafe(&buf[6]) + 10;
    if (tag_size > buf_size) tag_size = buf_size;

    /* For unsynchronized tags, work on a mutable copy */
    uint8_t *work_buf = NULL;
    if (tag_unsync) {
        work_buf = malloc(tag_size);
        if (!work_buf) return false;
        memcpy(work_buf, buf, tag_size);
        tag_size = 10 + de_unsync(work_buf + 10, tag_size - 10);
        buf = work_buf;
    }

    /* Frame header sizes differ by version */
    int id_len = (version == 2) ? 3 : 4;
    int frame_hdr_size = (version == 2) ? 6 : 10;

    /* Skip extended header if present (v2.3/v2.4 only) */
    size_t pos = 10;
    if (version >= 3 && (flags & 0x40)) {
        if (pos + 4 > tag_size) goto done;
        size_t ext_size = (version == 4) ? read_syncsafe(&buf[pos]) : read_be32(&buf[pos]);
        pos += ext_size;
    }

    /* Walk frames */
    while (pos + frame_hdr_size <= tag_size) {
        const uint8_t *fhdr = &buf[pos];

        /* Frame ID must start with an uppercase letter */
        if (fhdr[0] < 'A' || fhdr[0] > 'Z') break;

        size_t frame_size;
        if (version == 2) {
            frame_size = read_be24(&fhdr[3]);
        } else if (version == 4) {
            frame_size = read_syncsafe(&fhdr[4]);
        } else {
            frame_size = read_be32(&fhdr[4]);
        }

        pos += frame_hdr_size;
        if (frame_size == 0 || pos + frame_size > tag_size) break;

        const uint8_t *data = &buf[pos];
        size_t data_len = frame_size;

        /* Check for text fields we want */
        char *target = match_frame_id(fhdr, id_len, meta);
        if (target) {
            decode_text_frame(data, data_len, target, ID3_FIELD_MAX);
        } else if (is_track_frame(fhdr, id_len)) {
            char tmp[16];
            decode_text_frame(data, data_len, tmp, sizeof(tmp));
            meta->track_number = atoi(tmp);
        }

        pos += frame_size;
    }

done:
    free(work_buf);
    meta->has_metadata = (meta->title[0] || meta->artist[0] || meta->album[0]);
    return meta->has_metadata;
}


/* --- Public API --- */

void id3_parse(FILE *f, size_t file_size, id3_metadata_t *meta) {
    memset(meta, 0, sizeof(*meta));

    long saved = ftell(f);

    /* Try ID3v2 from the start of the file */
    fseek(f, 0, SEEK_SET);

    uint8_t header[10];
    if (fread(header, 1, 10, f) == 10 && memcmp(header, "ID3", 3) == 0) {
        size_t tag_size = read_syncsafe(&header[6]) + 10;
        if (tag_size <= ID3V2_MAX_TAG_SIZE) {
            uint8_t *tag_buf = malloc(tag_size);
            if (tag_buf) {
                fseek(f, 0, SEEK_SET);
                size_t rd = fread(tag_buf, 1, tag_size, f);
                if (rd == tag_size) {
                    parse_id3v2(tag_buf, tag_size, meta);
                }
                free(tag_buf);
            }
        }
    }

    /* Fall back to ID3v1 if nothing found */
    if (!meta->has_metadata) {
        parse_id3v1(f, file_size, meta);
    }

    fseek(f, saved, SEEK_SET);
}
