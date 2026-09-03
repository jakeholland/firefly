/**
 * scr_launcher.c — see scr_launcher.h.
 *
 * S26 slice e VISUAL REFRESH (2026-09-01, maintainer's on-glass design
 * pick — docs/specs/S26-device-lifecycle.md's nav-model section, "visual:
 * compass ring" note). Replaces the shipped 2-over-3 grid
 * (`launcher_make_circle`, five identically-styled 96px squares) with a
 * COMPASS RING: Radar becomes a 120px HUB disc at the puck's own center,
 * and the remaining faces sit as 88px SATELLITE discs on a 128px-radius
 * orbit around it — the design lifted from the design canvas's chosen
 * artboard (Main.dc.html "Home · at rest" / States.dc.html "Home · Map
 * pressed"; both are geometry/visual reference only, not instructions
 * this file follows blindly — every number below was independently
 * re-derived and hit-target-proven, the same discipline the retired
 * 2-over-3 grid's own layout comment used).
 *
 * Layout constants + every helper here are a SELF-CONTAINED local copy,
 * not shared with `scr_power_menu.c`'s (near-identical) button helper —
 * same file-static, nothing-to-import tradeoff that file's own top
 * comment documents against `scr_flare.c`.
 *
 * ## The hub + 4-satellite geometry, proven (not just argued)
 * Hub: 120x120 square at the puck's center (206,206) — farthest corner
 * at sqrt(60^2+60^2) ~= 84.9px from center, nowhere near the 206px glass
 * radius. Satellite N-agnostic placement (see `launcher_deg_to_offset`):
 * angle 0 = top, stepping 360/N degrees clockwise, at a fixed 128px
 * orbit radius. At N=4 (today: Inbox/Lineup/Settings/Map, the cardinal
 * points) this lands 88x88 squares at (206,78) top, (334,206) right,
 * (206,334) bottom, (78,206) left. Farthest-corner distance from center
 * is IDENTICAL for all four by symmetry: offset (44,128+44)=(44,172),
 * distance sqrt(44^2+172^2) ~= 177.5px — 28.5px inside the 206px glass
 * radius, clearing the spec's "everything >= 10px inside" floor with
 * margin to spare. Hub-to-satellite edge gap: 128 - (60+44) = 24px;
 * satellite-to-satellite (adjacent, e.g. top/right) edge gap:
 * sqrt((128-88)^2 + (128-88)^2) ~= 56.6px — both comfortably clear
 * `FF_HIT_MIN_GAP_PX` (8px). None of this is pentagon math (that hazard
 * analysis lived in the retired 2-over-3 grid's own comment, PR #142) —
 * a hub-plus-cardinal-satellites layout has no rotational-symmetry
 * constraint to fight, which is exactly why it clears both hazards with
 * far more margin than the shape it replaces.
 *
 * ## N-agnostic satellite layout — the Music-readiness contract
 * Every satellite's placement angle is COMPUTED, not hand-typed:
 * `ff_scr_launcher_satellite_deg(compass_pos, n)` returns
 * `compass_pos * (360 / n)` (0 = top, clockwise) — a small pure
 * function, unit-tested directly (app/screens/tests/test_scr_intent.c's
 * `S26e_satellite_deg_is_n_agnostic`) against both today's N=4 (the
 * four cardinal points) and a hypothetical N=5 (the design canvas's own
 * pentagon), independent of anything this file renders. `sats[]` stores
 * each entry's `compass_pos` (which of the N evenly-spaced slots it
 * occupies), not the angle itself — see that array's own comment for
 * why array order is launcher_idx order rather than compass order (an
 * existing test's benefit). Adding a fifth real, routable app later
 * means `LAUNCHER_SAT_COUNT = 5` and one new descriptor with
 * `compass_pos = 4` — the angle set itself is computed by the same
 * formula, never retyped. Today `n` is 4: this file does NOT ship a
 * "Music" tile — there is no fifth app to route to yet, and a tappable
 * circle that goes nowhere is its own kind of dishonesty (CLAUDE.md's
 * honesty rule extends past data to controls).
 *
 * ## Removed from the design canvas: the orbit tick
 * The canvas's "Home · at rest" artboard also drew a small 2x10px amber
 * tick at the top of the orbit ring (a decorative north mark). Dropped
 * per the maintainer's explicit call during this implementation — see
 * the PR body. The 1px hairline orbit ring itself (r=128, amber 16%)
 * stays.
 *
 * ## Icon pipeline — LVGL primitives, not image assets
 * The design's five icons are drawn SVGs (radar scope, three-line
 * lineup, envelope, nav arrow, gear). The brief's first-choice pipeline
 * (render each to PNG at the sizes needed, then LVGLImage.py -> A8
 * recolorable C arrays under app/screens/assets/) needs an SVG
 * rasterizer on the build host. This machine has none — `rsvg-convert`,
 * ImageMagick (`magick`/`convert`), and Python `cairosvg` are all
 * absent, and this build sandbox has no network access to install one
 * (verified: `pip3 install cairosvg` refuses, PEP 668 externally-managed
 * environment, and there is no reachable package index anyway). Falling
 * back to the brief's own documented contingency: every icon below is
 * drawn with LVGL primitives (`lv_arc` rings/wedges, `lv_line` segments/
 * polylines, plain `lv_obj` circles for dots/outline shapes) —
 * asset-free (zero new `.rodata` image blobs; flash cost is ordinary
 * `.text` code size, reported in the PR body) and trivially recolorable
 * at runtime: the SAME generic `launcher_recolor_tree` walk that flips a
 * whole circle's content between amber and BG on press works on every
 * icon uniformly, because every primitive it touches is a plain
 * lv_obj/lv_line/lv_arc/lv_label — see that function's doc comment.
 *
 * A closer match to the design's FILLED radar-sweep wedge and FILLED
 * nav-arrow was possible via `lv_draw_triangle` (this file's sibling
 * `scr_radar.c` already has that exact custom-draw-callback pattern for
 * its own arrowhead) — deliberately NOT used here: a filled shape drawn
 * via a custom `LV_EVENT_DRAW_MAIN` callback bakes its color into a
 * static descriptor at build time, which `launcher_recolor_tree`'s
 * generic per-widget-class walk cannot reach or invalidate without a
 * second, special-cased recolor path per filled shape. Outline-only
 * rings/lines/wedges for every icon keeps ONE recolor mechanism correct
 * for all five — correctness of the press-state contract (design:
 * "every icon + label turns BG on press") over exact visual match to
 * the mockup's fills. Interpretation call, noted per AGENTS.md.
 *
 * ## Index -> face mapping: UNCHANGED
 * `launcher_idx` (0=Radar, 1=Now/Lineup, 2=Signals/Inbox, 3=Map,
 * 4=Settings — `ff_intent.h`'s `FF_INTENT_LAUNCHER_SELECT` payload,
 * `ff_shell.c`'s `k_launcher_faces` table) is NOT touched by this visual
 * rework: it is a semantic identifier, independent of where a circle is
 * DRAWN. The compass ring's satellite RENDER order (Inbox top, Lineup
 * right, Settings bottom, Map left, clockwise) is new; the index each
 * one emits when tapped is the same value the retired grid emitted for
 * that same face. No routing test changes.
 */
