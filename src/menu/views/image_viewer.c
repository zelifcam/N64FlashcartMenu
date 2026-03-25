#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "../sound.h"

#include "../jpeg_decoder.h"
#include "../png_decoder.h"
#include "views.h"

#ifdef HEAP_DEBUG
#define heap_debugf(tag) do { \
    heap_stats_t _h; sys_get_heap_stats(&_h); \
    debugf("[HEAP] %-30s used=%lu free=%lu\n", \
           (tag), (unsigned long)_h.used, \
           (unsigned long)(_h.total - _h.used)); \
} while(0)
#else
#define heap_debugf(tag) ((void)0)
#endif


static bool show_message;
static bool image_loading;
static bool image_set_as_background;
static bool is_jpeg;
static surface_t *image;


static char *convert_error_message (int err, bool jpeg) {
    if (jpeg) {
        switch ((jpeg_err_t)err) {
            case JPEG_ERR_INT: return "Internal JPEG decoder error";
            case JPEG_ERR_BUSY: return "JPEG decode already in process";
            case JPEG_ERR_OUT_OF_MEM: return "JPEG decode failed due to insufficient memory";
            case JPEG_ERR_NO_FILE: return "JPEG decoder couldn't open file";
            case JPEG_ERR_BAD_FILE: return "Invalid JPEG file";
            default: return "Unknown JPEG decoder error";
        }
    } else {
        switch ((png_err_t)err) {
            case PNG_ERR_INT: return "Internal PNG decoder error";
            case PNG_ERR_BUSY: return "PNG decode already in process";
            case PNG_ERR_OUT_OF_MEM: return "PNG decode failed due to insufficient memory";
            case PNG_ERR_NO_FILE: return "PNG decoder couldn't open file";
            case PNG_ERR_BAD_FILE: return "Invalid PNG file";
            default: return "Unknown PNG decoder error";
        }
    }
}

static void png_callback (png_err_t err, surface_t *decoded_image, void *callback_data) {
    menu_t *menu = (menu_t *)(callback_data);

    image_loading = false;
    image = decoded_image;
    heap_debugf("imgview_png_cb: done");

    if (err != PNG_OK) {
        menu_show_error(menu, convert_error_message(err, false));
    }
}

static void jpeg_callback (jpeg_err_t err, surface_t *decoded_image, void *callback_data) {
    menu_t *menu = (menu_t *)(callback_data);

    image_loading = false;
    image = decoded_image;
    heap_debugf("imgview_jpeg_cb: done");

    if (err != JPEG_OK) {
        menu_show_error(menu, convert_error_message(err, true));
    }
}


static void process (menu_t *menu) {
    if (menu->actions.back) {
        if (show_message) {
            show_message = false;
        } else {
            menu->next_mode = MENU_MODE_BROWSER;
        }
        sound_play_effect(SFX_EXIT);
    } else if (menu->actions.enter && image) {
        if (show_message) {
            show_message = false;
            image_set_as_background = true;
            menu->next_mode = MENU_MODE_BROWSER;
        } else {
            show_message = true;
        }
        sound_play_effect(SFX_ENTER);
    }
}

static void draw (menu_t *menu, surface_t *d) {
    if (!image) {
        rdpq_attach(d, NULL);

        ui_components_background_draw();

        float progress = is_jpeg ? jpeg_decoder_get_progress() : png_decoder_get_progress();
        ui_components_loader_draw(progress, "Loading image...");
    } else {
        rdpq_attach_clear(d, NULL);

        uint16_t x = (d->width / 2) - (image->width / 2);
        uint16_t y = (d->height / 2) - (image->height / 2);

        rdpq_mode_push();
            rdpq_set_mode_copy(false);
            rdpq_tex_blit(image, x, y, NULL);
        rdpq_mode_pop();

        if (show_message) {
            ui_components_messagebox_draw(
                "Set \"%s\" as background image?\n\n"
                "A: Yes, B: Back",
                menu->browser.entry->name
            );
        } else if (image_set_as_background) {
            ui_components_messagebox_draw("Preparing background…");
        }
    }

    rdpq_detach_show();
}

static void deinit (menu_t *menu) {
    heap_debugf("imgview_deinit: enter");
    if (image_loading) {
        if (is_jpeg) {
            jpeg_decoder_abort();
        } else {
            png_decoder_abort();
        }
    }

    if (image) {
        if (image_set_as_background) {
            ui_components_background_replace_image(image);
        } else {
            surface_free(image);
            free(image);
        }
    }
    image = NULL;
    heap_debugf("imgview_deinit: done");
}


void view_image_viewer_init (menu_t *menu) {
    heap_debugf("imgview_init: enter");
    show_message = false;
    image_loading = true;
    image_set_as_background = false;
    image = NULL;

    path_t *path = path_clone_push(menu->browser.directory, menu->browser.entry->name);
    const char *ext = strrchr(menu->browser.entry->name, '.');
    is_jpeg = (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0));

    if (is_jpeg) {
        jpeg_err_t err = jpeg_decoder_start(path_get(path), 640, 480,
                                            jpeg_callback, menu);
        if (err != JPEG_OK) {
            image_loading = false;
            menu_show_error(menu, convert_error_message(err, true));
        }
    } else {
        png_err_t err = png_decoder_start(path_get(path), 640, 480,
                                          png_callback, menu);
        if (err != PNG_OK) {
            image_loading = false;
            menu_show_error(menu, convert_error_message(err, false));
        }
    }

    path_free(path);
}

void view_image_viewer_display (menu_t *menu, surface_t *display) {
    process(menu);

    /* Poll the active decoder */
    if (image_loading) {
        if (is_jpeg) {
            jpeg_decoder_poll();
        } else {
            png_decoder_poll();
        }
    }

    draw(menu, display);

    if (menu->next_mode != MENU_MODE_IMAGE_VIEWER) {
        deinit(menu);
    }
}
