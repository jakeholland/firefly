/**
 * scr_settings.c — see scr_settings.h.
 *
 * ## One scrolling list, pinned centered header (S21 restyle)
 * A settings screen is a single vertically-scrolling list of rows, with the
 * header (back button + "SETTINGS" + name) PINNED at the top so "back" is
 * always reachable no matter how far the rows are scrolled. #105 paginated
 * instead, purely because the tap-target sweep
 * (`targets/sim/tests/test_face_hit_targets.c`) read each clickable's
 * ABSOLUTE, scroll-shifted rect and failed any row scrolled off-glass; S21
 * makes that sweep scroll-aware (checks a scroll row against the scroll
 * VIEWPORT, not its momentary absolute position), which removes the only
 * reason pagination existed.
 *
 * ## Round-safe framing — the key constraint on a 412 ROUND puck
 * NOTHING may cross the round glass edge. Two devices enforce that here:
 *   - The header group (back button + title + name) is CENTERED in the top
 *     band, not tucked in a top-left corner — the corners of the square are
 *     off-glass on the physical circle, so a corner-anchored control is
 *     clipped by the bezel. The group is a fixed-width block centered
 *     horizontally (`FF_SETTINGS_HDR_W`, start x computed once), placed low
 *     enough that the back button's own corners clear the r=206 circle.
 *   - The scroll rows live in ONE `lv_obj` list container positioned as a
 *     rectangle INSCRIBED in the round glass across its whole height (its
 *     x-inset is `ff_layout_safe_margin_x` evaluated over the container's
 *     full vertical span, i.e. bound by its lower edge — the point nearest
 *     the bottom pole). Every row is a child placed in container-relative
 *     coordinates with a uniform inner width, so any row shown at any scroll
 *     position is on-glass by construction, with no per-row margin math.
 * Top and bottom edge-fade scrims (BG->transparent gradients over the
 * viewport edges) soften rows scrolling in/out; they are non-clickable
 * chrome and never affect the sweep.
 *
 * ## One consistent row language
 * Every row reads label LEFT (muted, uppercase), control RIGHT. Controls are
 * PILLS (rounded ~12px):
 *   - Toggle pairs (UNITS FT|MI, SHARE LIVE|GHOST, HAPTICS ON|OFF,
 *     GLOW ON|OFF, COLORBLIND ON|OFF) render two pills; the ACTIVE one is
 *     amber-on-ink, the inactive one surface-on-muted. Both pills of a pair
 *     forward to the SAME toggle callback with no user_data, so (a) tapping
 *     either flips the two-state setting and (b) the hit-target sweep treats
 *     them as ONE logical control (its composite-control exclusion keys off
 *     matching cb+user_data), letting the pair sit at a tight ~6px gap
 *     without tripping the 8px adjacency floor two INDEPENDENT controls owe.
 *   - Value rows (WATER NUDGE, QUIET HOURS) render one surface/ink value
 *     pill; the row LABEL is itself a tap target forwarding to the same
 *     callback (no dead left half — PR #68), again composite with its pill.
 *   - CALIBRATE TOUCH is a full-width surface pill with a thin amber border,
 *     an ACTION (amber text), not a stored value.
 * Every control is a bare `FF_INTENT_*` emitter — range validation and
 * persistence are the shell's (`ff_shell.c`), same "screens stay pure
 * renderers" split every face uses.
 *
 * ## Brightness is its own taller row
 * A "BRIGHTNESS" caption + "%" value over a full-width THIN track. To keep a
 * full-width thin track that still clears the 44px tap floor AND stays
 * on-glass, the interactive `lv_slider` is a transparent full-width, 44px
 * hit strip (a thin styled track cannot be both 6px tall for the look and
 * 44px tall for the floor, and widening its hit area with ext_click_area
 * would push it off the round glass horizontally). The visible thin track,
 * amber fill, and round amber knob are drawn as non-clickable decorations
 * beneath/over it — the slider still emits FF_SETTING_BRIGHTNESS on release.
 *
 * ## UTC offset is NOT a row here
 * The festpack supplies the timezone (`fp_pack_t.utc_offset_min`), so the
 * manual UTC stepper is gone. `ff_settings.utc_offset_min` / `_set` and the
 * wall-clock logic that reads them are untouched — only the Settings UI for
 * it was removed.
 *
 * ## `my_name` is NOT editable in this slice
 * Renaming needs its own live text-entry session and shell-seam draft field;
 * this file renders the current `my_name` as a caption under the title.
 */