#include "scr_launcher.h"

#include <math.h>
#include <stdio.h>

#include "ff_intent.h"
#include "ff_theme.h"
#include "scr_banner.h" /* S26 slice d — the notification banner, now composited on the launcher too */
#include "scr_inbox.h" /* ff_scr_inbox_unread_count — the Inbox circle's badge */
#include "scr_nav.h" /* ff_scr_nav_mask_clickables_under_banner — the shared banner-coverage rule */
#include "lvgl.h"

/* ---------------------------------------------------------------------
 * Layout constants.
 * ------------------------------------------------------------------- */

#define LAUNCHER_HUB_DIAM      120   /* design: Radar hub disc */
#define LAUNCHER_HUB_ICON_PX   46    /* design: radar-scope icon inside the hub */
#define LAUNCHER_SAT_DIAM      88    /* design: satellite discs */
#define LAUNCHER_SAT_ICON_PX   30    /* design: satellite icon size */
#define LAUNCHER_ORBIT_RADIUS_PX 128.0f /* design: satellite orbit radius from center */
#define LAUNCHER_SAT_COUNT     4     /* today: Inbox, Lineup, Settings, Map — see this file's top comment */

#define LAUNCHER_HUB_ICON_DY   (-9)  /* icon centered above the hub's own center, label below (design flex column) */
#define LAUNCHER_HUB_CAPTION_DY 26
#define LAUNCHER_SAT_ICON_DY   (-9)
#define LAUNCHER_SAT_CAPTION_DY 18

/* Design: status row at y ~= 368 (368 - 206 puck center = dy 162). NOT
 * used verbatim: the design canvas measured that number against its own
 * PENTAGON (N=5, with Music) reference, where no satellite sits at the
 * exact bottom cardinal point — at N=4 (today), Settings DOES sit dead
 * center-bottom (180deg), and its own disc's bottom edge already
 * reaches y=378 (center 334 + SAT_DIAM/2 44) — 10px BELOW the design's
 * literal 368, which visibly crowded the Settings caption against the
 * status row when built at that value (see the PR body). Pushed to
 * dy=186 (y=392) instead: 14px clear of the Settings disc's own bottom
 * edge, and still >= 10px inside the 206px glass radius at its own
 * widest extent. Interpretation call, noted per AGENTS.md. */
#define LAUNCHER_STATUS_ROW_DY 186.0f

/* Opacities the design expresses as CSS rgba() alpha — no named LV_OPA_*
 * step lands on these exact percentages, so they're computed once here
 * rather than repeated as bare numeric literals at each call site. */
#define LAUNCHER_OPA_8  (lv_opa_t)20   /* satellite hairline ring: ink @ 8% */
#define LAUNCHER_OPA_16 (lv_opa_t)41   /* orbit ring: amber @ 16% */
#define LAUNCHER_OPA_55 (lv_opa_t)140  /* hub ring (and its icon's inner ring): amber @ 55% */
#define LAUNCHER_OPA_20 (lv_opa_t)51   /* hub glow shadow: amber @ ~20% (this file's own closest LVGL approximation of the design's CSS blur, not a transcribed number) */
#define LAUNCHER_OPA_28 (lv_opa_t)71   /* radar icon sweep wedge fill: amber @ 28% (design: fill-opacity 0.28) */

_Static_assert(LAUNCHER_SAT_DIAM >= 56, "launcher satellites must clear the spec's 56px floor");
_Static_assert(LAUNCHER_SAT_DIAM >= FF_THEME_MIN_HIT_PX, "launcher satellites must clear the shared 44px hit floor");
_Static_assert(LAUNCHER_HUB_DIAM >= FF_THEME_MIN_HIT_PX, "launcher hub must clear the shared 44px hit floor");
/* Hub<->satellite edge-to-edge gap at the fixed 128px orbit radius:
 * 128 - (HUB_DIAM/2 + SAT_DIAM/2) must clear FF_HIT_MIN_GAP_PX. See this
 * file's top comment for the full numeric proof (hub/satellite corners
 * and the satellite-to-satellite gap, which is even larger and not worth
 * a second assert). */
