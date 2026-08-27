/**
 * scr_settings.c — see scr_settings.h.
 *
 * ## Round-glass layout, from the start (not retrofitted)
 * Every position below is derived from `ff_layout_safe_margin_x`
 * (app/screens/ff_layout.h), the same shared primitive `scr_compose.c`/
 * `scr_signals.c` use — computed from each row's own worst-case
 * (farthest-from-center) y, not eyeballed pixel offsets. This file learns
 * from those two files' history rather than repeating it: PR #25's UX
 * review caught Compose's chrome sitting tens of pixels off the round
 * glass because it was positioned against the puck's SQUARE bounding box.
 * `targets/sim/tests/test_face_hit_targets.c` sweeps every committed
 * fixture (including this face's, once fixtures exist for it) and fails
 * the build if any hit-rect ever drifts outside the circle or under the
 * 44px hit-target floor — see that file for the assertion this is
 * checked against on every build.
 *
 * ## Row budget: why every control is "label + one cycling value chip"
 * Eight settings (units, share mode, haptics, night glow, water-nudge,
 * quiet hours, UTC offset, colorblind — S17 slice a added the last one)
 * plus a name caption and a back button do not fit the 440px puck as
 * eight full editors (a segmented FT/M control, a three-way share
 * selector, on/off switches, ...) without either shrinking rows under
 * the 44px hit-target floor or pushing the bottom rows out into the
 * pole, where the circle narrows to almost nothing (see
 * FF_SETTINGS_ROW5_Y's own margin at the bottom of this file's layout
 * constants — re-checked, not assumed, when the colorblind row was
 * added; see that constant's own comment). Every row instead follows the ONE pattern the spec
 * itself already prescribes for water/quiet ("tap cycles presets") —
 * extended uniformly to every setting, including the two booleans and
 * share mode: a dim label names the setting, a single value chip on the
 * right shows the CURRENT value in text (so the 2-second glance test
 * still passes — "FT" or "GHOST" reads instantly, same as "90 MIN"
 * would), and a tap advances to the next value in a small fixed cycle.
 * Units and share mode also SHARE one row (two half-width chips) rather
 * than each claiming a full row, since both are short single-word values
 * — same economy applied to haptics/night-glow. This is a judgment call
 * (flagged per AGENTS.md), not dictated by the spec's own "FT/M
 * segmented, share row, two toggles" line, which reads as a description
 * of WHAT settings exist rather than a literal widget-shape mandate — the
 * mockup (this repo's actual layout authority, per CLAUDE.md) is not
 * in-tree for this agent to consult (ff_theme.h's header comment records
 * the same access gap).
 *
 * ## `my_name` is NOT editable in this slice
 * Renaming needs its own live text-entry session — the shell has exactly
 * one such seam today (`ff_shell_t.compose_draft`, S16 slice c3), and it
 * is Compose's, not a generic "any screen can borrow the T9 engine"
 * facility: it resets on every `OPEN_COMPOSE`, its projection
 * (`ff_app_state_t.compose`) is the Compose face's own render slot, and
 * `FF_INTENT_T9_KEY`/`_INSERT`/`_BACKSPACE` write into it unconditionally
 * whenever a takeover isn't up — reusing it here would mean typing a new
 * name while a half-composed message is pending SILENTLY clobbers that
 * draft (or vice versa: opening Settings mid-compose and tapping "back"
 * leaves stray keystrokes sitting in the compose draft), with no shared
 * face to notice or prevent the collision. That is exactly "shell state
 * the seam doesn't carry" — a real rename flow needs its own draft field
 * in `shell_t` (mirroring `compose_draft`) plus a new open/commit intent
 * pair, which is `[api]` shell-seam work beyond a screen-only slice.
 * Flagged per the task brief's own escape hatch rather than forced: this
 * file renders the current `my_name` as a plain caption under the header
 * and ships everything else. Tracked as a follow-up, not silently
 * dropped.
 */
#include "scr_settings.h"

#include <math.h>
#include <stdio.h>

