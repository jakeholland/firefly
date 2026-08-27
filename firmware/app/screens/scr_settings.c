/**
 * scr_settings.c — see scr_settings.h.
 *
 * ## One scrolling list, pinned header (S21 — replaces #105's pagination)
 * The maintainer's request (docs/specs/S21-settings-rework.md §1): a
 * settings screen is a scroll list, so this face is now ONE vertically-
 * scrolling list holding every row — units, share, haptics, night glow,
 * water, quiet, brightness slider, UTC offset, colorblind, and the new
 * CALIBRATE TOUCH — with the header (back button + SETTINGS + name) PINNED
 * at the top so "back" is always reachable no matter how far the rows are
 * scrolled. #105 paginated instead, purely because the tap-target sweep
 * (`targets/sim/tests/test_face_hit_targets.c`) read each clickable's
 * ABSOLUTE, scroll-shifted rect and failed any row scrolled off-glass;
 * S21 makes that sweep scroll-aware (checks a scroll row against the
 * scroll VIEWPORT, not its momentary absolute position), which removes the
 * only reason pagination existed. See that file's header for the model.
 *
 * ## Structure — a pinned header + an inscribed scroll viewport
 * The header is built directly on `puck` at absolute coordinates (so the
 * sweep checks the back button absolutely, exactly as before). Below it
 * sits ONE `lv_obj` list container:
 *   - positioned as a rectangle INSCRIBED in the round glass across its
 *     whole height (its x-inset is `ff_layout_safe_margin_x` evaluated over
 *     the container's full vertical span, i.e. bound by its lower edge,
 *     which is the point nearest the bottom pole) — so any row shown
 *     anywhere in the viewport is on-glass by construction;
 *   - `LV_OBJ_FLAG_SCROLLABLE`, vertical-only (`LV_DIR_VER`), scrollbar
 *     AUTO — a NORMAL settings-list scroll, deliberately NOT the face
 *     tileview's (correctly) disabled user-scroll;
 *   - NOT clickable itself (a plain scroll region; the clickable rows
 *     inside it take the press and LVGL walks up to scroll this parent),
 *     so it is not itself a tap target the sweep has to reason about.
 * Every row is a child of that container, placed in container-relative
 * coordinates with a uniform inner width — no per-row safe-margin math,
 * because a scrolling row's absolute y is not fixed; the container's own
 * inscribed rectangle is what keeps every row on-glass at every scroll
 * position.
 *
 * ## Each row still follows the ONE pill grammar #105 established
 * A dim label names the setting; a value chip on the right shows the
 * CURRENT value in text (so the 2-second glance test still passes); a tap
 * advances to the next value in a small fixed cycle. Two short single-word
 * settings (units+share, haptics+night-glow) SHARE one row as two
 * half-width chips. Every control is a bare `FF_INTENT_*` emitter — range
 * validation and persistence are the shell's (`ff_shell.c`), same
 * "screens stay pure renderers" split every face uses.
 *
 * ## `my_name` is NOT editable in this slice
 * Unchanged from #105: renaming needs its own live text-entry session and
 * shell-seam draft field (the shell has exactly one such seam today, and
 * it is Compose's). This file renders the current `my_name` as a plain
 * caption under the header and ships everything else — tracked as a
 * follow-up, not silently dropped.
 */
#include "scr_settings.h"

#include <math.h>
#include <stdio.h>

#include "ff_intent.h" /* S16c1/S11b — the emit seam; S21 — FF_INTENT_CALIBRATE_TOUCH */
#include "ff_layout.h"
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST (core/include/ff_settings.h) */
#include "ff_theme.h"
#include "ff_wall.h" /* FF_WALL_OFFSET_MIN_LO/_HI — the UTC-offset stepper's own bounds */

/* ---------------------------------------------------------------------
 * Layout constants.
 * ------------------------------------------------------------------- */

#define FF_SETTINGS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

#define FF_SETTINGS_HEADER_Y 16