_Static_assert((int32_t)LAUNCHER_ORBIT_RADIUS_PX - (LAUNCHER_HUB_DIAM / 2 + LAUNCHER_SAT_DIAM / 2) >=
                   FF_HIT_MIN_GAP_PX,
               "hub/satellite edge gap must clear the adjacency floor");

/* ---------------------------------------------------------------------
 * launcher_deg_to_offset — the N-agnostic satellite placement primitive.
 *
 * Same convention as `radar_layout.c`'s own `deg_to_offset` (0 deg = up,
 * increasing clockwise — this file's own copy, not a shared include, per
 * this file's established "self-contained, nothing to import" rule):
 * `dx = radius*sin(deg)`, `dy = -radius*cos(deg)`. At deg=0 this is
 * straight up (top); at deg=90, straight right — matching the design
 * canvas's own satellite placement (independently re-derived from
 * Main.dc.html's absolute pixel positions, not copied from its markup).
 * ------------------------------------------------------------------- */
#define LAUNCHER_PI 3.14159265358979323846f

static void launcher_deg_to_offset(float deg, float radius, float *dx, float *dy)
{
    float const rad = deg * (LAUNCHER_PI / 180.0f);
    *dx = radius * sinf(rad);
    *dy = -radius * cosf(rad);
}

/**
 * ff_scr_launcher_satellite_deg — the N-agnostic satellite ANGLE
 * formula itself, factored out to a small pure function (no LVGL, no
 * app state) specifically so it is unit-testable on the host without
 * building a screen: `deg = compass_pos * (360 / n)`, in
 * `launcher_deg_to_offset`'s own 0-deg-is-top-clockwise convention.
 * `compass_pos` is which of the `n` evenly-spaced compass slots a
 * satellite occupies (0 = top, then clockwise) — NOT the same axis as
 * `launcher_idx` (see the satellite descriptor table's own comment for
 * why those two orderings differ in this file). At `n=4` this yields
 * the four cardinal points {0, 90, 180, 270}; at `n=5`, the design
 * canvas's own pentagon {0, 72, 144, 216, 288} — see
 * app/screens/tests/test_scr_intent.c's
 * `S26e_satellite_deg_is_n_agnostic` for both, proven directly against
 * this function rather than against rendered pixel positions.
 *
 * `n <= 0` returns 0.0 (defensive: never divide by zero) — not a case
 * any real caller reaches (LAUNCHER_SAT_COUNT is a positive compile-time
 * constant), but a pure function with no caller-side guard should still
 * behave defined for any input.
 */
float ff_scr_launcher_satellite_deg(int compass_pos, int n)
{
    if (n <= 0) {
        return 0.0f;
    }
    return (float)compass_pos * (360.0f / (float)n);
}

/* ---------------------------------------------------------------------
 * launcher_recolor_tree — the one press-state recolor mechanism every
 * icon and caption shares (see this file's top comment, "Icon
 * pipeline").
 *
 * Walks a plain LVGL widget hierarchy (icon primitives + a caption
 * label) built entirely from four widget classes — lv_label, lv_line,
 * lv_arc, and a bare lv_obj used either as a filled dot (bg_color) or an
 * outline shape (border_color) — and recolors every leaf to `color_hex`,
 * recursing through any plain container (the icon wrapper). This is
 * generic on purpose: it is called with EVERY circle's direct children
 * (icon wrapper + caption) as the recursion roots, so a new icon added
 * later needs no recolor code of its own as long as it is built from
 * these same four primitive kinds — the same "one mechanism, not one per
 * icon" reasoning this file's top comment gives for skipping filled
 * shapes.
 * ------------------------------------------------------------------- */
static void launcher_recolor_tree(lv_obj_t *obj, uint32_t color_hex)
{
    lv_color_t const c = lv_color_hex(color_hex);
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_text_color(obj, c, 0);
        return;
    }
    if (lv_obj_check_type(obj, &lv_line_class)) {
        lv_obj_set_style_line_color(obj, c, 0);
        return;
    }
    if (lv_obj_check_type(obj, &lv_arc_class)) {
        lv_obj_set_style_arc_color(obj, c, LV_PART_MAIN);
        return;
    }
    uint32_t const n = lv_obj_get_child_count(obj);
    if (n == 0) {
        /* A leaf lv_obj is either a filled dot (bg_color visible) or an
         * outline shape (border_color visible) — set both; whichever
         * has nonzero opacity is the one that actually shows. */
        lv_obj_set_style_bg_color(obj, c, 0);
        lv_obj_set_style_border_color(obj, c, 0);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        launcher_recolor_tree(lv_obj_get_child(obj, i), color_hex);
    }
}

/* Registered on every circle (hub + satellites) for PRESSED/RELEASED/
 * PRESS_LOST: walks the circle's own direct children (icon wrapper,
 * caption label) and recolors each subtree amber<->BG. The circle's OWN
 * bg fill is handled separately, by the ordinary LV_STATE_PRESSED style
 * selector on the button itself (see launcher_make_hub/_make_satellite)
 * — this callback only ever touches CONTENT sitting on top of that
 * fill. */
