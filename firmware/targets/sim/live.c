/**
 * live.c — see live.h.
 */
#include "live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff_proto.h"
#include "fp_pack.h"

/* 256 KiB read budget for ff_live_load_pack: generous for any real
 * festpack.json (schedules included), same "fixed, documented budget"
 * discipline as fixture.c's FIX_MAX_JSON_LEN. */
#define FF_LIVE_PACK_MAX (256 * 1024)

/* ---------------------------------------------------------------------
 * Feed helpers.
 * ------------------------------------------------------------------- */

static char const *live_member_name(ff_live_t const *lv, uint32_t node_id)
{
    for (uint8_t i = 0; i < lv->crew.count; i++) {
        if (lv->crew.members[i].node_id == node_id) return lv->crew.members[i].name;
    }
    return "";
}

/* Inserts one feed item at index 0 (newest-first), shifting the rest down
 * and dropping the oldest once FF_APP_SIGNALS_MAX_ITEMS is reached — same
 * eviction shape ff_app_state.h documents for the real S08 ring, kept
 * simple here since this slice only needs "the pipeline works", not full
 * feed semantics (unread bookkeeping, read-state, etc — see live.h's
 * scope note). */
static void live_feed_push(ff_live_t *lv, ff_app_feed_kind_t kind, char const *from_name, char const *text,
                            uint32_t now_ms)
{
    ff_app_signals_t *sig = &lv->state->signals;
    uint8_t const max = FF_APP_SIGNALS_MAX_ITEMS;
    uint8_t const count_after = (sig->n_items < max) ? (uint8_t)(sig->n_items + 1) : max;

    for (uint8_t i = (uint8_t)(count_after - 1); i > 0; i--) {
        sig->items[i] = sig->items[i - 1];
        lv->feed_rx_ms[i] = lv->feed_rx_ms[i - 1];
    }

    ff_app_feed_item_t *it = &sig->items[0];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    if (from_name != NULL) {
        strncpy(it->from_name, from_name, sizeof(it->from_name) - 1);
    }
    if (text != NULL) {
        strncpy(it->text, text, sizeof(it->text) - 1);
    }
    it->unread = true;

    lv->feed_rx_ms[0] = now_ms;
    sig->n_items = count_after;
    sig->unread_count = (uint8_t)(sig->unread_count < 0xFF ? sig->unread_count + 1 : sig->unread_count);
}

/* ---------------------------------------------------------------------
 * mc_events_t callbacks.
 * ------------------------------------------------------------------- */

static void live_ev_state(void *u, mc_state_t s)
{
    (void)u;
    (void)s; /* nothing to project onto ff_app_state_t yet (no "connecting..."
                UI in this slice) — see live.h's scope note. */
}

static void live_ev_node(void *u, mc_nodeinfo_t const *n)
{
    ff_live_t *lv = (ff_live_t *)u;
    ff_crew_member_t *m = ff_crew_upsert(&lv->crew, n->node_num);
    if (m == NULL) return; /* roster full (FF_CREW_MAX) — dropped, not crashed */

    ff_crew_set_paired(&lv->crew, n->node_num, true); /* see live.h: no S09 pairing yet */

    char const *name = n->has_short_name ? n->short_name : (n->has_long_name ? n->long_name : "");
    if (name[0] != '\0') {
        strncpy(m->name, name, sizeof(m->name) - 1);
        m->name[sizeof(m->name) - 1] = '\0';
        m->initial = name[0];
    }
    if (n->has_battery_level) {
        m->battery_pct = (int8_t)(n->battery_level > 100 ? 100 : n->battery_level);
    }
    if (n->has_position) {
        ff_latlon_t p = {n->position.lat, n->position.lon};
        uint32_t now = lv->clock->now_ms(lv->clock->user);
        ff_crew_on_position(&lv->crew, n->node_num, p, now);
    }
}

static void live_ev_position(void *u, uint32_t node, mc_position_t const *p)
{
    ff_live_t *lv = (ff_live_t *)u;
    (void)ff_crew_upsert(&lv->crew, node); /* ensure a slot exists even if
                                               NodeInfo hasn't arrived yet */
    ff_latlon_t pos = {p->lat, p->lon};
    uint32_t now = lv->clock->now_ms(lv->clock->user);
    ff_crew_on_position(&lv->crew, node, pos, now);
}