#include "ff_intent.h" /* S16c1/S11b — the emit seam */
#include "ff_layout.h"
#include "ff_settings.h" /* FF_SHARE_LIVE/_ZONES/_GHOST (core/include/ff_settings.h) */
#include "ff_theme.h"
#include "ff_wall.h" /* FF_WALL_OFFSET_MIN_LO/_HI — the UTC-offset stepper's own bounds */

/* ---------------------------------------------------------------------
 * Layout constants — see this file's header comment for the row budget.
 * ------------------------------------------------------------------- */

#define FF_SETTINGS_SAFETY_PX 10.0f /* see scr_compose.c's FF_COMPOSE_SAFETY_PX — same rationale */

#define FF_SETTINGS_HEADER_Y 16

/* Back button — S15 slice c enlarged it past the 44px floor (maintainer
 * field feedback: the Settings back button was hard to hit even sober; the
 * escape hatch someone most needs in a hurry must be an obvious, comfortable
 * 2am/gloves target). 44x44 -> FF_SETTINGS_BACK_W x FF_SETTINGS_BACK_H
 * (64x46). The enlargement is WIDTH-led (64, +45%): the 412 round glass
 * (radius 206) is materially tighter at the top pole than the old 440 puck,
 * and here the button competes for vertical space with a six-row settings
 * stack that must ALL clear the hit floor and stay inside a smaller circle —
 * so height grows only to 46 (past the floor with margin) while width, which
 * costs no vertical budget, carries the rest. A left-anchored button near
 * the top is pushed toward centre-x by the narrowing circle, so rather than a
 * small pill tucked left of a puck-centred title (which no longer fits beside
 * a bigger button), the back button and the SETTINGS title/name form one
 * left-to-right header GROUP whose top corners stay inside the circle.
 * Hit-target-sweep margins are in the PR body; the _Static_assert below
 * proves the 6-row stack still fits the puck square. (A materially BIGGER
 * back button at 412 would mean dropping a settings row — flagged for the
 * maintainer to judge on glass.) */
#define FF_SETTINGS_BACK_W 64
#define FF_SETTINGS_BACK_H 46
#define FF_SETTINGS_HEADER_H FF_SETTINGS_BACK_H

/* Left edge of the back button, and the x the title/name column hangs off
 * (to the right of the button). 127 keeps the button's top-left corner
 * inside the r=206 circle at FF_SETTINGS_HEADER_Y=16 ((127,16) is 42.34k <=
 * 206^2=42.44k from centre (206,206)) while placing the [back | SETTINGS]
 * group so it reads roughly centred on the puck's own x. */
#define FF_SETTINGS_BACK_X 127
#define FF_SETTINGS_HEADER_TEXT_X (FF_SETTINGS_BACK_X + FF_SETTINGS_BACK_W + 10) /* 201 */
#define FF_SETTINGS_TITLE_Y (FF_SETTINGS_HEADER_Y + 2)
#define FF_SETTINGS_NAME_Y  (FF_SETTINGS_HEADER_Y + 26)
#define FF_SETTINGS_NAME_H 12 /* caption, not a control — no hit-target floor applies */

/* Rows enlarged from the 44px floor to FF_SETTINGS_ROW_H (S15c). At 412 the
 * six-row stack is vertically budget-bound — the last row sits close to the
 * bottom pole where the circle has all but closed — so the rows grow as far
 * as the assert and the bottom row's own in-circle width allow (its
 * full-width chip must stay wide enough for "COLORBLIND OFF" at 412), not to
 * an arbitrary large size. Folding the name caption INTO the header group
 * (beside the title, not on its own line above the rows) is what buys the
 * rows the vertical room to clear the floor at all. */
#define FF_SETTINGS_ROW_H   46
#define FF_SETTINGS_ROW_GAP 10

/* Separation between the two chips sharing a row (units+share,
 * haptics+night-glow). PR #68 UX review (Bailey, blocking finding 2):
 * the original 10px was under 1mm of dead space at this puck's ~12px/mm
 * scale (37mm face, per docs/review/ux-raver.md) — "a mis-tap trap ...
 * not just a vibe". 24px (~2mm) gives each 44px-tall pill a real gap a
 * kandi'd or gloved thumb can land in without ambiguity which chip it
 * hit; re-checked against `test_face_hit_targets.c`'s sweep afterward
 * (each chip individually still clears the 44px floor and stays inside
 * the round glass at the widened width). */