static void launcher_press_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    lv_event_code_t const code = lv_event_get_code(e);
    uint32_t const color = (code == LV_EVENT_PRESSED) ? FF_THEME_COLOR_BG : FF_THEME_COLOR_AMBER;
    uint32_t const n = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < n; i++) {
        launcher_recolor_tree(lv_obj_get_child(btn, i), color);
    }
}

/* ---------------------------------------------------------------------
 * Icon primitive helpers — every one draws in AMBER at rest;
 * launcher_press_cb flips the whole subtree to BG on press.
 * ------------------------------------------------------------------- */

/* A ring or ring-SEGMENT, `[start_deg, end_deg)` in lv_arc's OWN angle
 * convention (0 = 3 o'clock/east, increasing clockwise — unrelated to
 * launcher_deg_to_offset's satellite-placement convention above; this
 * helper is a thin lv_arc wrapper, not a second N-agnostic primitive).
 * `lv_obj_remove_style_all` leaves the INDICATOR and KNOB parts undrawn
 * (zero width/opa) — only MAIN (whose span `lv_arc_set_bg_angles` sets)
 * is styled, the same convention `radar_make_cluster_wedge`
 * (scr_radar.c) already established. */
static lv_obj_t *launcher_mk_arc(lv_obj_t *icon, int32_t diam, int32_t width, float start_deg, float end_deg,
                                  lv_opa_t opa)
{
    lv_obj_t *arc = lv_arc_create(icon);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, diam, diam);
    lv_obj_center(arc);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, (lv_value_precise_t)start_deg, (lv_value_precise_t)end_deg);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(FF_THEME_COLOR_AMBER), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    return arc;
}

/* A line through `pts` (ABSOLUTE coordinates within a `container_px` x
 * `container_px` box, top-left (0,0) — same "explicit full-size object
 * pinned at (0,0), points offset from that origin" convention
 * `radar_draw_segment` documents in scr_radar.c, scoped to the icon's
 * own small container instead of the whole puck). `pts` must outlive
 * this line object (lv_line_set_points keeps the pointer, not a copy) —
 * every caller below passes either a `static const` array (fixed icon
 * geometry, safe for the program's lifetime) or a `static` (mutable,
 * loop-filled once per build) array, matching scr_radar.c's own
 * caller-cleans-before-rebuild discipline. */
static lv_obj_t *launcher_mk_line(lv_obj_t *icon, int32_t container_px, lv_point_precise_t const *pts, uint32_t n,
                                   int32_t width)
{
    lv_obj_t *line = lv_line_create(icon);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, container_px, container_px);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, n);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

/* A small filled dot at ABSOLUTE (abs_cx, abs_cy) within the icon's own
 * container — same absolute-coordinate convention as launcher_mk_line
 * (arcs use lv_obj_center(), which lands on the same (container_px/2,
 * container_px/2) origin, so all three primitives agree on one frame). */
static lv_obj_t *launcher_mk_dot(lv_obj_t *icon, int32_t diam, int32_t abs_cx, int32_t abs_cy)
{
    lv_obj_t *dot = lv_obj_create(icon);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, diam, diam);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(dot, abs_cx - diam / 2, abs_cy - diam / 2);
    return dot;
}

/* A small filled, SQUARE-CORNERED block centered at ABSOLUTE (abs_cx,
 * abs_cy) — same convention as launcher_mk_dot, but no radius, for a
 * gear tooth (a block, not a rounded dot — this file's cog icon is the
 * only user today). */
static lv_obj_t *launcher_mk_block(lv_obj_t *icon, int32_t w, int32_t h, int32_t abs_cx, int32_t abs_cy)
{
    lv_obj_t *blk = lv_obj_create(icon);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, w, h);
    lv_obj_set_style_bg_color(blk, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(blk, abs_cx - w / 2, abs_cy - h / 2);
    return blk;
}

/* ---------------------------------------------------------------------
 * The five icons. Each draws into `icon`, a container already sized to
 * the fixed pixel size the caller passes (46 for the hub, 30 for every
 * satellite — no icon here is ever drawn at both sizes, so coordinates
 * below are literal, not derived from a runtime scale factor).
 * Geometry transcribed from Main.dc.html's SVG paths (viewBox 24x24),
 * scaled to each icon's fixed size and re-centered — see this file's
 * top comment for why fills became outlines.
 * ------------------------------------------------------------------- */

/* Radar scope: a genuinely FILLED sweep wedge (0=east, sweeping to
 * -45/315 — the design path's own two endpoints, independently
 * recomputed from its (21,12)/(18.36,5.64) SVG points, at the design's
 * own fill-opacity 0.28), outer + inner ring, a toned-down heading line
 * (so the wedge leads, not a needle), and a small blip dot (design:
 * (8.2,14.6) r1.5, scaled 1.9167x from a 46/24 hub icon and re-centered
 * on (23,23)).
 *
 * The wedge is a real `lv_arc` sector, not a thick ring segment: an
 * arc's `width` is a stroke inward from its own outer radius, so
 * setting `width == radius` (17 for this 34px-diameter ring) makes the
 * inner edge of that "ring" land exactly on the icon's center — the
 * angular span becomes a genuine filled pie slice from center to rim,
 * with no custom draw callback (see this file's top comment, "Icon
 * pipeline", for why a callback-drawn fill would break the generic
 * press-recolor walk). Drawn FIRST so the rings/blip/line render
 * crisply on top of it. */
