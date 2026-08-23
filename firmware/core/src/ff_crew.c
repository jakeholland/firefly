#include "ff_crew.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------- */
/* internal helpers                                                     */
/* ------------------------------------------------------------------- */

static ff_crew_member_t *crew_find(ff_crew_t *c, uint32_t node_id, int *out_idx)
{
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->members[i].node_id == node_id) {
            if (out_idx) {
                *out_idx = (int)i;
            }
            return &c->members[i];
        }
    }
    return NULL;
}

/* find-or-create: shared by upsert/set_paired/on_position/on_rssi. Never
 * evicts (AC2: fixed no-eviction-in-v1 policy) — returns NULL once
 * FF_CREW_MAX distinct ids are already occupied and `node_id` isn't one
 * of them. */
static ff_crew_member_t *crew_find_or_create(ff_crew_t *c, uint32_t node_id, int *out_idx)
{
    ff_crew_member_t *m = crew_find(c, node_id, out_idx);
    if (m) {
        return m;
    }
    if (c->count >= FF_CREW_MAX) {
        return NULL;
    }

    uint8_t idx = c->count++;
    ff_crew_member_t *nm = &c->members[idx];
    memset(nm, 0, sizeof(*nm));
    nm->node_id = node_id;
    nm->battery_pct = -1;      /* unknown, per spec comment */
    nm->rssi_dbm = INT16_MIN;  /* never direct, per spec comment */
    nm->has_pos = false;       /* NEVER until the first on_position */

    c->rssi_hist_count[idx] = 0;
    c->rssi_hist_head[idx] = 0;

    if (out_idx) {
        *out_idx = (int)idx;
    }
    return nm;
}

/* ------------------------------------------------------------------- */
/* lifecycle / upsert / pairing                                         */
/* ------------------------------------------------------------------- */

void ff_crew_init(ff_crew_t *c, ff_clock_t const *clock)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->clock = clock;
    c->selected_slot = -1;
}

ff_crew_member_t *ff_crew_upsert(ff_crew_t *c, uint32_t node_id)
{
    if (!c) {
        return NULL;
    }
    return crew_find_or_create(c, node_id, NULL);
}

ff_crew_member_t const *ff_crew_find(ff_crew_t const *c, uint32_t node_id)
{
    if (!c) {
        return NULL;
    }
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->members[i].node_id == node_id) {
            return &c->members[i];
        }
    }
    return NULL;
}

void ff_crew_set_paired(ff_crew_t *c, uint32_t node_id, bool paired)
{
    if (!c) {
        return;
    }
    ff_crew_member_t *m = crew_find_or_create(c, node_id, NULL);
    if (m) {
        m->paired = paired;
    }
}

/* ------------------------------------------------------------------- */
/* position / rssi ingest                                               */
/* ------------------------------------------------------------------- */

void ff_crew_on_position(ff_crew_t *c, uint32_t node_id, ff_latlon_t p, uint32_t rx_time_ms)
{
    if (!c) {
        return;
    }
    ff_crew_member_t *m = crew_find_or_create(c, node_id, NULL);
    if (!m) {
        return;
    }
    m->pos = p;
    m->pos_age_ms = rx_time_ms; /* absolute rx timestamp - see header note */
    m->has_pos = true;
}

void ff_crew_on_rssi(ff_crew_t *c, uint32_t node_id, int16_t rssi_dbm)
{
    if (!c) {
        return;
    }
    int idx = -1;
    ff_crew_member_t *m = crew_find_or_create(c, node_id, &idx);
    if (!m) {
        return;
    }

    uint32_t now = (c->clock && c->clock->now_ms) ? c->clock->now_ms(c->clock->user) : 0u;

    m->rssi_dbm = rssi_dbm;
    m->rssi_age_ms = now; /* absolute rx timestamp - see header note */

    uint8_t head = c->rssi_hist_head[idx];
    c->rssi_hist[idx][head].t_ms = now;
    c->rssi_hist[idx][head].rssi_dbm = rssi_dbm;
    c->rssi_hist_head[idx] = (uint8_t)((head + 1u) % FF_CREW_RSSI_HIST_CAP);
    if (c->rssi_hist_count[idx] < FF_CREW_RSSI_HIST_CAP) {
        c->rssi_hist_count[idx]++;
    }
}

/* ------------------------------------------------------------------- */
/* freshness / close-range / trend                                      */
/* ------------------------------------------------------------------- */

ff_freshness_t ff_crew_freshness(ff_crew_member_t const *m, uint32_t now_ms)
{
    if (!m || !m->has_pos) {
        return FF_FRESH_NEVER;
    }
    uint32_t age = now_ms - m->pos_age_ms; /* wraparound-safe unsigned subtraction */
    if (age < FF_CREW_LIVE_MS) {
        return FF_FRESH_LIVE;
    }
    if (age <= FF_CREW_LOST_MS) {
        return FF_FRESH_STALE;
    }
    return FF_FRESH_LOST;
}

