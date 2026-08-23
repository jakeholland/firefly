/**
 * ff_feed.h — core/feed: the Signals-face event feed.
 *
 * Spec: docs/specs/S08-signals-t9.md, slice (b) — "feed + wiring + crew
 * filter" (feed itself). No wiring, no meshclient, no LVGL here — see
 * app/ff_wiring.c for the glue that pushes decoded mesh events in here,
 * and app/screens/scr_signals.c for the renderer that reads it out.
 *
 * Pure C11, no I/O, zero heap allocation — `ff_feed_t` is a plain struct
 * (fixed-size ring buffer only) safe to put on the stack or in a static.
 * Zero-initialize or call `ff_feed_init` before use.
 *
 * ## Ring buffer semantics
 * Cap `FF_FEED_CAP` (32) items, newest first for reading
 * (`ff_feed_at(f, 0)` is always the most recently pushed item, regardless
 * of how many have been pushed overall). Pushing past the cap evicts the
 * OLDEST item silently — unlike the fixture loader's fail-loud
 * over-cap-array convention (tests/fixtures/README.md), this is a live
 * ring buffer fed by a radio, not a hand-authored fixture: an attacker or
 * a chatty mesh can push arbitrarily many events, and the spec's own
 * wording ("33rd push evicts oldest") makes eviction the documented,
 * intended behavior here, not an error condition.
 *
 * ## Unread count
 * `ff_feed_t` tracks a running unread count (S08: "unread count drives a
 * badge on the page dot") incrementally — O(1) per push/evict/mark-read,
 * not recomputed by scanning the ring on every read. Pushing an item with
 * `unread == true` increments it; evicting a still-unread item (cap
 * reached) decrements it; `ff_feed_mark_all_read` zeroes it and clears
 * every item's `unread` flag (S08 AC3: "clears on face view" — the
 * Signals screen calls this once when it becomes the active face).
 */
#ifndef FF_FEED_H
#define FF_FEED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ring buffer capacity (S08 spec: "cap 32"). */
#define FF_FEED_CAP 32

/** Feed item text budget (S08 spec's `ff_feed_item_t` sketch: `char text[64]`). */
#define FF_FEED_TEXT_LEN 64

typedef enum {
    FEED_PULSE,
    FEED_TEXT,
    FEED_RALLY,
    FEED_STATUS,
    FEED_FLARE,
} ff_feed_kind_t;

typedef struct {
    ff_feed_kind_t kind;
    uint32_t       from_node; /* Meshtastic node num; 0 for self-originated (no node id) */
    uint32_t       at_ms;     /* caller's clock at receipt/creation */
    char           text[FF_FEED_TEXT_LEN];
    bool           unread;
} ff_feed_item_t;

/**
 * ff_feed_t — the whole ring buffer. Fully-defined (not opaque), same
 * "callers/tests can put it on the stack, inspect fields directly"
 * convention as `ff_crew_t`/`ff_t9_t`.
 *
 * Internal layout (not part of the public contract — use the accessors
 * below, not these fields directly): `items` is a physical circular
 * buffer; `head` is the slot the *next* push will write into; `count` is
 * how many of `items` currently hold valid data (saturates at
 * FF_FEED_CAP, never wraps back down except via `ff_feed_init`).
 */
typedef struct {
    ff_feed_item_t items[FF_FEED_CAP];
    uint8_t        head;
    uint8_t        count;
    uint16_t       unread_count;
} ff_feed_t;

/** ff_feed_init — clear a feed to empty (no items, unread_count 0). */
void ff_feed_init(ff_feed_t *f);

/**
 * ff_feed_push — push `*it` as the newest item. Once `ff_feed_count(f) ==
 * FF_FEED_CAP`, each further push evicts the oldest item (adjusting
 * `unread_count` down first if the evicted item was itself unread) before
 * writing the new one. `*it` is copied by value; the caller's copy may be
 * freed/reused immediately after this returns. No-op if `f` or `it` is
 * NULL.
 */
void ff_feed_push(ff_feed_t *f, ff_feed_item_t const *it);

/** ff_feed_count — number of valid items currently held, 0..FF_FEED_CAP. */
uint8_t ff_feed_count(ff_feed_t const *f);

/**
 * ff_feed_at — the `idx`-th newest item (0 = most recently pushed, 1 =
 * next-most-recent, ...). NULL if `f` is NULL or `idx >= ff_feed_count(f)`.
 * The returned pointer is valid until the next mutating call on `f`.
 */
ff_feed_item_t const *ff_feed_at(ff_feed_t const *f, uint8_t idx);

/** ff_feed_unread_count — current unread count (drives the Signals page-dot badge). */
uint16_t ff_feed_unread_count(ff_feed_t const *f);

/**
 * ff_feed_mark_all_read — clear every currently-held item's `unread` flag
 * and zero the running unread count. S08 AC3: "clears on face view" — call
 * this once when Signals becomes the active face, not on every render.
 */
void ff_feed_mark_all_read(ff_feed_t *f);

#ifdef __cplusplus
}
#endif

#endif /* FF_FEED_H */
