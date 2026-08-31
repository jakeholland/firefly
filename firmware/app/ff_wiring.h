/**
 * ff_wiring.h — app/ff_wiring.c: crew-filtered feed wiring.
 *
 * Spec: docs/specs/S08-signals-t9.md, slice (b). Per docs/ARCHITECTURE.md
 * this is one of the TWO files allowed to include core + meshclient + app
 * together — this one and `app/ff_shell.c` (S16 slice b1). Every other
 * module keeps the strict layering (core has no meshclient/LVGL
 * dependency; meshclient has no core/app dependency; app screens render
 * core state and forward input, no domain logic). The glue that turns
 * decoded Meshtastic events into `ff_feed_t` pushes has to live somewhere
 * that can see both sides, and this is one of the two somewheres.
 *
 * This header used to say "deliberately the ONE file". S16 deliberately
 * changes that rather than working around it: `ff_shell_cfg_t` holds an
 * `mc_transport_t`, so `ff_shell.h` breaks the old invariant by
 * construction, and the object that owns a live client, the core domain
 * state and the view snapshot *together across time* has nowhere else to
 * live. Recorded here rather than left as a claim the design no longer
 * holds — an implementer taking the old sentence literally would contort
 * the design to preserve an invariant this spec retires (S16,
 * "Layering — a correction to a prior claim").
 *
 * ## What this module does
 *  - `ff_wiring_on_private` / `ff_wiring_on_text` have exactly the call
 *    signatures `mc_events_t.on_private` / `.on_text` expect (see
 *    mc_client.h) — wire them in directly as the event callbacks with
 *    `mc_events_t.user = <ff_wiring_ctx_t*>`, OR call them straight from a
 *    test with synthetic (from, payload, len) values as a "mock event
 *    injector" (no live mc_client_t/transport/radio required — this is
 *    the injection seam S08 slice b's AC4 asks for).
 *  - **Crew-paired-sender filter**: an incoming event's `from` node is
 *    looked up via `ff_crew_find` — a READ-ONLY lookup, never
 *    `ff_crew_upsert` — if that lookup misses, or finds a member who
 *    isn't `paired`, the event is dropped: not pushed to the feed, no
 *    haptic. This module used to call `ff_crew_upsert` (find-**or-
 *    create**) here, which meant an unpaired/unknown sender's packet
 *    still claimed one of `ff_crew_t`'s fixed `FF_CREW_MAX` (8) slots
 *    even though the event itself got dropped — v1 has no eviction
 *    (`ff_crew.h`), so 8 packets from 8 distinct never-before-heard node
 *    ids permanently filled the roster and no real crew member could
 *    ever be paired again for the rest of the session (S08 PR #25 code
 *    review, MEDIUM finding — this module is the first live call path
 *    that lets untrusted RF input touch the roster at all, and at a
 *    festival with thousands of nodes in range this needs no malice,
 *    just normal RF volume). Fixed by the ruling: the paired roster and
 *    "which nodes has this puck heard" are two different things.
 *    Unknown senders are instead noted in `w->heard` (`core/include/
 *    ff_heard.h` — a separate, bounded, LRU-evictable list; see that
 *    header for why it lives in core/ alongside ff_crew, not here) if
 *    one was injected, so they're still surfaceable to a future
 *    "add from heard nodes" pairing UI (S04's stated v1 pairing model) —
 *    but never at the cost of a protected roster slot. Only an explicit
 *    user pairing action (`ff_crew_upsert`/`ff_crew_set_paired` from the
 *    Settings/pairing screen, not yet built) ever grows the roster;
 *    inbound radio traffic alone never does.
 *  - On a successful (paired-sender) push, fires the injected haptic
 *    callback once per pushed item. The spec's AC4 wording calls this out
 *    specifically for PULSE ("feed item + haptic callback"); this module
 *    fires it for every kind that reaches the feed (PULSE/RALLY/STATUS/
 *    TEXT), not just PULSE — a product judgment call (flagged here, and
 *    in the PR body), reasoned as: a silent buzz-less RALLY notification
 *    from a paired friend would be a worse UX gap than an extra buzz for
 *    non-PULSE kinds, and the spec gives no reason PULSE alone should
 *    buzz. FLARE_END/RALLY_CLEAR/ACK_PING are state-transition signals,
 *    not feed items (no `ff_feed_kind_t` value represents them) — decoded
 *    and silently dropped from the feed-push path, matching FEED_FLARE's
 *    absence of an END counterpart in the S08 kind list.
 *  - **Canned replies** (`ff_wiring_send_canned_reply`): OMW / 5 MIN send
 *    plain text via the injected sender; PULSE sends the `ff_proto`-
 *    encoded PULSE packet. Destination is `reply_ctx->from_node` if a
 *    reply context is given, else `MC_ADDR_BROADCAST` (S08 spec:
 *    "...to the feed item's sender if a reply-context exists, else
 *    broadcast"). Actually sending needs a live, want_config-ready
 *    `mc_client_t` — which a unit test has no easy way to stand up without
 *    a real transport and handshake (see test_meshclient.c's own mock-IO
 *    harness for how heavy that is) — so sending is behind the small
 *    `ff_wiring_sender_t` vtable below (docs/ARCHITECTURE.md principle 2:
 *    "interfaces are vtables + plain structs, injected at init"), not a
 *    direct `mc_send_text`/`mc_send_private` call. Production code gets a
 *    sender for free from `ff_wiring_init` (thin wrappers bound to a real
 *    `mc_client_t`); tests use `ff_wiring_init_with_sender` to inject a
 *    trivial mock that just records the call — that's this module's "mock
 *    mc" for AC6 ("mock mc captures dest").
 *  - Optimistic feed insertion for an outgoing canned reply (so the
 *    sender sees their own "omw" appear in their feed) is NOT implemented
 *    here — S08's own wording reserves that UX for the *composer's* sent
 *    text ("sent item appears in feed optimistically... ack UX v1.5"),
 *    not canned replies, and there's no spec acceptance criterion asking
 *    for it. Deferred, not silently forgotten.
 */
