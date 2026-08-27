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
 * ## Two pages, not one saturated stack (#99/#100)
 * The face now PAGINATES. Adding the #100 brightness slider to the old
 * single-page stack was the row that broke the camel's back: eight settings
 * (units, share, haptics, night glow, water, quiet, UTC, colorblind) plus a
 * brightness slider, a name caption, and a back button cannot all clear the
 * 44px hit floor AND stay inside the 412 round glass on one page — the last
 * rows land in the bottom pole where the circle has all but closed. The two
 * sanctioned fixes for that are a scrollable list or pagination; this face
 * PAGINATES, because a scrollable list is the higher-risk of the two against
 * this repo's own hardest gate. `test_face_hit_targets.c` walks every
 * CLICKABLE element and checks its ABSOLUTE hit-rect (lv_obj_get_click_area,
 * which returns scroll-shifted coords) against the circle — so a scrollable
 * list would put its off-viewport rows at off-glass coordinates and FAIL the
 * sweep, exactly the class of bug scr_nav.c's "build the active tile only"
 * fix (issue #29) closed. Pagination sidesteps it entirely: `ff_scr_settings_
 * build` builds ONLY the active page's controls (page 0: units/share,
 * haptics/glow, water, quiet · page 1: brightness slider, UTC, colorblind),
 * so every built control is always in the wide middle band, on-glass, clear
 * of the floor with margin. The page is shell-owned view state (like
 * Compose's ABC/123/SYM mode), cycled by a "PAGE n/2" chip via
 * FF_INTENT_SETTINGS_PAGE. Halving the per-page row count is also what buys
 * the enlarged back button its extra HEIGHT (#99) — see FF_SETTINGS_BACK_H.
 *
 * Each row still follows the ONE pattern the spec
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

/* Back button — GROWN for #99 (maintainer field feedback: still "pretty
 * tight" even sober after S15c's width-only bump; the escape hatch someone
 * most needs in a hurry must be an obvious, comfortable 2am/gloves target).
 * 64x46 -> 64x76 (+65% height, +65% area). The growth is HEIGHT-led, which
 * is exactly what #99's own note asks for ("if [pagination] frees vertical
 * budget, use it to grow the back button's HEIGHT too"): paginating this
 * face (see this file's header) means each page carries at most four control
 * rows instead of six, so row 0 starts far enough down the glass to leave
 * the header band a full 76px tall. WIDTH stays 64 on purpose: at the top
 * pole (y=16) the circle is only ~159px wide, and the button shares that
 * band with the SETTINGS title + name to its right (HEADER_TEXT_X=201) —
 * widening the button would push the title off the round glass. So height
 * carries the growth; the width is pinned by the title beside it, flagged
 * for the maintainer to judge on glass. The top corners are unchanged from
 * S15c (same BACK_X/HEADER_Y), so they clear the circle exactly as before;
 * the taller bottom edge sits at y=92, deep in the wide part of the glass. */
#define FF_SETTINGS_BACK_W 64
#define FF_SETTINGS_BACK_H 76
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

/* Rows — grown from the S15c 46px to 48 (past the 44 floor with real
 * margin now that pagination halved the per-page row count, so the sweep
 * clears with slack rather than scraping). */
#define FF_SETTINGS_ROW_H   48
#define FF_SETTINGS_ROW_GAP 12

/* Separation between the two chips sharing a row (units+share,
 * haptics+night-glow). PR #68 UX review (Bailey, blocking finding 2):
 * the original 10px was under 1mm of dead space at this puck's ~12px/mm
 * scale (37mm face, per docs/review/ux-raver.md) — "a mis-tap trap ...
 * not just a vibe". 24px (~2mm) gives each pill a real gap a kandi'd or
 * gloved thumb can land in without ambiguity which chip it hit; re-checked
 * against `test_face_hit_targets.c`'s sweep afterward. */
#define FF_SETTINGS_CHIP_GAP 24
#define FF_SETTINGS_ROW_STEP (FF_SETTINGS_ROW_H + FF_SETTINGS_ROW_GAP) /* 60 */
/* Row 0 begins 8px below the taller back button (bottom at y=92) — this pad
 * IS the header->row0 hit-target GAP, so it must clear FF_HIT_MIN_GAP_PX
 * (8). Only the LEFT half of row 0 sits directly under the button anyway;
 * 8 is the floor, and pagination leaves plenty of room below. */
#define FF_SETTINGS_ROWS_Y0 (FF_SETTINGS_HEADER_Y + FF_SETTINGS_HEADER_H + 8) /* 100 */

/* --- Page 0 stack: units/share, haptics/glow, water, quiet (4 rows). --- */
#define FF_SETTINGS_ROW0_Y (FF_SETTINGS_ROWS_Y0)                        /* 100: units + share       */
#define FF_SETTINGS_ROW1_Y (FF_SETTINGS_ROW0_Y + FF_SETTINGS_ROW_STEP)  /* 160: haptics + night glow */
#define FF_SETTINGS_ROW2_Y (FF_SETTINGS_ROW1_Y + FF_SETTINGS_ROW_STEP)  /* 220: water nudge          */
#define FF_SETTINGS_ROW3_Y (FF_SETTINGS_ROW2_Y + FF_SETTINGS_ROW_STEP)  /* 280: quiet hours (ends 328) */

/* --- Page 1 stack: brightness slider (#100), UTC stepper, colorblind. --- */
#define FF_SETTINGS_BRIGHT_CAP_Y (FF_SETTINGS_ROWS_Y0)                    /* 100: "BRIGHTNESS  70%" caption */
#define FF_SETTINGS_BRIGHT_CAP_H 24
#define FF_SETTINGS_SLIDER_Y (FF_SETTINGS_BRIGHT_CAP_Y + FF_SETTINGS_BRIGHT_CAP_H + 6) /* 130 */
#define FF_SETTINGS_SLIDER_H 48                                            /* clears the 44 floor with margin */
/* The UTC row sits +20 below the slider TRACK, not the usual +12: an
 * lv_slider's KNOB overhangs its track by ~7px top and bottom, so the
 * slider's real hit-rect (what test_face_hit_targets.c's adjacency pass
 * measures via lv_obj_get_click_area) is ~62px tall, not 48. A +12 track gap
 * left only 5px of clickable clearance to the "-" button — under the 8px
 * adjacency floor. +20 restores a real ~13px gap (verified by the sweep,
 * clang AND gcc-14). */
#define FF_SETTINGS_P1_UTC_Y (FF_SETTINGS_SLIDER_Y + FF_SETTINGS_SLIDER_H + 20) /* 198: UTC stepper */
#define FF_SETTINGS_P1_CB_Y  (FF_SETTINGS_P1_UTC_Y + FF_SETTINGS_ROW_STEP)      /* 250: colorblind (ends 298) */

/* --- Page-nav chip, shared by both pages: cycles FF_INTENT_SETTINGS_PAGE.
 * Sits below each page's controls in the wide lower-middle band, centred on
 * puck-x. A single full-hit-target chip ("PAGE 1/2" / "PAGE 2/2 >"), same
 * pill grammar as every other control. --- */
#define FF_SETTINGS_NAV_H FF_SETTINGS_ROW_H  /* 48 */
#define FF_SETTINGS_NAV_W 168
#define FF_SETTINGS_NAV_Y 340                /* 340..388 */

_Static_assert(FF_SETTINGS_ROW3_Y + FF_SETTINGS_ROW_H <= FF_THEME_PUCK_PX,
               "settings page-0 last row must stay inside the puck's own square");
_Static_assert(FF_SETTINGS_P1_CB_Y + FF_SETTINGS_ROW_H <= FF_THEME_PUCK_PX,
               "settings page-1 last row must stay inside the puck's own square");
_Static_assert(FF_SETTINGS_NAV_Y + FF_SETTINGS_NAV_H <= FF_THEME_PUCK_PX,
               "settings page-nav chip must stay inside the puck's own square");

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

/* --- WATER NUDGE row (page 0). Label + preset chip; dim OFF / amber set. --- */
static void settings_build_water_row(lv_obj_t *parent, int32_t y)
{
    int32_t margin = settings_safe_margin_x(y, FF_SETTINGS_ROW_H);
    int32_t chip_w = 110;
    int32_t label_w = FF_THEME_PUCK_PX - margin - chip_w - FF_SETTINGS_CHIP_GAP - margin;
    settings_build_row_label(parent, "WATER NUDGE", margin, y, label_w, FF_SETTINGS_ROW_H, settings_water_cb);

    char buf[16];
    settings_water_label(buf, sizeof(buf), s_settings.water_min);
    uint32_t const fg = (s_settings.water_min == 0) ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
    settings_make_chip(parent, buf, FF_THEME_PUCK_PX - margin - chip_w, y, chip_w, FF_SETTINGS_ROW_H,
                        FF_THEME_COLOR_SURFACE, fg, settings_water_cb, NULL);
}

/* --- QUIET HOURS row (page 0). Same OFF-color convention as water. --- */
static void settings_build_quiet_row(lv_obj_t *parent, int32_t y)
{
    int32_t margin = settings_safe_margin_x(y, FF_SETTINGS_ROW_H);
    int32_t chip_w = 110;
    int32_t label_w = FF_THEME_PUCK_PX - margin - chip_w - FF_SETTINGS_CHIP_GAP - margin;
    settings_build_row_label(parent, "QUIET HOURS", margin, y, label_w, FF_SETTINGS_ROW_H, settings_quiet_cb);

    settings_quiet_preset_t const *cur = settings_current_quiet(s_settings.quiet_from_min, s_settings.quiet_to_min);
    bool const is_off = (cur != NULL) && (cur->from_min == 0) && (cur->to_min == 0);
    uint32_t const fg = is_off ? FF_THEME_COLOR_DIM : FF_THEME_COLOR_AMBER;
    settings_make_chip(parent, (cur != NULL) ? cur->label : "CUSTOM", FF_THEME_PUCK_PX - margin - chip_w, y, chip_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg, settings_quiet_cb, NULL);
}

/* --- UTC OFFSET stepper row (page 1). "-" / value / "+", 60-min steps. --- */
static void settings_build_utc_row(lv_obj_t *parent, int32_t y)
{
    int32_t margin = settings_safe_margin_x(y, FF_SETTINGS_ROW_H);
    int32_t btn_w = FF_THEME_MIN_HIT_PX + 8; /* 52 — past the floor with margin (the sweep wants slack, #99) */

    settings_make_chip(parent, "-", margin, y, btn_w, FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK,
                        settings_utc_minus_cb, NULL);
    settings_make_chip(parent, "+", FF_THEME_PUCK_PX - margin - btn_w, y, btn_w, FF_SETTINGS_ROW_H,
                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, settings_utc_plus_cb, NULL);

    char buf[16];
    settings_utc_label(buf, sizeof(buf), s_settings.utc_offset_set, s_settings.utc_offset_min);
    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, buf);
    lv_obj_set_style_text_font(val, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(val, LV_ALIGN_TOP_MID, 0, y + (FF_SETTINGS_ROW_H - 16) / 2);
}

/* ---------------------------------------------------------------------
 * COLORBLIND (page 1) — S17 slice a. A single boolean with a SINGLE
 * full-width, self-describing chip ("COLORBLIND ON"/"COLORBLIND OFF"),
 * matching HAPTICS/NIGHT GLOW's self-describing-chip-text idiom. Same
 * green-on/dim-off convention — a plain toggle, not an "amber means
 * configured" value like water/quiet's presets.
 * ------------------------------------------------------------------- */

static void settings_colorblind_cb(lv_event_t *e)
{
    (void)e;
    settings_emit_int(FF_SETTING_COLORBLIND, s_settings.colorblind ? 0 : 1);
}

static void settings_build_colorblind_row(lv_obj_t *parent, int32_t y)
{
    int32_t margin = settings_safe_margin_x(y, FF_SETTINGS_ROW_H);
    int32_t row_w = FF_THEME_PUCK_PX - 2 * margin;
    uint32_t const fg = s_settings.colorblind ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_DIM;
    settings_make_chip(parent, s_settings.colorblind ? "COLORBLIND ON" : "COLORBLIND OFF", margin, y, row_w,
                        FF_SETTINGS_ROW_H, FF_THEME_COLOR_SURFACE, fg, settings_colorblind_cb, NULL);
}

/* ---------------------------------------------------------------------
 * BRIGHTNESS (page 1, #100) — a caption ("BRIGHTNESS  70%") over an
 * lv_slider spanning the row's in-circle width. The slider's range is the
 * setting's own honest bounds [FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT]
 * (ff_settings.h) — the floor is non-zero so the knob can never reach a
 * black, unrecoverable screen. The emit fires on LV_EVENT_RELEASED (once
 * per touch, tap-to-position included), not on every VALUE_CHANGED frame of
 * a drag, so a drag persists a single final value rather than spamming the
 * store; the shell clamps + persists and the next projection repaints the
 * knob + caption. On the sim this is a pure render (goldens are single-frame
 * and never fire a touch) — the knob simply sits at the stored percent,
 * which is why the slider needs no interaction to be golden-testable.
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

static void settings_build_brightness(lv_obj_t *parent)
{
    uint8_t const pct = settings_brightness_clamped();

    /* Caption: "BRIGHTNESS" left, "NN%" right — a plain label pair (not a
     * control), inset to the slider's own margin so the three read as one
     * block. */
    int32_t cap_margin = settings_safe_margin_x(FF_SETTINGS_BRIGHT_CAP_Y, FF_SETTINGS_BRIGHT_CAP_H);
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, "BRIGHTNESS");
    lv_obj_set_style_text_font(cap, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, cap_margin, FF_SETTINGS_BRIGHT_CAP_Y);

    char pctbuf[8];
    snprintf(pctbuf, sizeof(pctbuf), "%u%%", (unsigned)pct);
    lv_obj_t *pctlbl = lv_label_create(parent);
    lv_label_set_text(pctlbl, pctbuf);
    lv_obj_set_style_text_font(pctlbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(pctlbl, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_align(pctlbl, LV_ALIGN_TOP_RIGHT, -cap_margin, FF_SETTINGS_BRIGHT_CAP_Y);

    /* The slider itself — the row's full in-circle width, SLIDER_H tall so
     * its click box clears the 44 floor with margin. */
    int32_t margin = settings_safe_margin_x(FF_SETTINGS_SLIDER_Y, FF_SETTINGS_SLIDER_H);
    int32_t track_w = FF_THEME_PUCK_PX - 2 * margin;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, track_w, FF_SETTINGS_SLIDER_H);
    lv_obj_set_pos(slider, margin, FF_SETTINGS_SLIDER_Y);
    lv_slider_set_range(slider, FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT);
    lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    /* Explicit palette so it reads on the dark puck (the default theme
     * styles would fight FF_THEME_COLOR_BG): SURFACE track, amber filled
     * indicator + knob, matching the "amber = the live value" grammar the
     * water/quiet chips already use. */
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
 * Page-nav chip — cycles FF_INTENT_SETTINGS_PAGE. Shared by both pages,
 * centred in the wide lower-middle band. Text shows the CURRENT page
 * (never a mystery toggle, same rule Compose's mode chip follows).
 * ------------------------------------------------------------------- */

static void settings_page_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_SETTINGS_PAGE, .u = {0}};
    ff_intent_emit(&in);
}

static void settings_build_nav(lv_obj_t *parent)
{
    char buf[16];
    unsigned const page1 = (unsigned)s_settings.page + 1u; /* 1-based for the human */
    /* A trailing ">" on every page but the last hints "there's more"; the
     * cycle still wraps (last page -> page 0), so the chip is never a dead
     * end. */
    if (s_settings.page + 1 < FF_SETTINGS_PAGE_COUNT) {
        snprintf(buf, sizeof(buf), "PAGE %u/%u >", page1, (unsigned)FF_SETTINGS_PAGE_COUNT);
    } else {
        snprintf(buf, sizeof(buf), "PAGE %u/%u", page1, (unsigned)FF_SETTINGS_PAGE_COUNT);
    }
    int32_t x = (FF_THEME_PUCK_PX - FF_SETTINGS_NAV_W) / 2;
    settings_make_chip(parent, buf, x, FF_SETTINGS_NAV_Y, FF_SETTINGS_NAV_W, FF_SETTINGS_NAV_H,
                        FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK, settings_page_cb, NULL);
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
     * caption stacked in a column to its right. Shared by every page. --- */
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

    /* --- Only the ACTIVE page's controls are built (#99/#100). This is what
     * keeps every clickable on-glass for test_face_hit_targets.c's sweep: a
     * scrollable list would leave off-viewport rows at off-glass absolute
     * coords the sweep reads verbatim. Same "build the active tile only"
     * shape scr_nav.c uses (issue #29). --- */
    switch (s_settings.page) {
    default: /* out-of-range page (shouldn't happen — shell wraps) falls to page 0 */
    case 0:
        settings_build_row0(puck);
        settings_build_row1(puck);
        settings_build_water_row(puck, FF_SETTINGS_ROW2_Y);
        settings_build_quiet_row(puck, FF_SETTINGS_ROW3_Y);
        break;
    case 1:
        settings_build_brightness(puck);
        settings_build_utc_row(puck, FF_SETTINGS_P1_UTC_Y);
        settings_build_colorblind_row(puck, FF_SETTINGS_P1_CB_Y);
        break;
    }

    settings_build_nav(puck);
}
