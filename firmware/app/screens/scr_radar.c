/**
 * scr_radar.c — see scr_radar.h. Pure render: ff_radar_view_t -> LVGL
 * objects. No domain logic (CLAUDE.md) — every branch below is "how to
 * draw mode X", never "should this be mode X".
 *
 * Collision-free placement (arrow length, ring-dot position/clustering)
 * is NOT computed here — it's resolved by app/screens/radar_layout.h/.c
 * (a pure C11 module with no LVGL dependency, unit-tested directly with
 * geometric assertions — see app/screens/tests/test_radar_layout.c).
 * This file only draws the coordinates that module hands back; see
 * radar_layout.h's top comment for why (PR #16 UX review round 3 / code
 * review round 2: three rounds of one-off overlap patches produced three
 * new overlaps, because the old resolver lived in here mixed with LVGL
 * object creation and used iterative push instead of search).
 */
#include "scr_radar.h"

#include <stdio.h>
#include <string.h>

#include "ff_intent.h" /* S16c2 — the emit seam; see radar_flare_cb */
#include "ff_theme.h"
#include "radar_layout.h"

/* ---------------------------------------------------------------------
 * lv_line point storage.
 *
 * lv_line_set_points() keeps the POINTER it's given, not a copy — the
 * array must outlive the lv_line object. This file builds a fresh radar
 * screen every time its caller decides the rendered view changed (S16
 * slice d's dirty-driven rebuild — targets/sim/ctl_loop.c's
 * ff_ctl_loop_pump, and the live SDL window loop in main.c), which can
 * now happen many times per process. That is safe ONLY because of an
 * invariant the CALLER upholds, not this file: every rebuild is preceded
 * by `lv_obj_clean()` on the screen being rebuilt, which deletes every
 * `lv_line`/triangle-descriptor object from the PREVIOUS build before
 * this file's static pools reset their indices back to 0. By the time
 * `s_line_pt_next`/`s_tri_desc_next` are reused, nothing still-alive
 * references the points about to be overwritten.
 *
 * ISSUE #17 (closed by this discipline): building without ever tearing
 * down leaked LVGL objects without bound, and resetting these pools
 * while a still-alive `lv_line` from an earlier build pointed at them
 * corrupted that line silently. Both are fixed by the same rule — never
 * rebuild without cleaning first — enforced by the caller, not by this
 * file (this file has no way to know whether it's being called for the
 * first time or the fifty-thousandth). A caller that ever rebuilds
 * WITHOUT cleaning first (or clones this file's build call without that
 * discipline) reintroduces exactly this bug; there is nothing in this
 * file's own API that can prevent that mistake, only a well-documented
 * contract each caller must honour. See ctl_loop.h's top comment for the
 * caller-side half of this invariant. Same reasoning applies to the
 * triangle-descriptor pool below, and to scr_flare.c's own
 * `s_flare_mark_ray_pts`.
 * ------------------------------------------------------------------- */
#define FF_SCR_RADAR_MAX_LINE_SEGMENTS 16
static lv_point_precise_t s_line_pts[FF_SCR_RADAR_MAX_LINE_SEGMENTS][2];
static int s_line_pt_next;

static lv_point_precise_t *radar_alloc_line_pts(void)
{
    if (s_line_pt_next >= FF_SCR_RADAR_MAX_LINE_SEGMENTS) {
        s_line_pt_next = 0; /* defensive wrap: never index out of bounds */
    }
    return s_line_pts[s_line_pt_next++];
}

/* ---------------------------------------------------------------------
 * Small shared builders.
 * ------------------------------------------------------------------- */

static lv_obj_t *radar_make_chip(lv_obj_t *parent, char const *text, uint32_t bg_hex, uint32_t fg_hex, int32_t dy)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_style_bg_color(chip, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(chip, 14, 0);
    lv_obj_set_style_pad_right(chip, 14, 0);
    lv_obj_set_style_pad_top(chip, 6, 0);
    lv_obj_set_style_pad_bottom(chip, 6, 0);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    lv_obj_set_height(chip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE); /* an indicator, not a control */
    lv_obj_align(chip, LV_ALIGN_CENTER, 0, dy);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FF_THEME_FONT_CHIP, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg_hex), 0);
    lv_obj_center(label);

    return chip;
}

/* One clustered member's coloured wedge on a cluster marker's ring
 * (issue #18) — an lv_arc sized to the marker itself, so the wedges sit
 * inside the marker's existing one-dot-diameter footprint and no
 * collision geometry changes.
 *
 * `stale` selects the WIDTH, not the opacity (PR #41 UX review,
 * BLOCKING 3 — see RADAR_LAYOUT_CLUSTER_RING_STALE_W_PX's doc comment
 * for the measurements and the full reasoning). A stale member's wedge
 * is drawn at the lone ghost dot's own 2px, at the same outer radius and
 * the same LV_OPA_70, so "hollow/thin means old" is now one idiom across
 * the whole face instead of two contradictory ones. The previous pass
 * dimmed the fill instead, which only read as dim next to a bright
 * neighbour — so an all-stale cluster read as live.
 *
 * lv_obj_remove_style_all() leaves the INDICATOR part at arc_width 0 and
 * the KNOB part at bg_opa 0, so neither draws; only the MAIN part
 * (the background arc, whose span lv_arc_set_bg_angles sets) is styled
 * below. LV_OBJ_FLAG_CLICKABLE must be cleared explicitly — lv_arc is an
 * interactive widget by default, and a 34px clickable object would fail
 * targets/sim/tests/test_face_hit_targets.c's FF_THEME_MIN_HIT_PX floor
 * (correctly: it is an indicator, not a control). */
