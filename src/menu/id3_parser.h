/**
 * @file id3_parser.h
 * @brief ID3 tag metadata parser for MP3 files
 * @ingroup menu
 *
 * Parses ID3v1, ID3v2.2, ID3v2.3, and ID3v2.4 tags to extract
 * title, artist, album, and track number metadata.
 */

#ifndef ID3_PARSER_H__
#define ID3_PARSER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ID3_FIELD_MAX   (128)

typedef struct {
    char title[ID3_FIELD_MAX];
    char artist[ID3_FIELD_MAX];
    char album[ID3_FIELD_MAX];
    int track_number;
    bool has_metadata;
} id3_metadata_t;

/**
 * @brief Parse ID3 metadata from an open MP3 file.
 *
 * Tries ID3v2 first (from the beginning of the file), then falls back
 * to ID3v1 (last 128 bytes). The file position is restored afterward.
 * Any parsing failure results in zeroed metadata, never a crash.
 *
 * @param f         Open file handle (must be seekable).
 * @param file_size Total file size in bytes.
 * @param meta      Output metadata struct (zeroed on entry).
 */
void id3_parse(FILE *f, size_t file_size, id3_metadata_t *meta);

#endif /* ID3_PARSER_H__ */