/* Back button — GROWN for #99 (maintainer field feedback: still "pretty
 * tight" even sober; the escape hatch someone most needs in a hurry must be
 * an obvious, comfortable 2am/gloves target). 64x76. The top corners clear
 * the r=206 circle at y=16 ((127,16) is 42.34k <= 206^2=42.44k from centre);
 * the taller bottom edge sits at y=92, deep in the wide part of the glass.
 * WIDTH stays 64: at the top pole the circle is only ~159px wide and the
 * button shares that band with the SETTINGS title + name to its right
 * (HEADER_TEXT_X=201) — widening it would push the title off the round
 * glass. The header is PINNED (built directly on the puck, never inside the
 * scroll list), so the sweep checks the back button absolutely. */
#define FF_SETTINGS_BACK_W 64
#define FF_SETTINGS_BACK_H 76
#define FF_SETTINGS_HEADER_H FF_SETTINGS_BACK_H

#define FF_SETTINGS_BACK_X 127
#define FF_SETTINGS_HEADER_TEXT_X (FF_SETTINGS_BACK_X + FF_SETTINGS_BACK_W + 10) /* 201 */
#define FF_SETTINGS_TITLE_Y (FF_SETTINGS_HEADER_Y + 2)
#define FF_SETTINGS_NAME_Y  (FF_SETTINGS_HEADER_Y + 26)

/* The scroll viewport: begins 8px below the pinned header (bottom at y=92,
 * so this 8px pad clears FF_HIT_MIN_GAP_PX between the back button and the
 * first row when the list is scrolled to its top) and spans FF_SETTINGS_
 * LIST_H down the glass. LIST_H stops short of the bottom pole so the
 * inscribed viewport keeps a usable width: at its lower edge (y=356) the
 * circle half-width is ~141px, giving an inner width of ~262px after the
 * safety inset — ample for the rows. Rows below this budget simply scroll. */
#define FF_SETTINGS_LIST_Y (FF_SETTINGS_HEADER_Y + FF_SETTINGS_HEADER_H + 8) /* 100 */
#define FF_SETTINGS_LIST_H 256                                              /* 100..356 */

/* Rows — 48px tall clears the 44 floor with real margin; a 12px inter-row
 * gap clears the 8px adjacency floor with slack. */
#define FF_SETTINGS_ROW_H   48
#define FF_SETTINGS_ROW_GAP 12
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP) /* 60 */

/* Separation between the two chips sharing a row (PR #68 UX review: 10px was
 * under 1mm of dead space at this puck's ~12px/mm scale — a mis-tap trap;
 * 24px (~2mm) gives each pill a real gap a gloved thumb can land in). */
#define FF_SETTINGS_CHIP_GAP 24

/* --- Container-relative row y-positions (0 = top of the scroll content). ---
 * Uniform 60px steps, except the BRIGHTNESS block, which carries a caption
 * over its slider and reserves extra vertical air around the slider so its
 * ~7px-per-side knob overhang still clears the adjacency floor to the rows
 * above/below (the same knob-overhang note #105 recorded for its page-1
 * layout). */
#define FF_SETTINGS_REL_UNITS_Y   0                                          /* units + share            */
#define FF_SETTINGS_REL_HAPTICS_Y (FF_SETTINGS_REL_UNITS_Y + FF_SETTINGS_ROW_STEP)   /* 60  haptics + glow */
#define FF_SETTINGS_REL_WATER_Y   (FF_SETTINGS_REL_HAPTICS_Y + FF_SETTINGS_ROW_STEP) /* 120 water nudge    */
#define FF_SETTINGS_REL_QUIET_Y   (FF_SETTINGS_REL_WATER_Y + FF_SETTINGS_ROW_STEP)   /* 180 quiet hours    */
#define FF_SETTINGS_REL_BRIGHT_CAP_Y (FF_SETTINGS_REL_QUIET_Y + FF_SETTINGS_ROW_STEP)/* 240 "BRIGHTNESS NN%" */
#define FF_SETTINGS_BRIGHT_CAP_H  24
#define FF_SETTINGS_REL_SLIDER_Y  (FF_SETTINGS_REL_BRIGHT_CAP_Y + FF_SETTINGS_BRIGHT_CAP_H + 4) /* 268 */
#define FF_SETTINGS_SLIDER_H      48
#define FF_SETTINGS_REL_UTC_Y     (FF_SETTINGS_REL_SLIDER_Y + FF_SETTINGS_SLIDER_H + 20)  /* 336 UTC stepper */
#define FF_SETTINGS_REL_CB_Y      (FF_SETTINGS_REL_UTC_Y + FF_SETTINGS_ROW_STEP)          /* 396 colorblind  */
#define FF_SETTINGS_REL_CAL_Y     (FF_SETTINGS_REL_CB_Y + FF_SETTINGS_ROW_STEP)           /* 456 calibrate   */
#define FF_SETTINGS_CONTENT_H     (FF_SETTINGS_REL_CAL_Y + FF_SETTINGS_ROW_H)             /* 504 total       */

