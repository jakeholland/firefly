/**
 * lv_conf.h — Firefly sim-target LVGL v9 configuration.
 *
 * Minimal overrides only; every option not listed here falls back to the
 * documented default in lvgl/src/lv_conf_internal.h. See lv_conf_template.h
 * in the LVGL source tree (vendored via FetchContent) for the full option
 * reference.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* 32bpp (XRGB8888) keeps pixel layout simple for the stb_image_write PNG
 * dump in the headless screenshot path. */
#define LV_COLOR_DEPTH 32

/* SDL2 window + mouse driver for --window mode. Headless rendering does
 * not touch this path — it draws straight to an LVGL software buffer. */
#define LV_USE_SDL 1
#if LV_USE_SDL
    #define LV_SDL_INCLUDE_PATH     <SDL.h>
    #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
    #define LV_SDL_BUF_COUNT        1
    #define LV_SDL_ACCELERATED      1
    #define LV_SDL_FULLSCREEN       0
    #define LV_SDL_DIRECT_EXIT      1
#endif

/* One label ("FIREFLY") on a flat background — the 14px default font is
 * legible at the sim's scale without pulling in extra sizes. */
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_24

#endif /* LV_CONF_H */