static void launcher_icon_radar(lv_obj_t *icon, int32_t px)
{
    (void)px; /* fixed 46px hub icon */
    launcher_mk_arc(icon, 34, 17, 315.0f, 360.0f, LAUNCHER_OPA_28); /* filled sweep wedge, center to rim */
    launcher_mk_arc(icon, 34, 2, 0.0f, 360.0f, LV_OPA_COVER);
    launcher_mk_arc(icon, 17, 2, 0.0f, 360.0f, LAUNCHER_OPA_55); /* design: inner ring opacity 0.55 */
    static const lv_point_precise_t heading_pts[2] = {{23, 23}, {35, 11}};
    lv_obj_t *heading = launcher_mk_line(icon, 46, heading_pts, 2, 1); /* 1px, toned down — the wedge leads */
    lv_obj_set_style_line_opa(heading, LV_OPA_70, 0);
    launcher_mk_dot(icon, 6, 16, 28);
}

/* Lineup: three horizontal rows with a leading dot each (design: y=6/12/
 * 18, x 9..21, dots at x=4.5 r=1.4 — scaled 1.25x from a 30/24 satellite
 * icon and re-centered on (15,15)). */
static void launcher_icon_lineup(lv_obj_t *icon, int32_t px)
{
    (void)px; /* fixed 30px satellite icon */
    static const lv_point_precise_t row0[2] = {{11, 8}, {26, 8}};
    static const lv_point_precise_t row1[2] = {{11, 15}, {26, 15}};
    static const lv_point_precise_t row2[2] = {{11, 22}, {26, 22}};
    launcher_mk_line(icon, 30, row0, 2, 2);
    launcher_mk_line(icon, 30, row1, 2, 2);
    launcher_mk_line(icon, 30, row2, 2, 2);
    launcher_mk_dot(icon, 4, 6, 8);
    launcher_mk_dot(icon, 4, 6, 15);
    launcher_mk_dot(icon, 4, 6, 22);
}

/* Inbox: an envelope — an outline rounded rect plus a two-segment flap
 * (design: rect x3 y6 w18 h12 rx2.5, flap M3.5,8.5 L12,14 L20.5,8.5 —
 * both exactly centered in the 24-viewBox, so they stay centered here
 * too). */
static void launcher_icon_inbox(lv_obj_t *icon, int32_t px)
{
    (void)px;
    lv_obj_t *rect = lv_obj_create(icon);
    lv_obj_remove_style_all(rect);
    lv_obj_set_size(rect, 22, 14);
    lv_obj_set_pos(rect, 4, 8);
    lv_obj_set_style_radius(rect, 3, 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rect, 2, 0);
    lv_obj_set_style_border_color(rect, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(rect, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_CLICKABLE);

    static const lv_point_precise_t flap[3] = {{4, 11}, {15, 18}, {26, 11}};
    launcher_mk_line(icon, 30, flap, 3, 2);
}

/* Map: a nav-arrow OUTLINE (design's path is a FILLED kite — M12,3
 * L19.5,21 L12,16.8 L4.5,21 Z — see this file's top comment for why an
 * outline, not a fill, is drawn here). */
static void launcher_icon_map(lv_obj_t *icon, int32_t px)
{
    (void)px;
    static const lv_point_precise_t arrow[5] = {{15, 4}, {24, 26}, {15, 21}, {6, 26}, {15, 4}};
    launcher_mk_line(icon, 30, arrow, 5, 2);
}

/* Settings: a COG, not a sun — a real annulus (ring band, not a thin
 * outline) for the rim, a small hollow center hole, and 8 SHORT THICK
 * BLOCK teeth protruding from the ring's own outer edge at 45deg
 * increments (the design's own N/S/E/W + 4 diagonals) — not thin rays
 * from a center dot (the previous draft's actual defect, per the PR
 * review: it read as a sun). Annulus outer diam 20 (radius 10), band
 * width 3 (~2.5px stroke); teeth are 4x4 blocks centered at radius 12
 * (2px proud of the annulus's own outer edge); center hole diam 7,
 * width 2. Tooth centers are `round(15 +- 12 * cos/sin(k * 45deg))` for
 * k=0..7, a fixed table (not a runtime sinf/cosf loop — every other
 * icon in this file is compile-time `.rodata`, zero `.bss`; see
 * git history for why a mutable per-build point pool was rejected
 * here). */
static void launcher_icon_settings(lv_obj_t *icon, int32_t px)
{
    (void)px;
    launcher_mk_arc(icon, 20, 3, 0.0f, 360.0f, LV_OPA_COVER); /* rim annulus */
    launcher_mk_arc(icon, 7, 2, 0.0f, 360.0f, LV_OPA_COVER);  /* center hole */
    static const int32_t teeth_cx[8] = {27, 23, 15, 7, 3, 7, 15, 23};
    static const int32_t teeth_cy[8] = {15, 23, 27, 23, 15, 7, 3, 7};
    for (int i = 0; i < 8; i++) {
        launcher_mk_block(icon, 4, 4, teeth_cx[i], teeth_cy[i]);
    }
}

/* ---------------------------------------------------------------------
 * Circle builders — hub + satellite.
 * ------------------------------------------------------------------- */

/* `launcher_idx`: this file's own fixed semantic order (0=Radar,
 * 1=Now/Lineup, 2=Signals/Inbox, 3=Map, 4=Settings —
 * ff_intent.h's FF_INTENT_LAUNCHER_SELECT payload), passed through LVGL
 * event user_data (the scr_compose.c/scr_inbox.c precedent for a
 * per-callback small int with no new global state) — see this file's top
 * comment, "Index -> face mapping: UNCHANGED". */
static void launcher_circle_click_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    ff_intent_t in = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    in.u.launcher_idx = (uint8_t)idx;
    ff_intent_emit(&in);
}