static void radar_make_cluster_wedge(lv_obj_t *parent, radar_layout_wedge_t const *wedge, uint32_t color_hex,
                                      bool stale)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, (int32_t)RADAR_LAYOUT_DOT_PX, (int32_t)RADAR_LAYOUT_DOT_PX);
    lv_obj_center(arc);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, (lv_value_precise_t)wedge->start_deg, (lv_value_precise_t)wedge->end_deg);

    int32_t w = (int32_t)(stale ? RADAR_LAYOUT_CLUSTER_RING_STALE_W_PX : RADAR_LAYOUT_CLUSTER_RING_W_PX);
    lv_obj_set_style_arc_width(arc, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color_hex), LV_PART_MAIN);
    /* Full crew color either way — LV_OPA_70 for stale is the lone ghost
     * dot's own border opacity, not a further dimming on top of the
     * thickness change. */
    lv_obj_set_style_arc_opa(arc, stale ? LV_OPA_70 : LV_OPA_COVER, LV_PART_MAIN);
    /* Square wedge ends: rounded caps on a small arc would round away
     * most of a narrow wedge and blur the gaps that separate members. */
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
}

/* True iff EVERY member drawn on this cluster marker has a stale fix.
 *
 * Reads the same per-dot `stale` display flag the lone-dot branch below
 * already branches on to pick a style — an aggregation of a fact
 * `ff_radar_view_t` carries, not a decision about what that fact should
 * be (no domain `if` enters this file; CLAUDE.md). It exists because the
 * count digit is the brightest, crispest element on the marker, so on a
 * marker where nothing is current the most confident-looking thing on
 * screen was the number (PR #41 UX review, BLOCKING 3). */
static bool radar_cluster_all_stale(ff_radar_view_t const *r, radar_layout_wedge_t const *wedges, int n_wedges)
{
    for (int w = 0; w < n_wedges; w++) {
        if (!r->dots[wedges[w].index].stale) {
            return false;
        }
    }
    return n_wedges > 0;
}

/* A translucent ring hugging the puck's own edge — the STALE "rim tint"
 * (S06: "amber rim tint"; LOST deliberately gets none — see
 * radar_render_lost). Drawn here (not by the shell, which owns the puck
 * object) so this file stays a pure function of (parent, radar) with no
 * dependency on the shell's internal object tree. */
static void radar_build_rim_tint(lv_obj_t *parent, uint32_t color_hex, lv_opa_t opa)
{
    lv_obj_t *rim = lv_obj_create(parent);
    lv_obj_remove_style_all(rim);
    lv_obj_set_size(rim, FF_THEME_PUCK_PX - 4, FF_THEME_PUCK_PX - 4);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rim, 3, 0);
    lv_obj_set_style_border_color(rim, lv_color_hex(color_hex), 0);
    lv_obj_set_style_border_opa(rim, opa, 0);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(rim);
}

/* Status bar: clock / mesh / battery. Cross-mode chrome, not part of the
 * mode-specific truth table — always rendered from whatever the caller
 * populated in *r (ff_radar_compute never touches these three fields;
 * see ff_radar.h's deviation note). Its position (dy=
 * RADAR_LAYOUT_STATUS_BAR_DY) is also one of radar_layout's registered
 * reserved rectangles — every mode's dots and arrow steer clear of it. */