#include "scr_settings.h"

#include <math.h>
#include <stdio.h>

#include "ff_intent.h" /* the emit seam; FF_INTENT_CALIBRATE_TOUCH */
#include "ff_layout.h"
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST, FF_BRIGHTNESS_*_PCT */
#include "ff_theme.h"

/* ---------------------------------------------------------------------
 * Palette roles (redesign spec).
 * ------------------------------------------------------------------- */
#define FF_SETTINGS_PILL_RADIUS 12

/* Active/selected toggle pill: amber fill, near-black ink. */
#define FF_SETTINGS_PILL_ON_BG  FF_THEME_COLOR_AMBER
#define FF_SETTINGS_PILL_ON_FG  FF_THEME_COLOR_BG
/* Inactive toggle pill: surface fill, muted text. */
#define FF_SETTINGS_PILL_OFF_BG FF_THEME_COLOR_SURFACE
#define FF_SETTINGS_PILL_OFF_FG FF_THEME_COLOR_MUTED
/* Value pill: surface fill, primary ink. */
#define FF_SETTINGS_PILL_VAL_BG FF_THEME_COLOR_SURFACE
#define FF_SETTINGS_PILL_VAL_FG FF_THEME_COLOR_INK

/* ---------------------------------------------------------------------
 * Layout constants.
 * ------------------------------------------------------------------- */

#define FF_SETTINGS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

/* --- Pinned, horizontally-centered header. A fixed-width block so the back
 * button's absolute position is deterministic regardless of the (variable-
 * length, clipped) name: back button on the left, a title/name text column
 * to its right, the whole block centered in the top band. Placed at
 * FF_SETTINGS_HDR_Y (low enough that the 46px button's top corners clear the
 * r=206 circle even at the block's leftmost x). --- */
#define FF_SETTINGS_BACK_SZ   46
#define FF_SETTINGS_HDR_GAP   10
#define FF_SETTINGS_HDR_TEXT_W 140
#define FF_SETTINGS_HDR_W     (FF_SETTINGS_BACK_SZ + FF_SETTINGS_HDR_GAP + FF_SETTINGS_HDR_TEXT_W) /* 196 */
#define FF_SETTINGS_HDR_X     ((FF_THEME_PUCK_PX - FF_SETTINGS_HDR_W) / 2)                          /* 108 */
#define FF_SETTINGS_HDR_Y     34
#define FF_SETTINGS_HDR_TEXT_X (FF_SETTINGS_HDR_X + FF_SETTINGS_BACK_SZ + FF_SETTINGS_HDR_GAP)      /* 164 */
#define FF_SETTINGS_TITLE_Y   (FF_SETTINGS_HDR_Y + 4)  /* 38 — vertically nestled against the button */
#define FF_SETTINGS_NAME_Y    (FF_SETTINGS_TITLE_Y + 22) /* 60 */

/* The scroll viewport: an inscribed rectangle spanning the MIDDLE band, well
 * clear of the header above and the bottom curve below. Its x-inset is the
 * round-safe margin over its whole span (bound by the lower edge, nearest the
 * bottom pole) so a row shown anywhere in the viewport is on-glass. At the
 * lower edge (y=356) the circle half-width is ~141px -> inner width ~262px
 * after the safety inset — ample for the rows; lower rows simply scroll. */
