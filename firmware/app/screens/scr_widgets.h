/**
 * scr_widgets.h — app/screens: shared small-widget factories used by more
 * than one screen file.
 *
 * S17 debt cleanup (docs/specs/S17-usability-hardening.md's tap-target
 * work surfaced three near-identical "rounded pill button with a
 * centered label" builders that had drifted into three separate files —
 * `scr_flare.c`'s `flare_make_button`, `scr_power_menu.c`'s
 * `power_menu_make_button`, `scr_settings.c`'s `settings_make_pill` — each
 * a hand-rolled copy of the same shape with slightly different knobs
 * (filled/outlined + press feedback vs. a flat labeled chip with no press
 * state; align-centered-with-offset vs. absolute positioning; different
 * fonts). `scr_power_menu.c`'s own header comment used to call the
 * duplication with `scr_flare.c` deliberate ("nothing to import, both
 * file-static") — that call is reversed here: three copies of the same
 * ~30-line shape is exactly the kind of drift this repo's `AGENTS.md`
 * standing brief warns about once a THIRD copy (`settings_make_pill`)
 * showed up wanting the same shape with different constants, not a
 * different shape.
 *
 * `ff_scr_pill_create` is parameterized (filled/outlined, colours, press
 * feedback style, font, letter-spacing, and either alignment-with-offset
 * or absolute positioning) precisely so each of the three original call
 * sites renders BYTE-IDENTICAL to its pre-refactor pixels — this is a
 * pure extraction, not a restyle. See `./tests/run_goldens.sh`'s 72/72
 * byte-identical result in this PR for the proof.
 */
#ifndef FF_SCR_WIDGETS_H
#define FF_SCR_WIDGETS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_pill_press_t — which press-feedback style (if any) the pill gets.
 * LV_STATE_PRESSED-only in every case, so the resting render (what the
 * goldens capture) is never touched by this — only a real touch-down.
 *
 *  - NONE: no press style at all (`settings_make_pill`'s pills — the
 *    settings face uses its own row-level dim/highlight conventions
 *    instead, decided by the caller before the pill is ever built).
 *  - DIM:  a filled pill's own convention — a solid-color fill would
 *    flash a colour-on-itself if it tried to tint toward its own bg, so
 *    it darkens instead (an FF_THEME_COLOR_INK wash at LV_OPA_20).
 *  - TINT: an outlined pill's convention — its resting fill is the
 *    neutral surface colour, so it tints toward `press_tint_hex` at
 *    LV_OPA_40 on touch-down.
 */
typedef enum {
    FF_SCR_PILL_PRESS_NONE = 0,
    FF_SCR_PILL_PRESS_DIM,
    FF_SCR_PILL_PRESS_TINT,
} ff_scr_pill_press_t;

/**
 * ff_scr_pill_cfg_t — every knob the three original pill builders needed,
 * so each call site can reproduce its own exact pre-refactor styling.
 * Zero-initialize (`= {0}`) and set only the fields that matter for a
 * given call — every field's "unset" value (0 / false / NULL) is chosen
 * to be the least-surprising default (no border, no letter-spacing
 * override, align-centered rather than absolute-positioned, no press
 * feedback, no click handler).
 */
