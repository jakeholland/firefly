/**
 * ff_sigview.c — see ff_sigview.h.
 *
 * Pure C11 projection of ff_feed_t + ff_crew_t into an ordered row list,
 * plus the send-target state machine. No I/O, no LVGL, zero heap.
 */
#include "ff_sigview.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Presence (S22 AC2)
 * ------------------------------------------------------------------- */

ff_sigview_presence_t ff_sigview_presence(ff_freshness_t pos_fresh, uint32_t pos_age_ms, bool have_rssi,
                                          uint32_t rssi_age_ms, uint32_t *out_age_ms)
{
    /* Gather the honest sighting ages. The position leg offers an age ONLY
     * for a measured fix (LIVE/STALE/LOST). NEVER has no fix; ASSERTED is
     * silent on age (issue #33) — neither contributes a sighting age, no
     * matter what pos_age_ms happens to hold. */
    bool     have_pos_age = (pos_fresh == FF_FRESH_LIVE || pos_fresh == FF_FRESH_STALE ||
                         pos_fresh == FF_FRESH_LOST);
    bool     have_any = false;
    uint32_t age = 0;

    if (have_pos_age) {
        age     = pos_age_ms;
        have_any = true;
    }
    if (have_rssi) {
        if (!have_any || rssi_age_ms < age) {
            age = rssi_age_ms;
        }
        have_any = true;
    }

    if (!have_any) {
        return FF_PRESENCE_LINKED; /* paired but never a sighting — no honest age */
    }

    if (out_age_ms != NULL) {
        *out_age_ms = age;
    }
    /* Inclusive toward SEEN at the boundary, matching ff_crew's own
     * LIVE/STALE/LOST convention (age == FF_CREW_LOST_MS is still SEEN). */
    return (age > FF_CREW_LOST_MS) ? FF_PRESENCE_LOST : FF_PRESENCE_SEEN;
}

/* ---------------------------------------------------------------------
 * Build (S22 AC1)
 * ------------------------------------------------------------------- */

void ff_sigview_init(ff_sigview_t *v)
{
    if (v == NULL) {
        return;
    }
    memset(v, 0, sizeof(*v));
    v->target_kind = FF_TARGET_WHOLE_CREW;
    v->target_node = 0;
    v->row_count   = 0;
}

/* True iff any feed item was sent by `node_id`. `node_id == 0` (a
 * self-originated item's sentinel) never matches a real crew member. */
static bool feed_has_from(ff_feed_t const *feed, uint32_t node_id)
{
    if (feed == NULL || node_id == 0) {
        return false;
    }
    uint8_t n = ff_feed_count(feed);
    for (uint8_t i = 0; i < n; ++i) {
        ff_feed_item_t const *it = ff_feed_at(feed, i);
        if (it != NULL && it->from_node == node_id) {
            return true;
        }
    }
    return false;
}

/* Sort key for quiet-crew ordering: freshest sighting first. LINKED
 * members (no sighting) sort last via UINT32_MAX; SEEN (age small) sort
 * ahead of LOST (age > FF_CREW_LOST_MS) naturally by ascending age. */
static uint32_t quiet_sort_key(ff_sigrow_t const *r)
{
    return (r->presence == FF_PRESENCE_LINKED) ? UINT32_MAX : r->age_ms;
}