#define FF_SETTINGS_LIST_Y 100
#define FF_SETTINGS_LIST_H 256 /* 100..356 */

/* Rows — 48px tall clears the 44 floor with margin; 14px inter-row gap clears
 * the 8px adjacency floor with real slack. */
#define FF_SETTINGS_ROW_H   48
#define FF_SETTINGS_ROW_GAP 14
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP) /* 62 */

/* Toggle-pair pills: two >=44px pills at a tight 6px gap (safe because a
 * pair shares one callback — see sweep composite-control exclusion). */
#define FF_SETTINGS_TOGGLE_PILL_W 58
#define FF_SETTINGS_TOGGLE_GAP    6
#define FF_SETTINGS_TOGGLE_GRP_W  (2 * FF_SETTINGS_TOGGLE_PILL_W + FF_SETTINGS_TOGGLE_GAP) /* 122 */

/* Value pill (WATER/QUIET): one pill wide enough for "120 MIN"/"4A-10A". */
#define FF_SETTINGS_VALUE_PILL_W 96
#define FF_SETTINGS_VALUE_GAP    12

/* --- Container-relative row y-positions (0 = top of the scroll content). ---
 * BRIGHTNESS leads (its own taller block: caption over a slider), then the
 * uniform-step rows. */
#define FF_SETTINGS_REL_BRIGHT_CAP_Y 0
#define FF_SETTINGS_BRIGHT_CAP_H     22
#define FF_SETTINGS_REL_SLIDER_Y     30
#define FF_SETTINGS_SLIDER_H         44 /* transparent hit strip: clears the 44 floor; the visible track is thin chrome */
#define FF_SETTINGS_BRIGHT_BLOCK_H   (FF_SETTINGS_REL_SLIDER_Y + FF_SETTINGS_SLIDER_H) /* 74 */

#define FF_SETTINGS_REL_UNITS_Y (FF_SETTINGS_BRIGHT_BLOCK_H + FF_SETTINGS_ROW_GAP)     /* 88  */
#define FF_SETTINGS_REL_SHARE_Y (FF_SETTINGS_REL_UNITS_Y + FF_SETTINGS_ROW_STEP)       /* 150 */
#define FF_SETTINGS_REL_HAPTICS_Y (FF_SETTINGS_REL_SHARE_Y + FF_SETTINGS_ROW_STEP)     /* 212 */
#define FF_SETTINGS_REL_GLOW_Y  (FF_SETTINGS_REL_HAPTICS_Y + FF_SETTINGS_ROW_STEP)     /* 274 */
#define FF_SETTINGS_REL_WATER_Y (FF_SETTINGS_REL_GLOW_Y + FF_SETTINGS_ROW_STEP)        /* 336 */
#define FF_SETTINGS_REL_QUIET_Y (FF_SETTINGS_REL_WATER_Y + FF_SETTINGS_ROW_STEP)       /* 398 */
#define FF_SETTINGS_REL_CB_Y    (FF_SETTINGS_REL_QUIET_Y + FF_SETTINGS_ROW_STEP)       /* 460 */
#define FF_SETTINGS_REL_CAL_Y   (FF_SETTINGS_REL_CB_Y + FF_SETTINGS_ROW_STEP)          /* 522 */
#define FF_SETTINGS_CONTENT_H   (FF_SETTINGS_REL_CAL_Y + FF_SETTINGS_ROW_H)            /* 570 */

/* Edge-fade scrims over the viewport top/bottom. */
#define FF_SETTINGS_SCRIM_H 28

/**
 * settings_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout_safe_margin_x, bound to this puck's own center/radius and this
 * file's safety buffer — identical shape to scr_compose.c's
 * compose_safe_margin_x. Called ONCE, over the scroll container's whole
 * vertical span, to inset the viewport rectangle inside the round glass (its
 * lower edge, nearest the bottom pole, binds).
 */
