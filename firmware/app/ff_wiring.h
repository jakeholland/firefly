/**
 * ff_wiring.h — app/ff_wiring.c: crew-filtered feed wiring.
 *
 * Spec: docs/specs/S08-signals-t9.md, slice (b). Per docs/ARCHITECTURE.md
 * this is deliberately the ONE file allowed to include core + meshclient +
 * app together — every other module keeps the strict layering (core has no
 * meshclient/LVGL dependency; meshclient has no core/app dependency; app
 * screens render core state and forward input, no domain logic). The glue
 * that turns decoded Meshtastic events into `ff_feed_t` pushes has to live
 * somewhere that can see both sides, and this is that somewhere.
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
 *    looked up via `ff_crew_upsert` (find-or-create — hearing from a new
 *    node earns it an unpaired "merely heard" slot per S02's model, same
 *    as any other crew-observing code path); if that member isn't
 *    `paired`, the event is dropped — not pushed to the feed, no haptic.
 *    A node the roster has no room to track (`ff_crew_upsert` returns
 *    NULL because the roster is full and this id isn't already in it) is
 *    treated the same as "unpaired" — honest default: without a slot to
 *    read `paired` from, this code cannot claim the sender is trusted, so
 *    it drops rather than guesses trusted.
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
    ff_wiring_sender_t   sender;
    void (*haptic_cb)(void *user); /* NULL = no-op */
    void                *haptic_user;
    ff_clock_t const    *clock;
} ff_wiring_ctx_t;

/**
 * ff_wiring_init — production wiring: binds `w->sender` to `mc` via
 * internal thin wrapper functions (`mc_send_text`/`mc_send_private` on
 * the given, already-initialized `mc_client_t`). `mc` must outlive `w`.
 */
void ff_wiring_init(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, mc_client_t *mc,
                     void (*haptic_cb)(void *user), void *haptic_user, ff_clock_t const *clock);

/**
 * ff_wiring_init_with_sender — same as `ff_wiring_init`, but takes a
 * caller-supplied `ff_wiring_sender_t` directly instead of deriving one
 * from a real `mc_client_t`. This is the test-injection entry point (a
 * mock sender that just records `dest`/text/payload, no live radio or
 * handshake needed).
 */
void ff_wiring_init_with_sender(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, ff_wiring_sender_t sender,
                                 void (*haptic_cb)(void *user), void *haptic_user, ff_clock_t const *clock);

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

typedef enum {
    FF_WIRING_REPLY_OMW,
    FF_WIRING_REPLY_5MIN,
    FF_WIRING_REPLY_PULSE,
} ff_wiring_canned_reply_t;

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
