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
 * every item's `unread` flag (S08 AC3: "clears on face view").
 *
 * **Known gap (S08 PR #25 code review, MEDIUM finding, disclosed rather
 * than silently left checked off):** as of this PR, nothing outside this
 * module's own unit tests calls `ff_feed_mark_all_read` — the "clears on
 * face view" half of AC3 is implemented and correctly tested in
 * isolation, but not yet WIRED to an actual face-became-active event,
 * because no real app main loop exists yet to detect that transition and
 * hold a live `ff_feed_t` to call this on (`ff_app_state_t.active_face`
 * is currently read once per fixture load/render, never diffed against
 * a previous value — see issue #23's tracking comment for the full
 * writeup and why this is a slightly different gap in kind from that
 * issue's other five stub callbacks). The increment half of AC3 is real
 * end-to-end (`app/ff_wiring.c` pushes with `unread = true` on every
 * live feed item).
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

/**
 * ff_feed_dir_t — [api] S24 slice (a): the item's DIRECTION fact, an
 * honest record of how the item travelled, set by the push site (the
 * wiring / the shell's send paths) which is the only place that ever
 * knew it. This is what splits the CREW thread (broadcast traffic) from
 * 1:1 threads (direct traffic) in `ff_inbox` (ff_inbox.h).
 *
 * Honesty rule (S24 AC1): a push site records only what it actually
 * knows. UNKNOWN is a first-class value, not an error — today the whole
 * private-portnum inbound path (PULSE/RALLY/STATUS/FLARE) is UNKNOWN,
 * because `mc_events_t.on_private` does not carry the mesh packet's
 * `to` address to the wiring layer (mc_client.h; plumbing it through is
 * issue #123 — an `[api]` meshclient change that should land before or
 * with S24 slice (c), out of this core slice's scope). UNKNOWN is
 * deliberately the zero value so a
 * zero-initialized / legacy item is honestly "direction not recorded",
 * never accidentally "broadcast".
 *
 *  - FEED_DIR_UNKNOWN   — direction was not established at push time.
 *                         Never guessed into one of the other values.
 *  - FEED_DIR_BROADCAST — arrived addressed to everyone (mesh broadcast).
 *  - FEED_DIR_DIRECT    — arrived addressed specifically to THIS node
 *                         (to == our own node id, known at receipt). A
 *                         packet addressed to some OTHER specific node —
 *                         or one that arrived before we learned our own
 *                         id — is UNKNOWN, not DIRECT: "addressed to me"
 *                         is a claim this device must be able to attest.
 *  - FEED_DIR_OUT       — our OWN send, pushed at send time so threads
 *                         show both sides (S24). `to_node` below carries
 *                         the destination.
 */
typedef enum {
    FEED_DIR_UNKNOWN = 0,
    FEED_DIR_BROADCAST,
    FEED_DIR_DIRECT,
    FEED_DIR_OUT,
} ff_feed_dir_t;

typedef struct {
    ff_feed_kind_t kind;
    uint32_t       from_node; /* Meshtastic node num; 0 for self-originated (no node id) */
    uint32_t       at_ms;     /* caller's clock at receipt/creation */
    char           text[FF_FEED_TEXT_LEN];
    bool           unread;

    /* [api] S24 slice (a) — direction fact (see ff_feed_dir_t above). */
    ff_feed_dir_t dir;
    /* Destination of an outgoing item: meaningful iff dir == FEED_DIR_OUT.
     * A specific node id, or 0 for a whole-crew broadcast (0 is never a
     * valid member id — ff_sigview/ff_crew's own sentinel convention —
     * and core stays mesh-agnostic: mapping MC_ADDR_BROADCAST -> 0 is the
     * push site's job). Zeroed and meaningless for inbound items (their
     * addressing lives in `dir` itself; `from_node` names the peer). */
    uint32_t to_node;
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

/** ff_feed_unread_count — the feed's own running unread count. S24 note:
 * this no longer drives anything on-glass — both the Signals header
 * badge and the nav page-dot badge sum PER-CONVERSATION unread counts
 * (`ff_scr_signals_unread_count` over the ff_inbox model) instead,
 * because this total also counts direct items from non-paired senders
 * that honestly belong to no conversation, and a badge the rows can't
 * explain would read as a lie. Still the internal bookkeeping the
 * push/evict/mark-read paths maintain, and still bench/test-visible. */
uint16_t ff_feed_unread_count(ff_feed_t const *f);

/**
 * ff_feed_mark_all_read — clear every currently-held item's `unread` flag
 * and zero the running unread count. S08 AC3: "clears on face view" —
 * the INTENDED caller is whatever real event loop eventually owns face
 * transitions, calling this once when Signals becomes the active face
 * (not on every render) — see this header's top comment for the current
 * "nothing calls this yet" disclosure and issue #23.
 */
void ff_feed_mark_all_read(ff_feed_t *f);

/**
 * ff_feed_mark_read_at — [api] S24 slice (a): clear the `unread` flag of
 * the `idx`-th NEWEST item (same indexing as `ff_feed_at`: 0 = most
 * recently pushed), decrementing the running unread count iff that item
 * was actually unread — the same incremental O(1) bookkeeping the push/
 * evict paths keep, never a rescan. No-op if `f` is NULL, `idx` is out
 * of range, or the item is already read.
 *
 * This is the per-item seam `ff_inbox_mark_thread_read` (ff_inbox.h)
 * marks ONE conversation's items through, so opening a thread drives its
 * badge honestly without touching other threads' unread items — unlike
 * `ff_feed_mark_all_read`, which is the whole-feed "face viewed" clear.
 */
void ff_feed_mark_read_at(ff_feed_t *f, uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* FF_FEED_H */
