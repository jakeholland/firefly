/**
 * ff_feed.c — see ff_feed.h.
 */
#include "ff_feed.h"

#include <string.h>

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
