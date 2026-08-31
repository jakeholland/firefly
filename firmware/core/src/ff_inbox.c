/**
 * ff_inbox.c — see ff_inbox.h.
 *
 * Pure C11 projection of ff_feed_t + ff_crew_t into the S24 inbox ->
 * thread model. No I/O, no LVGL, zero heap.
 */
#include "ff_inbox.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Membership (the one rulebook — see ff_inbox.h's top comment)
 * ------------------------------------------------------------------- */

bool ff_inbox_item_in_conv(ff_feed_item_t const *it, ff_conv_kind_t kind, uint32_t node_id)
{
    if (it == NULL) {
        return false;
    }
    if (kind == FF_CONV_CREW) {
        /* Broadcast traffic, my own whole-crew sends, and UNKNOWN-
         * direction items (a placement decision, not a direction guess —
         * ff_inbox.h). */
        if (it->dir == FEED_DIR_BROADCAST || it->dir == FEED_DIR_UNKNOWN) {
            return true;
        }
        return it->dir == FEED_DIR_OUT && it->to_node == 0u;
    }
    /* MEMBER — node_id 0 is the "no node id" sentinel, never a member. */
    if (node_id == 0u) {
        return false;
    }
    if (it->dir == FEED_DIR_DIRECT) {
        return it->from_node == node_id;
    }
    return it->dir == FEED_DIR_OUT && it->to_node == node_id;
}

/* ---------------------------------------------------------------------
 * Conversations (S24 AC2)
 * ------------------------------------------------------------------- */

void ff_inbox_init(ff_inbox_t *ib)
{
    if (ib == NULL) {
        return;
    }
    memset(ib, 0, sizeof(*ib));
}

/* Fill one conversation's traffic fields (unread / item_count / newest-
 * item preview) by scanning the feed newest-first once. */
static void conv_fill_traffic(ff_inbox_conv_t *cv, ff_feed_t const *feed, uint32_t now_ms)
{
    uint8_t n = ff_feed_count(feed);
    for (uint8_t i = 0; i < n; ++i) {
        ff_feed_item_t const *it = ff_feed_at(feed, i);
        if (it == NULL || !ff_inbox_item_in_conv(it, cv->kind, cv->node_id)) {
            continue;
        }
        if (cv->item_count < UINT8_MAX) {
            cv->item_count++;
        }
        if (it->unread) {
            cv->unread++;
        }
        if (!cv->has_preview) {
            /* First match in a newest-first scan IS the newest item. */
            cv->has_preview     = true;
            cv->preview_kind    = it->kind;
            cv->preview_dir     = it->dir;
            cv->preview_age_ms  = now_ms - it->at_ms; /* unsigned; wraparound-safe */
            memcpy(cv->preview_text, it->text, sizeof(cv->preview_text));
        }
    }
}

/* Ordering class: 0 = has unread, 1 = read-with-traffic, 2 = quiet. */
static int conv_class(ff_inbox_conv_t const *cv)
{
    if (cv->unread > 0) {
        return 0;
    }
    return (cv->item_count > 0) ? 1 : 2;
}

/* Within-class sort key, ascending (smaller sorts first).
 * Classes 0/1: newest traffic first -> the preview age.
 * Class 2:     CREW first (key 0), then presence freshness — the
 *              ff_sigview quiet-crew precedent: freshest sighting first,
 *              LINKED (no sighting, no honest age) last. */
static uint32_t conv_sort_key(ff_inbox_conv_t const *cv)
{
    if (conv_class(cv) != 2) {
        return cv->preview_age_ms;
    }
    if (cv->kind == FF_CONV_CREW) {
        return 0u; /* the communal anchor row heads the quiet group */
    }
    return (cv->presence == FF_PRESENCE_LINKED) ? UINT32_MAX : cv->presence_age_ms;
}

/* Full ordering comparison: does `a` sort strictly before `b`?
 * class, then key, then CREW-before-member, then ascending node_id —
 * fully deterministic. */
static bool conv_before(ff_inbox_conv_t const *a, ff_inbox_conv_t const *b)
{
    int ca = conv_class(a);
    int cb = conv_class(b);
    if (ca != cb) {
        return ca < cb;
    }
    uint32_t ka = conv_sort_key(a);
    uint32_t kb = conv_sort_key(b);
    if (ka != kb) {
        return ka < kb;
    }
    if ((a->kind == FF_CONV_CREW) != (b->kind == FF_CONV_CREW)) {
        return a->kind == FF_CONV_CREW;
    }
    return a->node_id < b->node_id;
}

