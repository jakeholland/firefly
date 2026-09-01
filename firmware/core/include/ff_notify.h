/**
 * ff_notify.h — core/notify: the notification queue (S26 slice d).
 *
 * Spec: docs/specs/S26-device-lifecycle.md, "Notifications" + "(d)
 * ff_notify + message banner". `ff_flare_t`'s takeover (active / node /
 * expiry / dismiss) is the seed this generalises: a small, fixed-depth
 * FIFO queue of `{kind, tier, node_id, text, at_ms, expiry_ms}` entries,
 * with **kind** (MESSAGE / FLARE / RALLY / SYSTEM) naming what happened
 * and **tier** (BANNER / TAKEOVER) naming how urgently it must be shown.
 *
 * This slice only PUSHES/consumes BANNER-tier entries (the shell's
 * message/rally wiring); FLARE keeps its own, separate, unchanged
 * takeover path in `ff_flare.h` — folding it into this queue is a later
 * slice (spec: "Folding flare in is later"). The TAKEOVER tier value and
 * the FLARE/SYSTEM kinds exist here so the vocabulary is complete for
 * that later slice, but nothing in this repo pushes a TAKEOVER-tier
 * entry yet — see `ff_notify_push`'s doc comment for what that means for
 * `expiry_ms` today.
 *
 * Pure C11, no I/O, no allocation, no clock-reading of its own: every
 * entry point that needs "now" takes it as an explicit `uint32_t now_ms`
 * parameter, the same "explicit now_ms in, no hidden clock" shape
 * `ff_flare_t`/`ff_radar_compute` use and for the same reason (the
 * caller — the app tick, or an rx callback — always already has the
 * timestamp in hand). `ff_notify_t` is a plain, fully-defined,
 * fixed-size struct — safe on the stack or in a static, zero heap
 * allocation. Zero-initialize or call `ff_notify_init` before use.
 *
 * ## Ordering: a plain array, oldest-first
 * `items[0]` is always the OLDEST live entry (the FIFO head, what
 * `ff_notify_head` returns); `items[count-1]` is the newest. A push
 * appends at the end; a removal (dismiss/pop/expiry) closes the gap by
 * shifting later entries down one slot. `FF_NOTIFY_DEPTH` (4) is small
 * enough that this O(depth) shift costs nothing real, and it is what
 * makes `dismiss(idx)` a plain, unsurprising "remove the idx-th queued
 * notification" instead of needing a second index-translation layer over
 * a ring buffer.
 *
 * ## Overflow: the OLDEST is dropped
 * A push that would exceed `FF_NOTIFY_DEPTH` (and does not coalesce —
 * see below) evicts `items[0]` first, exactly like `ff_feed_t`'s ring
 * buffer evicts its own oldest item on overflow. A notification queue
 * exists to surface what is happening NOW; a 5th thing arriving is more
 * relevant than a 4th thing nobody has looked at yet.
 *
 * ## Coalescing (spec AC1: "a duplicate (same node+kind within 2s)
 * coalesces")
 * `ff_notify_push` first scans the queue for a LIVE entry (currently
 * present — a dismissed/popped/expired entry cannot coalesce into, since
 * it is no longer in `items`) with the same `kind` AND `node_id`. If more
 * than one such entry exists (a real but rare case — two RALLYs from the
 * same sender, spaced further apart than the coalesce window, both still
 * un-expired), the entry with the MOST RECENT `at_ms` is the one
 * compared against — the natural "the existing live entry" a debounce
 * measures against. If `now_ms` is within `FF_NOTIFY_COALESCE_MS` (2000)
 * of THAT entry's `at_ms` — **inclusive**, so a push at exactly +2000ms
 * still coalesces and +2001ms does not (this header's judgment call
 * below) — the match's `text`/`at_ms`/`expiry_ms` are overwritten in
 * place (no new entry, no reordering: a coalesced entry keeps its
 * original QUEUE POSITION, only its content and freshness update) rather
 * than adding a second entry. Otherwise a fresh entry is appended
 * (subject to the overflow rule above).
 *
 * ## Banner expiry (spec: "Banner expiry = 6000 ms after at_ms")
 * A BANNER-tier push sets `expiry_ms = at_ms + FF_NOTIFY_BANNER_TTL_MS`
 * (6000). `ff_notify_tick(q, now_ms)` drops (removes from the queue,
 * same shift-down as dismiss) every entry whose deadline has been
 * reached — **inclusive** boundary, matching `ff_flare_t`'s own
 * documented convention (`now_ms == expiry_ms` is already expired, not
 * one tick later). A TAKEOVER-tier push leaves `expiry_ms` at 0 (see
 * `ff_notify_push`'s doc comment) — `ff_notify_tick` treats 0 as "no
 * core-managed deadline" and never expires such an entry on its own;
 * nothing in this repo pushes one yet, so this is untested, declared
 * behavior for the vocabulary's completeness, not a load-bearing claim.
 *
 * ## Judgment calls (flagged per AGENTS.md; recorded in the S26(d) PR
 * body too)
 *  1. **Expiry boundary is INCLUSIVE**, matching `ff_flare_t`'s own
 *     documented convention (see "Banner expiry" above) — the two
 *     already-shipped core FSMs in this tree (`ff_flare`, and
 *     `ff_crew_freshness`'s STALE boundary) both treat "expiry" as the
 *     instant a fact stops being valid, not the last instant it still
 *     is; this module follows the same convention rather than inventing
 *     a third.
 *  2. **The coalesce window is also INCLUSIVE at its upper boundary**
 *     (+2000ms coalesces, +2001ms does not) — the spec's own AC1 wording
 *     names both boundaries explicitly ("within 2 s... and NOT
 *     coalescing at 2001 ms"), so this is transcription, not a choice.
 *  3. **TAKEOVER-tier `expiry_ms` defaults to 0 ("no core-managed
 *     deadline")** rather than inventing a duration the spec never
 *     states for this slice — see "Banner expiry" above. This is the one
 *     genuine judgment call in this header (the other two just
 *     transcribe explicit AC text): a fabricated number here would be
 *     exactly the kind of invented-behavior AGENTS.md's "if a spec is
 *     ambiguous, note the interpretation... do not silently invent
 *     behavior" rule warns against, given the spec is silent on it and
 *     this slice never exercises the path.
 *  4. **A coalesced push does not change the entry's queue POSITION**
 *     (only its content/freshness) — so a coalesced banner does not
 *     "jump the queue" ahead of older, still-live notifications; it
 *     keeps refreshing in place. Not spec-mandated either way; chosen
 *     for the least-surprising FIFO behavior (a notification that keeps
 *     re-arriving doesn't reorder past ones a user hasn't seen yet).
 */
