#include <stdlib.h>

#include "../mp3_player.h"
#include "../sound.h"
#include "../path.h"
#include "../ui_components/constants.h"
#include "views.h"


#define SEEK_SECONDS        (5)
#define SEEK_SECONDS_FAST   (60)

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

/* Shuffle state: array of browser indices that point to music files */
static int *shuffle_list = NULL;
static int shuffle_count = 0;
static int shuffle_pos = 0;


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

        /* Prev in shuffle restarts the current track */
        if (direction < 0) {
            return try_play_index(menu, current);
        }

        shuffle_pos++;

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
    rdpq_attach(d, NULL);

    ui_components_background_draw();

    ui_components_layout_draw();

    ui_components_seekbar_draw(mp3player_get_progress());

    const id3_metadata_t *meta = mp3player_get_metadata();

    const char *display_title = (meta->has_metadata && meta->title[0])
        ? meta->title : menu->browser.entry->name;

    /* Song title at top */
    ui_components_main_text_draw(
        STL_DEFAULT,
        ALIGN_CENTER, VALIGN_TOP,
        "%s",
        display_title
    );

    /* Ticker with metadata below the title */
    int ticker_x = SEEKBAR_X + 8;
    int ticker_w = SEEKBAR_WIDTH - 16;
    int ticker_y = VISIBLE_AREA_Y0 + 42;

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
        if (meta->track_number > 0) {
            int n = snprintf(p, remaining, "%s%d", need_sep ? sep : "", meta->track_number);
            p += n; remaining -= n; need_sep = true;
        }
        snprintf(p, remaining, "%s%dHz %.0fkbps",
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
