/**
 * ff_demoapply.c — see ff_demoapply.h. S23 slice (c)/(d): map a content-free
 * ff_demofeed event onto a demo crew node id, resolve its content, and apply
 * it through the shell's real mesh-inbound seam.
 *
 * Content splits two ways (ff_demoapply.h's honest-data note, S23 AC5):
 *  - FESTIVAL content — a RALLY's place name + lat/lon — is sourced from the
 *    loaded demo festpack (ff_demo_rally_point), never hardcoded.
 *  - SYNTHETIC chatter — the conversational string table below — is invented
 *    demo filler (crew traffic), not festival data, and demo-gated.
 *
 * Everything here is CONFIG_FF_DEMO_MODE demo content: reachable on device
 * only from app_main.c's demo block, dead-stripped from a field build, and it
 * writes no core state directly — it hands honest inbound events to the same
 * path a radio drives.
 */
#include "ff_demoapply.h"

#include <string.h>

#include "ff_demo.h" /* FF_DEMO_NODE_* — the canonical demo crew node ids */

/* ---------------------------------------------------------------------
 * idx -> node_id map.
 *
 * ff_demofeed emits member_idx 0..member_count-1 and knows no real ids
 * (ff_demofeed.h). This is the app-owned map, in ff_demo.c's canonical
 * crew order (DANA first). Seeded into the roster by ff_demo_seed, so
 * every id here is a paired demo crew member — which is why the events
 * pass ff_wiring's paired-sender filter and land in the feed.
 * ------------------------------------------------------------------- */
static const uint32_t FF_DEMO_LIVE_NODE_IDS[FF_DEMO_LIVE_MEMBER_COUNT] = {
    FF_DEMO_NODE_DANA, FF_DEMO_NODE_KEV, FF_DEMO_NODE_RILEY, FF_DEMO_NODE_MAYA, FF_DEMO_NODE_SAM,
};

uint32_t const *ff_demo_live_node_ids(uint8_t *out_count)
{
    if (out_count != NULL) *out_count = FF_DEMO_LIVE_MEMBER_COUNT;
    return FF_DEMO_LIVE_NODE_IDS;
}

/* ---------------------------------------------------------------------
 * Demo chatter table (text_ref -> string).
 *
 * FF_DEMOFEED_TEXT_REF_COUNT (16) SYNTHETIC conversational one-liners used as
 * a TEXT body or a STATUS. Each is <= FF_PROTO_STATUS_MAX (20) bytes so it is
 * valid as either without being silently clipped.
 *
 * This is demo-gated crew FILLER, NOT festival content: it invents no place,
 * stage, or landmark name (those come from the festpack — see
 * ff_demo_rally_point). So S23 AC5's "festival content is festpack-sourced"
 * rule doesn't bite here; this is the demo-gated exempt category the spec's
 * AC5 calls out (compiled out of any field build via the gating in
 * ff_demoapply.h). Keeping it in C, not the festpack, is deliberate: the
 * general fp_pack schema has no chatter field and these are fake strings
 * either way.
 * ------------------------------------------------------------------- */
static char const *const FF_DEMO_TEXTS[FF_DEMOFEED_TEXT_REF_COUNT] = {
    "on my way!",         /* 0  */
    "at the main stage",  /* 1  */
    "who's got water?",   /* 2  */
    "bass hollow now",    /* 3  */
    "meet at the tower",  /* 4  */
    "this set is unreal", /* 5  */
    "grabbing tacos",     /* 6  */
    "5 min out",          /* 7  */
    "found a shady spot", /* 8  */
    "where u at?",        /* 9  */
    "front left rail",    /* 10 */
    "heading back to camp",/* 11 — 20 bytes exactly */
    "save me a spot",     /* 12 */
    "lost my crew lol",   /* 13 */
    "encore incoming",    /* 14 */
    "see you at glow",    /* 15 */
};

char const *ff_demo_text_ref(uint8_t text_ref)
{
    if (text_ref >= FF_DEMOFEED_TEXT_REF_COUNT) return NULL;
    return FF_DEMO_TEXTS[text_ref];
}

/* ---------------------------------------------------------------------
 * Pure mapping.
 * ------------------------------------------------------------------- */

bool ff_demo_apply_plan(ff_demo_event_t const *ev, uint32_t const *node_ids, uint8_t member_count,
                        ff_demo_apply_plan_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->valid = false;
    out->dispatch = FF_DEMO_DISPATCH_NONE;

    if (ev == NULL || node_ids == NULL || member_count == 0) return false;
    if (ev->member_idx >= member_count) return false; /* out-of-range idx — honest gate */

    out->node_id = node_ids[ev->member_idx];

    if (ev->type == FF_DEMO_EVENT_PRESENCE_POKE) {
        out->valid = true;
        out->dispatch = FF_DEMO_DISPATCH_POKE;
        return true;
    }

    /* FF_DEMO_EVENT_SIGNAL: kind -> which callback + payload. */
    out->kind = ev->kind;
    switch (ev->kind) {
    case FEED_TEXT:
        out->dispatch = FF_DEMO_DISPATCH_TEXT;
        out->text = ff_demo_text_ref(ev->text_ref);
        break;
    case FEED_STATUS:
        out->dispatch = FF_DEMO_DISPATCH_PRIVATE;
        out->proto_type = FF_PROTO_TYPE_STATUS;
        out->text = ff_demo_text_ref(ev->text_ref);
        break;
    case FEED_RALLY:
        out->dispatch = FF_DEMO_DISPATCH_PRIVATE;
        out->proto_type = FF_PROTO_TYPE_RALLY;
        out->text = NULL; /* a rally's place name is festpack-sourced (S23 AC5),
                             resolved in ff_demo_apply_event, not from text_ref */
        break;
    case FEED_FLARE:
        out->dispatch = FF_DEMO_DISPATCH_PRIVATE;
        out->proto_type = FF_PROTO_TYPE_FLARE;
        out->text = NULL;
        break;
    default:
        /* Unknown kind — treat as invalid rather than guess a dispatch. */
        out->valid = false;
        out->dispatch = FF_DEMO_DISPATCH_NONE;
        return false;
    }

    out->valid = true;
    return true;
}

