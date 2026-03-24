#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../jpeg_decoder.h"
#include "../mp3_player.h"
#include "../png_decoder.h"
#include "../sound.h"
#include "../path.h"
#include "../ui_components/constants.h"
#include "utils/fs.h"
#include "views.h"


#define SEEK_SECONDS        (5)
#define SEEK_SECONDS_FAST   (60)
#define COVER_ART_MAX_SIZE  (238)

typedef enum {
    PLAYBACK_NORMAL,
    PLAYBACK_LOOP,
    PLAYBACK_REPEAT_ONE,
    PLAYBACK_SHUFFLE,
    PLAYBACK_PARTY,
    PLAYBACK_MODE_COUNT,
} playback_mode_t;

static const char *playback_mode_name[] = {
    [PLAYBACK_NORMAL]     = "Normal",
    [PLAYBACK_LOOP]       = "Loop",
    [PLAYBACK_REPEAT_ONE] = "Repeat One",
    [PLAYBACK_SHUFFLE]    = "Shuffle",
    [PLAYBACK_PARTY]      = "Party",
};

static playback_mode_t playback_mode = PLAYBACK_NORMAL;
static bool advance_failed = false;
static float ticker_offset = 0.0f;

#define QUEUE_LINE_HEIGHT   (16)
#define QUEUE_GAP           (12)
#define QUEUE_ARROW         "\xE2\x96\xB6 "  /* "▶ " UTF-8 */

/* Shuffle state: array of browser indices that point to music files */
static int *shuffle_list = NULL;
static int shuffle_count = 0;
static int shuffle_pos = 0;

/* Cover art state */
typedef enum {
    COVER_IDLE,
    COVER_LOADING_JPEG,
    COVER_LOADING_PNG,
} cover_state_t;

static cover_state_t cover_state = COVER_IDLE;
static surface_t *cover_image = NULL;

/* Cached blit parameters, computed once per loaded image */
static int cover_dst_x;
static int cover_dst_y;
static int cover_disp_size;
static int cover_s0;
static int cover_t0;

/* Directory scan state for async cover art search */
static const char *cover_image_extensions[] = { "png", "jpg", "jpeg", NULL };
static const char *preferred_cover_names[] = { "cover", "folder", "front", "album", "art", NULL };
static path_t *cover_dir = NULL;
static dir_t cover_dir_entry;
static bool cover_dir_scan_active = false;

static void try_next_cover_source(void);

/** Compute and cache blit parameters for the loaded cover_image. */
static void cover_cache_blit_params (void) {
    int iw = cover_image->width;
    int ih = cover_image->height;
    cover_s0 = (iw > ih) ? (iw - ih) / 2 : 0;
    cover_t0 = (ih > iw) ? (ih - iw) / 2 : 0;

    /* Position: centered horizontally, between ticker and seekbar */
    int text_bottom = VISIBLE_AREA_Y0 + 54;
    int bar_top = SEEKBAR_Y - BORDER_THICKNESS;
    int avail_h = bar_top - text_bottom - 16;
    int crop = (iw < ih) ? iw : ih;
    cover_disp_size = (avail_h < COVER_ART_MAX_SIZE) ? avail_h : COVER_ART_MAX_SIZE;
    if (cover_disp_size > crop) cover_disp_size = crop;
    cover_dst_x = DISPLAY_CENTER_X - cover_disp_size / 2;
    cover_dst_y = text_bottom + (avail_h - cover_disp_size) / 2;
}

/** Get the memory budget for cover art decode, based on free heap. */
static int cover_art_budget_size (void) {
    heap_stats_t heap;
    sys_get_heap_stats(&heap);
    size_t budget = (size_t)((heap.total - heap.used) * 0.8f);
    int dim = (int)sqrtf((float)(budget / 2));
    if (dim < COVER_ART_MAX_SIZE) dim = COVER_ART_MAX_SIZE;
    if (dim > 400) dim = 400;
    return dim;
}

/** Check if a PNG file's dimensions fit the memory budget.
 *  Reads only the 24-byte header. Returns true if safe to decode. */
