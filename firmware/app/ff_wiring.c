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

static void wiring_push_if_paired(ff_wiring_ctx_t *w, uint32_t from, ff_feed_kind_t kind, char const *text,
                                   size_t text_len)
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

    switch (type) {
    case FF_PROTO_TYPE_PULSE:
        wiring_push_if_paired(w, from, FEED_PULSE, NULL, 0);
        break;
    case FF_PROTO_TYPE_FLARE:
        wiring_push_if_paired(w, from, FEED_FLARE, NULL, 0);
        break;
    case FF_PROTO_TYPE_RALLY:
        wiring_push_if_paired(w, from, FEED_RALLY, msg.body.rally.name, strlen(msg.body.rally.name));
        break;
    case FF_PROTO_TYPE_STATUS:
        wiring_push_if_paired(w, from, FEED_STATUS, msg.body.status.text, strlen(msg.body.status.text));
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
    (void)to; /* addressed-to-me vs broadcast doesn't change feed-push eligibility */
    ff_wiring_ctx_t *w = (ff_wiring_ctx_t *)user;
    if (w == NULL) return;
    wiring_push_if_paired(w, from, FEED_TEXT, utf8, len);
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

int ff_wiring_send_canned_reply(ff_wiring_ctx_t *w, ff_wiring_canned_reply_t which, ff_feed_item_t const *reply_ctx)
{
    if (w == NULL || w->sender.send_text == NULL || w->sender.send_private == NULL) return -1;

    uint32_t dest = (reply_ctx != NULL) ? reply_ctx->from_node : MC_ADDR_BROADCAST;

    switch (which) {
    case FF_WIRING_REPLY_OMW:
        return w->sender.send_text(w->sender.ctx, dest, "omw");
    case FF_WIRING_REPLY_5MIN:
        return w->sender.send_text(w->sender.ctx, dest, "5 min");
    case FF_WIRING_REPLY_PULSE: {
        uint8_t buf[FF_PROTO_ENVELOPE_LEN];
        int n = ff_proto_encode_pulse(buf, sizeof(buf));
        if (n < 0) return -1;
        return w->sender.send_private(w->sender.ctx, dest, buf, (size_t)n);
    }
    }
    return -1;
}