typedef struct {
    int32_t w;
    int32_t h;

    /* Positioning: EITHER absolute (use_pos=true: lv_obj_set_pos(x, y),
     * `settings_make_pill`'s convention — settings positions every row's
     * children in list-relative coordinates) OR centered-with-vertical-
     * offset (use_pos=false: lv_obj_align(CENTER, 0, dy),
     * `flare_make_button`/`power_menu_make_button`'s convention — both
     * screens lay their buttons out as a vertical stack on the puck). */
    bool use_pos;
    int32_t x;
    int32_t y;
    int32_t dy;

    int32_t radius; /* LV_RADIUS_CIRCLE for a full pill, or an explicit px radius (FF_SETTINGS_PILL_RADIUS) */

    /* filled=true: a solid `bg_hex` fill (GO / Power off / every settings
     * pill). filled=false: an outlined pill — surface-color fill, a
     * `border_width`-px border in `bg_hex` (DISMISS / Reboot / Cancel;
     * settings never uses this). */
    bool filled;
    int32_t border_width;

    uint32_t bg_hex;
    uint32_t fg_hex; /* label color */

    ff_scr_pill_press_t press;
    uint32_t press_tint_hex; /* FF_SCR_PILL_PRESS_TINT only — the color an outlined pill's fill tints toward on press */

    lv_font_t const *font;
    int32_t letter_space; /* 0 = leave LVGL's font default untouched */

    lv_event_cb_t cb; /* NULL = no click handler wired */
    void *user_data;
} ff_scr_pill_cfg_t;

/**
 * ff_scr_pill_create — build one pill button (via ff_scr_button_create,
 * scr_nav.h — every pill this factory makes gets the shared PRESS_LOCK
 * fix for free, closing #145/#148's class of bug for the three screens
 * that call this) with a centered label, styled per `*cfg`. Returns the
 * button (`lv_obj_get_child(btn, 0)` is always the label — the same
 * contract the three original per-file helpers already had, relied on
 * by `scr_settings.c`'s brightness +/- font override).
 */
lv_obj_t *ff_scr_pill_create(lv_obj_t *parent, char const *text, ff_scr_pill_cfg_t const *cfg);

/**
 * ff_scr_glass_rim_create — build one edge-hugging ring, concentric with
 * the VISIBLE glass rather than the framebuffer (`FF_THEME_GLASS_CX/CY/R`,
 * `docs/hardware/glass-offset.md`), not the puck's own `(206,206)`
 * center.
 *
 * fix/flare-rim-glass-geometry: extracted from `scr_radar.c`'s
 * `radar_build_rim_tint` (the ORIGINAL, on-glass-proven implementation —
 * S06/#154/#155) once the flare sender overlay's own rim
 * (`ff_scr_flare_build_sender_overlay`) turned out to have the exact same
 * bug the radar rim was fixed for: it was still sized/aligned against the
 * framebuffer center (`FF_THEME_PUCK_PX - 4`, `lv_obj_center`), so on real
 * glass it showed the identical "opposite-side arc" sliver radar used to.
 * Rather than re-derive the fix a second time (and risk the two drifting
 * again the next time a board's measured offset changes), every rim now
 * shares this ONE function that knows the glass geometry. `scr_radar.c`'s
 * `radar_build_rim_tint` is now a thin wrapper over this with `border_px`
 * pinned at 5 — same size (`2 * FF_THEME_GLASS_R - 4`), same alignment
 * arithmetic, so its own goldens stay byte-identical (this is an
 * extraction, not a restyle — same proof shape as `ff_scr_pill_create`'s
 * own header comment above).
 *
 * `screen_flip` is an explicit parameter, not a hidden global — the same
 * convention `ff_theme_glass_cx`/`_cy` themselves document (multiple
 * independent translation units include this header; a file-static
 * "current orientation" would desync across them). Callers already have
 * `ff_app_settings_t.screen_flip` one frame away (`scr_nav.c` /
 * `scr_launcher.c` both thread it through to `ff_scr_radar_build`
 * already).
 *
 * Returns the built ring object so a caller that needs to animate it
 * (the flare sender overlay's pulse) can attach its own `lv_anim_t`
 * without this factory needing to know about animation at all — the same
 * "hand back the object, let the caller decide what else happens to it"
 * shape `ff_scr_pill_create` already uses for its click handler.
 */
lv_obj_t *ff_scr_glass_rim_create(lv_obj_t *parent, uint32_t color_hex, lv_opa_t opa, int32_t border_px,
                                   bool screen_flip);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_WIDGETS_H */
