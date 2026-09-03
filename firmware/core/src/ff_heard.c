/**
 * ff_heard.c — see ff_heard.h.
 */
#include "ff_heard.h"

#include <string.h>

void ff_heard_init(ff_heard_t *h)
{
    if (h == NULL) return;
    memset(h, 0, sizeof(*h));
}

static ff_heard_entry_t *heard_find(ff_heard_t *h, uint32_t node_id)
{
    for (uint8_t i = 0; i < h->count; i++) {
        if (h->entries[i].node_id == node_id) {
            return &h->entries[i];
        }
    }
    return NULL;
}

void ff_heard_note(ff_heard_t *h, uint32_t node_id, uint32_t now_ms)
{
    if (h == NULL) return;

    ff_heard_entry_t *existing = heard_find(h, node_id);
    if (existing != NULL) {
        existing->last_heard_ms = now_ms;
        return;
    }

    if (h->count < FF_HEARD_MAX) {
        h->entries[h->count].node_id = node_id;
        h->entries[h->count].last_heard_ms = now_ms;
        h->count++;
        return;
    }

    /* Full: evict the least-recently-heard entry — the one with the
     * LARGEST age (now_ms - last_heard_ms), computed with unsigned
     * subtraction so this stays correct across uint32 ms wraparound
     * (~49.7 days). Comparing the raw last_heard_ms values directly with
     * '<' breaks the moment one entry's timestamp is from before a wrap
     * and another's is from after it — see test_heard.c's wrap-case test. */
    uint8_t  lru_idx = 0;
    uint32_t lru_age = now_ms - h->entries[0].last_heard_ms;
    for (uint8_t i = 1; i < h->count; i++) {
        uint32_t age = now_ms - h->entries[i].last_heard_ms;
        if (age > lru_age) {
            lru_idx = i;
            lru_age = age;
        }
    }
    h->entries[lru_idx].node_id = node_id;
    h->entries[lru_idx].last_heard_ms = now_ms;
}

bool ff_heard_remove(ff_heard_t *h, uint32_t node_id)
{
    if (h == NULL) return false;
    for (uint8_t i = 0; i < h->count; i++) {
        if (h->entries[i].node_id != node_id) continue;
        /* Compact by moving the last entry down — ff_heard_at makes no
         * ordering promise beyond "each tracked id appears exactly once",
         * so a swap-remove is fine and keeps this O(1) past the find. */
        h->count--;
        h->entries[i] = h->entries[h->count];
        memset(&h->entries[h->count], 0, sizeof(h->entries[h->count]));
        return true;
    }
    return false;
}

uint8_t ff_heard_count(ff_heard_t const *h)
{
    if (h == NULL) return 0;
    return h->count;
}

ff_heard_entry_t const *ff_heard_at(ff_heard_t const *h, uint8_t idx)
{
    if (h == NULL || idx >= h->count) return NULL;
    return &h->entries[idx];
}

bool ff_heard_contains(ff_heard_t const *h, uint32_t node_id)
{
    if (h == NULL) return false;
    for (uint8_t i = 0; i < h->count; i++) {
        if (h->entries[i].node_id == node_id) return true;
    }
    return false;
}