#define FF_SETTINGS_CHIP_GAP 24
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP)
/* +8 (not a smaller pad): the enlarged back button's bottom edge sits at
 * HEADER_Y + BACK_H = 62, and row 0's chips span the full width directly
 * below it, so this pad IS the header->row0 hit-target GAP — it must clear
 * FF_HIT_MIN_GAP_PX (8). At 412 this stack is packed tight enough that 8 is
 * the value, not a comfort margin; flagged in the PR body. */
#define FF_SETTINGS_ROWS_Y0 (FF_SETTINGS_HEADER_Y + FF_SETTINGS_HEADER_H + 8)

#define FF_SETTINGS_ROW0_Y (FF_SETTINGS_ROWS_Y0)                        /* units + share       */
#define FF_SETTINGS_ROW1_Y (FF_SETTINGS_ROW0_Y + FF_SETTINGS_ROW_STEP)  /* haptics + night glow */
#define FF_SETTINGS_ROW2_Y (FF_SETTINGS_ROW1_Y + FF_SETTINGS_ROW_STEP)  /* water nudge          */
#define FF_SETTINGS_ROW3_Y (FF_SETTINGS_ROW2_Y + FF_SETTINGS_ROW_STEP)  /* quiet hours          */
#define FF_SETTINGS_ROW4_Y (FF_SETTINGS_ROW3_Y + FF_SETTINGS_ROW_STEP)  /* UTC offset stepper   */
/* S17 slice a: the colorblind toggle, the lowest row. S15c re-fit the whole
 * stack to the 412 puck: with FF_SETTINGS_ROWS_Y0=70, ROW_STEP=56 and
 * ROW_H=46, this row spans y=350..396, leaving 16px of square-bound slack
 * below it (the _Static_assert is the real proof) and — more bindingly — a
 * ~138px in-circle width for its full-width chip at y=396, which is what
 * keeps "COLORBLIND OFF" from overflowing at 412 (verified in the golden). */
#define FF_SETTINGS_ROW5_Y (FF_SETTINGS_ROW4_Y + FF_SETTINGS_ROW_STEP)  /* colorblind toggle    */

_Static_assert(FF_SETTINGS_ROW5_Y + FF_SETTINGS_ROW_H <= FF_THEME_PUCK_PX,
               "settings' last row must stay inside the puck's own square, let alone its circle");

/**
 * settings_safe_margin_x — thin int32_t/ceil wrapper around
 * ff_layout_safe_margin_x, bound to this puck's own center/radius
 * (ff_theme.h) and this file's safety buffer — identical shape to
 * scr_compose.c's compose_safe_margin_x / scr_signals.c's
 * signals_safe_margin_x (both wrap the same shared primitive around the
 * same puck geometry).
 */
static int32_t settings_safe_margin_x(int32_t top_y, int32_t h)
{
    float margin = ff_layout_safe_margin_x((float)top_y, (float)h, (float)FF_THEME_PUCK_RADIUS_PX,
                                            (float)FF_THEME_PUCK_RADIUS_PX, FF_SETTINGS_SAFETY_PX);
    return (int32_t)ceilf(margin);
}

/* ---------------------------------------------------------------------
 * Static build-time snapshot — same convention as scr_compose.c's
 * `s_mode`: every callback below needs to compute "current -> next" from
 * the settings this screen was built with, and LVGL event callbacks carry
 * no argument beyond `user_data`. A snapshot, not live state: nothing
 * here mutates it, and a tap only ever reports through the intent seam
 * (ff_scr_settings_build resets this at entry, so repeated calls within
 * one process — not currently done anywhere, see scr_compose.c's own note
 * on this hazard — start from a clean slate).
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
 * Chip widget: a pill button with a centered label, used for every
 * cycling value in this screen (units, share mode, haptics, night glow,
 * water, quiet). Deliberately the SAME shape scr_compose.c's
 * compose_make_key and scr_signals.c's signals_make_reply_button already
 * use — one more widget-shape convention this codebase repeats rather
 * than inventing a fourth (issue #24 tracks the eventual shared-widget
 * extraction; this file follows the existing pattern in the meantime,
 * same as those two did).
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
 * now itself a tap target forwarding to `cb` (PR #68 UX review, Bailey,
 * non-blocking finding: tapping the label half of a row used to be dead
 * air — no ripple, no state change — which at 2 a.m. reads as "is this
 * thing frozen" rather than "I tapped the wrong half"). The label sits
 * inside a `w`x`h` hit container spanning the row's own height (so it
 * clears the 44px floor by construction, same technique
 * `signals_build_row`'s icon+text rows already use) rather than the bare
 * label growing an `ext_click_area` of its own — a container gives the
 * label a real, checkable box `test_face_hit_targets.c`'s sweep can
 * assert on directly.
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
 * Row 0: UNITS (FT/M) + SHARE (LIVE/ZONES/GHOST). Two half-width chips.
 * ------------------------------------------------------------------- */