/* ---------------------------------------------------------------------
 * Festpack-sourced rally point (festival content — S23 AC5).
 * ------------------------------------------------------------------- */

/* The demo festpack's meetup landmark id. The demo layer legitimately knows
 * its OWN pack's landmark ids (as it knows FF_DEMO_NODE_*); the NAME and
 * POSITION it resolves still come from the pack, not a literal here. */
#define FF_DEMO_RALLY_LANDMARK_ID "firefly-tower"

bool ff_demo_rally_point(fp_pack_t const *pack, ff_latlon_t *out_at, char const **out_name)
{
    if (pack == NULL || out_at == NULL || out_name == NULL) return false;
    if (!pack->origin_known) return false; /* no honest place to point at */

    /* Position: the venue origin (the meetup landmark sits at the origin). */
    *out_at = pack->origin;

    /* Name: the meetup landmark by id, else the first landmark, else the
     * festival name — every candidate is festpack data. */
    char const *name = (pack->n_landmarks > 0) ? pack->landmarks[0].name : pack->name;
    for (uint8_t i = 0; i < pack->n_landmarks; i++) {
        if (strcmp(pack->landmarks[i].id, FF_DEMO_RALLY_LANDMARK_ID) == 0) {
            name = pack->landmarks[i].name;
            break;
        }
    }
    *out_name = name;
    return true;
}

/* ---------------------------------------------------------------------
 * Apply (glue) — encode + drive the mesh-inbound seam.
 * ------------------------------------------------------------------- */

void ff_demo_apply_event(mc_events_t const *ev, ff_demo_event_t const *event, uint32_t const *node_ids,
                         uint8_t member_count, fp_pack_t const *pack)
{
    if (ev == NULL) return;

    ff_demo_apply_plan_t plan;
    if (!ff_demo_apply_plan(event, node_ids, member_count, &plan) || !plan.valid) return;

    switch (plan.dispatch) {
    case FF_DEMO_DISPATCH_TEXT: {
        if (ev->on_text == NULL) return;
        char const *t = (plan.text != NULL) ? plan.text : "";
        ev->on_text(ev->user, plan.node_id, MC_ADDR_BROADCAST, t, strlen(t));
        break;
    }
    case FF_DEMO_DISPATCH_PRIVATE: {
        if (ev->on_private == NULL) return;
        uint8_t buf[FF_PROTO_MAX_PAYLOAD];
        int n = -1;
        switch (plan.proto_type) {
        case FF_PROTO_TYPE_FLARE:
            n = ff_proto_encode_flare(buf, sizeof(buf), FF_DEMO_LIVE_FLARE_DUR_S);
            break;
        case FF_PROTO_TYPE_RALLY: {
            /* Place name AND position from the demo festpack (S23 AC5). No
             * honest place (no pack / unknown origin) => send no rally. */
            ff_latlon_t at;
            char const *name = NULL;
            if (!ff_demo_rally_point(pack, &at, &name)) return;
            n = ff_proto_encode_rally(buf, sizeof(buf), at, (name != NULL) ? name : "");
            break;
        }
        case FF_PROTO_TYPE_STATUS:
            n = ff_proto_encode_status(buf, sizeof(buf), (plan.text != NULL) ? plan.text : "");
            break;
        default:
            return; /* plan never yields another proto type */
        }
        /* Demo-generated crew signals are crew-wide broadcasts (the plan
         * carries no per-member destination — mirrors the TEXT dispatch's
         * MC_ADDR_BROADCAST above; honest + deterministic, issue #123). */
        if (n > 0) ev->on_private(ev->user, plan.node_id, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
        break;
    }
    case FF_DEMO_DISPATCH_POKE: {
        if (ev->on_rx_meta == NULL) return;
        /* A DIRECT RSSI sample: shell_ev_rx_meta feeds it to ff_crew_on_rssi
         * (paired + DIRECT + has_rssi is the trust gate), refreshing the
         * member's rssi_age so its freshest sighting reads recent again. */
        mc_rx_meta_t m;
        memset(&m, 0, sizeof(m));
        m.has_rssi = true;
        m.rssi_dbm = FF_DEMO_LIVE_POKE_RSSI_DBM;
        m.rx_path = MC_RX_PATH_DIRECT;
        ev->on_rx_meta(ev->user, plan.node_id, &m);
        break;
    }
    case FF_DEMO_DISPATCH_NONE:
    default:
        break;
    }
}
