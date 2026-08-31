/**
 * ff_inbox.h — core/inbox: the Signals inbox -> thread view-model.
 *
 * Spec: docs/specs/S24-signals-inbox.md, slice (a), AC2. This is the pure
 * core the S24 inbox screen (slice b) and thread screens (slice c) render;
 * the screens only project this model and never decide conversation
 * membership, ordering, identity, presence, or unread counts themselves.
 *
 * Pure C11, no I/O, no LVGL, zero heap allocation — `ff_inbox_t` and
 * `ff_inbox_thread_t` are plain structs (fixed-size arrays only, caps
 * derived from FF_FEED_CAP / FF_CREW_MAX) safe to put on the stack or in
 * a static. Zero-initialize or call the init functions before use.
 *
 * ## What this module does
 * It projects the same two EXISTING sources ff_sigview does — the event
 * feed (`ff_feed_t`) and the crew roster (`ff_crew_t`) — into the S24
 * inbox model instead of S22's flat row list:
 *
 *   - CONVERSATIONS (`ff_inbox_build`): one CREW conversation (broadcast
 *     traffic) + one per PAIRED roster member (direct traffic), each with
 *     an honest unread count, a newest-item preview, and (members) the
 *     honest presence category — `ff_sigview_presence`, reused, never
 *     reimplemented.
 *   - THREADS (`ff_inbox_thread_build`): one conversation's items,
 *     oldest -> newest, joined to identity, direction preserved so the
 *     screen can side the bubbles (in vs out).
 *   - MARK-READ (`ff_inbox_mark_thread_read`): clears the unread flag of
 *     ONLY that conversation's items, through `ff_feed_mark_read_at`'s
 *     O(1)-per-item bookkeeping, so opening one thread never silently
 *     "reads" another thread's badge.
 *
 * ## Conversation membership (the one rulebook, used by build, thread
 * extraction and mark-read alike so the three can never disagree)
 *   - CREW: inbound FEED_DIR_BROADCAST items, outgoing whole-crew items
 *     (FEED_DIR_OUT with to_node == 0), and FEED_DIR_UNKNOWN items.
 *     UNKNOWN in CREW is a deliberate, disclosed PLACEMENT decision, not
 *     a direction guess: the item's `dir` stays UNKNOWN (screens can and
 *     should label it honestly), but every item must be renderable
 *     somewhere, and the communal thread is the only container that
 *     doesn't claim a private 1:1 relationship the data can't attest.
 *     (Today the entire inbound private-portnum path — PULSE/RALLY/
 *     STATUS/FLARE — is UNKNOWN; see ff_feed_dir_t's doc comment.)
 *   - MEMBER (node X): inbound FEED_DIR_DIRECT items with from_node == X,
 *     and outgoing items with to_node == X.
 *   - A conversation exists only for CREW and for PAIRED members. A
 *     DIRECT item from a sender who is not (or no longer) a paired
 *     roster member belongs to NO conversation: it is honestly absent
 *     from the inbox model (its unread still counts in the feed's own
 *     global count) rather than attributed to a fabricated identity —
 *     the same paired-only identity gate ff_sigview's join enforces.
 *
 * ## Ordering (S24 AC2)
 *   1. conversations with unread items — newest traffic first;
 *   2. read conversations that have traffic — newest traffic first;
 *   3. quiet conversations (no items): CREW first (the communal anchor
 *      row the spec always shows), then members by presence freshness
 *      (freshest sighting first, LINKED last — the ff_sigview quiet-crew
 *      precedent), ties by ascending node_id.
 *   Ties on traffic age (groups 1/2) break the same way: CREW first,
 *   then ascending node_id — every ordering is deterministic.
 *
 * ## Honesty rules (CLAUDE.md; review-enforced)
 *   - Identity is never fabricated: thread messages from a non-paired
 *     sender render `identity_known == false` (empty name), exactly like
 *     ff_sigview rows.
 *   - Presence is `ff_sigview_presence` over real evidence only; LINKED
 *     has no age (`presence_age_ms` meaningless), never a fabricated one.
 *   - Direction is read, never rewritten: an UNKNOWN item keeps
 *     FEED_DIR_UNKNOWN through preview and thread extraction.
 *   - Unread counts are counted from the items' real flags, and mark-read
 *     mutates only through ff_feed's own incremental bookkeeping.
 */