static void radar_build_status_bar(lv_obj_t *parent, ff_radar_view_t const *r)
{
    char buf[24];

    lv_obj_t *clock_lbl = lv_label_create(parent);
    lv_label_set_text(clock_lbl, r->clock_str[0] != '\0' ? r->clock_str : "--:--");
    lv_obj_set_style_text_font(clock_lbl, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_CENTER, -78, (int32_t)RADAR_LAYOUT_STATUS_BAR_DY);

    lv_obj_t *mesh_lbl = lv_label_create(parent);
    lv_label_set_text(mesh_lbl, r->mesh_ok ? "MESH" : "NO MESH");
    lv_obj_set_style_text_font(mesh_lbl, FF_THEME_FONT_LABEL, 0);
    /* UX review (non-blocking finding #6): losing the mesh radio breaks
     * the whole point of the puck (finding friends) — it must read at
     * least as alarming as a low battery, not as flat grey chrome. Same
     * alert color as the low-battery case below. */
    lv_obj_set_style_text_color(
        mesh_lbl, lv_color_hex(r->mesh_ok ? FF_THEME_COLOR_LIVE_GREEN : FF_THEME_COLOR_STALE_AMBER), 0);
    lv_obj_align(mesh_lbl, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_STATUS_BAR_DY);

    lv_obj_t *batt_lbl = lv_label_create(parent);
    if (r->batt_pct < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%", (int)r->batt_pct);
    }
    lv_label_set_text(batt_lbl, buf);
    lv_obj_set_style_text_font(batt_lbl, FF_THEME_FONT_LABEL, 0);
    bool batt_low = (r->batt_pct >= 0 && r->batt_pct <= FF_THEME_BATT_LOW_PCT);
    lv_obj_set_style_text_color(batt_lbl,
                                 lv_color_hex(batt_low ? FF_THEME_COLOR_STALE_AMBER : FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(batt_lbl, LV_ALIGN_CENTER, 78, (int32_t)RADAR_LAYOUT_STATUS_BAR_DY);
}

/* Crew ring: every dot ff_radar_compute produced, placed and clustered by
 * radar_layout_resolve_dots (see that function's doc comment) against
 * `*reg`. Renders exactly one visual marker per distinct cluster_id:
 *   - cluster_size == 1: the member's own initial/color, filled when
 *     LIVE, outline-only "ghost" otherwise (freshness != LIVE).
 *   - cluster_size > 1: a count digit inside a ring of one crew-colored
 *     WEDGE PER MEMBER — ORCHESTRATOR RULING (round 4): several crew
 *     members converged on nearly the same bearing get merged into ONE
 *     marker showing how many, never hidden (CLAUDE.md's honesty rule:
 *     dropping a *known* crew member from the ring is a lie by omission,
 *     the same category of dishonesty as fabricating one that isn't
 *     there).
 *
 *     ISSUE #18: that marker used to be a white outline around a digit,
 *     on the reasoning that no single crew color applies honestly to a
 *     mixed group. True — but the conclusion (paint it neutral) made the
 *     one element meaning "several of your friends are together over
 *     there" the one element that looked like a notification badge,
 *     because every other crew element on this ring is color + letter.
 *     The marker now spends its ring on the members themselves: one
 *     wedge each, in that member's own crew color, dimmed if that member
 *     is stale (radar_make_cluster_wedge above). No single color is
 *     claimed for the group — each member gets its own — so the honesty
 *     objection is answered rather than traded away, and the marker's
 *     SHAPE says "crew" before the digit is read. Wedge geometry comes
 *     from radar_layout_cluster_wedges (pure, unit-tested); this file
 *     only draws it. */
static void radar_build_dots(lv_obj_t *parent, ff_radar_view_t const *r, radar_layout_registry_t const *reg)
{
    if (r->n_dots == 0) {
        return;
    }

    /* One clamped count, used for every loop and every resolver call
     * below — radar_layout_resolve_dots and radar_layout_cluster_wedges
     * must agree on how many dots exist, or a cluster's membership scan
     * could run past what was actually resolved. */
    int n_dots = (r->n_dots < FF_CREW_MAX) ? (int)r->n_dots : FF_CREW_MAX;

    float ring_deg[FF_CREW_MAX];
    for (int i = 0; i < n_dots; i++) {
        ring_deg[i] = r->dots[i].ring_deg;
    }

    radar_layout_dot_result_t resolved[FF_CREW_MAX];
    radar_layout_resolve_dots(reg, ring_deg, n_dots, resolved);

    for (int i = 0; i < n_dots; i++) {
        if (resolved[i].cluster_id != (int)i) {
            continue; /* not this cluster's anchor member — its marker is drawn at cluster_id's iteration */
        }

        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, RADAR_LAYOUT_DOT_PX, RADAR_LAYOUT_DOT_PX);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE); /* indicator only in this slice */

        lv_obj_t *label = lv_label_create(dot);
        lv_obj_set_style_text_font(label, FF_THEME_FONT_LABEL, 0);

        if (resolved[i].cluster_size > 1) {
            /* Dark fill, no white outline (issue #18) — the ring is the
             * members' own colors now, drawn as wedges over this fill so
             * the gaps between them show through as dark. */
            lv_obj_set_style_bg_color(dot, lv_color_hex(FF_THEME_COLOR_SURFACE), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

            radar_layout_wedge_t wedges[FF_CREW_MAX];
            /* Negative means the resolver refused to emit a PARTIAL ring
             * rather than silently drop a member (see
             * radar_layout_cluster_wedges) — draw no wedges at all in
             * that case, which degrades to the plain count marker rather
             * than to a ring that shows some friends and not others.
             * Unreachable at this call site: out_max is FF_CREW_MAX and a
             * cluster can never exceed it. */
            int n_wedges = radar_layout_cluster_wedges(resolved, n_dots, (int)i, wedges, FF_CREW_MAX);
            if (n_wedges < 0) {
                n_wedges = 0;
            }
            for (int w = 0; w < n_wedges; w++) {
                ff_radar_dot_t const *member = &r->dots[wedges[w].index];
                radar_make_cluster_wedge(dot, &wedges[w], ff_theme_crew_color(member->color_idx), member->stale);
            }

            char count_buf[4];
            snprintf(count_buf, sizeof(count_buf), "%d", resolved[i].cluster_size);
            lv_label_set_text(label, count_buf);
            /* The digit follows the marker's overall freshness (PR #41
             * UX review, BLOCKING 3): full INK while at least one member
             * is current, MUTED when none are. Otherwise the one element
             * that looks like a hard fact stays at full strength on the
             * exact marker where nothing on it is current. */
            bool all_stale = radar_cluster_all_stale(r, wedges, n_wedges);
            lv_obj_set_style_text_color(
                label, lv_color_hex(all_stale ? FF_THEME_COLOR_MUTED : FF_THEME_COLOR_INK), 0);
            /* The label is built before the wedges in this function's
             * object order, so lift it back to the top of the marker's
             * children — LVGL draws siblings in tree order, and a 6px
             * wedge ring overlapping the digit's antialiased edge would
             * fray the one glyph that carries the count. */
            lv_obj_move_foreground(label);
        } else {
            ff_radar_dot_t const *d = &r->dots[i];
            uint32_t crew_hex = ff_theme_crew_color(d->color_idx);
            if (d->place) {
                /* issue #33 — KNOWN GAP: this treatment applies only to a
                 * place standing alone (this branch); a place clustered
                 * with live/stale friends (the `cluster_size > 1` branch
                 * above) still draws as an ordinary crew wedge — needs its
                 * own follow-up once a real mixed fixture exists to design
                 * against (no golden today combines a landmark with
                 * clustered crew).
                 *
                 * A SQUARE, not a circle — every other dot on this ring
                 * (live, stale, cluster) is round; a place is a different
                 * KIND of thing on the ring, not a differently-colored or
                 * differently-opaque friend, so it gets a different
                 * silhouette (same "kind, not degree" idiom RADAR_LOST's
                 * ghost arrowhead already uses). Solid fill, full opacity —
                 * unlike `stale`, a place isn't aging, so there is no
                 * dashed/hollow "not current" signal to draw here. */
                lv_obj_set_style_radius(dot, 6, 0);
                lv_obj_set_style_bg_color(dot, lv_color_hex(crew_hex), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            } else if (d->stale) {
                /* Outline-only "ghost" — readable as "not current"
                 * without needing to read any text (ux-raver honesty-read
                 * check). */
                lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(dot, 2, 0);
                lv_obj_set_style_border_color(dot, lv_color_hex(crew_hex), 0);
                lv_obj_set_style_border_opa(dot, LV_OPA_70, 0);
            } else {
                lv_obj_set_style_bg_color(dot, lv_color_hex(crew_hex), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            }
            char ch[2] = {d->initial, '\0'};
            lv_label_set_text(label, d->initial != '\0' ? ch : "");
            lv_obj_set_style_text_color(label, lv_color_hex(d->stale && !d->place ? crew_hex : FF_THEME_COLOR_BG), 0);
        }
        lv_obj_center(label);

        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)resolved[i].dx, (int32_t)resolved[i].dy);
    }
}

/* One straight lv_line segment from (from_dx,from_dy) to (to_dx,to_dy),
 * both expressed relative to the puck's own center, at `opa` opacity.
 *
 * lv_line draws each point at `object_top_left + point` (see
 * lvgl/src/widgets/line/lv_line.c's LV_EVENT_DRAW_MAIN handler) — it does
 * NOT auto-center a bounding box of arbitrary/negative points the way an
 * `lv_obj_align`'d fixed-size widget would. So every line object here is
 * given an EXPLICIT size equal to the whole puck and pinned to (0,0)
 * (`parent`'s own top-left, which — since `parent` is a zero-padding
 * tileview tile sized to the puck, see scr_nav.c — coincides with the
 * puck's top-left); every point is then offset by the puck's half-size so
 * "center-relative (0,0)" lands exactly on the puck's visual center. */
static void radar_draw_segment(lv_obj_t *parent, float from_dx, float from_dy, float to_dx, float to_dy,
                                uint32_t color_hex, lv_opa_t opa, int32_t width)
{
    const int32_t half = FF_THEME_PUCK_PX / 2;

    lv_point_precise_t *pts = radar_alloc_line_pts();
    pts[0].x = half + (int32_t)from_dx;
    pts[0].y = half + (int32_t)from_dy;
    pts[1].x = half + (int32_t)to_dx;
    pts[1].y = half + (int32_t)to_dy;

    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color_hex), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, opa, 0);
}

