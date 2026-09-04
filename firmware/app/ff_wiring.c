/**
 * ff_wiring.c — see ff_wiring.h.
 */
#include "ff_wiring.h"

#include <string.h>

#include "ff_proto.h"

/* ---------------------------------------------------------------------
 * Crew-paired-sender filter + feed push.
 * ------------------------------------------------------------------- */

static uint32_t wiring_now_ms(ff_wiring_ctx_t const *w)
{
    return (w->clock != NULL && w->clock->now_ms != NULL) ? w->clock->now_ms(w->clock->user) : 0;
}

static void wiring_push_if_paired(ff_wiring_ctx_t *w, uint32_t from, ff_feed_kind_t kind, ff_feed_dir_t dir,
                                   char const *text, size_t text_len)
{
    if (w == NULL || w->crew == NULL || w->feed == NULL) return;

    /* READ-ONLY lookup — never ff_crew_upsert here (S08 PR #25 code
     * review, MEDIUM finding: find-or-CREATE on every inbound packet let
     * untrusted RF input consume the roster's fixed, non-evicting
     * FF_CREW_MAX slots regardless of whether the sender ever turned out
     * to be paired — see ff_wiring.h's header comment for the full
     * ruling). A miss (never-heard id, or a roster with no room to have
     * ever tracked it) and a hit-but-unpaired member are both handled
     * the same way: drop the event, and — for a miss specifically — note
     * the sender in the bounded, LRU-evictable heard list instead, so a
     * future pairing UI can still offer it without ever costing a
     * protected roster slot. */
    ff_crew_member_t const *m = ff_crew_find(w->crew, from);
    if (m == NULL) {
        if (w->heard != NULL) {
            ff_heard_note(w->heard, from, wiring_now_ms(w));
        }
        return;
    }
    if (!m->paired) return; /* known (already tracked as heard, or paired-then-unpaired) but not trusted */

    ff_feed_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.from_node = from;
    it.dir = dir; /* S24 AC1 — the caller's honest direction fact, recorded verbatim */
    it.at_ms = (w->clock != NULL && w->clock->now_ms != NULL) ? w->clock->now_ms(w->clock->user) : 0;
    if (text != NULL && text_len > 0) {
        size_t n = text_len;
        if (n >= sizeof(it.text)) n = sizeof(it.text) - 1;
        memcpy(it.text, text, n);
        it.text[n] = '\0';
    }
    it.unread = true;

    ff_feed_push(w->feed, &it);

    if (w->haptic_cb != NULL) {
        w->haptic_cb(w->haptic_user);
    }
}

/* ---------------------------------------------------------------------
 * mc_events_t-shaped handlers.
 * ------------------------------------------------------------------- */

/* S24 AC1 — classify the direction fact `to` carries, honestly:
 *  - the mesh broadcast address        -> BROADCAST;
 *  - our own node id (when we know it) -> DIRECT (addressed to me);
 *  - anything else                     -> UNKNOWN. That covers "we have
 *    not learned our own id yet", "addressed to some OTHER specific
 *    node" (a channel-decodable third-party DM the radio surfaced), and
 *    a producer's explicit MC_ADDR_UNKNOWN — in none of those cases can
 *    this device attest "addressed to me", and DIRECT is exactly that
 *    claim.
 * One classifier shared by the text and private paths (issue #123
 * resolved: mc_events_t.on_private now carries `to`, so a 1:1 rally
 * classifies DIRECT the same way a 1:1 text does — and a whole-crew
 * rally stays BROADCAST, because that is what its `to` says). */
static ff_feed_dir_t wiring_classify_dir(ff_wiring_ctx_t const *w, uint32_t to)
{
    if (to == MC_ADDR_BROADCAST) return FEED_DIR_BROADCAST;
    if (w->has_self_node && to == w->self_node) return FEED_DIR_DIRECT;
    return FEED_DIR_UNKNOWN;
}

void ff_wiring_on_private(void *user, uint32_t from, uint32_t to, uint32_t portnum, uint8_t const *payload,
                           size_t len)
{
    ff_wiring_ctx_t *w = (ff_wiring_ctx_t *)user;
    if (w == NULL || portnum != FF_PORTNUM) return;

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int type = ff_proto_decode(payload, len, &msg);

    ff_feed_dir_t const dir = wiring_classify_dir(w, to);

    switch (type) {
    case FF_PROTO_TYPE_FLARE:
        wiring_push_if_paired(w, from, FEED_FLARE, dir, NULL, 0);
        break;
    case FF_PROTO_TYPE_RALLY:
        wiring_push_if_paired(w, from, FEED_RALLY, dir, msg.body.rally.name,
                              strlen(msg.body.rally.name));
        break;
    case FF_PROTO_TYPE_STATUS:
        wiring_push_if_paired(w, from, FEED_STATUS, dir, msg.body.status.text,
                              strlen(msg.body.status.text));
        break;
    case FF_PROTO_TYPE_RESERVED_01:
        /* Retired PULSE (0x01), 2026-09-02 — a well-formed frame from an
         * old puck build, not malformed input (ff_proto_decode already
         * told us that by returning a positive type here, not 0). Still
         * honestly nothing: no feed item, no notify/banner. Bumped so a
         * dropped retired frame stays bench-visible without a feed item
         * or a log call (see ff_wiring.h's retired_frame_count doc).
         * Presence/"heard" is unaffected — see this file's/header's note
         * on mc_events_t.on_rx_meta firing independent of this switch. */
        w->retired_frame_count++;
        break;
    case FF_PROTO_TYPE_FLARE_END:
    case FF_PROTO_TYPE_RALLY_CLEAR:
    case FF_PROTO_TYPE_ACK_PING:
    default:
        /* State-transition signals (or an unrecognized/malformed decode,
         * type == 0) — not feed items, see ff_wiring.h's header note. */
        break;
    }
}

