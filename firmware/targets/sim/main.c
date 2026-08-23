/**
 * ffsim — Firefly desktop sim target (S13 slice a).
 *
 * Two modes:
 *   ffsim                          window mode: opens an SDL window,
 *                                  runs LVGL's normal timer loop.
 *   ffsim --headless --screenshot DIR
 *                                  renders exactly one frame into an
 *                                  offscreen LVGL buffer (no SDL, no
 *                                  display server required) and writes
 *                                  it to DIR/boot.png, then exits 0.
 *
 * The boot screen itself is scaffolding: a dark puck circle with the
 * centered "FIREFLY" wordmark. Real screens arrive with S06+.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "lvgl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "ff_version.h"

#define FF_SIM_WINDOW_W 456
#define FF_SIM_WINDOW_H 456

#define FF_COLOR_BG_DARK 0x0b0b10
#define FF_COLOR_AMBER   0xffc66b

/* Builds the boot placeholder UI (dark puck + centered "FIREFLY" label)
 * on whatever the current default display's active screen is. */
static void ff_build_boot_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_SIM_WINDOW_W - 16, FF_SIM_WINDOW_H - 16);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(puck);
    lv_label_set_text(label, "FIREFLY");
    lv_obj_set_style_text_color(label, lv_color_hex(FF_COLOR_AMBER), 0);
    lv_obj_center(label);
}

/* Full-frame render mode: the whole buffer is the flushed frame, so the
 * flush callback only needs to signal completion. */
static void ff_headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

/* Converts an LVGL XRGB8888 framebuffer (byte order B,G,R,X per pixel;
 * see lv_color32_t) to tightly packed RGB24 and writes it as a PNG. */
static int ff_write_boot_png(const char *dir)
{
    const int32_t w = FF_SIM_WINDOW_W;
    const int32_t h = FF_SIM_WINDOW_H;
    const uint32_t buf_size = (uint32_t)(w * h * 4);

    uint8_t *xrgb_buf = malloc(buf_size);
    if (xrgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %u byte framebuffer\n", buf_size);
        return 1;
    }

    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_buffers(disp, xrgb_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, ff_headless_flush_cb);
    lv_display_set_default(disp);

    ff_build_boot_screen();
    lv_refr_now(disp);

    uint8_t *rgb_buf = malloc((size_t)w * (size_t)h * 3);
    if (rgb_buf == NULL) {
        fprintf(stderr, "ffsim: out of memory allocating %ld byte PNG buffer\n", (long)w * h * 3);
        free(xrgb_buf);
        return 1;
    }
    for (int32_t i = 0; i < w * h; i++) {
        const uint8_t b = xrgb_buf[i * 4 + 0];
        const uint8_t g = xrgb_buf[i * 4 + 1];
        const uint8_t r = xrgb_buf[i * 4 + 2];
        rgb_buf[i * 3 + 0] = r;
        rgb_buf[i * 3 + 1] = g;
        rgb_buf[i * 3 + 2] = b;
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/boot.png", dir);

    int ok = stbi_write_png(path, w, h, 3, rgb_buf, w * 3);

    free(rgb_buf);
    free(xrgb_buf);

    if (!ok) {
        fprintf(stderr, "ffsim: failed to write %s\n", path);
        return 1;
    }

    printf("ffsim: wrote %s\n", path);
    return 0;
}

static int ff_run_headless(const char *screenshot_dir)
{
    lv_init();
    int rc = ff_write_boot_png(screenshot_dir);
    lv_deinit();
    return rc;
}

static void ff_run_window(void)
{
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)SDL_GetTicks);

    lv_display_t *disp = lv_sdl_window_create(FF_SIM_WINDOW_W, FF_SIM_WINDOW_H);
    lv_sdl_window_set_title(disp, "Firefly (ffsim)");
    lv_sdl_mouse_create();

    ff_build_boot_screen();

    while (true) {
        uint32_t next_ms = lv_timer_handler();
        SDL_Delay(next_ms > 0 ? next_ms : 1);
    }
}

int main(int argc, char **argv)
{
    bool headless = false;
    const char *screenshot_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_dir = argv[++i];
        }
    }

    printf("ffsim: %s\n", ff_version_string());

    if (headless) {
        if (screenshot_dir == NULL) {
            fprintf(stderr, "ffsim: --headless requires --screenshot DIR\n");
            return 1;
        }
        return ff_run_headless(screenshot_dir);
    }

    ff_run_window();
    return 0;
}