/* ---------------------------------------------------------------------
 * Filled/outline triangle (arrowhead).
 *
 * LVGL has no filled-polygon widget, so this uses a plain lv_obj plus a
 * custom LV_EVENT_DRAW_MAIN callback that calls the low-level
 * lv_draw_triangle() API directly (lvgl.h -> core/lv_obj.h ->
 * lv_obj_draw.h -> draw/lv_draw_triangle.h — already transitively
 * visible, no extra include needed). Same static-storage-pool lifetime
 * pattern, and the same caller-cleans-before-rebuild safety invariant, as
 * the line-point pool above (issue #17, closed) — see that comment.
 * ------------------------------------------------------------------- */
#define FF_SCR_RADAR_MAX_TRIANGLES 2
typedef struct {
    lv_point_precise_t p[3];
    lv_color_t color;
    lv_opa_t opa;
} radar_tri_desc_t;
static radar_tri_desc_t s_tri_descs[FF_SCR_RADAR_MAX_TRIANGLES];
static int s_tri_desc_next;

static radar_tri_desc_t *radar_alloc_tri_desc(void)
{
    if (s_tri_desc_next >= FF_SCR_RADAR_MAX_TRIANGLES) {
        s_tri_desc_next = 0; /* defensive wrap: never index out of bounds */
    }
    return &s_tri_descs[s_tri_desc_next++];
}

static void radar_triangle_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    radar_tri_desc_t *td = (radar_tri_desc_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    for (int i = 0; i < 3; i++) {
        dsc.p[i].x = td->p[i].x + area.x1;
        dsc.p[i].y = td->p[i].y + area.y1;
    }
    dsc.color = td->color;
    dsc.opa = td->opa;
    lv_draw_triangle(layer, &dsc);
}

/* A filled triangle at (p0,p1,p2), each expressed relative to the puck's
 * own center — same convention and same "explicit full-puck-size object
 * pinned at (0,0)" positioning trick as radar_draw_segment (see its doc
 * comment for why: this repo's chrome objects don't rely on LVGL
 * auto-centering an arbitrary/negative-coordinate bounding box). */
static void radar_draw_filled_triangle(lv_obj_t *parent, float p0x, float p0y, float p1x, float p1y, float p2x,
                                        float p2y, uint32_t color_hex, lv_opa_t opa)
{
    const int32_t half = FF_THEME_PUCK_PX / 2;

    radar_tri_desc_t *td = radar_alloc_tri_desc();
    td->p[0].x = half + (int32_t)p0x;
    td->p[0].y = half + (int32_t)p0y;
    td->p[1].x = half + (int32_t)p1x;
    td->p[1].y = half + (int32_t)p1y;
    td->p[2].x = half + (int32_t)p2x;
    td->p[2].y = half + (int32_t)p2y;
    td->color = lv_color_hex(color_hex);
    td->opa = opa;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, FF_THEME_PUCK_PX, FF_THEME_PUCK_PX);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, radar_triangle_draw_cb, LV_EVENT_DRAW_MAIN, td);
}

/* Outline-only triangle: three thin segments, no fill — the LOST "ghost"
 * arrowhead (PR #16 UX review finding #1's ruling: "arrow reduced to a
 * faint outline-only ghost (no fill)"). A shape distinct in KIND from
 * LIVE/STALE's solid filled head, not just a dimmer copy of the same
 * silhouette — that's the point: STALE and LOST must read as different
 * screens, not different opacities of the same screen. */