static void live_ev_text(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    (void)to;
    ff_live_t *lv = (ff_live_t *)u;

    char text[FF_APP_TEXT_LEN];
    size_t n = (len < sizeof(text) - 1) ? len : sizeof(text) - 1;
    memcpy(text, utf8, n);
    text[n] = '\0';

    uint32_t now = lv->clock->now_ms(lv->clock->user);
    live_feed_push(lv, FF_APP_FEED_TEXT, live_member_name(lv, from), text, now);
}

static void live_ev_private(void *u, uint32_t from, uint32_t portnum, uint8_t const *payload, size_t len)
{
    if (portnum != FF_PORTNUM) return; /* not the firefly protocol */

    ff_live_t *lv = (ff_live_t *)u;
    ff_proto_msg_t msg;
    int type = ff_proto_decode(payload, len, &msg);
    if (type <= 0) return; /* malformed/unrecognized — silently dropped, matches
                               mc_client.h's own "skipped silently but counted" spirit */

    uint32_t now = lv->clock->now_ms(lv->clock->user);
    char const *name = live_member_name(lv, from);

    switch (type) {
        case FF_PROTO_TYPE_PULSE:
            live_feed_push(lv, FF_APP_FEED_PULSE, name, "", now);
            break;
        case FF_PROTO_TYPE_STATUS:
            live_feed_push(lv, FF_APP_FEED_STATUS, name, msg.body.status.text, now);
            break;
        default:
            break; /* FLARE/RALLY/ACK_PING: out of scope for this slice, see live.h */
    }
}

/* ---------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------- */

void ff_live_init(ff_live_t *lv, ff_app_state_t *state, ff_clock_t const *clock)
{
    memset(lv, 0, sizeof(*lv));
    lv->state = state;
    lv->clock = clock;
    lv->tcp.fd = -1;
    ff_crew_init(&lv->crew, clock);
    ff_radar_smooth_reset(&lv->smooth);
    lv->heading_deg = 0.0f; /* see live.h: fixed north placeholder, no compass in sim */
    lv->my_pos_ok = false;
}

void ff_live_set_my_pos(ff_live_t *lv, ff_latlon_t pos)
{
    lv->my_pos = pos;
    lv->my_pos_ok = true;
}

int ff_live_load_pack(ff_live_t *lv, char const *path)
{
    if (path == NULL) return -1;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > FF_LIVE_PACK_MAX) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return -1;
    }

    fp_pack_t pack;
    fp_result_t r = fp_parse(buf, n, &pack);
    free(buf);
    if (r != FP_OK) return -1;

    if (pack.origin_known) {
        ff_live_set_my_pos(lv, pack.origin);
    }
    return 0;
}

void ff_live_attach(ff_live_t *lv, mc_transport_t t)
{
    mc_events_t ev = {0};
    ev.on_state = live_ev_state;
    ev.on_node = live_ev_node;
    ev.on_position = live_ev_position;
    ev.on_text = live_ev_text;
    ev.on_private = live_ev_private;
    ev.user = lv;

    mc_init(&lv->mc, t, ev, lv->clock);
    mc_connect(&lv->mc);
    lv->attached = true;
}

int ff_live_connect(ff_live_t *lv, char const *host, uint16_t port)
{
    if (mc_tcp_open(&lv->tcp, host, port) != 0) return -1;
    ff_live_attach(lv, mc_tcp_transport(&lv->tcp));
    return 0;
}

void ff_live_tick(ff_live_t *lv, uint32_t now_ms)
{
    if (lv->attached) {
        mc_tick(&lv->mc, now_ms);
    }

    ff_radar_compute(&lv->state->radar, &lv->smooth, &lv->crew, lv->heading_deg, lv->my_pos, lv->my_pos_ok,
                      lv->state->settings.imperial, now_ms);

    for (uint8_t i = 0; i < lv->state->signals.n_items; i++) {
        uint32_t age_ms = now_ms - lv->feed_rx_ms[i]; /* unsigned subtraction: wraparound-safe */
        ff_fmt_age(lv->state->signals.items[i].age_str, sizeof(lv->state->signals.items[i].age_str), age_ms);
    }
}

void ff_live_close(ff_live_t *lv)
{
    mc_tcp_close(&lv->tcp);
    lv->attached = false;
}