void ff_sigview_build(ff_sigview_t *v, ff_feed_t const *feed, ff_crew_t const *crew, uint32_t now_ms)
{
    if (v == NULL) {
        return;
    }

    /* Rebuild rows only — target state is persistent and must survive. */
    v->row_count = 0;

    /* 1. RECENT — feed items newest-first (the feed's own order). */
    uint8_t fn = (feed != NULL) ? ff_feed_count(feed) : 0;
    for (uint8_t i = 0; i < fn && v->row_count < FF_SIGVIEW_MAX_ROWS; ++i) {
        ff_feed_item_t const *it = ff_feed_at(feed, i);
        if (it == NULL) {
            continue;
        }
        ff_sigrow_t *row = &v->rows[v->row_count++];
        memset(row, 0, sizeof(*row));
        row->kind      = FF_SIGROW_RECENT;
        row->feed_kind = it->kind;
        row->unread    = it->unread;
        row->age_ms    = now_ms - it->at_ms; /* unsigned; wraparound-safe */

        /* Join by from_node -> crew identity. A self-originated item
         * (from_node == 0) carries no node id, and a from_node with no
         * roster match is an explicitly-unknown sender — never fabricated. */
        ff_crew_member_t const *m =
            (crew != NULL && it->from_node != 0) ? ff_crew_find(crew, it->from_node) : NULL;
        if (m != NULL) {
            row->identity_known = true;
            row->node_id        = m->node_id;
            row->color_idx      = m->color_idx;
            row->initial        = m->initial;
            memcpy(row->name, m->name, sizeof(row->name));
        } else {
            row->identity_known = false;
            row->node_id        = 0;
        }
    }

    /* 2. DIVIDER — always exactly one "· CREW ·" marker (a stable
     * structural separator the screen can rely on; it may choose not to
     * draw a trailing divider when no quiet crew follow). */
    if (v->row_count < FF_SIGVIEW_MAX_ROWS) {
        ff_sigrow_t *row = &v->rows[v->row_count++];
        memset(row, 0, sizeof(*row));
        row->kind = FF_SIGROW_DIVIDER;
    }

    /* 3. CREW_QUIET — paired members with NO recent feed item. */
    uint16_t quiet_start = v->row_count;
    uint8_t  cn          = (crew != NULL) ? crew->count : 0;
    for (uint8_t i = 0; i < cn && v->row_count < FF_SIGVIEW_MAX_ROWS; ++i) {
        ff_crew_member_t const *m = &crew->members[i];
        if (!m->paired) {
            continue;
        }
        if (feed_has_from(feed, m->node_id)) {
            continue; /* already shown as a RECENT row */
        }

        ff_freshness_t fresh    = ff_crew_freshness(m, now_ms);
        uint32_t       pos_age  = now_ms - m->pos_age_ms;
        bool           have_rssi = (m->rssi_dbm != INT16_MIN);
        uint32_t       rssi_age = now_ms - m->rssi_age_ms;

        uint32_t              age      = 0;
        ff_sigview_presence_t presence = ff_sigview_presence(fresh, pos_age, have_rssi, rssi_age, &age);

        ff_sigrow_t *row = &v->rows[v->row_count++];
        memset(row, 0, sizeof(*row));
        row->kind           = FF_SIGROW_CREW_QUIET;
        row->identity_known = true;
        row->node_id        = m->node_id;
        row->color_idx      = m->color_idx;
        row->initial        = m->initial;
        memcpy(row->name, m->name, sizeof(row->name));
        row->presence = presence;
        row->age_ms   = (presence == FF_PRESENCE_LINKED) ? 0u : age;
    }

    /* Order quiet crew: freshest sighting first, ties by ascending
     * node_id for determinism. Insertion sort — n <= FF_CREW_MAX (8). */
    for (uint16_t i = quiet_start + 1; i < v->row_count; ++i) {
        ff_sigrow_t key = v->rows[i];
        uint32_t    kk  = quiet_sort_key(&key);
        uint16_t    j   = i;
        while (j > quiet_start) {
            ff_sigrow_t const *prev = &v->rows[j - 1];
            uint32_t           pk   = quiet_sort_key(prev);
            if (pk < kk || (pk == kk && prev->node_id <= key.node_id)) {
                break;
            }
            v->rows[j] = v->rows[j - 1];
            --j;
        }
        v->rows[j] = key;
    }
}

uint16_t ff_sigview_row_count(ff_sigview_t const *v)
{
    return (v != NULL) ? v->row_count : 0;
}

ff_sigrow_t const *ff_sigview_row_at(ff_sigview_t const *v, uint16_t idx)
{
    if (v == NULL || idx >= v->row_count) {
        return NULL;
    }
    return &v->rows[idx];
}

/* ---------------------------------------------------------------------
 * Target (S22 AC3/AC4)
 * ------------------------------------------------------------------- */

bool ff_sigview_target_select(ff_sigview_t *v, ff_crew_t const *crew, uint32_t node_id)
{
    if (v == NULL || crew == NULL || node_id == 0) {
        return false;
    }
    ff_crew_member_t const *m = ff_crew_find(crew, node_id);
    if (m == NULL || !m->paired) {
        return false; /* unknown or non-paired — reject, target unchanged */
    }
    v->target_kind = FF_TARGET_MEMBER;
    v->target_node = node_id;
    return true;
}

void ff_sigview_target_clear(ff_sigview_t *v)
{
    if (v == NULL) {
        return;
    }
    v->target_kind = FF_TARGET_WHOLE_CREW;
    v->target_node = 0;
}

void ff_sigview_target_reset_after_send(ff_sigview_t *v)
{
    ff_sigview_target_clear(v);
}

ff_target_kind_t ff_sigview_target_kind(ff_sigview_t const *v)
{
    return (v != NULL) ? v->target_kind : FF_TARGET_WHOLE_CREW;
}

uint32_t ff_sigview_target_node(ff_sigview_t const *v)
{
    return (v != NULL && v->target_kind == FF_TARGET_MEMBER) ? v->target_node : 0u;
}

bool ff_sigview_rally_needs_confirm(ff_sigview_t const *v)
{
    return ff_sigview_target_kind(v) == FF_TARGET_WHOLE_CREW;
}