static void radar_draw_outline_triangle(lv_obj_t *parent, float p0x, float p0y, float p1x, float p1y, float p2x,
                                         float p2y, uint32_t color_hex, lv_opa_t opa)
{
    radar_draw_segment(parent, p0x, p0y, p1x, p1y, color_hex, opa, 4);
    radar_draw_segment(parent, p1x, p1y, p2x, p2y, color_hex, opa, 4);
    radar_draw_segment(parent, p2x, p2y, p0x, p0y, color_hex, opa, 4);
}

typedef enum {
    RADAR_ARROW_SOLID,  /* LIVE: solid tail + filled head, full opacity */
    RADAR_ARROW_DASHED, /* STALE: dashed tail + filled head, reduced opacity */
    RADAR_ARROW_GHOST,  /* LOST (real fix): faint dashed tail + OUTLINE-ONLY head */
} radar_arrow_style_t;

/* Draws an ALREADY-RESOLVED arrow (radar_layout_resolve_arrow's output —
 * this file does no arrow placement math itself, see the file's top
 * comment): a thin tail from puck-center to the head's base, then a
 * triangular head from that base to the tip. `style` controls the tail's
 * dash pattern and whether the head is filled or outline-only. */
static void radar_draw_arrow(lv_obj_t *parent, radar_layout_arrow_t const *arrow, uint32_t color_hex, lv_opa_t opa,
                              radar_arrow_style_t style)
{
    if (style == RADAR_ARROW_SOLID) {
        radar_draw_segment(parent, 0.0f, 0.0f, arrow->base_dx, arrow->base_dy, color_hex, opa, 6);
    } else {
        enum { N_DASHES = 3 };
        for (int i = 0; i < N_DASHES; i++) {
            float t0 = ((float)i + 0.15f) / (float)N_DASHES;
            float t1 = ((float)i + 0.65f) / (float)N_DASHES;
            radar_draw_segment(parent, arrow->base_dx * t0, arrow->base_dy * t0, arrow->base_dx * t1,
                                arrow->base_dy * t1, color_hex, opa, 5);
        }
    }

    if (style == RADAR_ARROW_GHOST) {
        radar_draw_outline_triangle(parent, arrow->tip_dx, arrow->tip_dy, arrow->left_dx, arrow->left_dy,
                                     arrow->right_dx, arrow->right_dy, color_hex, opa);
    } else {
        radar_draw_filled_triangle(parent, arrow->tip_dx, arrow->tip_dy, arrow->left_dx, arrow->left_dy,
                                    arrow->right_dx, arrow->right_dy, color_hex, opa);
    }
}

static lv_obj_t *radar_build_name_label(lv_obj_t *parent, char const *name, int32_t dy)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, (name != NULL && name[0] != '\0') ? name : "(unnamed)");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(FF_THEME_COLOR_INK), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, dy);
    return label;
}

/* issue #47 — `imprecise` dims the distance text (INK -> MUTED) rather
 * than changing its font/size: the number itself already carries the "~"
 * prefix and is an area scale, not a point distance (ff_radar_compute);
 * de-emphasizing it visually is the remaining, cheap signal that this
 * reading is coarser than the ordinary case, without adding new chrome
 * that risks a layout collision (radar_layout.h's registry is unchanged
 * by this — same rect, same DY, just a different color at one call
 * site). */
static lv_obj_t *radar_build_distance_label_ex(lv_obj_t *parent, char const *dist_str, int32_t dy, bool imprecise)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, (dist_str != NULL && dist_str[0] != '\0') ? dist_str : "-- m");
    lv_obj_set_style_text_font(label, FF_THEME_FONT_DISTANCE, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(imprecise ? FF_THEME_COLOR_MUTED : FF_THEME_COLOR_INK), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, dy);
    return label;
}

static lv_obj_t *radar_build_distance_label(lv_obj_t *parent, char const *dist_str, int32_t dy)
{
    return radar_build_distance_label_ex(parent, dist_str, dy, false);
}

/* issue #47 — appends " - AREA" to a mode chip's text when the selected
 * member's distance is a precision-degraded area estimate rather than a
 * point reading (r->dist_imprecise). A plain hyphen, not U+00B7 MIDDLE
 * DOT — same LVGL built-in-font ASCII-only constraint documented at
 * radar_render_nofix's headline text below. No-op (buffer already holds
 * the caller's base text) when not imprecise. */
static void radar_append_area_suffix(char *buf, size_t n, bool imprecise)
{
    if (!imprecise) {
        return;
    }
    size_t len = strlen(buf);
    if (len < n) {
        snprintf(buf + len, n - len, " - AREA");
    }
}

/* issue #47 — the shared "~" idiom LOST/CLOSE already use for imprecision
 * (RADAR_LOST's real-fix distance, RADAR_CLOSE's big number) must not
 * double up when the underlying dist_str is ALREADY an area-scale string
 * carrying its own "~" (ff_radar_compute sets that prefix directly when
 * r->dist_imprecise). `already_tilde` says which case this is. */
static void radar_dist_with_tilde(char *out, size_t n, char const *dist_str, bool already_tilde)
{
    if (dist_str == NULL || dist_str[0] == '\0') {
        snprintf(out, n, "~?");
        return;
    }
    snprintf(out, n, already_tilde ? "%s" : "~%s", dist_str);
}

/* ---------------------------------------------------------------------
 * Per-mode renderers. Vertical positions come from radar_layout.h's
 * RADAR_LAYOUT_* constants (not local magic numbers) — the SAME values
 * radar_layout_build_registry uses to declare its reserved rectangles,
 * so there is exactly one place either could drift out of sync with the
 * other, and that place is radar_layout.h itself.
 * ------------------------------------------------------------------- */

