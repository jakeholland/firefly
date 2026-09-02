#ifndef FF_FLARE_MARK_H
#define FF_FLARE_MARK_H

/**
 * ff_flare_mark.h — pure geometry for the Firefly flare mark: the
 * eight-ray burst with unequal ray lengths and a long north ray
 * (TRADEMARKS.md: "the eight-ray burst logo with unequal rays and a long
 * north ray").
 *
 * Shared, core-free (no LVGL types, no core domain state, header-only —
 * no .c file, no CMake registration needed on either side) so the two
 * places in this repo that draw the mark stay pixel-identical instead of
 * two hand-copied literals that can silently drift apart:
 *   - app/screens/scr_flare.c's flare_build_mark — the LVGL flare
 *     takeover screen, the mark's original home (PR #20 UX review).
 *   - targets/esp32s3/components/ff_display/ff_display.c's
 *     ff_display_draw_boot_splash — the raw pre-LVGL boot splash (S26g,
 *     "can the boot animation be the flare animation?").
 *
 * Lives in core/include (not app/) purely as a neutral include path both
 * consumers already reach: ff-app-ui links ff-core transitively (via
 * ff-app -> ff-core), and the esp32s3 target's ff_display component's
 * CMakeLists.txt already REQUIRES ff_core and already includes one core
 * header (ff_touchcal.h) despite ff_display.h's older "NONE of this
 * touches core/ or app/" doc-comment claim — that claim was already
 * stale before this change; see ff_display.h's updated note. This header
 * adds no NEW category of coupling, just a second core header on the
 * same existing path.
 *
 * Plain data plus one `static inline` helper: no core .c source, no core
 * symbol, no link-time cost to include, and nothing here reads or writes
 * core domain state (ff_flare_t and friends) — this is geometry only.
 */

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FF_FLARE_MARK_N_RAYS 8

/* Fractions of the mark's max ray length, indexed CLOCKWISE from north
 * (index 0 = straight up — the deliberate standout ray, spec: "a long
 * north ray"). Taper UNEVENLY rather than a repeating long/short
 * alternation, so the shape reads as "a burst with a direction" and not
 * a generic sunburst/loading-spinner silhouette — PR #20 UX review
 * (BLOCKING), the finding that produced this exact table. Values
 * verbatim from scr_flare.c's original FLARE_MARK_RAY_FRAC; do not
 * renormalize or re-derive. */
static const float FF_FLARE_MARK_RAY_FRAC[FF_FLARE_MARK_N_RAYS] = {
    1.00f, 0.52f, 0.62f, 0.46f, 0.58f, 0.46f, 0.62f, 0.52f,
};

/* Canonical mark scale, in pixels, at the size both current consumers
 * happen to share: the takeover screen's puck (FF_THEME_PUCK_PX) and the
 * boot splash's panel (FF_LCD_H_RES/FF_LCD_V_RES) are both 412x412. A
 * future consumer at a different canvas size should scale
 * FF_FLARE_MARK_RAY_FRAC by its own max_len rather than assume these two
 * literals apply unchanged — see ff_flare_mark_ray_offset below. */
#define FF_FLARE_MARK_MAX_LEN_PX 42.0f
#define FF_FLARE_MARK_CENTER_R_PX 7.0f
/* Ray stroke width — scr_flare.c's `lv_obj_set_style_line_width(line, 5,
 * 0)` verbatim. Shared for the same reason as the length/radius above:
 * ray THICKNESS is as much a part of "the look" as ray length is, and a
 * rasterized capsule test (the boot splash's raw path has no LVGL line
 * object to style) needs the half-width as a number either way. */
#define FF_FLARE_MARK_LINE_W_PX 5.0f

/* Avoid the POSIX-only M_PI (undefined under strict -std=c11 on some
 * libcs) — same rationale as core/src/ff_geo.c's own FF_GEO_PI. Kept as
 * its own float literal (not #include "ff_geo.h") deliberately: geo's PI
 * is a `double` used for lat/lon precision, and pulling that header in
 * here would add a coupling this header's whole point is to avoid. */
#define FF_FLARE_MARK_PI 3.14159265358979323846f

/**
 * ff_flare_mark_ray_offset — the (dx,dy) endpoint of ray `i`, relative to
 * the mark's center, for a mark whose longest ray is `max_len` pixels.
 * `i` is clockwise from north (0 = straight up); screen +Y is DOWN, so
 * "up" is -Y — the same convention every screen coordinate space in this
 * repo already agrees on (LVGL and the raw panel buffer alike). Pure
 * math, no state: safe to call from a headless test or a pre-LVGL boot
 * path alike. `i` is taken mod FF_FLARE_MARK_N_RAYS so a caller iterating
 * with a plain `int` never needs its own bounds check.
 */
static inline void ff_flare_mark_ray_offset(int i, float max_len, float *dx, float *dy)
{
    int const idx = ((i % FF_FLARE_MARK_N_RAYS) + FF_FLARE_MARK_N_RAYS) % FF_FLARE_MARK_N_RAYS;
    float const angle_deg = (float)idx * (360.0f / (float)FF_FLARE_MARK_N_RAYS);
    float const rad = angle_deg * FF_FLARE_MARK_PI / 180.0f;
    float const len = max_len * FF_FLARE_MARK_RAY_FRAC[idx];
    *dx = sinf(rad) * len;
    *dy = -cosf(rad) * len;
}

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_MARK_H */