static bool png_dimensions_ok (const char *path, int max_dim) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[24];
    size_t rd = fread(hdr, 1, 24, f);
    fclose(f);
    if (rd < 24) return false;
    /* PNG header: bytes 16-19 = width, 20-23 = height (big-endian) */
    if (memcmp(hdr, "\x89PNG", 4) != 0) return false;
    int w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    int h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    return (w <= max_dim && h <= max_dim);
}

static void cover_art_callback (jpeg_err_t err, surface_t *image, void *data) {
    cover_state = COVER_IDLE;
    if (err == JPEG_OK && image) {
        cover_image = image;
        cover_cache_blit_params();
    } else {
        debugf("Cover art decode failed (err %d), trying next\n", err);
        try_next_cover_source();
    }
}

static void cover_art_png_callback (png_err_t err, surface_t *image, void *data) {
    cover_state = COVER_IDLE;
    if (err == PNG_OK && image) {
        cover_image = image;
        cover_cache_blit_params();
    } else {
        debugf("PNG cover decode failed (err %d), trying next\n", err);
        try_next_cover_source();
    }
}

/** Try to load a cover art file. Returns true if decode started. */
static bool try_cover_path (const char *path, int max_size) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    ext++;

    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        cover_state = COVER_LOADING_JPEG;
        jpeg_err_t err = jpeg_decoder_start((char *)path, max_size, max_size,
                                            (jpeg_callback_t *)cover_art_callback, NULL);
        if (err != JPEG_OK) {
            cover_state = COVER_IDLE;
            debugf("JPEG start failed: %s (err %d)\n", path, err);
            return false;
        }
        return true;
    } else if (strcasecmp(ext, "png") == 0) {
        if (!png_dimensions_ok(path, max_size)) {
            debugf("PNG too large, skipping: %s\n", path);
            return false;
        }
        cover_state = COVER_LOADING_PNG;
        png_err_t err = png_decoder_start((char *)path, max_size, max_size,
                                          cover_art_png_callback, NULL);
        if (err != PNG_OK) {
            cover_state = COVER_IDLE;
            debugf("PNG start failed: %s (err %d)\n", path, err);
            return false;
        }
        return true;
    }
    return false;
}

/** Continue scanning the directory for the next image file. */
static void try_next_cover_source (void) {
    if (!cover_dir_scan_active || !cover_dir) return;

    int max_size = cover_art_budget_size();

    while (true) {
        if (cover_dir_entry.d_type == DT_REG &&
            file_has_extensions(cover_dir_entry.d_name, cover_image_extensions)) {
            path_t *candidate = path_clone_push(cover_dir, cover_dir_entry.d_name);

            /* Advance scan state now — callback may call us again before we return */
            if (dir_findnext(path_get(cover_dir), &cover_dir_entry) != 0) {
                cover_dir_scan_active = false;
            }

            if (try_cover_path(path_get(candidate), max_size)) {
                path_free(candidate);
                return;
            }
            path_free(candidate);
        } else {
            if (dir_findnext(path_get(cover_dir), &cover_dir_entry) != 0) {
                cover_dir_scan_active = false;
                return;
            }
        }
    }
}

/** Scan the directory for a cover image, preferring known filenames. */
static void scan_directory_for_cover (path_t *dir) {
    int max_size = cover_art_budget_size();

    /* First pass: look for preferred filenames (cover.jpg, folder.png, etc.) */
    for (const char **name = preferred_cover_names; *name; name++) {
        for (const char **ext = cover_image_extensions; *ext; ext++) {
            char filename[64];
            snprintf(filename, sizeof(filename), "%s.%s", *name, *ext);
            path_t *candidate = path_clone_push(dir, filename);

            struct stat st;
            if (stat(path_get(candidate), &st) == 0) {
                if (try_cover_path(path_get(candidate), max_size)) {
                    path_free(candidate);
                    return;
                }
            }
            path_free(candidate);
        }
    }

    /* Second pass: fall back to first image file found */
    if (cover_dir) { path_free(cover_dir); cover_dir = NULL; }
    cover_dir = path_clone(dir);
    if (dir_findfirst(path_get(cover_dir), &cover_dir_entry) == 0) {
        cover_dir_scan_active = true;
        try_next_cover_source();
    }
}

