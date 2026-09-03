/**
 * ff_demo.c — see ff_demo.h. S20 demo-mode seeding.
 *
 * Everything here drives the shell's real inbound seam so the honesty
 * rules (roster trust, position freshness, the wall-clock plausibility
 * gate, the paired-sender feed filter) all run exactly as they would on a
 * live mesh. No core state is written directly; no freshness is faked.
 */
#include "ff_demo.h"

#include <string.h>

#include "ff_latlon.h"
#include "ff_proto.h"

#include "mc_client.h" /* mc_nodeinfo_t / mc_position_t / mc_rx_meta_t — the inbound structs */

/* A "site beacon" node whose only job is to carry the plausible mesh
 * timestamp that bootstraps the wall-clock latch — the demo stand-in for
 * the first NodeInfo of a want_config handshake. Not a crew member; it is
 * noted in the heard list and never paired, exactly like any unknown node
 * that only ever announced itself. */
#define FF_DEMO_NODE_BEACON 0x00DE30FFu

/* The venue origin / The Firefly Tower — the central meetup landmark and
 * where the wearer ("you") is standing. Matches the festpack's origin and
 * the tower feature (firefly-fields.festpack.json). */
#define FF_DEMO_ORIGIN_LAT 43.7000
#define FF_DEMO_ORIGIN_LON (-121.5000)

/* One seeded crew member's demo facts. `age_s` is how long ago the fix
 * was received (0 = right now => LIVE); `has_pos` false is SAM's honest
 * no-fix. */
typedef struct {
    uint32_t node_id;
    char const *name;
    int8_t battery_pct;
    bool has_pos;
    double lat, lon;
    int32_t age_s;
    bool direct_rssi;   /* seed a DIRECT RSSI sample (=> close-range) */
    int16_t rssi_dbm;
} ff_demo_member_t;

/* The canonical crew, in default pairing order (DANA first => default
 * Radar selection is DANA's live arrow). Positions are within ~150 m of
 * the origin; ages give the four Radar states the spec calls for. */
static const ff_demo_member_t FF_DEMO_CREW[] = {
    /* DANA — up by The Beacon, ~55 m N, LIVE: fresh arrow + distance.
     * Kept clear of the 30 m close-range threshold so the Radar shows the
     * live ARROW (not close-range rings) — RILEY is the close-range one. */
    {FF_DEMO_NODE_DANA, "DANA", 82, true, 43.700494, -121.500010, 0, false, 0},
    /* KEV — at Bass Hollow, ~120 m SE, LIVE. */
    {FF_DEMO_NODE_KEV, "KEV", 64, true, 43.699281, -121.498882, 0, false, 0},
    /* RILEY — by the Ferris Wheel, ~16 m, LIVE and close-range (a strong
     * DIRECT RSSI sample drives ff_crew_close_range). */
    {FF_DEMO_NODE_RILEY, "RILEY", 91, true, 43.700108, -121.500124, 0, true, -45},
    /* MAYA — Camp Glow, fix received 25 min ago => "LAST SEEN 25 MIN". */
    {FF_DEMO_NODE_MAYA, "MAYA", 47, true, 43.700808, -121.501118, 25 * 60, false, 0},
    /* SAM — paired but NO position fix (honest unknown / no-fix state). */
    {FF_DEMO_NODE_SAM, "SAM", 73, false, 0.0, 0.0, 0, false, 0},
};
#define FF_DEMO_CREW_N ((int)(sizeof(FF_DEMO_CREW) / sizeof(FF_DEMO_CREW[0])))

/* Build a name/battery-only NodeInfo for a roster member (no position, so
 * it never touches the reconnect/replay position path — the fix arrives
 * separately via on_position with an honest rx_time). */