#ifndef FF_NOTIFY_H
#define FF_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed queue depth (spec: "fixed queue depth 4"). */
#define FF_NOTIFY_DEPTH 4

/** Notification preview-text budget. Mirrors `ff_feed.h`'s
 * `FF_FEED_TEXT_LEN` (64) — a banner's text is a short preview of
 * exactly the same class of content (a feed item's body / a rally
 * name), so the same budget applies. */
#define FF_NOTIFY_TEXT_MAX 64

/** Banner tier's time-to-live: `expiry_ms = at_ms + this`, spec's
 * "Banner expiry = 6000 ms after at_ms". */
#define FF_NOTIFY_BANNER_TTL_MS ((uint32_t)6000u)

/** Coalesce window: a push with the same kind+node_id within this many
 * ms of an existing live entry's `at_ms` replaces it instead of adding a
 * new one (spec AC1). Inclusive at the boundary — see this header's top
 * comment, judgment call 2. */
#define FF_NOTIFY_COALESCE_MS ((uint32_t)2000u)

/** What happened. Spec: "kind ∈ MESSAGE · FLARE · RALLY · SYSTEM." Only
 * MESSAGE and RALLY are pushed by this slice's shell wiring; FLARE/SYSTEM
 * are declared for the vocabulary's completeness (folding the existing
 * `ff_flare` takeover into this queue is a later slice; nothing pushes
 * SYSTEM yet either). */
typedef enum {
    FF_NOTIFY_MESSAGE = 0,
    FF_NOTIFY_FLARE,
    FF_NOTIFY_RALLY,
    FF_NOTIFY_SYSTEM,
} ff_notify_kind_t;

/** How urgently this must be shown. Spec: "tier ∈ BANNER · TAKEOVER." A
 * BANNER is the transient, non-blocking strip this slice renders; a
 * TAKEOVER is the full-screen demands-a-decision surface `ff_flare_t`'s
 * receive path already implements on its own — this slice pushes only
 * BANNER-tier entries. BANNER is the zero value: a zero-initialized/
 * default entry is the least-demanding tier, the same "the enum's first
 * member is the least-claiming state" convention `ff_app_state.h`'s
 * `now_state_t`/`ff_route_t` use. */
typedef enum {
    FF_NOTIFY_TIER_BANNER = 0,
    FF_NOTIFY_TIER_TAKEOVER,
} ff_notify_tier_t;

/** One queued notification. Fully-defined (not opaque), the
 * `ff_flare_t`/`ff_feed_item_t` "callers/tests can put it on the stack,
 * read any field directly" convention. */
typedef struct {
    ff_notify_kind_t kind;
    ff_notify_tier_t tier;
    uint32_t         node_id;               /* the sender/subject's node id; 0 = no specific node (SYSTEM) */
    char             text[FF_NOTIFY_TEXT_MAX]; /* short preview BODY (e.g. the message text, or the
                                              * rally name) — no sender-name prefix: identity is a
                                              * separate concern (node_id here; the caller/renderer
                                              * looks up a display name from it, same as every other
                                              * identity join in this app) so a renderer can show the
                                              * name in its own distinct style (e.g. crew color)
                                              * without it being duplicated inside this string. */
    uint32_t         at_ms;                 /* caller's clock when this was pushed (or last coalesced) */
    uint32_t         expiry_ms;             /* BANNER: at_ms + FF_NOTIFY_BANNER_TTL_MS. TAKEOVER: 0 ("no
                                              * core-managed deadline" — see this header's top comment,
                                              * judgment call 3). */
} ff_notify_entry_t;