static int32_t settings_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SETTINGS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Static build-time snapshot — same convention as scr_compose.c's `s_mode`:
 * every callback computes "current -> next" from the settings this screen was
 * built with; a tap only ever reports through the intent seam.
 * ------------------------------------------------------------------- */
static ff_app_settings_t s_settings;

/* ---------------------------------------------------------------------
 * Back "<" -> FF_INTENT_BACK.
 * ------------------------------------------------------------------- */
static void settings_back_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Generic int-setting emitter — every control below funnels through this.
 * ------------------------------------------------------------------- */
static void settings_emit_int(ff_setting_id_t id, int32_t v)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = id;
    in.u.setting.v.i = v;
    ff_intent_emit(&in);
}

/* ---------------------------------------------------------------------
 * Pill widget: a rounded button with a centered label. bg/fg carry the pill
 * role (active / inactive / value). `letter_space` is applied to the label.
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_pill(lv_obj_t *parent, char const *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t bg_hex, uint32_t fg_hex, int32_t letter_space, lv_event_cb_t cb,
                                     void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, FF_SETTINGS_PILL_RADIUS, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    if (letter_space != 0) {
        lv_obj_set_style_text_letter_space(label, letter_space, 0);
    }
    lv_obj_center(label);
    return btn;
}

/* ---------------------------------------------------------------------
 * Row container — a transparent, non-clickable, non-scrolling box the row's
 * label + control(s) live inside, positioned in list-relative coords.
 * ------------------------------------------------------------------- */
static lv_obj_t *settings_make_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, row_w, FF_SETTINGS_ROW_H);
    lv_obj_set_pos(row, 0, rel_y);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* A plain left caption (uppercase, muted), vertically centered in the row. */
static void settings_row_caption(lv_obj_t *row, char const *text)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

/* ---------------------------------------------------------------------
 * Toggle-pair row: label + two pills [left|right], the active one amber.
 * BOTH pills share `cb` with NULL user_data — one logical two-state control
 * (tap either to flip) and one composite pair to the adjacency sweep.
 * active_side: 0 = left pill active, 1 = right, -1 = neither (honest render
 * of a persisted value that maps to neither shown option).
 * ------------------------------------------------------------------- */
static void settings_build_toggle_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                      char const *left_text, char const *right_text, int active_side,
                                      lv_event_cb_t cb)
{
    lv_obj_t *row = settings_make_row(list, rel_y, row_w);
    settings_row_caption(row, label);

    int32_t const grp_x = row_w - FF_SETTINGS_TOGGLE_GRP_W;
    uint32_t const l_bg = (active_side == 0) ? FF_SETTINGS_PILL_ON_BG : FF_SETTINGS_PILL_OFF_BG;
    uint32_t const l_fg = (active_side == 0) ? FF_SETTINGS_PILL_ON_FG : FF_SETTINGS_PILL_OFF_FG;
    uint32_t const r_bg = (active_side == 1) ? FF_SETTINGS_PILL_ON_BG : FF_SETTINGS_PILL_OFF_BG;
    uint32_t const r_fg = (active_side == 1) ? FF_SETTINGS_PILL_ON_FG : FF_SETTINGS_PILL_OFF_FG;

    settings_make_pill(row, left_text, grp_x, 0, FF_SETTINGS_TOGGLE_PILL_W, FF_SETTINGS_ROW_H, l_bg, l_fg, 0, cb, NULL);
    settings_make_pill(row, right_text, grp_x + FF_SETTINGS_TOGGLE_PILL_W + FF_SETTINGS_TOGGLE_GAP, 0,
                       FF_SETTINGS_TOGGLE_PILL_W, FF_SETTINGS_ROW_H, r_bg, r_fg, 0, cb, NULL);
}

/* ---------------------------------------------------------------------
 * Value row: a clickable label (no dead left half) + one value pill, both
 * wired to the same `cb` (composite to the sweep). `dim` renders the pill
 * muted instead of ink (an honest "off"/unset value).
 * ------------------------------------------------------------------- */