static void demo_nodeinfo(mc_nodeinfo_t *n, uint32_t node_id, char const *name, int8_t battery_pct)
{
    memset(n, 0, sizeof(*n));
    n->node_num = node_id;
    n->has_short_name = true;
    /* Bounded copy (not snprintf) so gcc-14's -Wformat-truncation cannot
     * fire on a name it can't prove fits — CLAUDE.md's compiler note. */
    size_t ln = strlen(name);
    if (ln >= sizeof(n->short_name)) ln = sizeof(n->short_name) - 1u;
    memcpy(n->short_name, name, ln);
    n->short_name[ln] = '\0';
    if (battery_pct >= 0) {
        n->has_battery_level = true;
        n->battery_level = (uint32_t)battery_pct;
    }
    n->last_heard = (uint32_t)FF_DEMO_WALL_UNIX_S; /* agrees with the latch => no re-latch */
    n->rx_path = MC_RX_PATH_UNKNOWN;
}

/* Offer a live-packet position for `node_id` received `age_s` ago. The
 * shell ages it from rx_time against the latched wall clock (honest). */
static void demo_position(mc_events_t const *ev, uint32_t node_id, double lat, double lon, int32_t age_s)
{
    mc_position_t p;
    memset(&p, 0, sizeof(p));
    p.lat = lat;
    p.lon = lon;
    p.has_rx_time = true;
    p.rx_time = (uint32_t)(FF_DEMO_WALL_UNIX_S - (int64_t)age_s);
    p.loc_source = MC_LOC_INTERNAL; /* a real GPS measurement */
    p.has_precision_bits = false;   /* sender didn't state precision => rendered as an ordinary point */
    ev->on_position(ev->user, node_id, &p);
}

/* Push one firefly-protocol private packet from `from` into the feed.
 * Demo crew signals are crew-wide by construction, so their `to` is the
 * broadcast address — the same addressing the seed's on_text calls carry
 * (honest and deterministic, issue #123: never a fabricated 1:1). */
static void demo_private(mc_events_t const *ev, uint32_t from, uint8_t const *buf, int n)
{
    if (n > 0) ev->on_private(ev->user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
}

static void demo_seed_feed(mc_events_t const *ev)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];

    /* KEV drops two statuses in a row (2026-09-02: the second used to be
     * a PULSE, now retired — see ff_feed.h / docs/specs/S04's Amendments;
     * a second STATUS keeps this seed's item count and "one sender, two
     * signals" shape unchanged rather than silently thinning the feed). */
    demo_private(ev, FF_DEMO_NODE_KEV, buf, ff_proto_encode_status(buf, sizeof(buf), "at bass hollow!"));
    demo_private(ev, FF_DEMO_NODE_KEV, buf, ff_proto_encode_status(buf, sizeof(buf), "who's got water?"));

    /* A rally point dropped at The Firefly Tower (by DANA). */
    ff_latlon_t const tower = {FF_DEMO_ORIGIN_LAT, FF_DEMO_ORIGIN_LON};
    demo_private(ev, FF_DEMO_NODE_DANA, buf, ff_proto_encode_rally(buf, sizeof(buf), tower, "The Firefly Tower"));

    /* MAYA's canned reply, then a broadcast message (newest — shown first). */
    ev->on_text(ev->user, FF_DEMO_NODE_MAYA, MC_ADDR_BROADCAST, "omw", 3);
    char const *msg = "lineup is stacked tonight";
    ev->on_text(ev->user, FF_DEMO_NODE_DANA, MC_ADDR_BROADCAST, msg, strlen(msg));
}