#ifndef FF_WIRING_H
#define FF_WIRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_heard.h"
#include "ff_intent.h" /* ff_wiring_canned_reply_t — definition moved there, see below */

#include "mc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_wiring_sender_t — the "can send a message" seam canned replies go
 * through. `ctx` is passed back to both function pointers untouched.
 * Return 0 on success, negative on failure (mirrors mc_send_text/
 * mc_send_private's own return convention).
 */
typedef struct {
    int (*send_text)(void *ctx, uint32_t dest, char const *utf8);
    int (*send_private)(void *ctx, uint32_t dest, uint8_t const *payload, size_t len);
    void *ctx;
} ff_wiring_sender_t;

typedef struct {
    ff_feed_t          *feed;
    ff_crew_t           *crew;
    ff_heard_t          *heard; /* NULL = don't track heard-but-unpaired senders (still safe: just no UI list to populate) */
    ff_wiring_sender_t   sender;
    void (*haptic_cb)(void *user); /* NULL = no-op */
    void                *haptic_user;
    ff_clock_t const    *clock;

    /* S24 AC1 — our own node id, when known (set via
     * ff_wiring_set_self_node from the my_info event). Needed to record
     * an inbound text's direction honestly: DIRECT is the claim
     * "addressed to ME", which requires knowing who "me" is. Until it is
     * known, a specifically-addressed text records FEED_DIR_UNKNOWN
     * (never guessed DIRECT). */
    uint32_t self_node;
    bool     has_self_node;
} ff_wiring_ctx_t;

/**
 * ff_wiring_init — production wiring: binds `w->sender` to `mc` via
 * internal thin wrapper functions (`mc_send_text`/`mc_send_private` on
 * the given, already-initialized `mc_client_t`). `mc` must outlive `w`.
 * `heard` may be NULL if the caller has no heard-node UI to feed yet.
 */
void ff_wiring_init(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, ff_heard_t *heard, mc_client_t *mc,
                     void (*haptic_cb)(void *user), void *haptic_user, ff_clock_t const *clock);

/**
 * ff_wiring_init_with_sender — same as `ff_wiring_init`, but takes a
 * caller-supplied `ff_wiring_sender_t` directly instead of deriving one
 * from a real `mc_client_t`. This is the test-injection entry point (a
 * mock sender that just records `dest`/text/payload, no live radio or
 * handshake needed).
 */
void ff_wiring_init_with_sender(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, ff_heard_t *heard,
                                 ff_wiring_sender_t sender, void (*haptic_cb)(void *user), void *haptic_user,
                                 ff_clock_t const *clock);

/**
 * ff_wiring_on_private — `mc_events_t.on_private`-shaped handler. `user`
 * must be a `ff_wiring_ctx_t *` (cast internally). Ignores `portnum !=
 * FF_PORTNUM` (not this app's protocol — S04 rides its own private
 * portnum). Decodes via `ff_proto_decode`; on a recognized
 * feed-representable type (PULSE/RALLY/STATUS — FLARE is decoded but not
 * currently fed, see header note on FLARE_END/RALLY_CLEAR/ACK_PING),
 * applies the crew-paired-sender filter and pushes a feed item timestamped
 * at `w->clock`'s current `now_ms`.
 */
void ff_wiring_on_private(void *user, uint32_t from, uint32_t portnum, uint8_t const *payload, size_t len);

/**
 * ff_wiring_on_text — `mc_events_t.on_text`-shaped handler. `user` must be
 * a `ff_wiring_ctx_t *`. Applies the same crew-paired-sender filter as
 * `ff_wiring_on_private`, pushing a FEED_TEXT item on a paired sender.
 */
void ff_wiring_on_text(void *user, uint32_t from, uint32_t to, char const *utf8, size_t len);

/**
 * ff_wiring_set_self_node — S24 AC1: tell the wiring our own node id (the
 * my_info event's value) so `ff_wiring_on_text` can honestly classify a
 * specifically-addressed inbound text as DIRECT ("addressed to me") vs
 * UNKNOWN. No-op if `w` is NULL.
 */
void ff_wiring_set_self_node(ff_wiring_ctx_t *w, uint32_t self_node);

/**
 * ff_wiring_push_outgoing — S24: push OUR OWN send into the feed
 * (dir == FEED_DIR_OUT, from_node 0, unread false — a send is never a
 * badge or a buzz) so threads show both sides. `dest` is the MESH
 * destination the send was addressed to; MC_ADDR_BROADCAST maps to the
 * core-side "whole crew" to_node sentinel 0 (ff_feed.h). `text` may be
 * NULL/empty for textless kinds (PULSE). Callers push AFTER the sender
 * accepted the message — never fabricate a "sent" item for a refused
 * send. No-op if `w` or `w->feed` is NULL.
 */
void ff_wiring_push_outgoing(ff_wiring_ctx_t *w, ff_feed_kind_t kind, uint32_t dest, char const *text);

/* `ff_wiring_canned_reply_t` (OMW / 5MIN / PULSE) is still this module's
 * vocabulary, but its DEFINITION lives in app/include/ff_intent.h as of
 * S16 slice c1 (included above, so every existing consumer of this
 * header compiles unchanged): the intent union carries a
 * `ff_wiring_canned_reply_t reply` by the spec's exact wording, and
 * screens must be able to build intents without transitively including
 * mc_client.h — which including THIS header would do. Same members, same
 * order, same name; only the textual home moved. `[api]`. */

/**
 * ff_wiring_send_canned_reply — send OMW ("omw" text) / 5 MIN ("5 min"
 * text) / PULSE (encoded PULSE packet) via `w->sender`. Destination is
 * `reply_ctx->from_node` if `reply_ctx` is non-NULL, else
 * `MC_ADDR_BROADCAST`. Returns the sender's return value (0 success,
 * negative failure); -1 if `which` is unrecognized.
 */
int ff_wiring_send_canned_reply(ff_wiring_ctx_t *w, ff_wiring_canned_reply_t which, ff_feed_item_t const *reply_ctx);

#ifdef __cplusplus
}
#endif

#endif /* FF_WIRING_H */
