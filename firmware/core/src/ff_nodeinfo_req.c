/**
 * ff_nodeinfo_req.c — see ff_nodeinfo_req.h.
 */
#include "ff_nodeinfo_req.h"

#include <string.h>

void ff_nodeinfo_req_init(ff_nodeinfo_req_t *r)
{
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
}

static ff_nodeinfo_req_entry_t *nodeinfo_req_find(ff_nodeinfo_req_t *r, uint32_t node_id)
{
    for (uint8_t i = 0; i < r->count; i++) {
        if (r->entries[i].node_id == node_id) {
            return &r->entries[i];
        }
    }
    return NULL;
}

bool ff_nodeinfo_req_should_send(ff_nodeinfo_req_t *r, uint32_t node_id, uint32_t now_ms)
{
    if (r == NULL || node_id == 0u) return false;

    ff_nodeinfo_req_entry_t *existing = nodeinfo_req_find(r, node_id);
    if (existing != NULL) {
        uint32_t const age = now_ms - existing->last_sent_ms; /* wraparound-safe unsigned subtraction */
        if (age < FF_NODEINFO_REQ_RATE_LIMIT_MS) return false;
        existing->last_sent_ms = now_ms;
        return true;
    }

    if (r->count < FF_NODEINFO_REQ_MAX) {
        r->entries[r->count].node_id = node_id;
        r->entries[r->count].last_sent_ms = now_ms;
        r->count++;
        return true;
    }

    /* Full: evict the least-recently-REQUESTED entry (largest age),
     * same policy and same wraparound-safe comparison as ff_heard_note
     * (ff_heard.c) — see this header's own doc comment for why this
     * table can only ever fill under roster churn. */
    uint8_t  lru_idx = 0;
    uint32_t lru_age = now_ms - r->entries[0].last_sent_ms;
    for (uint8_t i = 1; i < r->count; i++) {
        uint32_t const age = now_ms - r->entries[i].last_sent_ms;
        if (age > lru_age) {
            lru_idx = i;
            lru_age = age;
        }
    }
    r->entries[lru_idx].node_id = node_id;
    r->entries[lru_idx].last_sent_ms = now_ms;
    return true;
}