/**
 * ff_notify_t — the whole queue: a plain, oldest-first array (see this
 * header's top comment) plus a count. Zero-initializing yields an empty
 * queue (`count == 0`), so `ff_notify_init` is equivalent to that but
 * makes call sites read as intentional, the `ff_flare_init`/
 * `ff_inbox_init` precedent.
 */
typedef struct {
    ff_notify_entry_t items[FF_NOTIFY_DEPTH];
    uint8_t           count;
} ff_notify_t;

/* Zero-heap guard, the `ff_inbox_t`/`ff_sigview_t` precedent: pin the
 * struct so an accidental heap-shaped or oversized field fails the build
 * loudly rather than silently growing. */
_Static_assert(sizeof(ff_notify_t) < 512,
               "ff_notify_t grew unexpectedly large - check for accidental "
               "heap-shaped fields; zero-heap-allocation is an S26(d) AC");

/** ff_notify_init — clear `q` to an empty queue. NULL-safe (no-op). */
void ff_notify_init(ff_notify_t *q);

/** ff_notify_count — number of entries currently queued, 0..FF_NOTIFY_DEPTH.
 * 0 if `q` is NULL. */
uint8_t ff_notify_count(ff_notify_t const *q);

/**
 * ff_notify_push — push a notification of `kind`/`tier` from `node_id`
 * with preview `text`, timestamped `now_ms`. `text` is copied and safely
 * truncated to fit `FF_NOTIFY_TEXT_MAX` (NUL-terminated); NULL is treated
 * as "".
 *
 * COALESCE FIRST (spec AC1): if a live entry with the same `kind` AND
 * `node_id` exists, and `now_ms` is within `FF_NOTIFY_COALESCE_MS` of
 * that entry's `at_ms` (inclusive), this OVERWRITES that entry's
 * `text`/`at_ms`/`expiry_ms` in place — same queue position, no new
 * entry, `ff_notify_count` unchanged — and returns. See this header's
 * top comment for tie-breaking when more than one live entry matches.
 *
 * Otherwise APPENDS a new entry as the newest (`items[count]`). If the
 * queue is already at `FF_NOTIFY_DEPTH`, the OLDEST entry (`items[0]`)
 * is evicted first (spec: "Overflow drops the OLDEST") — `ff_notify_count`
 * stays at `FF_NOTIFY_DEPTH`.
 *
 * `expiry_ms` is computed from `tier`: `FF_NOTIFY_TIER_BANNER` gets
 * `now_ms + FF_NOTIFY_BANNER_TTL_MS`; `FF_NOTIFY_TIER_TAKEOVER` gets 0
 * (no core-managed deadline — this header's top comment, judgment call
 * 3; unexercised by this slice).
 *
 * No-op (returns false) if `q` is NULL. Returns true otherwise (a push
 * always succeeds — coalesce and overflow-eviction are both successful
 * outcomes, never a rejection).
 */
bool ff_notify_push(ff_notify_t *q, ff_notify_kind_t kind, ff_notify_tier_t tier, uint32_t node_id,
                    char const *text, uint32_t now_ms);

/**
 * ff_notify_head — the oldest entry currently queued (spec: "head
 * (oldest live)"), i.e. `&q->items[0]`, or NULL if `q` is NULL or empty.
 * "Live" depends on the caller having ticked `q` first (`ff_notify_tick`
 * is what removes expired entries) — this accessor itself does no expiry
 * check, the same "tick, then read" sequencing `ff_shell_tick` already
 * uses for `ff_flare_tick`/`ff_flare_locked_node`. The returned pointer
 * is valid until the next mutating call on `q` (push/pop/dismiss/tick).
 */
ff_notify_entry_t const *ff_notify_head(ff_notify_t const *q);

/**
 * ff_notify_pop — remove the oldest entry (`items[0]`), shifting the
 * rest down one slot. Returns true iff an entry was removed (false, no-op,
 * if `q` is NULL or empty).
 */
bool ff_notify_pop(ff_notify_t *q);

/**
 * ff_notify_dismiss — remove the entry at `idx` (0-based, oldest-first —
 * the same indexing `ff_notify_head`/`items[0]` use), shifting later
 * entries down one slot to close the gap. Returns true iff an entry was
 * removed (false, no-op, if `q` is NULL or `idx >= ff_notify_count(q)`).
 */
bool ff_notify_dismiss(ff_notify_t *q, uint8_t idx);

/**
 * ff_notify_tick — drop every entry whose deadline has been reached as of
 * `now_ms`: an entry with `expiry_ms != 0` (TAKEOVER-tier entries with
 * `expiry_ms == 0` never expire via this call — see this header's top
 * comment) where `now_ms` has reached `expiry_ms`, INCLUSIVE (this
 * header's judgment call 1). Removed entries are dropped in oldest-first
 * order, each closing the gap exactly like `ff_notify_dismiss`. No-op if
 * `q` is NULL. Wraparound-safe against `now_ms`/`expiry_ms` `uint32_t`
 * rollover, matching `ff_clock_t`'s documented convention.
 */
void ff_notify_tick(ff_notify_t *q, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_NOTIFY_H */
