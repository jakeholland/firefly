/**
 * ff_feed.c — see ff_feed.h.
 */
#include "ff_feed.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe ACK-timeout deadline check */

void ff_feed_init(ff_feed_t *f)
{
    if (f == NULL) return;
    memset(f, 0, sizeof(*f));
}

void ff_feed_push(ff_feed_t *f, ff_feed_item_t const *it)
{
    if (f == NULL || it == NULL) return;

    bool full = (f->count == FF_FEED_CAP);
    if (full) {
        /* Evicting the oldest item, currently at `head` (the ring's
         * write cursor also happens to be the oldest surviving slot once
         * full — see ff_feed_at's doc comment for the index math this
         * mirrors). Adjust unread_count BEFORE overwriting it. */
        ff_feed_item_t *oldest = &f->items[f->head];
        if (oldest->unread && f->unread_count > 0) {
            f->unread_count--;
        }
    }

    f->items[f->head] = *it;
    if (f->items[f->head].unread) {
        f->unread_count++;
    }

    f->head = (uint8_t)((f->head + 1) % FF_FEED_CAP);
    if (!full) {
        f->count++;
    }
}

uint8_t ff_feed_count(ff_feed_t const *f)
{
    if (f == NULL) return 0;
    return f->count;
}

ff_feed_item_t const *ff_feed_at(ff_feed_t const *f, uint8_t idx)
{
    if (f == NULL || idx >= f->count) return NULL;

    /* `head` is the slot the NEXT push will write to, i.e. one past the
     * newest item physically. Newest item is therefore at (head - 1),
     * and the idx-th newest walks backward from there, wrapping through
     * FF_FEED_CAP. Adding a full extra lap (2*CAP) before the modulo
     * keeps the intermediate value non-negative without needing signed
     * arithmetic on the uint8_t fields. */
    uint32_t phys = ((uint32_t)f->head + (2u * FF_FEED_CAP) - 1u - idx) % FF_FEED_CAP;
    return &f->items[phys];
}

uint16_t ff_feed_unread_count(ff_feed_t const *f)
{
    if (f == NULL) return 0;
    return f->unread_count;
}

void ff_feed_mark_read_at(ff_feed_t *f, uint8_t idx)
{
    if (f == NULL || idx >= f->count) return;

    /* Same idx-th-newest -> physical-slot math as ff_feed_at (see its
     * comment for the +2*CAP trick), applied to the mutable item. */
    uint32_t phys = ((uint32_t)f->head + (2u * FF_FEED_CAP) - 1u - idx) % FF_FEED_CAP;
    ff_feed_item_t *it = &f->items[phys];
    if (it->unread) {
        it->unread = false;
        if (f->unread_count > 0) {
            f->unread_count--;
        }
    }
}

void ff_feed_mark_all_read(ff_feed_t *f)
{
    if (f == NULL) return;
    for (uint8_t i = 0; i < f->count; i++) {
        f->items[i].unread = false;
    }
    f->unread_count = 0;
}

/* ---------------------------------------------------------------------
 * Outbox delivery status (2026-09-07) — see ff_feed.h's own doc
 * comments. All four searches below walk `f->items[0..count)` by
 * PHYSICAL index, same as `ff_feed_mark_all_read` above: order doesn't
 * matter for a search keyed on an item's own identity (outbox_id /
 * packet_id), only that every currently-held item is visited once, and
 * physical index 0..count-1 already holds exactly that set (ff_feed_at's
 * own doc comment explains why: it's the "idx-th NEWEST" math that needs
 * the wraparound trick, not a plain visit-everything walk).
 * ------------------------------------------------------------------- */

void ff_feed_set_send_status_by_outbox_id(ff_feed_t *f, uint32_t outbox_id, ff_feed_send_status_t status,
                                           uint32_t at_ms)
{
    if (f == NULL || outbox_id == 0u) return;
    for (uint8_t i = 0; i < f->count; i++) {
        ff_feed_item_t *it = &f->items[i];
        if (it->dir == FEED_DIR_OUT && it->outbox_id == outbox_id) {
            it->send_status  = status;
            it->status_at_ms = at_ms;
            return;
        }
    }
}

void ff_feed_mark_sent_by_outbox_id(ff_feed_t *f, uint32_t outbox_id, uint32_t packet_id, bool want_ack,
                                     uint32_t at_ms)
{
    if (f == NULL || outbox_id == 0u) return;
    for (uint8_t i = 0; i < f->count; i++) {
        ff_feed_item_t *it = &f->items[i];
        if (it->dir == FEED_DIR_OUT && it->outbox_id == outbox_id) {
            it->send_status  = FF_SEND_SENT;
            it->packet_id    = packet_id;
            it->want_ack     = want_ack;
            it->status_at_ms = at_ms;
            return;
        }
    }
}

bool ff_feed_set_ack_by_packet_id(ff_feed_t *f, uint32_t packet_id, bool ok, uint32_t at_ms)
{
    if (f == NULL || packet_id == 0u) return false;
    for (uint8_t i = 0; i < f->count; i++) {
        ff_feed_item_t *it = &f->items[i];
        if (it->dir == FEED_DIR_OUT && it->want_ack && it->send_status == FF_SEND_SENT &&
            it->packet_id == packet_id) {
            it->send_status  = ok ? FF_SEND_DELIVERED : FF_SEND_NO_ACK;
            it->status_at_ms = at_ms;
            return true;
        }
    }
    return false;
}

void ff_feed_expire_pending_acks(ff_feed_t *f, uint32_t now_ms, uint32_t timeout_ms)
{
    if (f == NULL) return;
    for (uint8_t i = 0; i < f->count; i++) {
        ff_feed_item_t *it = &f->items[i];
        if (it->dir == FEED_DIR_OUT && it->want_ack && it->send_status == FF_SEND_SENT &&
            ff_time_reached(now_ms, it->status_at_ms + timeout_ms)) {
            it->send_status  = FF_SEND_NO_ACK;
            it->status_at_ms = now_ms;
        }
    }
}