static void radar_render_live(lv_obj_t *parent, ff_radar_view_t const *r, radar_layout_registry_t const *reg)
{
    radar_layout_arrow_t arrow;
    radar_layout_resolve_arrow(reg, r->arrow_deg, &arrow);
    radar_draw_arrow(parent, &arrow, FF_THEME_COLOR_AMBER, LV_OPA_COVER, RADAR_ARROW_SOLID);

    radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_STACK_NAME_DY);
    radar_build_distance_label_ex(parent, r->dist_str, (int32_t)RADAR_LAYOUT_STACK_DIST_DY, r->dist_imprecise);

    char chip_text[24];
    snprintf(chip_text, sizeof(chip_text), "LIVE");
    radar_append_area_suffix(chip_text, sizeof(chip_text), r->dist_imprecise);
    radar_make_chip(parent, chip_text, FF_THEME_COLOR_LIVE_GREEN, FF_THEME_COLOR_BG,
                     (int32_t)RADAR_LAYOUT_STACK_CHIP_DY);
}

static void radar_render_stale(lv_obj_t *parent, ff_radar_view_t const *r, radar_layout_registry_t const *reg)
{
    radar_build_rim_tint(parent, FF_THEME_COLOR_STALE_AMBER, LV_OPA_50);

    radar_layout_arrow_t arrow;
    radar_layout_resolve_arrow(reg, r->arrow_deg, &arrow);
    /* S06 spec: "dashed arrow at 28% opacity". LV_OPA values are 0-255;
     * 28% of 255 rounds to 71. */
    radar_draw_arrow(parent, &arrow, FF_THEME_COLOR_STALE_AMBER, 71, RADAR_ARROW_DASHED);

    radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_STACK_NAME_DY);
    radar_build_distance_label_ex(parent, r->dist_str, (int32_t)RADAR_LAYOUT_STACK_DIST_DY, r->dist_imprecise);

    char chip_text[40];
    snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
    radar_append_area_suffix(chip_text, sizeof(chip_text), r->dist_imprecise);
    radar_make_chip(parent, chip_text, FF_THEME_COLOR_STALE_AMBER, FF_THEME_COLOR_BG,
                     (int32_t)RADAR_LAYOUT_STACK_CHIP_DY);
}

/* issue #33 — RADAR_PLACE: a landmark's asserted position, rendered as a
 * place rather than a person whose whereabouts were just checked.
 *
 * Deliberately reuses LIVE's SOLID arrow style (not dashed/ghost): the
 * arrow points at a real coordinate, and there is no "aging" fact to
 * signal with a fading/outline treatment — an asserted position doesn't
 * decay, it just was never measured in the first place. What sets this
 * apart from LIVE is entirely in the CHIP and the color: no "LIVE" claim
 * (that would assert a fresh measurement that never happened), no rim
 * tint (that's STALE's aging cue), and a neutral/muted arrow color
 * instead of LIVE's energetic amber — this reading carries no freshness
 * signal to be energetic ABOUT. `age_str` is always "" here
 * (ff_radar_compute), so no age ever reaches this function to render. */
static void radar_render_place(lv_obj_t *parent, ff_radar_view_t const *r, radar_layout_registry_t const *reg)
{
    radar_layout_arrow_t arrow;
    radar_layout_resolve_arrow(reg, r->arrow_deg, &arrow);
    radar_draw_arrow(parent, &arrow, FF_THEME_COLOR_MUTED, LV_OPA_COVER, RADAR_ARROW_SOLID);

    radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_STACK_NAME_DY);
    radar_build_distance_label_ex(parent, r->dist_str, (int32_t)RADAR_LAYOUT_STACK_DIST_DY, r->dist_imprecise);

    /* "FIXED POSITION", never "PLACED" — issue #33's binding ruling:
     * LOC_MANUAL means "not measured," it does NOT certify deliberate or
     * recent placement. "PLACED" (or any past-tense verb) would imply a
     * human action this data cannot honestly attest to; "FIXED POSITION"
     * describes what the node IS (a landmark with a configured
     * coordinate), not when or how it got that way. */
    char chip_text[24];
    snprintf(chip_text, sizeof(chip_text), "FIXED POSITION");
    radar_append_area_suffix(chip_text, sizeof(chip_text), r->dist_imprecise);
    radar_make_chip(parent, chip_text, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_INK,
                     (int32_t)RADAR_LAYOUT_STACK_CHIP_DY);
}