/**
 * settings_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout_safe_margin_x, bound to this puck's own center/radius
 * (ff_theme.h) and this file's safety buffer — identical shape to
 * scr_compose.c's compose_safe_margin_x / scr_signals.c's
 * signals_safe_margin_x. Here it is called ONCE, over the scroll
 * container's whole vertical span, to inset the viewport rectangle inside
 * the round glass (its lower edge, nearest the bottom pole, binds).
 */
static int32_t settings_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SETTINGS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Static build-time snapshot — same convention as #105 / scr_compose.c's
 * `s_mode`: every callback below computes "current -> next" from the
 * settings this screen was built with, and LVGL event callbacks carry no
 * argument beyond `user_data`. A snapshot, not live state: nothing here
 * mutates it, and a tap only ever reports through the intent seam.
 * ------------------------------------------------------------------- */
static ff_app_settings_t s_settings;

/* ---------------------------------------------------------------------
 * Back "<" -> FF_INTENT_BACK (already routed — S16 pops the modal route).
 * ------------------------------------------------------------------- */
static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Generic int-setting emitter — every chip below funnels through this.
 * ------------------------------------------------------------------- */
static void settings_emit_int(ff_setting_id_t id, int32_t v)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = id;
    in.u.setting.v.i = v;
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Chip widget: a pill button with a centered label — the SAME shape
 * scr_compose.c/scr_signals.c already use (issue #24 tracks the eventual
 * shared-widget extraction; this file follows the existing pattern).
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_chip(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t bg_hex, uint32_t fg_hex, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);
    return btn;
}

/**
 * settings_build_row_label — the dim row caption (e.g. "WATER NUDGE"),
 * itself a tap target forwarding to `cb` (PR #68 UX review: tapping the
 * label half of a row used to be dead air). The label sits inside a `w`x`h`
 * hit container spanning the row's own height (so it clears the 44px floor
 * by construction) — a container gives the label a real, checkable box the
 * sweep can assert on. The label and its value chip share `cb` with no
 * user_data, so the sweep's composite-control exclusion treats them as one
 * logical control (two hit-rects, one setter), not two independent buttons.
 */
static void settings_build_row_label(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                      lv_event_cb_t cb)
{
    lv_obj_t *hit = lv_obj_create(parent);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, w, h);
    lv_obj_set_pos(hit, x, y);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_clear_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *lbl = lv_label_create(hit);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

/* ---------------------------------------------------------------------
 * Row: UNITS (FT/M) + SHARE (LIVE/ZONES/GHOST). Two half-width chips.
 * ------------------------------------------------------------------- */

static void settings_units_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_IMPERIAL, s_settings.imperial ? 0 : 1);
}

/**
 * ZONES is deliberately NOT cycled into (PR #68 UX review, blocking
 * finding 1): selecting ZONES does not change sharing behavior from LIVE
 * in v1, so cycling it in as a third confident option would let someone
 * pick "zones only" believing they've restricted their share radius when
 * they haven't — the worst place for a silent no-op is a location-privacy
 * control. LIVE<->GHOST only, a plain two-stop loop, until the ZONES
 * backend ships. A persisted FF_SHARE_ZONES still renders honestly (see
 * settings_share_name) and one tap moves it to GHOST, never back to ZONES.
 */