/** Start the cover art loading process for the current track. */
static void load_cover_art (path_t *directory) {
    /* Abort any in-progress decode */
    if (cover_state == COVER_LOADING_JPEG) jpeg_decoder_abort();
    if (cover_state == COVER_LOADING_PNG) png_decoder_abort();
    cover_state = COVER_IDLE;

    /* Free previous image */
    if (cover_image) {
        surface_free(cover_image);
        free(cover_image);
        cover_image = NULL;
    }
    cover_disp_size = 0;
    cover_dir_scan_active = false;

    const id3_metadata_t *meta = mp3player_get_metadata();

    /* Try embedded cover art first */
    if (meta->has_cover_art && meta->cover_art_path[0]) {
        int max_size = cover_art_budget_size();
        if (try_cover_path(meta->cover_art_path, max_size)) return;
    }

    /* Fall back to directory scan */
    scan_directory_for_cover(directory);
}

static char *convert_error_message (mp3player_err_t err) {
    switch (err) {
        case MP3PLAYER_ERR_OUT_OF_MEM: return "MP3 player failed due to insufficient memory";
        case MP3PLAYER_ERR_IO: return "I/O error during MP3 playback";
        case MP3PLAYER_ERR_NO_FILE: return "No MP3 file is loaded";
        case MP3PLAYER_ERR_INVALID_FILE: return "Invalid MP3 file";
        default: return "Unknown MP3 player error";
    }
}