static void radar_render_lost(lv_obj_t *parent, ff_radar_view_t const *r, radar_layout_registry_t const *reg)
{
    /* RENDERER CONTRACT (ff_radar.h): mode == RADAR_LOST alone doesn't
     * distinguish "genuinely old fix" from "never fixed" — key off
     * age_str, not mode, per the header's explicit instruction. */
    bool never_fixed = (r->age_str[0] == '\0');

    if (!never_fixed) {
        /* ORCHESTRATOR RULING (PR #16 UX review finding #1, follow-up):
         * LOST must differ from STALE in KIND, not degree — at a
         * 2-second strobe-lit glance it has to read as a different
         * screen, not a dimmer one. "STALE says this is a few minutes
         * old, LOST says do not trust this." Four structural differences
         * from STALE, not one opacity tweak:
         *   1. NO rim tint at all (STALE gets one; LOST gets none).
         *   2. The arrowhead is OUTLINE-ONLY (RADAR_ARROW_GHOST) — an
         *      unfilled sketch, not a dimmer copy of STALE's filled
         *      triangle. A different silhouette, not just a different
         *      opacity.
         *   3. Distance is "~"-prefixed (like CLOSE's "~15 m") to signal
         *      imprecision — this position is old enough we're no longer
         *      claiming it's exact.
         *   4. The chip is a dark/muted pill ("give up on" language),
         *      not STALE's bright amber pill ("aging but plausible").
         */
        radar_layout_arrow_t arrow;
        radar_layout_resolve_arrow(reg, r->arrow_deg, &arrow);
        /* ~30% opacity: dimmer than LIVE's full strength but bumped up
         * from an earlier, too-faint pass (UX review round 3, non-
         * blocking finding #3: "may be faint enough to read as no arrow
         * at all rather than an old one") — the outline-only shape above
         * already carries most of the "different kind" signal; this is
         * tuned to stay clearly *present* while still reading as
         * untrusted, not to vanish. */
        radar_draw_arrow(parent, &arrow, FF_THEME_COLOR_MUTED, 77, RADAR_ARROW_GHOST);

        radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_STACK_NAME_DY);

        /* issue #47: `r->dist_str` already carries its OWN leading "~"
         * when dist_imprecise (ff_radar_compute) — radar_dist_with_tilde
         * avoids stacking a second one ("~~5.8 km"). */
        char lost_dist[24];
        radar_dist_with_tilde(lost_dist, sizeof(lost_dist), r->dist_str, r->dist_imprecise);
        radar_build_distance_label(parent, lost_dist, (int32_t)RADAR_LAYOUT_STACK_DIST_DY);

        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST SEEN %s", r->age_str);
        radar_append_area_suffix(chip_text, sizeof(chip_text), r->dist_imprecise);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_DIM, FF_THEME_COLOR_INK,
                         (int32_t)RADAR_LAYOUT_STACK_CHIP_DY);
    } else {
        /* Never had a fix at all: arrow_valid is already false (nothing
         * to point at, honestly) — no arrow, no rim tint, no invented
         * distance. A distinct headline so this is never confused with a
         * "LAST SEEN" reading (CLAUDE.md honesty rule; PR #13 review
         * finding #2). */
        lv_obj_t *headline = lv_label_create(parent);
        lv_label_set_text(headline, "NO FIX YET");
        lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
        lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
        lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NEVER_HEADLINE_DY);

        radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_NEVER_NAME_DY);

        lv_obj_t *sub = lv_label_create(parent);
        lv_label_set_text(sub, "Waiting for their first GPS fix");
        lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NEVER_SUB_DY);
    }
}

/* S16 slice c2: the CLOSE-mode FLARE button emits FF_INTENT_FLARE_START
 * through the intent seam (replaces the S10-slice-b stub that took a live
 * `ff_flare_t *flare_rt` and called `ff_flare_send_begin` directly — this
 * file no longer includes ff_flare.h at all, see scr_radar.h's doc
 * comment on this [api] change). No branching of its own: the shell
 * decides what pressing FLARE means (`ff_shell_intent`'s
 * FF_INTENT_FLARE_START case), including the default duration and the
 * real clock reading — this file never touches either. Unbound
 * (golden/headless rendering, which never fires a click), the emit is a
 * safe no-op. */
static void radar_flare_cb(lv_event_t *e)
{
    (void)e;
    ff_intent_t in = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_intent_emit(&in);
}

/* lv_anim_exec_xcb_t is `void(*)(void*, int32_t)`; lv_obj_set_style_opa
 * takes a 3rd (selector) argument, so it can't be used directly as an
 * exec callback (a raw function-pointer cast that drops an argument is
 * undefined behavior) — this thin wrapper supplies LV_PART_MAIN. */
static void radar_anim_set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void radar_render_close(lv_obj_t *parent, ff_radar_view_t const *r)
{
    /* Three pulsing rings (S06: "LVGL anim, 1.2 s period"). Headless
     * single-frame capture never runs the animation timer, so every
     * golden deterministically shows animation-start state — see this
     * function's anim setup below. CLOSE has no arrow (arrow_valid is
     * always false in this mode — ff_radar.h), so there's nothing here
     * for radar_layout_resolve_arrow to do. */
    static const int32_t ring_radii[3] = {38, 64, 90};
    static const lv_opa_t ring_opa[3] = {LV_OPA_80, LV_OPA_50, LV_OPA_20};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(parent);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, ring_radii[i] * 2, ring_radii[i] * 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(FF_THEME_COLOR_LIVE_GREEN), 0);
        lv_obj_set_style_border_opa(ring, ring_opa[i], 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_CLOSE_RING_CY);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_exec_cb(&a, radar_anim_set_opa_cb);
        lv_anim_set_values(&a, ring_opa[i], 0);
        lv_anim_set_duration(&a, 1200); /* S06 spec: "LVGL anim, 1.2 s period" */
        lv_anim_set_reverse_duration(&a, 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, (uint32_t)(i * 150));
        lv_anim_start(&a);
    }

    /* issue #47: CLOSE is only reachable here via the RSSI leg when the
     * position is imprecise (ff_radar_compute gates the DISTANCE leg off
     * for a degraded fix) — a real signal-strength proximity reading
     * alongside a coordinate that could be kilometers off. Showing that
     * coordinate's own "~5.8 km area" text as CLOSE's big pulsing-ring
     * headline would directly contradict the rings ("you are basically
     * standing together" next to "5.8 km"), so this mode names the fact
     * it actually has (RSSI says nearby) instead of a distance number it
     * cannot honestly produce. */
    char big_dist[24];
    if (r->dist_imprecise) {
        snprintf(big_dist, sizeof(big_dist), "NEARBY");
    } else {
        snprintf(big_dist, sizeof(big_dist), "~%s", (r->dist_str[0] != '\0') ? r->dist_str : "?");
    }
    radar_build_distance_label(parent, big_dist, (int32_t)RADAR_LAYOUT_CLOSE_RING_CY);

    radar_build_name_label(parent, r->name, (int32_t)RADAR_LAYOUT_CLOSE_NAME_DY);

    char const *trend_text = "STEADY";
    uint32_t trend_color = FF_THEME_COLOR_MUTED;
    if (r->trend > 0) {
        trend_text = "GETTING CLOSER";
        trend_color = FF_THEME_COLOR_LIVE_GREEN;
    } else if (r->trend < 0) {
        trend_text = "GETTING FARTHER";
        trend_color = FF_THEME_COLOR_STALE_AMBER;
    }
    radar_make_chip(parent, trend_text, trend_color, FF_THEME_COLOR_BG, (int32_t)RADAR_LAYOUT_CLOSE_CHIP_DY);

    /* FLARE button: S06 spec "48 px high, full hit area" — also clears
     * docs/review/ux-raver.md's >=44px tap-target floor with margin. */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, FF_THEME_FLARE_BTN_H_PX);
    lv_obj_set_style_bg_color(btn, lv_color_hex(FF_THEME_COLOR_AMBER), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_CLOSE_FLARE_DY);
    lv_obj_add_event_cb(btn, radar_flare_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "FLARE");
    lv_obj_set_style_text_font(btn_label, FF_THEME_FONT_NAME, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(FF_THEME_COLOR_BG), 0);
    lv_obj_center(btn_label);
}