#ifndef FF_INBOX_H
#define FF_INBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_sigview.h" /* ff_sigview_presence_t — presence is REUSED, not reinvented */

#ifdef __cplusplus
extern "C" {
#endif

/** Which conversation. CREW is the zero value (a zeroed key is the
 * communal thread, never accidentally a member). */
typedef enum {
    FF_CONV_CREW = 0,
    FF_CONV_MEMBER,
} ff_conv_kind_t;

/** Max conversations: the CREW row + every possible paired member. */
#define FF_INBOX_MAX_CONVS (1 + FF_CREW_MAX)

/** Max messages a thread view can hold: every feed item could belong to
 * one conversation. */
#define FF_INBOX_MAX_MSGS FF_FEED_CAP

/**
 * One conversation row, ready to render. Fields not relevant to the
 * row's kind are zeroed (a CREW row has no identity/presence; a
 * traffic-less row has no preview).
 */
typedef struct {
    ff_conv_kind_t kind;
    uint32_t       node_id; /* MEMBER: the member's node id; CREW: 0 */

    /* Identity — MEMBER only, copied from the paired roster member. */
    char    name[16];
    char    initial;
    uint8_t color_idx;

    /* Traffic. */
    uint16_t unread;     /* count of this conversation's unread items */
    uint8_t  item_count; /* total items currently in this conversation */

    /* Newest-item preview — meaningful iff `has_preview` (item_count > 0).
     * `preview_dir` lets the screen render an outgoing newest item
     * honestly ("You: omw") and label an UNKNOWN-direction item as such. */
    bool           has_preview;
    ff_feed_kind_t preview_kind;
    ff_feed_dir_t  preview_dir;
    char           preview_text[FF_FEED_TEXT_LEN];
    uint32_t       preview_age_ms; /* now_ms - newest item's at_ms */

    /* Presence — MEMBER only (`presence_valid`), via ff_sigview_presence.
     * `presence_age_ms` is meaningful iff presence is SEEN or LOST;
     * LINKED has no honest age (ff_sigview.h). */
    bool                  presence_valid;
    ff_sigview_presence_t presence;
    uint32_t              presence_age_ms;
} ff_inbox_conv_t;

/**
 * ff_inbox_t — the conversation list. Fully-defined (not opaque), the
 * ff_feed_t/ff_sigview_t "callers/tests can put it on the stack" plain-
 * struct convention. Derived state only: rebuild per tick from the
 * sources; nothing here persists.
 */
typedef struct {
    ff_inbox_conv_t convs[FF_INBOX_MAX_CONVS];
    uint8_t         conv_count;
} ff_inbox_t;

/* Zero-heap guard, the ff_sigview_t precedent: pin the struct so an
 * accidental heap-shaped or oversized field fails the build loudly. */
_Static_assert(sizeof(ff_inbox_t) < 2048,
               "ff_inbox_t grew unexpectedly large - check for accidental "
               "heap-shaped fields; zero-heap-allocation is an S24 AC");

/** One thread message, ready to render as a sided bubble. */
typedef struct {
    ff_feed_kind_t kind;
    ff_feed_dir_t  dir; /* FEED_DIR_OUT = my side; anything else = theirs */

    /* Sender identity — inbound items, joined by from_node to a PAIRED
     * roster member (never fabricated; false for an outgoing item too —
     * its "identity" is this device). */
    bool     identity_known;
    uint32_t node_id;
    char     name[16];
    char     initial;
    uint8_t  color_idx;

    char     text[FF_FEED_TEXT_LEN];
    uint32_t age_ms; /* now_ms - item.at_ms */
    bool     unread;
} ff_inbox_msg_t;

/** ff_inbox_thread_t — one conversation's messages, OLDEST first (index
 * 0), newest last — the order a chat screen renders top-to-bottom. */
typedef struct {
    ff_inbox_msg_t msgs[FF_INBOX_MAX_MSGS];
    uint8_t        msg_count;
} ff_inbox_thread_t;

_Static_assert(sizeof(ff_inbox_thread_t) < 4096,
               "ff_inbox_thread_t grew unexpectedly large - check for "
               "accidental heap-shaped fields; zero-heap-allocation is an "
               "S24 AC");

/** ff_inbox_init — clear to an empty conversation list. Equivalent to
 * zero-initialization; provided so call sites read as intentional. */
void ff_inbox_init(ff_inbox_t *ib);

/**
 * ff_inbox_build — (re)compute the ordered conversation list from `feed`
 * and `crew` as of `now_ms` (membership + ordering rules: header top
 * comment). The CREW conversation is ALWAYS present — even with no
 * traffic and no crew, the screen needs its honest "no signals yet" row.
 * No-op if `ib` is NULL; a NULL `feed`/`crew` is the empty source.
 */
void ff_inbox_build(ff_inbox_t *ib, ff_feed_t const *feed, ff_crew_t const *crew, uint32_t now_ms);

/** ff_inbox_conv_count — conversations the last build produced (>= 1
 * after any build: CREW is always present). 0 if `ib` is NULL. */
uint8_t ff_inbox_conv_count(ff_inbox_t const *ib);

/** ff_inbox_conv_at — the `idx`-th conversation (0-based, top-to-bottom).
 * NULL if `ib` is NULL or `idx >= ff_inbox_conv_count(ib)`. Valid until
 * the next `ff_inbox_build` on `ib`. */
ff_inbox_conv_t const *ff_inbox_conv_at(ff_inbox_t const *ib, uint8_t idx);

/**
 * ff_inbox_item_in_conv — the membership predicate (header top comment):
 * does `it` belong to conversation (`kind`, `node_id`)? Pure; false if
 * `it` is NULL, or for a MEMBER key with node_id 0. Exposed so build /
 * thread extraction / mark-read demonstrably share one rulebook (and so
 * tests can probe it directly). NOTE: deliberately does NOT know about
 * pairing — whether a member CONVERSATION exists is `ff_inbox_build`'s
 * paired-gated decision; this only answers item-to-key matching.
 */
bool ff_inbox_item_in_conv(ff_feed_item_t const *it, ff_conv_kind_t kind, uint32_t node_id);

/**
 * ff_inbox_thread_build — extract conversation (`kind`, `node_id`)'s
 * items from `feed`, OLDEST first, each joined to `crew` identity (the
 * paired-only gate) with direction preserved. No-op if `t` is NULL; a
 * NULL `feed`/`crew` is the empty source (a NULL crew yields honest
 * identity_known == false messages, never a guessed name).
 */
void ff_inbox_thread_build(ff_inbox_thread_t *t, ff_feed_t const *feed, ff_crew_t const *crew,
                           ff_conv_kind_t kind, uint32_t node_id, uint32_t now_ms);

/** ff_inbox_thread_count — messages the last thread build produced. */
uint8_t ff_inbox_thread_count(ff_inbox_thread_t const *t);

/** ff_inbox_thread_at — the `idx`-th message (0 = OLDEST). NULL if `t` is
 * NULL or `idx >= ff_inbox_thread_count(t)`. Valid until the next
 * `ff_inbox_thread_build` on `t`. */
ff_inbox_msg_t const *ff_inbox_thread_at(ff_inbox_thread_t const *t, uint8_t idx);

/**
 * ff_inbox_mark_thread_read — S24 "per-thread mark-read on open": clear
 * the unread flag of every item belonging to conversation (`kind`,
 * `node_id`) — and ONLY those — via `ff_feed_mark_read_at`, so the
 * feed's O(1) running unread count stays coherent and other threads'
 * badges survive untouched. Returns the number of items newly marked
 * read (0 if `feed` is NULL or nothing matched/was unread).
 */
uint16_t ff_inbox_mark_thread_read(ff_feed_t *feed, ff_conv_kind_t kind, uint32_t node_id);

#ifdef __cplusplus
}
#endif

#endif /* FF_INBOX_H */