static void settings_build_value_row(lv_obj_t *list, int32_t rel_y, int32_t row_w, char const *label,
                                     char const *value, bool dim, lv_event_cb_t cb)
{
    lv_obj_t *row = settings_make_row(list, rel_y, row_w);

    int32_t const label_w = row_w - FF_SETTINGS_VALUE_PILL_W - FF_SETTINGS_VALUE_GAP;

    /* Clickable hit box wrapping the caption (its DIRECT child is the label —
     * the label-tap test keys off parent(label) being clickable). */
    lv_obj_t *hit = lv_obj_create(row);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, label_w, FF_SETTINGS_ROW_H);
    lv_obj_set_pos(hit, 0, 0);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(hit);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    uint32_t const fg = dim ? FF_THEME_COLOR_MUTED : FF_SETTINGS_PILL_VAL_FG;
    settings_make_pill(row, value, row_w - FF_SETTINGS_VALUE_PILL_W, 0, FF_SETTINGS_VALUE_PILL_W, FF_SETTINGS_ROW_H,
                       FF_SETTINGS_PILL_VAL_BG, fg, 0, cb, NULL);
}

/* ---------------------------------------------------------------------
 * UNITS (FT|MI).
 * ------------------------------------------------------------------- */
static void settings_units_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_IMPERIAL, s_settings.imperial ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * SHARE (LIVE|GHOST). ZONES is deliberately NOT cycled into (PR #68 UX
 * review, blocking finding 1): selecting ZONES does not change sharing
 * behavior from LIVE in v1, so a tap moves LIVE<->GHOST only. A persisted
 * ZONES renders as neither pill active and one tap moves it to GHOST.
 * ------------------------------------------------------------------- */
static void settings_share_cb(lv_event_t *e)
{
    (void)e;
    uint8_t const next = (s_settings.share_mode == FF_SHARE_GHOST) ? FF_SHARE_LIVE : FF_SHARE_GHOST;
    settings_emit_int(FF_SETTING_SHARE_MODE, next);
}