static void settings_share_cb(lv_event_t *e)
{
    (void)e;
    uint8_t const next = (s_settings.share_mode == FF_SHARE_GHOST) ? FF_SHARE_LIVE : FF_SHARE_GHOST;
    settings_emit_int(FF_SETTING_SHARE_MODE, next);
}

static char const *settings_share_name(uint8_t mode)
{
    switch (mode) {
    case FF_SHARE_LIVE: return "LIVE";
    case FF_SHARE_ZONES: return "ZONES";
    case FF_SHARE_GHOST: return "GHOST";
    default: return "?";
    }
}

static void settings_build_units_share_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    int32_t gap = FF_SETTINGS_CHIP_GAP;
    int32_t units_w = (row_w - gap) * 2 / 5;
    int32_t share_w = row_w - gap - units_w;

    settings_make_chip(list, s_settings.imperial ? "FT" : "M", 0, y, units_w, FF_SETTINGS_ROW_H,
                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, settings_units_cb, NULL);
    settings_make_chip(list, settings_share_name(s_settings.share_mode), units_w + gap, y, share_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER, settings_share_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Row: HAPTICS + NIGHT GLOW. Two half-width self-describing on/off chips.
 * ------------------------------------------------------------------- */

static void settings_haptics_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_HAPTICS, s_settings.haptics ? 0 : 1);
}

static void settings_night_glow_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_NIGHT_GLOW, s_settings.night_glow ? 0 : 1);
}

static void settings_build_haptics_glow_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    int32_t gap = FF_SETTINGS_CHIP_GAP;
    int32_t half_w = (row_w - gap) / 2;

    settings_make_chip(list, s_settings.haptics ? "BUZZ ON" : "BUZZ OFF", 0, y, half_w, FF_SETTINGS_ROW_H,
                        FF_THEME_COLOR_SURFACE, s_settings.haptics ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM,
                        settings_haptics_cb, NULL);
    settings_make_chip(list, s_settings.night_glow ? "GLOW ON" : "GLOW OFF", half_w + gap, y, half_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                        s_settings.night_glow ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM,
                        settings_night_glow_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Row: WATER NUDGE — label + a chip cycling the spec's v1 presets.
 * ------------------------------------------------------------------- */

static uint16_t const kWaterPresets[] = {0, 45, 90, 120};
enum { N_WATER_PRESETS = sizeof(kWaterPresets) / sizeof(kWaterPresets[0]) };

static uint16_t settings_next_water(uint16_t current)
{
    for (size_t i = 0; i < N_WATER_PRESETS; i++) {
        if (kWaterPresets[i] == current) return kWaterPresets[(i + 1) % N_WATER_PRESETS];
    }
    return kWaterPresets[0]; /* a persisted value outside the v1 cycle resets onto it */
}

static void settings_water_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_WATER_MIN, settings_next_water(s_settings.water_min));
}

static void settings_water_label(char *buf, size_t n, uint16_t water_min)
{
    if (water_min == 0) {
        snprintf(buf, n, "OFF");
    } else {
        snprintf(buf, n, "%u MIN", (unsigned)water_min);
    }
}

