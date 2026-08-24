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

/* Boot placeholder / S13 debug face: one label ("FIREFLY") on a flat
 * background — kept as the LVGL default so that pre-S06 rendering
 * (fixture_view.c, main.c's boot screen) is unaffected. */
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_24

/* S06 (app/theme/ff_theme.h): the radar face's type scale. Nearest
 * built-in sizes to the mockup's 21px name / 36px distance / ~11-12px
 * chip — see ff_theme.h's own comment for the exact rounding rationale
 * (rounds UP to clear docs/review/ux-raver.md's legibility floor, never
 * down). LV_FONT_MONTSERRAT_20 backs NOFIX/NOSEL headline text. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_36 1

/* S06 (app/screens/scr_nav.c): three-face swipe shell. */
#define LV_USE_TILEVIEW 1

/* S06 (app/screens/scr_radar.c): the crew-colored wedge ring on a cluster
 * marker (issue #18). LVGL's own default for this is already 1, but it is
 * declared explicitly here because a rendering feature the radar face
 * cannot draw without should fail at configure time in a stripped-down
 * build, not silently render markers with no ring at all. */
#define LV_USE_ARC 1

#endif /* LV_CONF_H */