static void radar_render_nofix(lv_obj_t *parent, ff_radar_view_t const *r)
{
    lv_obj_t *headline = lv_label_create(parent);
    /* S06 spec literally writes "NO FIX · RADIO ONLY" (U+00B7 MIDDLE DOT),
     * but LVGL's built-in Montserrat bitmap fonts only cover the ASCII
     * printable range by default (no supplemental-Latin glyphs compiled
     * in) — that codepoint renders as a tofu/replacement box, not a dot.
     * Substituted with a plain hyphen, which reads the same way and is
     * guaranteed renderable without pulling in a wider (larger) font
     * subset for one punctuation mark. */
    lv_label_set_text(headline, "NO FIX - RADIO ONLY");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_set_width(headline, 320);
    lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NOFIX_HEADLINE_DY);

    if (r->name[0] != '\0') {
        char sub[40];
        snprintf(sub, sizeof(sub), "Looking for %s", r->name);
        lv_obj_t *sub_lbl = lv_label_create(parent);
        lv_label_set_text(sub_lbl, sub);
        lv_obj_set_style_text_font(sub_lbl, FF_THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_color(sub_lbl, lv_color_hex(FF_THEME_COLOR_DIM), 0);
        lv_obj_align(sub_lbl, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NOFIX_SUB_DY);
    }

    /* Honest extra: NOFIX means *my* fix/heading is unusable, but the
     * selected member's own last-known age doesn't depend on that (see
     * ff_radar.h's doc comment) — surface it if we have it, rather than
     * silently dropping data the compute layer went out of its way to
     * still report. */
    if (r->age_str[0] != '\0') {
        char chip_text[40];
        snprintf(chip_text, sizeof(chip_text), "LAST KNOWN %s", r->age_str);
        radar_make_chip(parent, chip_text, FF_THEME_COLOR_SURFACE, FF_THEME_COLOR_MUTED,
                         (int32_t)RADAR_LAYOUT_NOFIX_CHIP_DY);
    }
}

static void radar_render_nosel(lv_obj_t *parent)
{
    lv_obj_t *headline = lv_label_create(parent);
    lv_label_set_text(headline, "NO CREW SELECTED");
    lv_obj_set_style_text_font(headline, FF_THEME_FONT_HEADLINE, 0);
    lv_obj_set_style_text_color(headline, lv_color_hex(FF_THEME_COLOR_MUTED), 0);
    lv_obj_align(headline, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NOSEL_HEADLINE_DY);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "Pair a friend in Settings");
    lv_obj_set_style_text_font(sub, FF_THEME_FONT_LABEL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(FF_THEME_COLOR_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, (int32_t)RADAR_LAYOUT_NOSEL_SUB_DY);
}

/* ---------------------------------------------------------------------
 * Entry point.
 * ------------------------------------------------------------------- */

void ff_scr_radar_build(lv_obj_t *parent, ff_radar_view_t const *radar)
{
    if (parent == NULL || radar == NULL) {
        return;
    }

    s_line_pt_next = 0;
    s_tri_desc_next = 0;

    radar_build_status_bar(parent, radar);

    /* ONE reserved-region registry for this render, built before any
     * movable element is resolved — every movable element (dots below,
     * the arrow inside the per-mode renderers) resolves against this
     * SAME registry, no exceptions (see radar_layout.h's top comment). */
    bool never_fixed = (radar->mode == RADAR_LOST && radar->age_str[0] == '\0');
    radar_layout_registry_t reg;
    radar_layout_build_registry(radar->mode, never_fixed, &reg);

    radar_build_dots(parent, radar, &reg);

    switch (radar->mode) {
    case RADAR_LIVE:
        radar_render_live(parent, radar, &reg);
        break;
    case RADAR_STALE:
        radar_render_stale(parent, radar, &reg);
        break;
    case RADAR_PLACE:
        radar_render_place(parent, radar, &reg);
        break;
    case RADAR_LOST:
        radar_render_lost(parent, radar, &reg);
        break;
    case RADAR_CLOSE:
        radar_render_close(parent, radar);
        break;
    case RADAR_NOFIX:
        radar_render_nofix(parent, radar);
        break;
    case RADAR_NOSEL:
    default:
        radar_render_nosel(parent);
        break;
    }
}
