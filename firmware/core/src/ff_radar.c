#include "ff_radar.h"

#include <math.h>
#include <string.h>

#include "ff_geo.h"

/* Arrow smoothing time constant (docs/specs/S06-radar-face.md: "exponential,
 * time-constant 250 ms"). */
#define FF_RADAR_SMOOTH_TAU_MS 250.0f

/* ------------------------------------------------------------------- */
/* small helpers                                                        */
/* ------------------------------------------------------------------- */

static void radar_copy_str(char *dst, size_t dst_sz, char const *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* One exponential-smoothing step toward `target_deg`, wrap-aware via
 * ff_geo_angdiff_deg. First-ever call (has_prev == false) snaps straight
 * to the target — there is no prior value to smooth from. See ff_radar.h's
 * ff_radar_compute doc comment for why composing several small steps here
 * is mathematically identical to one big step over the same total dt. */
static float radar_smooth_step(ff_radar_smooth_t *s, float target_deg, uint32_t now_ms)
{
    target_deg = ff_geo_wrap_deg(target_deg);

    if (!s->has_prev) {
        s->smoothed_deg = target_deg;
        s->has_prev = true;
        s->last_update_ms = now_ms;
        return s->smoothed_deg;
    }

    uint32_t dt_ms = now_ms - s->last_update_ms; /* wraparound-safe, per ff_clock_t's convention */
    float alpha = 1.0f - expf(-(float)dt_ms / FF_RADAR_SMOOTH_TAU_MS);
    if (alpha < 0.0f) {
        alpha = 0.0f;
    } else if (alpha > 1.0f) {
        alpha = 1.0f;
    }

    float diff = ff_geo_angdiff_deg(s->smoothed_deg, target_deg);
    s->smoothed_deg = ff_geo_wrap_deg(s->smoothed_deg + diff * alpha);
    s->last_update_ms = now_ms;
    return s->smoothed_deg;
}

/* Crew ring dots: independent of the current selection/mode. Every paired
 * member with a known fix gets a heading-relative dot; unpaired members
 * and members with no fix ever are excluded (S06 AC3). Requires a usable
 * bearing frame (my_pos_ok && heading valid) — with neither, no bearing is
 * honestly computable for anyone, so the ring is left empty rather than
 * fabricated. */
static void radar_compute_dots(ff_radar_view_t *v, ff_crew_t const *crew, float heading_deg, bool heading_ok,
                                ff_latlon_t my_pos, bool my_pos_ok, uint32_t now_ms)
{
    v->n_dots = 0;
    if (!crew || !my_pos_ok || !heading_ok) {
        return;
    }

    for (uint8_t i = 0; i < crew->count && v->n_dots < FF_CREW_MAX; i++) {
        ff_crew_member_t const *m = &crew->members[i];
        if (!m->paired || !m->has_pos) {
            continue;
        }

        float bearing = ff_geo_bearing_deg(my_pos, m->pos);
        ff_radar_dot_t *d = &v->dots[v->n_dots];
        d->ring_deg = ff_geo_arrow_deg(bearing, heading_deg);
        d->initial = m->initial;
        d->color_idx = m->color_idx;
        ff_freshness_t const dot_fresh = ff_crew_freshness(m, now_ms);
        /* issue #33: an asserted member is a place, not an aging friend —
         * `place` and `stale` are mutually exclusive (ff_radar_dot_t's doc
         * comment). ASSERTED can only ever be dot_fresh's value here since
         * FF_FRESH_NEVER is impossible (this loop already required
         * m->has_pos above). */
        d->place = (dot_fresh == FF_FRESH_ASSERTED);
        d->stale = !d->place && (dot_fresh != FF_FRESH_LIVE);
        /* issue #74: same gate ff_radar_compute applies to the SELECTED
         * member's dist_imprecise below, applied per-member here so every
         * ring dot carries its own honest precision fact instead of only
         * whoever happens to be selected. my_pos_ok is already guaranteed
         * true at this point (checked at this function's top); m->has_pos
         * is already guaranteed true (checked just above). */
        d->imprecise = m->has_precision_bits && m->precision_bits < FF_CREW_POS_PRECISION_MIN_BITS;
        v->n_dots++;
    }
}

/* ------------------------------------------------------------------- */
/* public API                                                            */
/* ------------------------------------------------------------------- */

void ff_radar_smooth_reset(ff_radar_smooth_t *s)
{
    if (!s) {
        return;
    }
    s->has_prev = false;
    s->smoothed_deg = 0.0f;
    s->last_update_ms = 0;
}

void ff_radar_compute(ff_radar_view_t *v, ff_radar_smooth_t *smooth, ff_crew_t *crew, float heading_deg,
                       ff_latlon_t my_pos, bool my_pos_ok, bool imperial, uint32_t now_ms)
{
    if (!v || !smooth || !crew) {
        return;
    }

    bool heading_ok = heading_deg >= 0.0f;

    radar_compute_dots(v, crew, heading_deg, heading_ok, my_pos, my_pos_ok, now_ms);

    ff_crew_member_t *member = ff_crew_selected(crew);
    if (!member) {
        v->mode = RADAR_NOSEL;
        v->arrow_valid = false;
        v->name[0] = '\0';
        v->dist_str[0] = '\0';
        v->age_str[0] = '\0';
        v->trend = 0;
        v->arrow_deg = smooth->smoothed_deg; /* frozen: nothing to smooth toward */
        return;
    }

    radar_copy_str(v->name, sizeof(v->name), member->name);

    float distance_m = -1.0f; /* -1: unknown, matches ff_crew_close_range's convention */
    if (my_pos_ok && member->has_pos) {
        distance_m = ff_geo_distance_m(my_pos, member->pos);
    }

    /* issue #47: known-degraded precision means `distance_m` above claims
     * a confidence the wire data doesn't carry — the coordinate itself
     * could be off by kilometers. Absent has_precision_bits is NOT
     * degraded (see ff_radar.h's doc comment on this asymmetry); it
     * renders exactly like an ordinary fix. */
    bool imprecise = my_pos_ok && member->has_pos && member->has_precision_bits &&
                      member->precision_bits < FF_CREW_POS_PRECISION_MIN_BITS;
    v->dist_imprecise = imprecise;

    /* The value CLOSE-by-distance is allowed to trust: a degraded fix's
     * distance leg is treated as unknown (-1) here, same convention as
     * "no my_pos"/"no member position" above — never let a coordinate
     * that could be kilometers off silently produce a false "standing
     * next to them" reading. The RSSI leg of ff_crew_close_range is
     * untouched: it takes no distance argument and is measured by our own
     * radio, carrying no coordinate-precision dependency at all. */
    float distance_for_close = imprecise ? -1.0f : distance_m;

    if (imprecise) {
        /* An honest area statement, not a fabricated point distance — the
         * grid size IS the whole claim: "your friend is somewhere in a
         * cell this big," never a metre-looking number. Reuses the "~"
         * prefix idiom RADAR_LOST/RADAR_CLOSE already use for imprecision
         * elsewhere on this face. */
        float grid_m = ff_crew_pos_precision_grid_m(member->precision_bits);
        char grid_str[FF_RADAR_STR_LEN - 1];
        ff_fmt_distance(grid_str, sizeof(grid_str), grid_m, imperial);
        v->dist_str[0] = '~';
        radar_copy_str(v->dist_str + 1, sizeof(v->dist_str) - 1, grid_str);
    } else if (distance_m >= 0.0f) {
        ff_fmt_distance(v->dist_str, sizeof(v->dist_str), distance_m, imperial);
    } else {
        v->dist_str[0] = '\0';
    }

    /* issue #33: RADAR_PLACE's age is never honestly knowable (the
     * receive timestamp is receive time, not placement time — see
     * ff_radar.h's RADAR_PLACE paragraph), so it is withheld here
     * regardless of `has_pos`, the same way `dist_str` is withheld above
     * when its underlying fact is unknown. Checked directly on the
     * member's own flag rather than waiting for the freshness switch
     * below, so this holds no matter what mode the member ultimately
     * resolves to (e.g. an asserted member that is also CLOSE by RSSI). */
    if (member->has_pos && !member->pos_asserted) {
        uint32_t age_ms = now_ms - member->pos_age_ms; /* wraparound-safe */
        ff_fmt_age(v->age_str, sizeof(v->age_str), age_ms);
    } else {
        v->age_str[0] = '\0';
    }

    v->trend = ff_crew_rssi_trend(crew, member->node_id, now_ms);

    bool have_bearing = my_pos_ok && heading_ok && member->has_pos;
    if (have_bearing) {
        float bearing = ff_geo_bearing_deg(my_pos, member->pos);
        float target = ff_geo_arrow_deg(bearing, heading_deg);
        v->arrow_deg = radar_smooth_step(smooth, target, now_ms);
    } else {
        v->arrow_deg = smooth->smoothed_deg; /* frozen */
    }

    if (!my_pos_ok || !heading_ok) {
        v->mode = RADAR_NOFIX;
        v->arrow_valid = false;
        return;
    }

    if (ff_crew_close_range(member, distance_for_close, now_ms)) {
        v->mode = RADAR_CLOSE;
        v->arrow_valid = false;
        return;
    }

    ff_freshness_t fresh = ff_crew_freshness(member, now_ms);
    switch (fresh) {
    case FF_FRESH_LIVE:
        v->mode = RADAR_LIVE;
        break;
    case FF_FRESH_STALE:
        v->mode = RADAR_STALE;
        break;
    case FF_FRESH_ASSERTED:
        /* issue #33 — its own mode, never folded into LIVE/STALE/LOST. */
        v->mode = RADAR_PLACE;
        break;
    case FF_FRESH_LOST:
    case FF_FRESH_NEVER:
    default:
        /* FF_FRESH_NEVER folded into RADAR_LOST — see ff_radar.h's doc
         * comment for the rationale. */
        v->mode = RADAR_LOST;
        break;
    }
    v->arrow_valid = have_bearing;
}

bool ff_radar_batt_is_low(int8_t batt_pct)
{
    /* Unknown (< 0) is honestly NOT low — see this function's doc
     * comment in ff_radar.h ("honest data over pretty data": an unknown
     * reading never escalates to an alarm). */
    return batt_pct >= 0 && batt_pct <= FF_BATT_LOW_PCT;
}

ff_batt_icon_t ff_radar_batt_icon(int8_t batt_pct)
{
    if (batt_pct < 0) return FF_BATT_ICON_UNKNOWN;
    if (batt_pct >= FF_BATT_ICON_FULL_MIN_PCT) return FF_BATT_ICON_FULL;
    if (batt_pct >= FF_BATT_ICON_3_MIN_PCT) return FF_BATT_ICON_3;
    if (batt_pct >= FF_BATT_ICON_2_MIN_PCT) return FF_BATT_ICON_2;
    if (batt_pct >= FF_BATT_ICON_1_MIN_PCT) return FF_BATT_ICON_1;
    return FF_BATT_ICON_EMPTY;
}