bool ff_crew_close_range(ff_crew_member_t const *m, float distance_m, uint32_t now_ms)
{
    if (!m) {
        return false;
    }
    if (distance_m >= 0.0f && distance_m < FF_CREW_CLOSE_RANGE_M) {
        return true;
    }
    if (m->rssi_dbm == INT16_MIN) {
        return false; /* never had a direct packet */
    }
    uint32_t rssi_age = now_ms - m->rssi_age_ms; /* wraparound-safe */
    if (rssi_age < FF_CREW_CLOSE_RANGE_RSSI_AGE_MS && m->rssi_dbm > FF_CREW_CLOSE_RANGE_RSSI_DBM) {
        return true;
    }
    return false;
}

int8_t ff_crew_rssi_trend(ff_crew_t const *c, uint32_t node_id, uint32_t now_ms)
{
    if (!c) {
        return 0;
    }

    int idx = -1;
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->members[i].node_id == node_id) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        return 0;
    }

    long sum_old = 0, sum_new = 0;
    int n_old = 0, n_new = 0;
    uint8_t cnt = c->rssi_hist_count[idx];

    for (uint8_t k = 0; k < cnt; k++) {
        uint32_t t = c->rssi_hist[idx][k].t_ms;
        uint32_t age = now_ms - t; /* wraparound-safe */
        if (age > FF_CREW_RSSI_TREND_WINDOW_MS) {
            continue; /* outside the 5s window */
        }
        if (age < FF_CREW_RSSI_TREND_WINDOW_MS / 2u) {
            sum_new += c->rssi_hist[idx][k].rssi_dbm;
            n_new++;
        } else {
            sum_old += c->rssi_hist[idx][k].rssi_dbm;
            n_old++;
        }
    }

    if (n_old == 0 || n_new == 0) {
        return 0; /* not enough spread across the window to judge a trend */
    }

    double avg_old = (double)sum_old / n_old;
    double avg_new = (double)sum_new / n_new;
    double delta = avg_new - avg_old;

    if (delta > FF_CREW_RSSI_TREND_THRESHOLD_DBM) {
        return 1;
    }
    if (delta < -FF_CREW_RSSI_TREND_THRESHOLD_DBM) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------- */
/* selection                                                             */
/* ------------------------------------------------------------------- */

ff_crew_member_t *ff_crew_selected(ff_crew_t *c)
{
    if (!c) {
        return NULL;
    }

    if (c->selected_slot >= 0 && (uint8_t)c->selected_slot < c->count &&
        c->members[c->selected_slot].paired) {
        return &c->members[c->selected_slot];
    }

    /* Current selection (if any) is gone or unpaired: self-heal to the
     * first paired member, per spec ("survives members
     * appearing/disappearing"). */
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->members[i].paired) {
            c->selected_slot = (int8_t)i;
            return &c->members[i];
        }
    }
    c->selected_slot = -1;
    return NULL;
}

void ff_crew_select_next(ff_crew_t *c)
{
    if (!c) {
        return;
    }

    /* Normalize first so "next" is always relative to a real, valid
     * current selection (or none). */
    ff_crew_member_t *cur = ff_crew_selected(c);
    if (!cur) {
        return; /* no paired members to select among */
    }

    uint8_t start = (uint8_t)c->selected_slot;
    for (uint8_t step = 1; step <= c->count; step++) {
        uint8_t i = (uint8_t)((start + step) % c->count);
        if (c->members[i].paired) {
            c->selected_slot = (int8_t)i;
            return;
        }
    }
    /* Only the current member is paired: wraps to itself (no-op). */
}

/* ------------------------------------------------------------------- */
/* formatting                                                            */
/* ------------------------------------------------------------------- */

void ff_fmt_distance(char *buf, size_t n, float meters, bool imperial)
{
    if (!buf || n == 0) {
        return;
    }
    if (meters < 0.0f) {
        meters = 0.0f; /* defensive: distance is never negative in practice */
    }

    if (imperial) {
        float feet = meters / 0.3048f;
        if (feet < 1000.0f) {
            snprintf(buf, n, "%.0f ft", (double)feet);
        } else {
            float miles = meters / 1609.344f;
            snprintf(buf, n, "%.1f mi", (double)miles);
        }
    } else {
        if (meters < 1000.0f) {
            snprintf(buf, n, "%.0f m", (double)meters);
        } else {
            snprintf(buf, n, "%.1f km", (double)(meters / 1000.0f));
        }
    }
}

void ff_fmt_age(char *buf, size_t n, uint32_t age_ms)
{
    if (!buf || n == 0) {
        return;
    }

    uint32_t age_s = age_ms / 1000u;
    if (age_s < 60u) {
        snprintf(buf, n, "%u SEC", (unsigned)age_s);
    } else if (age_s < 3600u) {
        snprintf(buf, n, "%u MIN", (unsigned)(age_s / 60u));
    } else {
        snprintf(buf, n, "%u HR", (unsigned)(age_s / 3600u));
    }
}