void ff_inbox_build(ff_inbox_t *ib, ff_feed_t const *feed, ff_crew_t const *crew, uint32_t now_ms)
{
    if (ib == NULL) {
        return;
    }
    memset(ib, 0, sizeof(*ib));

    /* CREW — always present (the screen's honest "no signals yet" row
     * needs a model row to render, spec "Empty/edge states"). */
    {
        ff_inbox_conv_t *cv = &ib->convs[ib->conv_count++];
        cv->kind    = FF_CONV_CREW;
        cv->node_id = 0u;
        conv_fill_traffic(cv, feed, now_ms);
    }

    /* One conversation per PAIRED roster member (the identity gate —
     * ff_sigview's join precedent; an unpaired/merely-heard node never
     * gets a conversation). */
    uint8_t cn = (crew != NULL) ? crew->count : 0;
    for (uint8_t i = 0; i < cn && ib->conv_count < FF_INBOX_MAX_CONVS; ++i) {
        ff_crew_member_t const *m = &crew->members[i];
        if (!m->paired) {
            continue;
        }
        ff_inbox_conv_t *cv = &ib->convs[ib->conv_count++];
        cv->kind      = FF_CONV_MEMBER;
        cv->node_id   = m->node_id;
        cv->initial   = m->initial;
        cv->color_idx = m->color_idx;
        memcpy(cv->name, m->name, sizeof(cv->name));

        /* Presence — the same honest legs ff_sigview_build feeds
         * ff_sigview_presence (position freshness + direct-packet RSSI
         * age; ASSERTED/NEVER contribute nothing — ff_sigview.h). */
        ff_freshness_t fresh     = ff_crew_freshness(m, now_ms);
        uint32_t       pos_age   = now_ms - m->pos_age_ms;
        bool           have_rssi = (m->rssi_dbm != INT16_MIN);
        uint32_t       rssi_age  = now_ms - m->rssi_age_ms;

        uint32_t age       = 0;
        cv->presence       = ff_sigview_presence(fresh, pos_age, have_rssi, rssi_age, &age);
        cv->presence_valid = true;
        cv->presence_age_ms = (cv->presence == FF_PRESENCE_LINKED) ? 0u : age;

        conv_fill_traffic(cv, feed, now_ms);
    }

    /* Order (S24 AC2): unread first (newest traffic first within), then
     * read-with-traffic by newest traffic, then quiet by presence
     * freshness — insertion sort, n <= FF_INBOX_MAX_CONVS (9). */
    for (uint8_t i = 1; i < ib->conv_count; ++i) {
        ff_inbox_conv_t key = ib->convs[i];
        uint8_t         j   = i;
        while (j > 0 && conv_before(&key, &ib->convs[j - 1])) {
            ib->convs[j] = ib->convs[j - 1];
            --j;
        }
        ib->convs[j] = key;
    }
}

uint8_t ff_inbox_conv_count(ff_inbox_t const *ib)
{
    return (ib != NULL) ? ib->conv_count : 0;
}

ff_inbox_conv_t const *ff_inbox_conv_at(ff_inbox_t const *ib, uint8_t idx)
{
    if (ib == NULL || idx >= ib->conv_count) {
        return NULL;
    }
    return &ib->convs[idx];
}

/* ---------------------------------------------------------------------
 * Thread extraction (S24 AC2)
 * ------------------------------------------------------------------- */

void ff_inbox_thread_build(ff_inbox_thread_t *t, ff_feed_t const *feed, ff_crew_t const *crew,
                           ff_conv_kind_t kind, uint32_t node_id, uint32_t now_ms)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));

    /* Walk the feed OLDEST-first: ff_feed_at(0) is the newest, so index
     * count-1 is the oldest — iterate down. */
    uint8_t n = ff_feed_count(feed);
    for (uint8_t i = n; i > 0 && t->msg_count < FF_INBOX_MAX_MSGS; --i) {
        ff_feed_item_t const *it = ff_feed_at(feed, (uint8_t)(i - 1));
        if (it == NULL || !ff_inbox_item_in_conv(it, kind, node_id)) {
            continue;
        }
        ff_inbox_msg_t *msg = &t->msgs[t->msg_count++];
        msg->kind   = it->kind;
        msg->dir    = it->dir; /* preserved verbatim — UNKNOWN stays UNKNOWN */
        msg->age_ms = now_ms - it->at_ms;
        msg->unread = it->unread;
        memcpy(msg->text, it->text, sizeof(msg->text));

        /* Identity join — inbound items only, PAIRED members only (the
         * ff_sigview gate: a merely-heard or since-unpaired sender is an
         * explicitly-unknown identity, never a guessed name). An outgoing
         * item has no sender to join (it is this device's own). */
        ff_crew_member_t const *found =
            (crew != NULL && it->dir != FEED_DIR_OUT && it->from_node != 0u)
                ? ff_crew_find(crew, it->from_node)
                : NULL;
        if (found != NULL && found->paired) {
            msg->identity_known = true;
            msg->node_id        = found->node_id;
            msg->initial        = found->initial;
            msg->color_idx      = found->color_idx;
            memcpy(msg->name, found->name, sizeof(msg->name));
        }
    }
}

uint8_t ff_inbox_thread_count(ff_inbox_thread_t const *t)
{
    return (t != NULL) ? t->msg_count : 0;
}

ff_inbox_msg_t const *ff_inbox_thread_at(ff_inbox_thread_t const *t, uint8_t idx)
{
    if (t == NULL || idx >= t->msg_count) {
        return NULL;
    }
    return &t->msgs[idx];
}

/* ---------------------------------------------------------------------
 * Per-thread mark-read (S24 AC2)
 * ------------------------------------------------------------------- */

uint16_t ff_inbox_mark_thread_read(ff_feed_t *feed, ff_conv_kind_t kind, uint32_t node_id)
{
    if (feed == NULL) {
        return 0;
    }
    uint16_t marked = 0;
    uint8_t  n      = ff_feed_count(feed);
    for (uint8_t i = 0; i < n; ++i) {
        ff_feed_item_t const *it = ff_feed_at(feed, i);
        if (it == NULL || !it->unread || !ff_inbox_item_in_conv(it, kind, node_id)) {
            continue;
        }
        ff_feed_mark_read_at(feed, i); /* keeps the O(1) unread count coherent */
        marked++;
    }
    return marked;
}