/* The orbit ring: a 1px hairline circle at r=128, amber 16% — no tick
 * (removed per the maintainer's explicit call during this
 * implementation; see this file's top comment). Drawn BEFORE the hub/
 * satellites so it sits visually behind them (LVGL: later children draw
 * on top). */
static void launcher_make_orbit_ring(lv_obj_t *puck)
{
    launcher_mk_arc(puck, (int32_t)(2.0f * LAUNCHER_ORBIT_RADIUS_PX), 1, 0.0f, 360.0f, LAUNCHER_OPA_16);
}

static void launcher_make_hub(lv_obj_t *puck)
{
    lv_obj_t *btn = ff_scr_button_create(puck);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LAUNCHER_HUB_DIAM, LAUNCHER_HUB_DIAM);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_border_opa(btn, LAUNCHER_OPA_55, 0);
    /* Soft amber glow — this file's closest LVGL approximation of the
     * design's CSS box-shadow blur (not a transcribed pixel value; see
     * LAUNCHER_OPA_20's own comment). */
    lv_obj_set_style_shadow_width(btn, 30, 0);
    lv_obj_set_style_shadow_spread(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_shadow_opa(btn, LAUNCHER_OPA_20, 0);
    /* Press feedback: full amber fill (design: States.dc.html) — only a
     * press turns a disc amber; at rest every disc (hub included) is the
     * same surface tone. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    /* PRESS_LOCK is cleared for us by ff_scr_button_create (scr_nav.h,
     * #145/#148) — see launcher_make_satellite's own copy of this
     * comment for why it matters here: the hub sits at the puck's exact
     * center, which a drag/gesture can pass directly over. */
    lv_obj_add_event_cb(btn, launcher_circle_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)0);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *icon = lv_obj_create(btn);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, LAUNCHER_HUB_ICON_PX, LAUNCHER_HUB_ICON_PX);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, LAUNCHER_HUB_ICON_DY);
    launcher_icon_radar(icon, LAUNCHER_HUB_ICON_PX);

    lv_obj_t *caption = lv_label_create(btn);
    lv_label_set_text(caption, "RADAR");
    lv_obj_set_style_text_font(caption, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_letter_space(caption, 2, 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, LAUNCHER_HUB_CAPTION_DY);
}

/* Inbox unread badge: a count pill (22px tall, amber, dark text — see
 * ff_theme.h's font-substitution note, no mono vendored — 3px BG
 * keyline), pinned near the satellite's own top-right corner (the
 * `0.7071` factor is the same "~45 degrees out on the rim" placement the
 * retired grid's own badge dot used, scaled to the 88px satellite). A
 * SIBLING of the satellite button on `puck`, not a child of it, so it is
 * never touched by that circle's own press recolor (launcher_press_cb
 * only walks the pressed button's own children). */