static void settings_build_water_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    int32_t chip_w = 110;
    int32_t label_w = row_w - chip_w - FF_SETTINGS_CHIP_GAP;
    settings_build_row_label(list, "WATER NUDGE", 0, y, label_w, FF_SETTINGS_ROW_H, settings_water_cb);

    char buf[16];
    settings_water_label(buf, sizeof(buf), s_settings.water_min);
    uint32_t const fg = (s_settings.water_min == 0) ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
    settings_make_chip(list, buf, row_w - chip_w, y, chip_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg,
                        settings_water_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Row: QUIET HOURS — label + a chip cycling the spec's v1 presets. Each
 * preset sets BOTH quiet_from_min/quiet_to_min (two ff_settings_t fields),
 * so a tap emits two FF_INTENT_SETTING_SET; the shell persists once per
 * changed field, never in a torn state a reader could observe between them.
 * ------------------------------------------------------------------- */

typedef struct {
    uint16_t from_min;
    uint16_t to_min;
    char const *label;
} settings_quiet_preset_t;

static settings_quiet_preset_t const kQuietPresets[] = {
    {0, 0, "OFF"},
    {120, 480, "2A-8A"},
    {240, 600, "4A-10A"},
};
enum { N_QUIET_PRESETS = sizeof(kQuietPresets) / sizeof(kQuietPresets[0]) };

static settings_quiet_preset_t const *settings_next_quiet(uint16_t from_min, uint16_t to_min)
{
    for (size_t i = 0; i < N_QUIET_PRESETS; i++) {
        if (kQuietPresets[i].from_min == from_min && kQuietPresets[i].to_min == to_min) {
            return &kQuietPresets[(i + 1) % N_QUIET_PRESETS];
        }
    }
    return &kQuietPresets[0];
}

static settings_quiet_preset_t const *settings_current_quiet(uint16_t from_min, uint16_t to_min)
{
    for (size_t i = 0; i < N_QUIET_PRESETS; i++) {
        if (kQuietPresets[i].from_min == from_min && kQuietPresets[i].to_min == to_min) {
            return &kQuietPresets[i];
        }
    }
    return NULL; /* a persisted value outside the v1 cycle: render honestly, don't fake a preset name */
}

static void settings_quiet_cb(lv_event_t *e)
{
    (void)e;
    settings_quiet_preset_t const *next = settings_next_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    settings_emit_int(FF_SETTING_QUIET_FROM_MIN, next->from_min);
    settings_emit_int(FF_SETTING_QUIET_TO_MIN, next->to_min);
}

static void settings_build_quiet_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    int32_t chip_w = 110;
    int32_t label_w = row_w - chip_w - FF_SETTINGS_CHIP_GAP;
    settings_build_row_label(list, "QUIET HOURS", 0, y, label_w, FF_SETTINGS_ROW_H, settings_quiet_cb);

    settings_quiet_preset_t const *cur = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    bool const is_off = (cur != NULL) && (cur->from_min == 0) && (cur->to_min == 0);
    uint32_t const fg = is_off ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
    settings_make_chip(list, (cur != NULL) ? cur->label : "CUSTOM", row_w - chip_w, y, chip_w, FF_SETTINGS_ROW_H,
                        FF_THEME_COLOR_SURFACE, fg, settings_quiet_cb, NULL);
}

/* ---------------------------------------------------------------------
 * BRIGHTNESS (#100) — a caption ("BRIGHTNESS  NN%") over an lv_slider
 * spanning the row width. Range is the setting's honest bounds
 * [FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT] — the floor is non-zero so
 * the knob can never reach a black, unrecoverable screen. Emits on
 * LV_EVENT_RELEASED (once per touch, tap-to-position included), so a drag
 * persists a single final value; on the sim this is a pure render (goldens
 * never fire a touch — the knob just sits at the stored percent).
 * ------------------------------------------------------------------- */

static uint8_t settings_brightness_clamped(void)
{
    uint32_t v = s_settings.brightness_pct;
    if (v < FF_BRIGHTNESS_MIN_PCT) v = FF_BRIGHTNESS_MIN_PCT;
    if (v > FF_BRIGHTNESS_MAX_PCT) v = FF_BRIGHTNESS_MAX_PCT;
    return (uint8_t)v;
}

static void settings_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);
    settings_emit_int(FF_SETTING_BRIGHTNESS, v);
}