static void settings_units_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_IMPERIAL, s_settings.imperial ? 0 : 1);
}

/**
 * ZONES is deliberately NOT cycled into (PR #68 UX review, Bailey,
 * blocking finding 1). Per docs/specs/S11-settings.md's Behavior section
 * ("v1: LIVE/GHOST honored; ZONES=LIVE + issue"), selecting ZONES in this
 * build does not change sharing behavior from LIVE at all — cycling it in
 * as a third, equally-confident amber option would let someone pick
 * "zones only" believing they've restricted their share radius when they
 * haven't, which for a location-privacy control is the worst possible
 * place for a silent no-op (the checklist's "would I ever follow wrong
 * data confidently" item, and yes, here). Fixed as reviewed: LIVE<->GHOST
 * only, a plain two-stop loop, until the ZONES backend (spec slice c)
 * ships and this comment comes out — NOT grayed out or marked
 * "(soon)": an unexplained disabled option at 2 a.m. reads as broken,
 * and absence is cleaner than a control that announces its own
 * incompleteness (per the review's own instruction).
 *
 * If `share_mode` somehow already reads FF_SHARE_ZONES (e.g. a value
 * persisted by some future build that finishes the backend and is then
 * downgraded), one tap moves it to GHOST — same two-stop loop, never
 * back into ZONES from a tap either way.
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

static void settings_build_row0(lv_obj_t *parent)
{
    int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW0_Y, FF_SETTINGS_ROW_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;
    int32_t gap = FF_SETTINGS_CHIP_GAP;
    int32_t units_w = (row_w - gap) * 2 / 5;
    int32_t share_w = row_w - gap - units_w;

    settings_make_chip(parent, s_settings.imperial ? "FT" : "M", margin, FF_SETTINGS_ROW0_Y, units_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, settings_units_cb, NULL);
    settings_make_chip(parent, settings_share_name(s_settings.share_mode), margin + units_w + gap,
                        FF_SETTINGS_ROW0_Y, share_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_AMBER,
                        settings_share_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Row 1: HAPTICS + NIGHT GLOW. Two half-width on/off chips.
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

static void settings_build_row1(lv_obj_t *parent)
{
    int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW1_Y, FF_SETTINGS_ROW_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;
    int32_t gap = FF_SETTINGS_CHIP_GAP;
    int32_t half_w = (row_w - gap) / 2;

    settings_make_chip(parent, s_settings.haptics ? "BUZZ ON" : "BUZZ OFF", margin, FF_SETTINGS_ROW1_Y, half_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                        s_settings.haptics ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM, settings_haptics_cb,
                        NULL);
    settings_make_chip(parent, s_settings.night_glow ? "GLOW ON" : "GLOW OFF", margin + half_w + gap,
                        FF_SETTINGS_ROW1_Y, half_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                        s_settings.night_glow ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM,
                        settings_night_glow_cb, NULL);
}

/* ---------------------------------------------------------------------
 * Row 2: WATER NUDGE — label + a chip cycling the spec's v1 presets
 * (off/45/90/120, docs/specs/S11-settings.md's Behavior section).
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
 * Row 3: QUIET HOURS — label + a chip cycling the spec's v1 presets
 * (off/2a-8a/4a-10a). Each preset sets BOTH quiet_from_min/quiet_to_min,
 * which are two separate ff_settings_t fields (core/include/ff_settings.h)
 * and so two separate FF_INTENT_SETTING_SET emits per tap — the shell
 * persists once per changed field (shell_setting_set's own "changed only"
 * gate), not once per tap, so a tap that changes both fields still only
 * ever writes twice, never in a torn state a reader could observe between
 * them (both dispatch synchronously, same call stack).
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
 * Row 4: UTC OFFSET — "-" / value / "+" stepper, 60-minute steps, clamped
 * to the same [FF_WALL_OFFSET_MIN_LO, FF_WALL_OFFSET_MIN_HI] range
 * ff_shell.c's shell_setting_set validates against (core/include/ff_wall.h)
 * — clamped here too so a tap at either end of the real-world range is a
 * harmless no-op rather than a dead button silently rejected one layer up
 * (CLAUDE.md's "honest data" cuts against a control that looks live but
 * never visibly does anything). Unset (`utc_offset_set == false`) starts
 * from 0 (UTC) on the first tap — there is no "current" numeric value to
 * step from otherwise.
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

/* ---------------------------------------------------------------------
 * Row 5: COLORBLIND — S17 slice a. A single boolean with a SINGLE
 * full-width, self-describing chip ("COLORBLIND ON"/"COLORBLIND OFF"),
 * matching HAPTICS/NIGHT GLOW's self-describing-chip-text idiom (row 1)
 * rather than WATER NUDGE/QUIET HOURS' separate-label-plus-chip shape
 * (rows 2/3) — NOT a stylistic choice, a geometry one: this is the
 * LOWEST row on the face, close enough to the puck's pole that
 * `settings_safe_margin_x` returns a much larger margin here than at any
 * row above it (verified: ~139px at this row's y, vs. ~54px at row 2's),
 * which left a rows-2/3-shaped fixed-110px chip only ~28px of label
 * width to work with — under the 44px hit-target floor
 * (`test_face_hit_targets.c` caught this in review; see AGENTS.md's
 * standing brief on why that sweep exists). A single chip spanning the
 * row's own margin-to-margin width scales WITH the available space
 * instead of fighting it, the same way row 1's half-width chips already
 * do. Same green-on/dim-off color convention as haptics/night-glow — a
 * plain toggle, not an "amber means configured" value like water/quiet's
 * presets.
 * ------------------------------------------------------------------- */

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
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

    /* --- Header GROUP: enlarged back button (dead-end escape, ux-raver
     * checklist item 6) on the left, with the SETTINGS title and the name
     * caption stacked in a column to its right. S15c: the back button is now
     * big enough that it no longer tucks left of a puck-centred title, so the
     * three read as one left-to-right group centred near puck-x (see the
     * FF_SETTINGS_BACK_X / _HEADER_TEXT_X comments). Positioned by fixed
     * puck-local coordinates (not settings_safe_margin_x) precisely because
     * this group is placed as a whole rather than inset row-by-row. --- */

    /* Filled chip background (PR #68 UX review, Bailey, non-blocking):
     * every other tappable thing on this screen is a solid rounded-rect
     * pill; a transparent BACK button was the one control with the
     * LEAST affordance despite being the escape hatch someone most needs
     * in a hurry. Same FF_THEME_COLOR_SURFACE fill as every other chip,
     * matching this screen's own visual grammar instead of standing out
     * as an exception to it. */
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

    /* --- Name caption (display-only, this slice — see header comment).
     * Left-aligned under the title, in the header group's right-hand column
     * (S15c: folded into the header band rather than a dedicated row above
     * the settings, to buy the six rows the vertical budget to clear the
     * hit-target floor at 412). --- */
    lv_obj_t *name_lbl = lv_label_create(puck);
    char name_buf[FF_APP_NAME_LEN + 8];
    snprintf(name_buf, sizeof(name_buf), "NAME: %s", (s_settings.my_name[0] != '\0') ? s_settings.my_name : "(unset)");
    lv_label_set_text(name_lbl, name_buf);
    lv_obj_set_style_text_font(name_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, FF_SETTINGS_HEADER_TEXT_X, FF_SETTINGS_NAME_Y);

    settings_build_row0(puck);
    settings_build_row1(puck);

    /* --- Row 2: WATER NUDGE. ---
     * OFF-color convention (PR #68 UX review, Bailey, non-blocking):
     * dim grey for the off state, matching haptics/night-glow's
     * green-on/grey-off row exactly for the "off" half — amber stays
     * reserved for an actively configured value (this chip isn't a
     * plain boolean like haptics/glow, so it doesn't borrow green for
     * "on"), but OFF now reads the same dim grey everywhere on this
     * screen instead of amber in some rows and grey in others. */
    {
        int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW2_Y, FF_SETTINGS_ROW_H);
        int32_t chip_w = 110;
        int32_t label_w = FF_THEME_PUCK_PX - margin - chip_w - FF_SETTINGS_CHIP_GAP - margin;
        settings_build_row_label(puck, "WATER NUDGE", margin, FF_SETTINGS_ROW2_Y, label_w, FF_SETTINGS_ROW_H,
                                  settings_water_cb);

        char buf[16];
        settings_water_label(buf, sizeof(buf), s_settings.water_min);
        uint32_t const fg = (s_settings.water_min == 0) ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
        settings_make_chip(puck, buf, FF_THEME_PUCK_PX - margin - chip_w, FF_SETTINGS_ROW2_Y, chip_w,
                            FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg, settings_water_cb, NULL);
    }

    /* --- Row 3: QUIET HOURS. Same OFF-color convention as row 2. --- */
    {
        int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW3_Y, FF_SETTINGS_ROW_H);
        int32_t chip_w = 110;
        int32_t label_w = FF_THEME_PUCK_PX - margin - chip_w - FF_SETTINGS_CHIP_GAP - margin;
        settings_build_row_label(puck, "QUIET HOURS", margin, FF_SETTINGS_ROW3_Y, label_w, FF_SETTINGS_ROW_H,
                                  settings_quiet_cb);

        settings_quiet_preset_t const *cur = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
        bool const is_off = (cur != NULL) && (cur->from_min == 0) && (cur->to_min == 0);
        uint32_t const fg = is_off ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
        settings_make_chip(puck, (cur != NULL) ? cur->label : "CUSTOM", FF_THEME_PUCK_PX - margin - chip_w,
                            FF_SETTINGS_ROW3_Y, chip_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg,
                            settings_quiet_cb, NULL);
    }

    /* --- Row 4: UTC OFFSET stepper. --- */
    {
        int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW4_Y, FF_SETTINGS_ROW_H);
        int32_t btn_w = FF_THEME_MIN_HIT_PX;

        settings_make_chip(puck, "-", margin, FF_SETTINGS_ROW4_Y, btn_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE,
                            FF_THEME_COLOR_INK, settings_utc_minus_cb, NULL);
        settings_make_chip(puck, "+", FF_THEME_PUCK_PX - margin - btn_w, FF_SETTINGS_ROW4_Y, btn_w,
                            FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, settings_utc_plus_cb, NULL);

        char buf[16];
        settings_utc_label(buf, sizeof(buf), s_settings.utc_offset_set, s_settings.utc_offset_min);
        lv_obj_t *val = lv_label_create(puck);
        lv_label_set_text(val, buf);
        lv_obj_set_style_text_font(val, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(FF_THEME_COLOR_INK), 0);
        lv_obj_align(val, LV_ALIGN_TOP_MID, 0, FF_SETTINGS_ROW4_Y + (FF_SETTINGS_ROW_H - 16) / 2);
    }

    /* --- Row 5: COLORBLIND. Same OFF-color convention as haptics/glow
     * (row 1): dim grey off, live-green on — a plain toggle. Single
     * full-width chip — see this file's row-5 comment above for why. --- */
    {
        int32_t margin = settings_safe_margin_x(FF_SETTINGS_ROW5_Y, FF_SETTINGS_ROW_H);
        int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;

        uint32_t const fg = s_settings.colorblind ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM;
        settings_make_chip(puck, s_settings.colorblind ? "COLORBLIND ON" : "COLORBLIND OFF", margin,
                            FF_SETTINGS_ROW5_Y, row_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg,
                            settings_colorblind_cb, NULL);
    }
}