uint32_t ff_wiring_retired_frame_count(ff_wiring_ctx_t const *w)
{
    return (w == NULL) ? 0u : w->retired_frame_count;
}

void ff_wiring_on_text(void *user, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    ff_wiring_ctx_t *w = (ff_wiring_ctx_t *)user;
    if (w == NULL) return;

    wiring_push_if_paired(w, from, FEED_TEXT, wiring_classify_dir(w, to), utf8, len);
}

void ff_wiring_set_self_node(ff_wiring_ctx_t *w, uint32_t self_node)
{
    if (w == NULL) return;
    w->self_node = self_node;
    w->has_self_node = true;
}

/* ---------------------------------------------------------------------
 * Init.
 * ------------------------------------------------------------------- */

static int wiring_mc_send_text(void *ctx, uint32_t dest, char const *utf8)
{
    return mc_send_text((mc_client_t *)ctx, dest, utf8);
}

/* `[api]`: `flags` (FF_WIRE_WANT_ACK — see ff_wiring.h) reaches
 * mc_send_private's own `want_ack` bool directly. Every pre-existing
 * caller passed no flags before this parameter existed, and 0 still maps
 * to `want_ack == false` — unchanged behavior for every call site that
 * doesn't opt in. */
static int wiring_mc_send_private(void *ctx, uint32_t dest, uint8_t const *payload, size_t len, uint32_t flags)
{
    bool const want_ack = (flags & FF_WIRE_WANT_ACK) != 0u;
    return mc_send_private((mc_client_t *)ctx, dest, FF_PORTNUM, payload, len, want_ack);
}

void ff_wiring_init_with_sender(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, ff_heard_t *heard,
                                 ff_wiring_sender_t sender, void (*haptic_cb)(void *user), void *haptic_user,
                                 ff_clock_t const *clock)
{
    if (w == NULL) return;
    memset(w, 0, sizeof(*w));
    w->feed = feed;
    w->crew = crew;
    w->heard = heard;
    w->sender = sender;
    w->haptic_cb = haptic_cb;
    w->haptic_user = haptic_user;
    w->clock = clock;
}

void ff_wiring_init(ff_wiring_ctx_t *w, ff_feed_t *feed, ff_crew_t *crew, ff_heard_t *heard, mc_client_t *mc,
                     void (*haptic_cb)(void *user), void *haptic_user, ff_clock_t const *clock)
{
    ff_wiring_sender_t sender;
    sender.send_text = wiring_mc_send_text;
    sender.send_private = wiring_mc_send_private;
    sender.ctx = mc;
    ff_wiring_init_with_sender(w, feed, crew, heard, sender, haptic_cb, haptic_user, clock);
}

/* ---------------------------------------------------------------------
 * Canned replies.
 * ------------------------------------------------------------------- */

void ff_wiring_push_outgoing(ff_wiring_ctx_t *w, ff_feed_kind_t kind, uint32_t dest, char const *text)
{
    if (w == NULL || w->feed == NULL) return;

    ff_feed_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.from_node = 0; /* self-originated: no node id (ff_feed.h's existing sentinel) */
    it.dir = FEED_DIR_OUT;
    /* Core stays mesh-agnostic: the broadcast address maps to the
     * "whole crew" sentinel 0 (ff_feed.h's to_node contract). */
    it.to_node = (dest == MC_ADDR_BROADCAST) ? 0u : dest;
    it.at_ms = wiring_now_ms(w);
    if (text != NULL && text[0] != '\0') {
        size_t n = strlen(text);
        if (n >= sizeof(it.text)) n = sizeof(it.text) - 1;
        memcpy(it.text, text, n);
        it.text[n] = '\0';
    }
    it.unread = false; /* my own send is never "unread" — no badge, no haptic */

    ff_feed_push(w->feed, &it);
    /* Deliberately no haptic: the buzz is an inbound-event cue. */
}

int ff_wiring_send_canned_reply(ff_wiring_ctx_t *w, ff_wiring_canned_reply_t which, ff_feed_item_t const *reply_ctx)
{
    /* Context -> destination resolution only; the sends themselves live
     * in ff_wiring_send_canned_reply_to (S24 slice c), so the reply-
     * context path and the thread-scope path can never drift. */
    return ff_wiring_send_canned_reply_to(w, which,
                                           (reply_ctx != NULL) ? reply_ctx->from_node : MC_ADDR_BROADCAST);
}

int ff_wiring_send_canned_reply_to(ff_wiring_ctx_t *w, ff_wiring_canned_reply_t which, uint32_t dest)
{
    if (w == NULL || w->sender.send_text == NULL || w->sender.send_private == NULL) return -1;

    int rc = -1;
    ff_feed_kind_t kind = FEED_TEXT;
    char const *text = NULL;

    switch (which) {
    case FF_WIRING_REPLY_OMW:
        text = "omw";
        rc = w->sender.send_text(w->sender.ctx, dest, text);
        break;
    case FF_WIRING_REPLY_5MIN:
        text = "5 min";
        rc = w->sender.send_text(w->sender.ctx, dest, text);
        break;
    }
    /* No default: -Wswitch flags any new ff_wiring_canned_reply_t member
     * left unhandled (the house convention); an out-of-enum value skips
     * the switch, leaving rc == -1 and pushing nothing. */

    /* S24 — a successful send pushes its own OUTGOING feed item so
     * threads show both sides. Gated on the sender accepting the message
     * (rc == 0): a refused send (link down, encode failure) must not
     * fabricate a "sent" entry in the conversation. */
    if (rc == 0) {
        ff_wiring_push_outgoing(w, kind, dest, text);
    }
    return rc;
}
