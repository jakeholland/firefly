/**
 * ff_notify.c — see ff_notify.h.
 */
#include "ff_notify.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check;
                        * see ff_notify.h's judgment call (1) for the
                        * INCLUSIVE-at-the-boundary convention this uses. */

/* Remove items[idx], shifting later entries down one slot. Caller
 * guarantees idx < q->count. */
static void notify_remove_at(ff_notify_t *q, uint8_t idx)
{
    for (uint8_t i = idx; i + 1u < q->count; i++) {
        q->items[i] = q->items[i + 1u];
    }
    q->count--;
}

void ff_notify_init(ff_notify_t *q)
{
    if (q == NULL) return;
    memset(q, 0, sizeof(*q));
}

uint8_t ff_notify_count(ff_notify_t const *q)
{
    return (q != NULL) ? q->count : 0u;
}

ff_notify_push_result_t ff_notify_push(ff_notify_t *q, ff_notify_kind_t kind, ff_notify_tier_t tier, uint32_t node_id,
                                        ff_notify_conv_t conv, char const *text, uint32_t now_ms)
{
    if (q == NULL) return FF_NOTIFY_PUSH_REJECTED;

    /* Coalesce: find the LIVE entry with matching kind+node_id+conv
     * (judgment call 5 — conv joins the match key so a group message and
     * a direct message from the same sender never merge into one banner
     * that can only remember one destination) whose at_ms is the most
     * recent (ties broken by scan order — the queue holds at most
     * FF_NOTIFY_DEPTH entries, so this is cheap and exact). See
     * ff_notify.h's top comment for the full rationale. */
    int match = -1;
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->items[i].kind == kind && q->items[i].node_id == node_id && q->items[i].conv == conv) {
            if (match < 0 || (int32_t)(q->items[i].at_ms - q->items[(uint8_t)match].at_ms) > 0) {
                match = (int)i;
            }
        }
    }

    uint32_t expiry_ms = (tier == FF_NOTIFY_TIER_BANNER) ? (now_ms + FF_NOTIFY_BANNER_TTL_MS) : 0u;

    if (match >= 0 &&
        (int32_t)(now_ms - q->items[(uint8_t)match].at_ms) <= (int32_t)FF_NOTIFY_COALESCE_MS) {
        ff_notify_entry_t *e = &q->items[(uint8_t)match];
        e->at_ms = now_ms;
        e->expiry_ms = expiry_ms;
        e->text[0] = '\0';
        if (text != NULL) {
            size_t n = strlen(text);
            if (n >= sizeof(e->text)) n = sizeof(e->text) - 1u;
            memcpy(e->text, text, n);
            e->text[n] = '\0';
        }
        return FF_NOTIFY_PUSH_COALESCED;
    }

    /* No coalesce: append. Evict the oldest first if already full (spec:
     * "Overflow drops the OLDEST"). */
    if (q->count >= FF_NOTIFY_DEPTH) {
        notify_remove_at(q, 0);
    }

    ff_notify_entry_t *e = &q->items[q->count];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->tier = tier;
    e->node_id = node_id;
    e->conv = conv;
    e->at_ms = now_ms;
    e->expiry_ms = expiry_ms;
    if (text != NULL) {
        size_t n = strlen(text);
        if (n >= sizeof(e->text)) n = sizeof(e->text) - 1u;
        memcpy(e->text, text, n);
        e->text[n] = '\0';
    }
    q->count++;
    return FF_NOTIFY_PUSH_NEW;
}

ff_notify_entry_t const *ff_notify_head(ff_notify_t const *q)
{
    if (q == NULL || q->count == 0u) return NULL;
    return &q->items[0];
}

bool ff_notify_pop(ff_notify_t *q)
{
    if (q == NULL || q->count == 0u) return false;
    notify_remove_at(q, 0);
    return true;
}

bool ff_notify_dismiss(ff_notify_t *q, uint8_t idx)
{
    if (q == NULL || idx >= q->count) return false;
    notify_remove_at(q, idx);
    return true;
}

void ff_notify_tick(ff_notify_t *q, uint32_t now_ms)
{
    if (q == NULL) return;
    /* Oldest-first removal, re-checking index 0 each time an entry is
     * dropped (a dropped items[0] shifts the next entry into slot 0) —
     * simple and correct at FF_NOTIFY_DEPTH's tiny scale. */
    uint8_t i = 0;
    while (i < q->count) {
        ff_notify_entry_t const *e = &q->items[i];
        if (e->expiry_ms != 0u && ff_time_reached(now_ms, e->expiry_ms)) {
            notify_remove_at(q, i);
            /* do not advance i: the next entry has shifted into slot i */
        } else {
            i++;
        }
    }
}