static void format_time (char *buffer, float seconds) {
    int s = (int)seconds;
    if (s >= 3600) {
        sprintf(buffer, "%02d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
    } else {
        sprintf(buffer, "%02d:%02d", s / 60, s % 60);
    }
}


/* Build the shuffle list by collecting all music file indices, then Fisher-Yates shuffle. */
static void build_shuffle_list (menu_t *menu) {
    int count = menu->browser.entries;

    /* Count music files first */
    int music_count = 0;
    for (int i = 0; i < count; i++) {
        if (menu->browser.list[i].type == ENTRY_TYPE_MUSIC) {
            music_count++;
        }
    }

    /* Free old list if any */
    if (shuffle_list) {
        free(shuffle_list);
        shuffle_list = NULL;
    }

    shuffle_count = 0;
    shuffle_pos = 0;

    if (music_count == 0) return;

    shuffle_list = malloc(music_count * sizeof(int));
    if (!shuffle_list) return;

    /* Collect indices */
    for (int i = 0; i < count; i++) {
        if (menu->browser.list[i].type == ENTRY_TYPE_MUSIC) {
            shuffle_list[shuffle_count++] = i;
        }
    }

    /* Fisher-Yates shuffle, seeded from hardware entropy */
    srand(getentropy32());
    for (int i = shuffle_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = shuffle_list[i];
        shuffle_list[i] = shuffle_list[j];
        shuffle_list[j] = tmp;
    }
}

/** Try to load and play the track at the given browser index.
 *  Returns true on success. */
static bool try_play_index (menu_t *menu, int idx) {
    if (idx < 0 || idx >= menu->browser.entries) return false;

    entry_t *e = &menu->browser.list[idx];
    if (e->type != ENTRY_TYPE_MUSIC) return false;

    path_t *path = path_clone_push(menu->browser.directory, e->name);
    mp3player_err_t err = mp3player_load(path_get(path));
    path_free(path);
    if (err != MP3PLAYER_OK) return false;

    err = mp3player_play();
    if (err != MP3PLAYER_OK) return false;

    menu->browser.selected = idx;
    menu->browser.entry = e;
    advance_failed = false;
    ticker_offset = 0.0f;
    load_cover_art(menu->browser.directory);
    return true;
}

/** Try to skip to the next (direction=1) or previous (direction=-1) music file.
 *  Behavior depends on the current playback mode. */
static bool try_skip_track (menu_t *menu, int direction) {
    int current = menu->browser.selected;
    int count = menu->browser.entries;

    if (playback_mode == PLAYBACK_REPEAT_ONE) {
        /* Reload and restart the current track */
        return try_play_index(menu, current);
    }

    if (playback_mode == PLAYBACK_SHUFFLE || playback_mode == PLAYBACK_PARTY) {
        if (!shuffle_list || shuffle_count == 0) return false;

        shuffle_pos += direction;

        /* Walked before the start */
        if (shuffle_pos < 0) {
            shuffle_pos = 0;
            return false;
        }

        /* Walked past the end */
        if (shuffle_pos >= shuffle_count) {
            if (playback_mode == PLAYBACK_PARTY) {
                build_shuffle_list(menu);
                shuffle_pos = 0;
            } else {
                return false;
            }
        }

        /* Try each remaining entry in the shuffle list, skip broken files */
        while (shuffle_pos < shuffle_count) {
            if (try_play_index(menu, shuffle_list[shuffle_pos])) return true;
            shuffle_pos++;
        }
        return false;
    }

    /* Normal and Loop: scan linearly */
    for (int idx = current + direction; idx >= 0 && idx < count; idx += direction) {
        if (try_play_index(menu, idx)) return true;
    }

    /* Loop mode wraps to the other end of the directory */
    if (playback_mode == PLAYBACK_LOOP) {
        int start = (direction > 0) ? 0 : count - 1;
        int stop = current;
        for (int idx = start; idx != stop; idx += direction) {
            if (try_play_index(menu, idx)) return true;
        }
    }

    return false;
}

static void process (menu_t *menu) {
    mp3player_err_t err;

    err = mp3player_process();
    if (err != MP3PLAYER_OK) {
        menu_show_error(menu, convert_error_message(err));
        return;
    }

    /* Auto-advance to next track when current one finishes */
    if (mp3player_is_finished() && !advance_failed) {
        if (!try_skip_track(menu, 1)) {
            advance_failed = true;
        }
    }

    if (menu->actions.back) {
        sound_play_effect(SFX_EXIT);
        menu->next_mode = MENU_MODE_BROWSER;
    } else if (menu->actions.enter) {
        err = mp3player_toggle();
        if (err != MP3PLAYER_OK) {
            menu_show_error(menu, convert_error_message(err));
        }
        sound_play_effect(SFX_ENTER);
    } else if (menu->actions.options) {
        /* R button: cycle playback mode */
        playback_mode = (playback_mode + 1) % PLAYBACK_MODE_COUNT;
        advance_failed = false;
        if (playback_mode == PLAYBACK_SHUFFLE || playback_mode == PLAYBACK_PARTY) {
            build_shuffle_list(menu);
        }
    } else if (menu->actions.go_up || menu->actions.go_down) {
        try_skip_track(menu, menu->actions.go_up ? -1 : 1);
    } else if (menu->actions.go_left || menu->actions.go_right) {
        int seconds = menu->actions.go_fast ? SEEK_SECONDS_FAST : SEEK_SECONDS;
        err = mp3player_seek(menu->actions.go_left ? (-seconds) : seconds);
        if (err != MP3PLAYER_OK) {
            menu_show_error(menu, convert_error_message(err));
        }
    }
}

static void draw (menu_t *menu, surface_t *d) {
    if (cover_state == COVER_LOADING_PNG) {
        png_decoder_poll();
    }

    rdpq_attach(d, NULL);

    ui_components_background_draw();

    ui_components_layout_draw();

    ui_components_seekbar_draw(mp3player_get_progress());

    const id3_metadata_t *meta = mp3player_get_metadata();

    const char *display_title = (meta->has_metadata && meta->title[0])
        ? meta->title : menu->browser.entry->name;

    /* Center the title + ticker as a unit between top edge and art/content */
    int content_top_y = VISIBLE_AREA_Y0 + 54;
    int bar_top_y = SEEKBAR_Y - BORDER_THICKNESS;
    int art_top_approx = cover_image
        ? content_top_y + ((bar_top_y - content_top_y - 16 - cover_disp_size) / 2)
        : content_top_y;

    int header_area_top = VISIBLE_AREA_Y0;
    int header_unit_h = 30; /* title (~15px) + gap + ticker (~15px) */
    int header_y = header_area_top + (art_top_approx - header_area_top - header_unit_h) / 2;

    int ticker_x = SEEKBAR_X + 8;
    int ticker_w = SEEKBAR_WIDTH - 16;
    int title_y = header_y + 12; /* baseline offset */
    int ticker_y = title_y + 16;

    /* Song title */
    rdpq_text_printn(
        &(rdpq_textparms_t) {
            .style_id = STL_DEFAULT,
            .width = SEEKBAR_WIDTH,
            .align = ALIGN_CENTER,
        },
        FNT_DEFAULT,
        SEEKBAR_X, title_y,
        display_title, strlen(display_title)
    );

    char ticker_str[512] = "";
    {
        char *p = ticker_str;
        size_t remaining = sizeof(ticker_str);
        const char *sep = " \xC2\xB7 "; /* " · " UTF-8 middle dot */
        bool need_sep = false;
        if (meta->artist[0]) {
            int n = snprintf(p, remaining, "%s", meta->artist);
            p += n; remaining -= n; need_sep = true;
        }
        if (meta->album[0]) {
            int n = snprintf(p, remaining, "%s%s", need_sep ? sep : "", meta->album);
            p += n; remaining -= n; need_sep = true;
        }
        snprintf(p, remaining, "%s%dHz \xC2\xB7 %.0fkbps",
                 need_sep ? sep : "",
                 mp3player_get_samplerate(),
                 (double)(mp3player_get_bitrate() / 1000));
    }

    size_t ticker_len = strlen(ticker_str);

    if (ticker_len > 0) {
        /* Measure actual pixel width */
        rdpq_textmetrics_t metrics = rdpq_text_printn(
            &(rdpq_textparms_t) { .style_id = STL_DEFAULT },
            FNT_DEFAULT,
            -1000, -1000,
            ticker_str, ticker_len
        );
        int full_width = (int)metrics.advance_x;

        if (full_width <= ticker_w) {
            /* Fits on screen, just center it */
            rdpq_text_printn(
                &(rdpq_textparms_t) {
                    .style_id = STL_DEFAULT,
                    .width = ticker_w,
                    .align = ALIGN_CENTER,
                },
                FNT_DEFAULT,
                ticker_x, ticker_y,
                ticker_str, ticker_len
            );
        } else {
            /* Too wide, scroll it */
            const char *wrap_pad = "     ";
            char scroll_str[576];
            snprintf(scroll_str, sizeof(scroll_str), "%s%s%s%s", ticker_str, wrap_pad, ticker_str, wrap_pad);
            size_t scroll_len = strlen(scroll_str);

            int cycle_width = full_width + (int)rdpq_text_printn(
                &(rdpq_textparms_t) { .style_id = STL_DEFAULT },
                FNT_DEFAULT, -1000, -1000,
                wrap_pad, strlen(wrap_pad)
            ).advance_x;

            ticker_offset += 0.5f;
            if ((int)ticker_offset >= cycle_width) {
                ticker_offset -= (float)cycle_width;
            }

            rdpq_set_scissor(SEEKBAR_X, ticker_y - 12, SEEKBAR_X + SEEKBAR_WIDTH, ticker_y + 4);

            rdpq_text_printn(
                &(rdpq_textparms_t) { .style_id = STL_DEFAULT },
                FNT_DEFAULT,
                ticker_x - (int)ticker_offset, ticker_y,
                scroll_str, scroll_len
            );

            rdpq_set_scissor(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        }
    }

    /* Build the queue view: list of browser indices for music files */
    int queue_indices[64];
    int queue_count = 0;
    int queue_current = -1;

    if (playback_mode == PLAYBACK_SHUFFLE || playback_mode == PLAYBACK_PARTY) {
        /* Use the shuffle list directly */
        if (shuffle_list && shuffle_count > 0) {
            for (int i = 0; i < shuffle_count && queue_count < 64; i++) {
                queue_indices[queue_count] = shuffle_list[i];
                if (i == shuffle_pos) queue_current = queue_count;
                queue_count++;
            }
        }
    } else {
        /* Normal/Loop/Repeat: scan browser list for music files in order */
        for (int i = 0; i < menu->browser.entries && queue_count < 64; i++) {
            if (menu->browser.list[i].type == ENTRY_TYPE_MUSIC) {
                if (i == menu->browser.selected) queue_current = queue_count;
                queue_indices[queue_count++] = i;
            }
        }
    }

    /* Compute the content area between ticker and seekbar */
    int content_top = VISIBLE_AREA_Y0 + 54;
    int content_bottom = SEEKBAR_Y - BORDER_THICKNESS - 8;
    int content_h = content_bottom - content_top;

    int art_size = cover_image ? cover_disp_size : 0;
    int max_queue_lines = content_h / QUEUE_LINE_HEIGHT;
    if (max_queue_lines < 1) max_queue_lines = 1;
    if (max_queue_lines > 15) max_queue_lines = 15;

    /* Determine visible queue window */
    int window_start = 0;
    int window_end = queue_count;

    if (queue_count > max_queue_lines && queue_current >= 0) {
        /* List doesn't fit, scroll with 2 previous tracks for context */
        int prev_context = (playback_mode == PLAYBACK_SHUFFLE || playback_mode == PLAYBACK_PARTY) ? 0 : 2;
        window_start = queue_current - prev_context;
        if (window_start < 0) window_start = 0;
        window_end = window_start + max_queue_lines;
        if (window_end > queue_count) {
            window_end = queue_count;
            window_start = queue_count - max_queue_lines;
            if (window_start < 0) window_start = 0;
        }
    }

    /* Calculate the queue text width */
    int queue_w = 280;

    /* Layout: art on left, queue on right, centered as a unit */
    int block_w;
    int art_x, art_y;
    int queue_x, queue_y;

    if (cover_image && art_size > 0) {
        block_w = art_size + QUEUE_GAP + queue_w;
        int block_x = DISPLAY_CENTER_X - block_w / 2;
        art_x = block_x;
        art_y = content_top + (content_h - art_size) / 2;
        queue_x = block_x + art_size + QUEUE_GAP;
    } else {
        block_w = queue_w;
        queue_x = DISPLAY_CENTER_X - queue_w / 2;
    }

    int visible_queue_h = (window_end - window_start) * QUEUE_LINE_HEIGHT;

    if (cover_image && art_size > 0) {
        /* Align queue top with cover art top, offset for text baseline */
        queue_y = art_y + 12;
    } else {
        /* No art: center the queue vertically */
        queue_y = content_top + (content_h - visible_queue_h) / 2;
    }

    /* Render cover art */
    if (cover_image) {
        int crop = (cover_image->width < cover_image->height)
                   ? cover_image->width : cover_image->height;
        rdpq_mode_push();
        rdpq_set_mode_standard();
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
        rdpq_mode_blender(0);
        rdpq_tex_blit(cover_image, art_x, art_y, &(rdpq_blitparms_t){
            .s0 = cover_s0, .t0 = cover_t0,
            .width = crop, .height = crop,
            .scale_x = (float)cover_disp_size / crop,
            .scale_y = (float)cover_disp_size / crop,
            .filtering = true,
        });
        rdpq_mode_pop();
    }

    /* Render queue list */
    if (queue_count > 0) {
        for (int i = window_start; i < window_end; i++) {
            int idx = queue_indices[i];
            entry_t *e = &menu->browser.list[idx];

            /* Strip .mp3 extension for cleaner display */
            char name_buf[64];
            strncpy(name_buf, e->name, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
            char *dot = strrchr(name_buf, '.');
            if (dot) *dot = '\0';

            char line_buf[80];
            if (i == queue_current) {
                snprintf(line_buf, sizeof(line_buf), QUEUE_ARROW "%s", name_buf);
            } else {
                snprintf(line_buf, sizeof(line_buf), "  %s", name_buf);
            }

            int line_y = queue_y + (i - window_start) * QUEUE_LINE_HEIGHT;

            int style = (i == queue_current) ? STL_DEFAULT : STL_GRAY;

            rdpq_text_printn(
                &(rdpq_textparms_t) {
                    .style_id = style,
                    .width = queue_w,
                    .wrap = WRAP_ELLIPSES,
                },
                FNT_DEFAULT,
                queue_x, line_y,
                line_buf, strlen(line_buf)
            );
        }
    }

    /* Draw elapsed and duration times on the seekbar */
    char elapsed_str[16];
    char duration_str[16];
    float duration = mp3player_get_duration();
    format_time(elapsed_str, duration * mp3player_get_progress());
    format_time(duration_str, duration);

    int time_y = SEEKBAR_Y + 18;

    rdpq_text_printn(
        &(rdpq_textparms_t) { .style_id = STL_DEFAULT },
        FNT_DEFAULT,
        ticker_x, time_y,
        elapsed_str, strlen(elapsed_str)
    );

    rdpq_text_printn(
        &(rdpq_textparms_t) {
            .style_id = STL_DEFAULT,
            .width = ticker_w,
            .align = ALIGN_RIGHT,
        },
        FNT_DEFAULT,
        ticker_x, time_y,
        duration_str, strlen(duration_str)
    );

    ui_components_actions_bar_text_draw(
        STL_DEFAULT,
        ALIGN_LEFT, VALIGN_TOP,
        "A: %s\n"
        "B: Exit\n",
        mp3player_is_playing() ? "Pause" : mp3player_is_finished() ? "Play again" : "Play"
    );

    ui_components_actions_bar_text_draw(
        STL_DEFAULT,
        ALIGN_CENTER, VALIGN_TOP,
        "◀ Rewind | Fast forward ▶\n"
        "▲ Prev track | Next track ▼\n"
    );

    ui_components_actions_bar_text_draw(
        STL_DEFAULT,
        ALIGN_RIGHT, VALIGN_TOP,
        "R: %s\n",
        playback_mode_name[playback_mode]
    );

    rdpq_detach_show();
}

static void deinit (void) {
    if (cover_state == COVER_LOADING_JPEG) jpeg_decoder_abort();
    if (cover_state == COVER_LOADING_PNG) png_decoder_abort();
    cover_state = COVER_IDLE;
    if (cover_image) {
        surface_free(cover_image);
        free(cover_image);
        cover_image = NULL;
    }
    cover_disp_size = 0;
    if (cover_dir) {
        path_free(cover_dir);
        cover_dir = NULL;
    }
    cover_dir_scan_active = false;

    if (shuffle_list) {
        free(shuffle_list);
        shuffle_list = NULL;
    }
    shuffle_count = 0;

    sound_init_default();
    mp3player_deinit();
}


void view_music_player_init (menu_t *menu) {
    mp3player_err_t err;

    playback_mode = PLAYBACK_NORMAL;
    advance_failed = false;
    ticker_offset = 0.0f;

    err = mp3player_init();
    if (err != MP3PLAYER_OK) {
        menu_show_error(menu, convert_error_message(err));
        mp3player_deinit();
        return;
    }

    path_t *path = path_clone_push(menu->browser.directory, menu->browser.entry->name);

    err = mp3player_load(path_get(path));
    if (err != MP3PLAYER_OK) {
        menu_show_error(menu, convert_error_message(err));
        mp3player_deinit();
    } else {
        sound_init_mp3_playback();
        mp3player_mute(false);
        err = mp3player_play();
        if (err != MP3PLAYER_OK) {
            menu_show_error(menu, convert_error_message(err));
            mp3player_deinit();
        } else {
            load_cover_art(menu->browser.directory);
        }
    }

    path_free(path);
}

void view_music_player_display (menu_t *menu, surface_t *display) {
    process(menu);

    draw(menu, display);

    if (menu->next_mode != MENU_MODE_MUSIC_PLAYER) {
        deinit();
    }
}