/* ---------------------------------------------------------------------
 * HAPTICS / GLOW / COLORBLIND — plain booleans.
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

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
}

/* ---------------------------------------------------------------------
 * WATER NUDGE — label + value pill cycling the spec's v1 presets.
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

/* ---------------------------------------------------------------------
 * QUIET HOURS — label + value pill cycling the spec's v1 presets. Each
 * preset sets BOTH quiet_from_min/quiet_to_min, so a tap emits two
 * FF_INTENT_SETTING_SET; the shell persists once per changed field.
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

/* ---------------------------------------------------------------------
 * BRIGHTNESS — caption + "%" over a full-width thin track. The interactive
 * lv_slider is a transparent 44px hit strip (clears the tap floor, stays
 * on-glass at full width); the visible track/fill/knob are non-clickable
 * decorations. Emits on RELEASED (one persisted value per touch).
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

/* A bare non-clickable decoration box. */
static lv_obj_t *settings_deco_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg_hex,
                                   int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void settings_build_brightness(lv_obj_t *list, int32_t row_w)
{
    uint8_t const pct = settings_brightness_clamped();

    lv_obj_t *cap = lv_label_create(list);
    lv_label_set_text(cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(cap, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(cap, 2, 0);
    lv_obj_set_pos(cap, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);

    char pctbuf[8];
    snprintf(pctbuf, sizeof(pctbuf), "%u%%", (unsigned)pct);
    lv_obj_t *pctlbl = lv_label_create(list);
    lv_label_set_text(pctlbl, pctbuf);
    lv_obj_set_style_text_font(pctlbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(pctlbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(pctlbl, LV_ALIGN_TOP_RIGHT, 0, FF_SETTINGS_REL_BRIGHT_CAP_Y);

    /* --- Decorative thin track + amber fill + round amber knob. The knob's
     * center travels [knob_r, row_w - knob_r] over [MIN, MAX]%. --- */
    int32_t const track_h = 6;
    int32_t const track_y = FF_SETTINGS_REL_SLIDER_Y + (FF_SETTINGS_SLIDER_H - track_h) / 2;
    int32_t const knob_sz = 19;
    int32_t const knob_r = knob_sz / 2;

    float const frac = (float)(pct - FF_BRIGHTNESS_MIN_PCT) / (float)(FF_BRIGHTNESS_MAX_PCT - FF_BRIGHTNESS_MIN_PCT);
    int32_t const knob_cx = knob_r + (int32_t)lroundf(frac * (float)(row_w - knob_sz));

    settings_deco_box(list, 0, track_y, row_w, track_h, FF_THEME_COLOR_SURFACE, 3); /* track */
    if (knob_cx > 0) {
        settings_deco_box(list, 0, track_y, knob_cx, track_h, FF_THEME_COLOR_AMBER, 3); /* amber fill to knob center */
    }

    int32_t const knob_y = FF_SETTINGS_REL_SLIDER_Y + (FF_SETTINGS_SLIDER_H - knob_sz) / 2;
    lv_obj_t *knob = settings_deco_box(list, knob_cx - knob_r, knob_y, knob_sz, knob_sz, FF_THEME_COLOR_AMBER,
                                       LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(knob, 5, 0); /* ~5px dark ring so the knob reads over the track */
    lv_obj_set_style_border_color(knob, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_border_opa(knob, LV_OPA_COVER, 0);

    /* --- The interactive, transparent full-width hit strip. --- */
    lv_obj_t *slider = lv_slider_create(list);
    lv_obj_remove_style_all(slider);
    lv_obj_set_size(slider, row_w, FF_SETTINGS_SLIDER_H);
    lv_obj_set_pos(slider, 0, FF_SETTINGS_REL_SLIDER_Y);
    /* lv_slider's constructor sets an ~8px knob-grab ext_click_area (LV_DPX(8),
     * see lv_slider.c); reset it so the hit strip is exactly its 44px box —
     * the visible knob is our own decoration, and the extension would eat into
     * the adjacency gap to the row below (and the on-glass margin sideways). */
    lv_obj_set_ext_click_area(slider, 0);
    lv_slider_set_range(slider, FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, settings_brightness_cb, LV_EVENT_RELEASED, NULL);
}

/* ---------------------------------------------------------------------
 * CALIBRATE TOUCH — full-width surface pill, thin amber border, amber text.
 * On tap emits the shell-owned FF_INTENT_CALIBRATE_TOUCH (the shell runs the
 * device crosshair flow; a no-op in the sim). An action, not a stored value.
 * ------------------------------------------------------------------- */
static void settings_calibrate_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_CALIBRATE_TOUCH, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_calibrate_row(lv_obj_t *list, int32_t rel_y, int32_t row_w)
{
    lv_obj_t *pill = settings_make_pill(list, "CALIBRATE TOUCH", 0, rel_y, row_w, FF_SETTINGS_ROW_H,
                                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER, 2, settings_calibrate_cb, NULL);
    lv_obj_set_style_border_width(pill, 2, 0); /* ~1.5px, rounded up to a device pixel */
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_40 + LV_OPA_10 / 2 /* ~45% */, 0);
}

/* ---------------------------------------------------------------------
 * Edge-fade scrim: a non-clickable BG->transparent vertical gradient over a
 * viewport edge. `top` picks which edge is solid.
 * ------------------------------------------------------------------- */
static void settings_build_scrim(lv_obj_t *puck, int32_t x, int32_t y, int32_t w, bool top)
{
    lv_obj_t *scrim = lv_obj_create(puck);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, w, FF_SETTINGS_SCRIM_H);
    lv_obj_set_pos(scrim, x, y);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(scrim, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_dir(scrim, LV_GRAD_DIR_VER, 0);
    /* Solid edge opaque, inner edge transparent. */
    lv_obj_set_style_bg_main_opa(scrim, top ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_opa(scrim, top ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
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

    /* --- PINNED, centered header: back button + SETTINGS title + name.
     * Built directly on the puck (never inside the scroll list) so it never
     * scrolls away and the sweep checks the back button at its absolute
     * position. --- */
    lv_obj_t *back = lv_button_create(puck);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, FF_SETTINGS_BACK_SZ, FF_SETTINGS_BACK_SZ);
    lv_obj_set_pos(back, FF_SETTINGS_HDR_X, FF_SETTINGS_HDR_Y);
    lv_obj_set_style_bg_color(back, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_font(back_lbl, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(puck);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_pos(title, FF_SETTINGS_HDR_TEXT_X, FF_SETTINGS_TITLE_Y);

    lv_obj_t *name_lbl = lv_label_create(puck);
    lv_obj_set_width(name_lbl, FF_SETTINGS_HDR_TEXT_W);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(name_lbl, (s_settings.my_name[0] != '\0') ? s_settings.my_name : "(unset)");
    lv_obj_set_style_text_font(name_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(name_lbl, 1, 0);
    lv_obj_set_pos(name_lbl, FF_SETTINGS_HDR_TEXT_X, FF_SETTINGS_NAME_Y);

    /* --- The scroll list: an inscribed rectangle in the round glass across
     * its whole height, vertical-only user scroll, scrollbar AUTO. Not
     * clickable itself — the clickable rows inside take the press and LVGL
     * scrolls this parent. --- */
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

    settings_build_brightness(list, row_w);
    settings_build_toggle_row(list, FF_SETTINGS_REL_UNITS_Y, row_w, "UNITS", "FT", "MI",
                              s_settings.imperial ? 0 : 1, settings_units_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_SHARE_Y, row_w, "SHARE", "LIVE", "GHOST",
                              (s_settings.share_mode == FF_SHARE_LIVE)    ? 0
                              : (s_settings.share_mode == FF_SHARE_GHOST) ? 1
                                                                          : -1,
                              settings_share_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_HAPTICS_Y, row_w, "HAPTICS", "ON", "OFF",
                              s_settings.haptics ? 0 : 1, settings_haptics_cb);
    settings_build_toggle_row(list, FF_SETTINGS_REL_GLOW_Y, row_w, "GLOW", "ON", "OFF",
                              s_settings.night_glow ? 0 : 1, settings_night_glow_cb);

    char water_buf[16];
    settings_water_label(water_buf, sizeof(water_buf), s_settings.water_min);
    settings_build_value_row(list, FF_SETTINGS_REL_WATER_Y, row_w, "WATER NUDGE", water_buf,
                             s_settings.water_min == 0, settings_water_cb);

    settings_quiet_preset_t const *quiet = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    bool const quiet_off = (quiet != NULL) && (quiet->from_min == 0) && (quiet->to_min == 0);
    settings_build_value_row(list, FF_SETTINGS_REL_QUIET_Y, row_w, "QUIET HOURS",
                             (quiet != NULL) ? quiet->label : "CUSTOM", quiet_off, settings_quiet_cb);

    settings_build_toggle_row(list, FF_SETTINGS_REL_CB_Y, row_w, "COLORBLIND", "ON", "OFF",
                              s_settings.colorblind ? 0 : 1, settings_colorblind_cb);
    settings_build_calibrate_row(list, FF_SETTINGS_REL_CAL_Y, row_w);

    /* --- Edge-fade scrims over the viewport top/bottom (chrome; drawn on the
     * puck, above the list, non-clickable). --- */
    settings_build_scrim(puck, list_margin, FF_SETTINGS_LIST_Y, row_w, true);
    settings_build_scrim(puck, list_margin, FF_SETTINGS_LIST_Y + FF_SETTINGS_LIST_H - FF_SETTINGS_SCRIM_H, row_w,
                         false);
}