static void settings_build_brightness(lv_obj_t *list, int32_t row_w)
{
    uint8_t const pct = settings_brightness_clamped();

    lv_obj_t *cap = lv_label_create(list);
    lv_label_set_text(cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(cap, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_set_pos(cap, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);

    char pctbuf[8];
    snprintf(pctbuf, sizeof(pctbuf), "%u%%", (unsigned)pct);
    lv_obj_t *pctlbl = lv_label_create(list);
    lv_label_set_text(pctlbl, pctbuf);
    lv_obj_set_style_text_font(pctlbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(pctlbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(pctlbl, LV_ALIGN_TOP_RIGHT, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);
    /* Right-align against the row width — the label is a child of the list
     * container whose content is row_w wide, so TOP_RIGHT with a 0 x-offset
     * lands it at the row's right edge. */

    lv_obj_t *slider = lv_slider_create(list);
    lv_obj_set_size(slider, row_w, FF_SETTINGS_SLIDER_H);
    lv_obj_set_pos(slider, 0, FF_SETTINGS_REL_SLIDER_Y);
    lv_slider_set_range(slider, FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    /* Explicit palette so it reads on the dark puck: SURFACE track, amber
     * filled indicator + knob (the "amber = the live value" grammar the
     * water/quiet chips already use). */
    lv_obj_set_style_bg_color(slider, lv_color_hex(FF_THEME_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(FF_THEME_COLOR_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(FF_THEME_COLOR_AMBER), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_brightness_cb, LV_EVENT_RELEASED, NULL);
}

/* ---------------------------------------------------------------------
 * Row: UTC OFFSET — "-" / value / "+" stepper, 60-minute steps, clamped
 * to the same [FF_WALL_OFFSET_MIN_LO, FF_WALL_OFFSET_MIN_HI] range
 * ff_shell.c validates against — clamped here too so a tap at either end
 * is a harmless no-op rather than a dead button. Unset starts from 0 (UTC).
 * ------------------------------------------------------------------- */

static int32_t settings_utc_base(void)
{
    return s_settings.utc_offset_set ? (int32_t)s_settings.utc_offset_min : 0;
}

static void settings_utc_step(int32_t delta)
{
    int32_t v = settings_utc_base() + delta;
    if (v < FF_WALL_OFFSET_MIN_LO) v = FF_WALL_OFFSET_MIN_LO;
    if (v > FF_WALL_OFFSET_MIN_HI) v = FF_WALL_OFFSET_MIN_HI;
    settings_emit_int(FF_SETTING_UTC_OFFSET_MIN, v);
}

static void settings_utc_minus_cb(lv_event_t *e)
{
    (void)e;
    settings_utc_step(-60);
}

static void settings_utc_plus_cb(lv_event_t *e)
{
    (void)e;
    settings_utc_step(60);
}

static void settings_utc_label(char *buf, size_t n, bool set, int16_t off_min)
{
    if (!set) {
        snprintf(buf, n, "UNSET");
        return;
    }
    int32_t a = off_min;
    char sign = (a < 0) ? '-' : '+';
    if (a < 0) a = -a;
    snprintf(buf, n, "UTC%c%d:%02d", sign, (int)(a / 60), (int)(a % 60));
}

static void settings_build_utc_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    int32_t btn_w = FF_THEME_MIN_HIT_PX + 8; /* 52 — past the floor with margin */

    settings_make_chip(list, "-", 0, y, btn_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK,
                        settings_utc_minus_cb, NULL);
    settings_make_chip(list, "+", row_w - btn_w, y, btn_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                        FF_THEME_COLOR_INK, settings_utc_plus_cb, NULL);

    char buf[16];
    settings_utc_label(buf, sizeof(buf), s_settings.utc_offset_set, s_settings.utc_offset_min);
    lv_obj_t *val = lv_label_create(list);
    lv_label_set_text(val, buf);
    lv_obj_set_style_text_font(val, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(val, LV_ALIGN_TOP_MID, 0, y + (FF_SETTINGS_ROW_H - 16) / 2);
}

/* ---------------------------------------------------------------------
 * COLORBLIND (S17 slice a) — a single boolean, one full-width
 * self-describing chip. Green-on/dim-off, a plain toggle.
 * ------------------------------------------------------------------- */

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
}

static void settings_build_colorblind_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    uint32_t const fg = s_settings.colorblind ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM;
    settings_make_chip(list, s_settings.colorblind ? "COLORBLIND ON" : "COLORBLIND OFF", 0, y, row_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg, settings_colorblind_cb, NULL);
}

/* ---------------------------------------------------------------------
 * CALIBRATE TOUCH (S21 §3) — a single full-width chip that, on tap, emits
 * the shell-owned FF_INTENT_CALIBRATE_TOUCH. The screen stays a pure
 * emitter: the shell runs the device crosshair flow (via its injected
 * calibrate hook), installs + persists the solved transform, and returns
 * here. In the sim there is no touch panel, so the shell handles the intent
 * as a no-op (the injected hook is NULL) — the row still renders, and
 * goldens/tests stay green because nothing fires a touch. INK (not amber):
 * an action, not a stored value.
 * ------------------------------------------------------------------- */

static void settings_calibrate_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_calibrate_row(lv_obj_t *list, int32_t y, int32_t row_w)
{
    settings_make_chip(list, "CALIBRATE TOUCH", 0, y, row_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                        FF_THEME_COLOR_INK, settings_calibrate_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_settings_build(ff_app_settings_t const *settings)
{
    if (settings == NULL) {
        return;
    }

    s_settings = *settings;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *puck = lv_obj_create(scr);
    lv_obj_remove_style_all(puck);
    lv_obj_set_size(puck, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_align(puck, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(puck, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(puck, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(puck, 0, 0);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(puck, LV_OBJ_FLAG_CLICKABLE); /* base lv_obj defaults clickable; this one is a plain backdrop */

    /* --- PINNED header: enlarged back button on the left, SETTINGS title +
     * name caption stacked to its right. Built directly on the puck (never
     * inside the scroll list) so it never scrolls away and the sweep checks
     * the back button at its absolute position. --- */
    lv_obj_t *back = lv_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_SETTINGS_BACK_W, FF_SETTINGS_BACK_H);
    lv_obj_set_pos(back, FF_SETTINGS_BACK_X, FF_SETTINGS_HEADER_Y);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_font(back_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, FF_SETTINGS_HEADER_TEXT_X, FF_SETTINGS_TITLE_Y);

    lv_obj_t *name_lbl = lv_label_create(puck);
    char name_buf[FF_APP_NAME_LEN + 8];
    snprintf(name_buf, sizeof(name_buf), "NAME: %s", (s_settings.my_name[0] != '\0') ? s_settings.my_name : "(unset)");
    lv_label_set_text(name_lbl, name_buf);
    lv_obj_set_style_text_font(name_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, FF_SETTINGS_HEADER_TEXT_X, FF_SETTINGS_NAME_Y);

    /* --- The scroll list: a rectangle inscribed in the round glass across
     * its whole height (so any row shown at any scroll position is on-glass
     * by construction), vertical-only user scroll, scrollbar AUTO. Not
     * clickable itself — the clickable rows inside take the press and LVGL
     * scrolls this parent. Rows are placed in container-relative coords with
     * a uniform inner width. --- */
    int32_t list_margin = settings_safe_margin_x(FF_SETTINGS_LIST_Y, FF_SETTINGS_LIST_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * list_margin;

    lv_obj_t *list = lv_obj_create(puck);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, row_w, FF_SETTINGS_LIST_H);
    lv_obj_set_pos(list, list_margin, FF_SETTINGS_LIST_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE); /* a plain scroll region, not a tap target */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    /* Scroll range comes from the children's own extents (the rows are
     * absolutely positioned, not a flex layout): the lowest row's bottom at
     * FF_SETTINGS_CONTENT_H (504) below the 256-tall viewport is what makes
     * lv_obj_get_scroll_bottom > 0, which is exactly the signal the
     * scroll-aware sweep keys off to treat a row as "checked when scrolled
     * into view" (test_face_hit_targets.c). */

    settings_build_units_share_row(list, FF_SETTINGS_REL_UNITS_Y, row_w);
    settings_build_haptics_glow_row(list, FF_SETTINGS_REL_HAPTICS_Y, row_w);
    settings_build_water_row(list, FF_SETTINGS_REL_WATER_Y, row_w);
    settings_build_quiet_row(list, FF_SETTINGS_REL_QUIET_Y, row_w);
    settings_build_brightness(list, row_w);
    settings_build_utc_row(list, FF_SETTINGS_REL_UTC_Y, row_w);
    settings_build_colorblind_row(list, FF_SETTINGS_REL_CB_Y, row_w);
    settings_build_calibrate_row(list, FF_SETTINGS_REL_CAL_Y, row_w);
}
