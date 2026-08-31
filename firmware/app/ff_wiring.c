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

void ff_wiring_on_private(void *user, uint32_t from, uint32_t portnum, uint8_t const *payload, size_t len)
{
    ff_wiring_ctx_t *w = (ff_wiring_ctx_t *)user;
    if (w == NULL || portnum != FF_PORTNUM) return;

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int type = ff_proto_decode(payload, len, &msg);

    /* S24 AC1 — direction on the private path is UNKNOWN, honestly:
     * mc_events_t.on_private does not carry the mesh packet's `to`
     * address (mc_client.h), so this layer never learns whether a
     * PULSE/RALLY/STATUS/FLARE was broadcast or addressed to us. Unknown
     * is stored as unknown — never guessed into BROADCAST/DIRECT.
     * (Plumbing `to` through on_private is issue #123; see
     * ff_feed_dir_t's doc comment.) */
    switch (type) {
    case FF_PROTO_TYPE_PULSE:
        wiring_push_if_paired(w, from, FEED_PULSE, FEED_DIR_UNKNOWN, NULL, 0);
        break;
    case FF_PROTO_TYPE_FLARE:
        wiring_push_if_paired(w, from, FEED_FLARE, FEED_DIR_UNKNOWN, NULL, 0);
        break;
    case FF_PROTO_TYPE_RALLY:
        wiring_push_if_paired(w, from, FEED_RALLY, FEED_DIR_UNKNOWN, msg.body.rally.name,
                              strlen(msg.body.rally.name));
        break;
    case FF_PROTO_TYPE_STATUS:
        wiring_push_if_paired(w, from, FEED_STATUS, FEED_DIR_UNKNOWN, msg.body.status.text,
                              strlen(msg.body.status.text));
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

void ff_wiring_on_text(void *user, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    ff_wiring_ctx_t *w = (ff_wiring_ctx_t *)user;
    if (w == NULL) return;

    /* S24 AC1 — record the direction fact `to` carries, honestly:
     *  - the mesh broadcast address        -> BROADCAST;
     *  - our own node id (when we know it) -> DIRECT (addressed to me);
     *  - anything else                     -> UNKNOWN. That covers both
     *    "we have not learned our own id yet" and "addressed to some
     *    OTHER specific node" (a channel-decodable third-party DM the
     *    radio surfaced) — in neither case can this device attest
     *    "addressed to me", and DIRECT is exactly that claim. */
    ff_feed_dir_t dir = FEED_DIR_UNKNOWN;
    if (to == MC_ADDR_BROADCAST) {
        dir = FEED_DIR_BROADCAST;
    } else if (w->has_self_node && to == w->self_node) {
        dir = FEED_DIR_DIRECT;
    }
    wiring_push_if_paired(w, from, FEED_TEXT, dir, utf8, len);
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

static int wiring_mc_send_private(void *ctx, uint32_t dest, uint8_t const *payload, size_t len)
{
    return mc_send_private((mc_client_t *)ctx, dest, FF_PORTNUM, payload, len, false);
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
    if (w == NULL || w->sender.send_text == NULL || w->sender.send_private == NULL) return -1;

    uint32_t dest = (reply_ctx != NULL) ? reply_ctx->from_node : MC_ADDR_BROADCAST;

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
    case FF_WIRING_REPLY_PULSE: {
        uint8_t buf[FF_PROTO_ENVELOPE_LEN];
        int n = ff_proto_encode_pulse(buf, sizeof(buf));
        if (n < 0) return -1;
        kind = FEED_PULSE;
        rc = w->sender.send_private(w->sender.ctx, dest, buf, (size_t)n);
        break;
    }
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