int ff_demo_seed(ff_shell_t *sh, char const *festpack_json, size_t festpack_len, uint32_t *clock_ms,
                 uint32_t primary_node)
{
    if (sh == NULL || clock_ms == NULL || festpack_json == NULL || festpack_len == 0) return -1;

    /* 1. Pin the injected monotonic clock at the demo instant. */
    *clock_ms = FF_DEMO_NOW_MS;

    /* 2. Parse the festpack (also tightens the wall plausibility window to
     * the festival's dates). */
    if (ff_shell_load_pack(sh, festpack_json, festpack_len) != 0) return -1;

    mc_events_t const ev = ff_shell_events(sh);
    if (ev.on_node == NULL || ev.on_position == NULL || ev.on_rx_meta == NULL || ev.on_private == NULL ||
        ev.on_text == NULL) {
        return -1;
    }

    /* 3. Latch the wall clock from a plausible mesh timestamp (bootstrap,
     * exactly as the first want_config NodeInfo would). */
    {
        mc_nodeinfo_t beacon;
        memset(&beacon, 0, sizeof(beacon));
        beacon.node_num = FF_DEMO_NODE_BEACON;
        beacon.last_heard = (uint32_t)FF_DEMO_WALL_UNIX_S;
        beacon.rx_path = MC_RX_PATH_UNKNOWN;
        ev.on_node(ev.user, &beacon);
    }
    if (ff_shell_wall(sh).src == FF_WALL_UNKNOWN) return -1; /* the latch must have taken */

    /* Build the pairing order: `primary_node` first (=> default Radar
     * selection), then the rest in canonical order. */
    int order[FF_DEMO_CREW_N];
    int n_order = 0;
    for (int i = 0; i < FF_DEMO_CREW_N; i++) {
        if (FF_DEMO_CREW[i].node_id == primary_node) order[n_order++] = i;
    }
    for (int i = 0; i < FF_DEMO_CREW_N; i++) {
        if (FF_DEMO_CREW[i].node_id != primary_node) order[n_order++] = i;
    }

    /* 4a. Add each member to the roster UNPAIRED, name it, and record its
     * fix (if any). Seeding while un-paired keeps MAYA's older, disagreeing
     * timestamp at BOOTSTRAP tier, which ff_wall_observe refuses to re-latch
     * from — so a stale fix cannot drag the wall clock backwards. */
    for (int k = 0; k < n_order; k++) {
        ff_demo_member_t const *mem = &FF_DEMO_CREW[order[k]];
        (void)ff_shell_pair(sh, mem->node_id, false); /* upsert into the roster, un-paired for now */

        mc_nodeinfo_t n;
        demo_nodeinfo(&n, mem->node_id, mem->name, mem->battery_pct);
        ev.on_node(ev.user, &n);

        if (mem->has_pos) {
            demo_position(&ev, mem->node_id, mem->lat, mem->lon, mem->age_s);
        }
    }

    /* 4b. Now pair everyone (the explicit user-pairing action). */
    for (int k = 0; k < n_order; k++) {
        (void)ff_shell_pair(sh, FF_DEMO_CREW[order[k]].node_id, true);
    }

    /* 4c. RILEY's close-range: a fresh DIRECT RSSI sample (paired + DIRECT
     * is the trust gate ff_crew_close_range's RSSI leg needs). */
    for (int k = 0; k < n_order; k++) {
        ff_demo_member_t const *mem = &FF_DEMO_CREW[order[k]];
        if (!mem->direct_rssi) continue;
        mc_rx_meta_t m;
        memset(&m, 0, sizeof(m));
        m.has_rssi = true;
        m.rssi_dbm = mem->rssi_dbm;
        m.rx_path = MC_RX_PATH_DIRECT;
        ev.on_rx_meta(ev.user, mem->node_id, &m);
    }

    /* 5. Where "you" are (The Firefly Tower) + a heading, so Radar points
     * and the Map places YOU. */
    ff_latlon_t const me = {FF_DEMO_ORIGIN_LAT, FF_DEMO_ORIGIN_LON};
    ff_shell_set_my_pos(sh, me);
    ff_shell_set_heading(sh, 0.0f); /* facing north */

    /* 6. The Signals feed. fix/audio-init-order-seed-silence: this is the
     * one seeding step that can reach `shell_sound` at all (a rally +
     * two messages, pushed through the real ev.on_private/ev.on_text
     * path — steps 1-5 above never sound: pairing, NodeInfo, position,
     * and RSSI pushes don't). Bracketed so this whole seeded feed lands
     * SILENTLY — it is boot-time history, not a live arrival (S27-sounds.
     * md Amendments, "seeded/replayed history"). Unmuted again
     * immediately after, so the S23 live demo feed (ff_demo_apply_event,
     * ff_demoapply.c — arrives later, over real time, via app_main's own
     * tick loop) sounds normally: that generator's whole point is
     * events arriving, which SHOULD chime. */
    ff_shell_set_sound_muted_for_seed(sh, true);
    demo_seed_feed(&ev);
    ff_shell_set_sound_muted_for_seed(sh, false);

    return 0;
}