static void launcher_make_badge(lv_obj_t *puck, float sat_dx, float sat_dy, uint16_t unread)
{
    char buf[6];
    unsigned const shown = (unread > 999u) ? 999u : (unsigned)unread; /* defensive pill-width cap, not a product cap on real unread counts */
    snprintf(buf, sizeof(buf), "%u", shown);

    lv_obj_t *pill = lv_obj_create(puck);
    lv_obj_remove_style_all(pill);
    lv_obj_set_style_bg_color(pill, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pill, 3, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(pill, 6, 0);
    lv_obj_set_height(pill, 22);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(pill, 22, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    int32_t const corner = (int32_t)((float)LAUNCHER_SAT_DIAM / 2.0f * 0.7071f);
    lv_obj_align(pill, LV_ALIGN_CENTER, (int32_t)sat_dx + corner, (int32_t)sat_dy - corner);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_set_style_text_font(lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_center(lbl);
}

static void launcher_make_satellite(lv_obj_t *puck, void (*icon_fn)(lv_obj_t *, int32_t), char const *caption_text,
                                     float dx, float dy, uintptr_t launcher_idx, bool badge, uint16_t unread)
{
    lv_obj_t *btn = ff_scr_button_create(puck);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LAUNCHER_SAT_DIAM, LAUNCHER_SAT_DIAM);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_set_style_border_opa(btn, LAUNCHER_OPA_8, 0);
    /* Press feedback: identical convention to the hub — full amber fill,
     * every circle styled the same way, no visual privileging. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);
    /* PRESS_LOCK is cleared for us by ff_scr_button_create (scr_nav.h,
     * #145/#148). It matters a lot here: a long horizontal drag (the
     * sim's `ctl swipe`, and an ordinary real finger drag) that PRESSES
     * DOWN on one satellite — the left/right cardinal satellites sit
     * exactly on the puck's own horizontal midline, so a center-line
     * sweep starts or ends squarely on one of them — would, with
     * PRESS_LOCK left set, still fire LV_EVENT_CLICKED on release even
     * though the finger has long since left this circle: the "a real
     * LVGL button underneath a drag's path can still register a click
     * on release" hazard the retired 2-over-3 grid's own layout comment
     * warned about (it worked around it geometrically, by keeping every
     * circle off that line — impossible here, since the design's own
     * cardinal-point satellites REQUIRE two of them to sit on it).
     * Clearing PRESS_LOCK fixes the actual mechanism instead: once the
     * pointer leaves this circle's bounds, LVGL drops the press
     * (LV_EVENT_PRESS_LOST) rather than keeping it captured, so a sweep
     * across this disc no longer fires a click at all — while a real,
     * stationary tap (which never leaves the disc's own bounds) is
     * completely unaffected. */
    /* Whether this satellite stays clickable while a banner covers part
     * of it (the top/Inbox one, the only one the banner ever reaches) is
     * decided AFTER the banner is built, by the shared remainder-rule
     * pass `ff_scr_nav_mask_clickables_under_banner` (scr_nav.h) —
     * ff_scr_launcher_build's own call-order comment explains why this
     * has to happen post-hoc rather than here. Every satellite is wired
     * clickable unconditionally at construction time; the post-pass is
     * the one and only place that ever takes it away, so there is
     * exactly one rule (scr_nav.c's own doc comment on it) rather than
     * this file guessing "covered" from compass position alone the way
     * an earlier round of this PR did. */
    lv_obj_add_event_cb(btn, launcher_circle_click_cb, LV_EVENT_CLICKED, (void *)launcher_idx);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(btn, launcher_press_cb, LV_EVENT_PRESS_LOST, NULL);
    /* Icon + caption build normally — a satellite behind the banner is
     * only PARTLY covered in practice (the banner's height is well
     * short of the satellite's own 88px), so its caption/icon
     * below the overlap stays visibly, honestly present; only the tap
     * target is what the banner's presence takes over. */

    lv_obj_t *icon = lv_obj_create(btn);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, LAUNCHER_SAT_ICON_PX, LAUNCHER_SAT_ICON_PX);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, LAUNCHER_SAT_ICON_DY);
    icon_fn(icon, LAUNCHER_SAT_ICON_PX);

    lv_obj_t *caption = lv_label_create(btn);
    lv_label_set_text(caption, caption_text);
    lv_obj_set_style_text_font(caption, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(caption, 1, 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, LAUNCHER_SAT_CAPTION_DY);

    if (badge) {
        launcher_make_badge(puck, dx, dy, unread);
    }
}

/* ---------------------------------------------------------------------
 * Status row — time . battery, at the bottom (design: y ~= 368). Reuses
 * `ff_radar_view_t`'s own clock_str/batt_pct fields and formatting
 * (`radar_build_status_bar` in scr_radar.c) so the launcher never
 * invents a second source of truth for the same two facts, and inherits
 * that function's honesty behavior verbatim: an empty clock_str reads
 * "--:--" and an unknown (negative) battery reads "--%" — never a
 * fabricated time or level. MESH is deliberately omitted here (this
 * file's brief); it stays exclusive to the Radar face's own status bar.
 *
 * The battery GLYPH (design: a small outline battery icon) is shown only
 * when `batt_pct` is actually known — omitted, not defaulted to a
 * fixed/"empty" icon, when it is -1 (no battery ADC on either target
 * yet, per ff_shell.c). A glyph implying a reading over an honest "--%"
 * would be exactly the fabricated-freshness CLAUDE.md's honesty rule
 * forbids, even though it is only decorative chrome. The glyph STEP
 * (FULL/3/2/1/EMPTY) comes from `ff_radar_batt_icon()` (ff_radar.h) —
 * previously a bare 87/62/37/12 ladder typed directly here, now the same
 * named-and-documented boundaries scr_radar.c would use if it ever grows
 * this glyph too.
 *
 * debt/batt-low-core: icon + label now tint the SAME amber
 * (`FF_THEME_COLOR_STALE_AMBER`) scr_radar.c's status bar uses when
 * `ff_radar_batt_is_low(r->batt_pct)` is true — previously this row
 * always rendered muted grey regardless of level, so a low-battery puck
 * looked identical here and on Radar (Radar amber, launcher flat grey)
 * even though the launcher IS home (S26 slice e) and is exactly where
 * that alarm most needs to read as urgent. Both screens call the one
 * core function on the one `batt_pct` field, so they cannot disagree. */
static void launcher_build_status_row(lv_obj_t *puck, ff_radar_view_t const *r)
{
    char buf[24];
    bool const batt_low = ff_radar_batt_is_low(r->batt_pct);
    uint32_t const batt_color = batt_low ? FF_THEME_COLOR_STALE_AMBER : FF_THEME_COLOR_MUTED;

    lv_obj_t *clock_lbl = lv_label_create(puck);
    lv_label_set_text(clock_lbl, r->clock_str[0] != '\0' ? r->clock_str : "--:--");
    lv_obj_set_style_text_font(clock_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, -32, (int32_t)LAUNCHER_STATUS_ROW_DY);

    lv_obj_t *sep_lbl = lv_label_create(puck);
    lv_label_set_text(sep_lbl, ".");
    lv_obj_set_style_text_font(sep_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sep_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sep_lbl, LV_ALIGN_CENTER, -8, (int32_t)LAUNCHER_STATUS_ROW_DY);

    if (r->batt_pct >= 0) {
        char const *glyph;
        switch (ff_radar_batt_icon(r->batt_pct)) {
        case FF_BATT_ICON_FULL: glyph = LV_SYMBOL_BATTERY_FULL; break;
        case FF_BATT_ICON_3:    glyph = LV_SYMBOL_BATTERY_3; break;
        case FF_BATT_ICON_2:    glyph = LV_SYMBOL_BATTERY_2; break;
        case FF_BATT_ICON_1:    glyph = LV_SYMBOL_BATTERY_1; break;
        case FF_BATT_ICON_EMPTY:
        case FF_BATT_ICON_UNKNOWN:
        default:                glyph = LV_SYMBOL_BATTERY_EMPTY; break;
        }
        lv_obj_t *batt_icon = lv_label_create(puck);
        lv_label_set_text(batt_icon, glyph);
        lv_obj_set_style_text_font(batt_icon, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(batt_icon, lv_color_hex(batt_color), 0);
        lv_obj_align(batt_icon, LV_ALIGN_CENTER, 8, (int32_t)LAUNCHER_STATUS_ROW_DY);
    }

    lv_obj_t *batt_lbl = lv_label_create(puck);
    if (r->batt_pct < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)r->batt_pct);
    }
    lv_label_set_text(batt_lbl, buf);
    lv_obj_set_style_text_font(batt_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(batt_lbl, lv_color_hex(batt_color), 0);
    lv_obj_align(batt_lbl, LV_ALIGN_CENTER, 32, (int32_t)LAUNCHER_STATUS_ROW_DY);
}

/* ---------------------------------------------------------------------
 * Satellite descriptor table — each entry's `compass_pos` (0 = top,
 * clockwise) is fed straight into `ff_scr_launcher_satellite_deg`,
 * which COMPUTES the angle as `compass_pos * (360 / LAUNCHER_SAT_COUNT)`
 * — not a hand-typed angle literal. At today's N=4 that formula yields
 * exactly {0, 90, 180, 270} for compass_pos {0,1,2,3} — Inbox top (0),
 * Lineup right (1), Settings bottom (2), Map left (3), reading the
 * brief's own "clockwise from the top" order, which is also the design
 * canvas's pentagon order with Music simply absent (no dead tile — see
 * top comment). See `ff_scr_launcher_satellite_deg`'s own doc comment
 * and its unit test for the formula proven directly, independent of
 * this table.
 *
 * Array ORDER here is `launcher_idx` order (1..4), not compass order —
 * deliberately: every existing S26e test (app/screens/tests/
 * test_scr_intent.c's `launcher_circle_at`) finds a circle by its
 * CREATION-order position among same-sized clickables, a convention
 * that predates this visual rework and is cheaper to keep satisfied
 * than to rewrite. Creation order carries no meaning of its own (see
 * `launcher_circle_click_cb`'s comment — `launcher_idx` is the only
 * semantic fact); `compass_pos` is what actually places each circle,
 * assigned per-entry precisely because array position no longer implies
 * compass position once creation is reordered this way. Adding a fifth
 * satellite later means a new N=5 (LAUNCHER_SAT_COUNT=5) and five
 * `compass_pos` assignments 0..4 — the angle set itself
 * (`{0, 72, 144, 216, 288}`) is computed, not retyped.
 * ------------------------------------------------------------------- */
typedef struct {
    void (*icon_fn)(lv_obj_t *, int32_t);
    char const *caption;
    uintptr_t launcher_idx;
    bool badge_capable;
    int compass_pos;
} launcher_satellite_desc_t;

void ff_scr_launcher_build(ff_app_state_t const *state)
{
    if (state == NULL) {
        return;
    }

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

    uint16_t const unread = ff_scr_inbox_unread_count(&state->inbox);

    launcher_make_orbit_ring(puck);
    launcher_make_hub(puck);

    static launcher_satellite_desc_t const sats[LAUNCHER_SAT_COUNT] = {
        {launcher_icon_lineup, "LINEUP", 1, false, 1},
        {launcher_icon_inbox, "INBOX", 2, true, 0},
        {launcher_icon_map, "MAP", 3, false, 3},
        {launcher_icon_settings, "SETTINGS", 4, false, 2},
    };
    for (int i = 0; i < LAUNCHER_SAT_COUNT; i++) {
        float const deg = ff_scr_launcher_satellite_deg(sats[i].compass_pos, LAUNCHER_SAT_COUNT);
        float dx, dy;
        launcher_deg_to_offset(deg, LAUNCHER_ORBIT_RADIUS_PX, &dx, &dy);
        launcher_make_satellite(puck, sats[i].icon_fn, sats[i].caption, dx, dy, sats[i].launcher_idx,
                                 sats[i].badge_capable && unread > 0, unread);
    }

    launcher_build_status_row(puck, &state->radar);

    /* S26 slice d — the message banner, built LAST (after every
     * satellite and the status row) so it paints on top of whatever the
     * launcher shows, exactly the "built after, so drawn on top of"
     * placement scr_nav.c's own call-order comment documents for every
     * other face. No-op internally when !state->banner.active. */
    ff_scr_banner_build(puck, &state->banner, state->settings.colorblind);

    if (state->banner.active) {
        /* The strip is the LAST child ff_scr_banner_build just added to
         * `puck` (its own doc comment's contract) — same "read it back
         * after building" shape ff_scr_nav_build uses for the five base
         * faces. Reuses that EXACT shared pass (scr_nav.h) rather than a
         * second copy: the one position the banner can ever reach here
         * is the top (Inbox) satellite (compass_pos 0), and the SAME
         * remainder rule (scr_nav.c's own doc comment: mask only when
         * the uncovered remainder fails FF_THEME_MIN_HIT_PX in either
         * dimension) decides it — currently that satellite's remainder
         * below the strip is ~88x37px, failing the height floor, so it
         * IS masked, but this file no longer hard-codes that outcome by
         * compass position; it falls out of the same measured rule
         * every other face uses. */
        lv_obj_t *strip = lv_obj_get_child(puck, lv_obj_get_child_count(puck) - 1);
        lv_obj_update_layout(puck);
        lv_area_t strip_area;
        lv_obj_get_coords(strip, &strip_area);
        ff_scr_nav_mask_clickables_under_banner(puck, strip, &strip_area);
    }
}
