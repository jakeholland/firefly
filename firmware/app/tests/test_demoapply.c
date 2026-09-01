/**
 * test_demoapply.c — S23 slice (c): the pure apply-mapping helper, plus an
 * integration pass that drives ff_demo_apply_event into a real seeded shell
 * and asserts a synthetic signal lands in the feed and a poke refreshes
 * presence — read from the real projection/accessors, never hand-poked.
 *
 * Spec: docs/specs/S23-demo-feed.md (AC2 indistinguishable-from-mesh,
 * AC3 presence transitions, AC5 content from the demo table).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_crew.h"
#include "ff_demo.h"
#include "ff_demoapply.h"
#include "ff_demofeed.h"
#include "ff_feed.h"
#include "ff_shell.h"
#include "ff_sigview.h"

#ifndef FF_DEMO_PACK_PATH
#error "FF_DEMO_PACK_PATH must be defined (path to firefly-fields.festpack.json)"
#endif

void setUp(void) {}
void tearDown(void) {}

/* =====================================================================
 * PURE helper — ff_demo_apply_plan (no shell/clock/radio).
 * ===================================================================== */

static ff_demo_event_t mk_signal(uint8_t idx, ff_feed_kind_t kind, uint8_t text_ref)
{
    ff_demo_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = FF_DEMO_EVENT_SIGNAL;
    e.member_idx = idx;
    e.kind = kind;
    e.text_ref = text_ref;
    return e;
}

static ff_demo_event_t mk_poke(uint8_t idx)
{
    ff_demo_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = FF_DEMO_EVENT_PRESENCE_POKE;
    e.member_idx = idx;
    return e;
}

/* S23c_AC_map_idx_to_node — member_idx resolves to the app's node id at
 * that index; the canonical map is the one the device apply loop uses. */
void test_S23c_map_idx_to_node(void)
{
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);
    TEST_ASSERT_NOT_NULL(ids);
    TEST_ASSERT_EQUAL_UINT8(FF_DEMO_LIVE_MEMBER_COUNT, n);

    ff_demo_apply_plan_t p;
    for (uint8_t i = 0; i < n; i++) {
        ff_demo_event_t e = mk_poke(i);
        TEST_ASSERT_TRUE(ff_demo_apply_plan(&e, ids, n, &p));
        TEST_ASSERT_TRUE(p.valid);
        TEST_ASSERT_EQUAL_UINT32(ids[i], p.node_id);
    }
    /* Index 0 is DANA, matching ff_demo.c's canonical crew order. */
    ff_demo_event_t e0 = mk_poke(0);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&e0, ids, n, &p));
    TEST_ASSERT_EQUAL_UINT32(FF_DEMO_NODE_DANA, p.node_id);
}

/* S23c_AC_idx_out_of_range — an idx >= member_count is the honest gate:
 * invalid, dispatch NONE, no node id trusted. */
void test_S23c_idx_out_of_range_invalid(void)
{
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);
    ff_demo_apply_plan_t p;

    ff_demo_event_t over = mk_signal(n, FEED_TEXT, 0); /* == member_count, out of range */
    TEST_ASSERT_FALSE(ff_demo_apply_plan(&over, ids, n, &p));
    TEST_ASSERT_FALSE(p.valid);
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_NONE, p.dispatch);

    ff_demo_event_t way = mk_poke(200);
    TEST_ASSERT_FALSE(ff_demo_apply_plan(&way, ids, n, &p));
    TEST_ASSERT_FALSE(p.valid);
}

/* S23c_AC_kind_dispatch — each SIGNAL feed kind maps to the right inbound
 * seam + proto type; TEXT/STATUS/RALLY carry a resolved string, PULSE/FLARE
 * carry none. */
void test_S23c_kind_dispatch(void)
{
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);
    ff_demo_apply_plan_t p;

    ff_demo_event_t t = mk_signal(0, FEED_TEXT, 3);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&t, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_TEXT, p.dispatch);
    TEST_ASSERT_NOT_NULL(p.text);
    TEST_ASSERT_EQUAL_STRING(ff_demo_text_ref(3), p.text);

    ff_demo_event_t s = mk_signal(1, FEED_STATUS, 5);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&s, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_PRIVATE, p.dispatch);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_STATUS, p.proto_type);
    TEST_ASSERT_EQUAL_STRING(ff_demo_text_ref(5), p.text);

    ff_demo_event_t r = mk_signal(2, FEED_RALLY, 4);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&r, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_PRIVATE, p.dispatch);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, p.proto_type);
    TEST_ASSERT_NULL(p.text); /* a rally's place name is festpack-sourced (S23 AC5), not text_ref */

    ff_demo_event_t pl = mk_signal(3, FEED_PULSE, 0);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&pl, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_PRIVATE, p.dispatch);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_PULSE, p.proto_type);
    TEST_ASSERT_NULL(p.text);

    ff_demo_event_t fl = mk_signal(4, FEED_FLARE, 0);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&fl, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_PRIVATE, p.dispatch);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, p.proto_type);
    TEST_ASSERT_NULL(p.text);

    ff_demo_event_t pk = mk_poke(0);
    TEST_ASSERT_TRUE(ff_demo_apply_plan(&pk, ids, n, &p));
    TEST_ASSERT_EQUAL_INT(FF_DEMO_DISPATCH_POKE, p.dispatch);
}

/* S23c_AC_text_ref_bounds — text_ref within the table resolves to a
 * non-NULL, non-empty string; an out-of-range ref resolves to NULL. */
void test_S23c_text_ref_bounds(void)
{
    for (uint8_t i = 0; i < FF_DEMOFEED_TEXT_REF_COUNT; i++) {
        char const *s = ff_demo_text_ref(i);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_TRUE(strlen(s) > 0);
        TEST_ASSERT_TRUE(strlen(s) <= 20); /* fits FF_PROTO_STATUS_MAX for every kind */
    }
    TEST_ASSERT_NULL(ff_demo_text_ref(FF_DEMOFEED_TEXT_REF_COUNT));
    TEST_ASSERT_NULL(ff_demo_text_ref(255));
}

/* S23c_AC_null_args — NULL args are safe no-ops returning false. */
void test_S23c_null_args(void)
{
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);
    ff_demo_apply_plan_t p;
    ff_demo_event_t e = mk_poke(0);

    TEST_ASSERT_FALSE(ff_demo_apply_plan(NULL, ids, n, &p));
    TEST_ASSERT_FALSE(ff_demo_apply_plan(&e, NULL, n, &p));
    TEST_ASSERT_FALSE(ff_demo_apply_plan(&e, ids, 0, &p));
    TEST_ASSERT_FALSE(ff_demo_apply_plan(&e, ids, n, NULL));
}

/* =====================================================================
 * INTEGRATION — drive ff_demo_apply_event into a real seeded shell.
 * ===================================================================== */

static char s_json[64 * 1024];
static size_t s_json_len;
static uint32_t s_clock_ms;
static ff_shell_t s_shell;
static fp_pack_t s_pack;
static ff_clock_t s_clock;

static uint32_t clock_now_ms(void *user)
{
    (void)user;
    return s_clock_ms;
}

static void load_pack(void)
{
    FILE *f = fopen(FF_DEMO_PACK_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open demo festpack — FF_DEMO_PACK_PATH wrong?");
    s_json_len = fread(s_json, 1, sizeof(s_json), f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN_UINT(0, s_json_len);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(s_json), s_json_len);
}

static void seed_shell(void)
{
    load_pack();
    s_clock.now_ms = clock_now_ms;
    s_clock.user = NULL;
    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = NULL;
    cfg.pack = &s_pack;
    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&s_shell, &cfg));
    TEST_ASSERT_EQUAL_INT(0, ff_demo_seed(&s_shell, s_json, s_json_len, &s_clock_ms, 0));
    (void)ff_shell_tick(&s_shell, s_clock_ms);
}

/* S23c_AC2_signal_lands_in_feed — a synthetic TEXT applied via
 * ff_demo_apply_event lands in ff_feed exactly like a mesh text: newest
 * item, from the mapped node, unread=true, carrying the demo string. */
void test_S23c_signal_lands_in_feed(void)
{
    seed_shell();
    ff_feed_t const *feed = ff_shell_feed(&s_shell);
    uint8_t const before = ff_feed_count(feed);

    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);

    ff_demo_event_t t = mk_signal(0 /* DANA, paired */, FEED_TEXT, 9);
    ff_demo_apply_event(&ev, &t, ids, n, &s_pack);

    feed = ff_shell_feed(&s_shell);
    TEST_ASSERT_EQUAL_UINT8(before + 1, ff_feed_count(feed));
    ff_feed_item_t const *it = ff_feed_at(feed, 0); /* newest */
    TEST_ASSERT_NOT_NULL(it);
    TEST_ASSERT_EQUAL_INT(FEED_TEXT, it->kind);
    TEST_ASSERT_EQUAL_UINT32(FF_DEMO_NODE_DANA, it->from_node);
    TEST_ASSERT_TRUE(it->unread);
    TEST_ASSERT_EQUAL_STRING(ff_demo_text_ref(9), it->text);
    ff_shell_close(&s_shell);
}

/* S23c_AC2_private_signal_lands — a STATUS applied via on_private decodes
 * and lands as a FEED_STATUS from the mapped node. */
void test_S23c_private_signal_lands(void)
{
    seed_shell();
    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);

    ff_demo_event_t s = mk_signal(1 /* KEV */, FEED_STATUS, 3);
    ff_demo_apply_event(&ev, &s, ids, n, &s_pack);

    ff_feed_t const *feed = ff_shell_feed(&s_shell);
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_NOT_NULL(it);
    TEST_ASSERT_EQUAL_INT(FEED_STATUS, it->kind);
    TEST_ASSERT_EQUAL_UINT32(FF_DEMO_NODE_KEV, it->from_node);
    TEST_ASSERT_EQUAL_STRING(ff_demo_text_ref(3), it->text);
    /* Issue #123 — demo private dispatch passes honest crew-wide
     * addressing, so the item classifies BROADCAST (CREW thread), never a
     * fabricated 1:1 and never a stale UNKNOWN. */
    TEST_ASSERT_EQUAL_INT(FEED_DIR_BROADCAST, it->dir);
    ff_shell_close(&s_shell);
}

/* S23d_AC5_rally_point_from_festpack — the meetup rally point is SOURCED from
 * the demo festpack: position = venue origin, name = the "firefly-tower"
 * landmark. Never a literal. NULL/unknown-origin packs resolve to false so no
 * rally is ever pointed at a fabricated place. */
void test_S23d_rally_point_from_festpack(void)
{
    load_pack();
    static fp_pack_t pack;
    TEST_ASSERT_EQUAL_INT(FP_OK, fp_parse(s_json, s_json_len, &pack));
    TEST_ASSERT_TRUE(pack.origin_known);

    ff_latlon_t at = {0};
    char const *name = NULL;
    TEST_ASSERT_TRUE(ff_demo_rally_point(&pack, &at, &name));
    /* Position is the festpack's venue origin (not a hardcoded literal). */
    TEST_ASSERT_EQUAL_DOUBLE(pack.origin.lat, at.lat);
    TEST_ASSERT_EQUAL_DOUBLE(pack.origin.lon, at.lon);
    /* Name is the festpack's meetup landmark. */
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("The Firefly Tower", name);

    /* NULL pack / NULL outs => false, no fabricated place. */
    TEST_ASSERT_FALSE(ff_demo_rally_point(NULL, &at, &name));
    TEST_ASSERT_FALSE(ff_demo_rally_point(&pack, NULL, &name));
    TEST_ASSERT_FALSE(ff_demo_rally_point(&pack, &at, NULL));

    /* An unknown-origin pack (venue lat/lon absent) => false. */
    static fp_pack_t no_origin;
    memset(&no_origin, 0, sizeof(no_origin));
    no_origin.origin_known = false;
    TEST_ASSERT_FALSE(ff_demo_rally_point(&no_origin, &at, &name));
}

/* S23d_AC5_rally_signal_festpack_sourced — a RALLY applied via
 * ff_demo_apply_event lands as FEED_RALLY from the mapped node carrying the
 * festpack landmark NAME (not a demo chatter string), proving festival
 * content is festpack-sourced end-to-end through the real feed path. */
void test_S23d_rally_signal_festpack_sourced(void)
{
    seed_shell();
    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);

    /* The name the feed item should carry, resolved from the same pack. */
    ff_latlon_t at = {0};
    char const *expect_name = NULL;
    TEST_ASSERT_TRUE(ff_demo_rally_point(&s_pack, &at, &expect_name));

    ff_demo_event_t r = mk_signal(0 /* DANA, paired */, FEED_RALLY, 4);
    ff_demo_apply_event(&ev, &r, ids, n, &s_pack);

    ff_feed_t const *feed = ff_shell_feed(&s_shell);
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_NOT_NULL(it);
    TEST_ASSERT_EQUAL_INT(FEED_RALLY, it->kind);
    TEST_ASSERT_EQUAL_UINT32(FF_DEMO_NODE_DANA, it->from_node);
    TEST_ASSERT_EQUAL_STRING(expect_name, it->text); /* festpack landmark name */
    ff_shell_close(&s_shell);
}

/* S23d_rally_no_pack_sends_nothing — with no pack (nothing to source a place
 * from), a RALLY is simply not sent: no feed item, no fabricated location. */
void test_S23d_rally_no_pack_sends_nothing(void)
{
    seed_shell();
    ff_feed_t const *feed = ff_shell_feed(&s_shell);
    uint8_t const before = ff_feed_count(feed);

    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);

    ff_demo_event_t r = mk_signal(0, FEED_RALLY, 4);
    ff_demo_apply_event(&ev, &r, ids, n, NULL); /* no pack */

    feed = ff_shell_feed(&s_shell);
    TEST_ASSERT_EQUAL_UINT8(before, ff_feed_count(feed)); /* unchanged */
    ff_shell_close(&s_shell);
}

/* S23c_AC3_poke_refreshes_presence — after aging SAM (paired, no fix, no
 * prior RSSI) to LOST/LINKED, a PRESENCE_POKE applied at a later demo time
 * makes his freshest sighting recent again (SEEN). Read through the real
 * ff_sigview_presence, computed from real crew state. */
void test_S23c_poke_refreshes_presence(void)
{
    seed_shell();
    mc_events_t ev = ff_shell_events(&s_shell);
    uint8_t n = 0;
    uint32_t const *ids = ff_demo_live_node_ids(&n);

    /* SAM has no position fix and (before any poke) no direct RSSI: no
     * evidence at all => LINKED. */
    ff_crew_t const *crew = ff_shell_crew(&s_shell);
    ff_crew_member_t const *sam = ff_crew_find(crew, FF_DEMO_NODE_SAM);
    TEST_ASSERT_NOT_NULL(sam);
    TEST_ASSERT_TRUE(sam->paired);

    uint32_t const now0 = s_clock_ms;
    ff_freshness_t f0 = ff_crew_freshness(sam, now0);
    bool have_rssi0 = (sam->rssi_dbm != INT16_MIN);
    ff_sigview_presence_t pr0 =
        ff_sigview_presence(f0, now0 - sam->pos_age_ms, have_rssi0, now0 - sam->rssi_age_ms, NULL);
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_LINKED, pr0);

    /* Advance the demo clock well past LOST, then poke SAM. The poke
     * timestamps rssi_age via ff_crew_on_rssi (shell's clock reads
     * s_clock_ms), so his freshest sighting is now ~0ms old => SEEN. */
    s_clock_ms = now0 + FF_CREW_LOST_MS + 60u * 1000u;
    ff_demo_event_t pk = mk_poke(4 /* SAM */);
    ff_demo_apply_event(&ev, &pk, ids, n, &s_pack);

    crew = ff_shell_crew(&s_shell);
    sam = ff_crew_find(crew, FF_DEMO_NODE_SAM);
    uint32_t const now1 = s_clock_ms;
    ff_freshness_t f1 = ff_crew_freshness(sam, now1);
    bool have_rssi1 = (sam->rssi_dbm != INT16_MIN);
    TEST_ASSERT_TRUE(have_rssi1); /* the poke gave him a direct RSSI sample */
    uint32_t age_out = UINT32_MAX;
    ff_sigview_presence_t pr1 =
        ff_sigview_presence(f1, now1 - sam->pos_age_ms, have_rssi1, now1 - sam->rssi_age_ms, &age_out);
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_SEEN, pr1);
    TEST_ASSERT_TRUE(age_out <= 1000u); /* freshest sighting is the just-applied poke */
    ff_shell_close(&s_shell);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_S23c_map_idx_to_node);
    RUN_TEST(test_S23c_idx_out_of_range_invalid);
    RUN_TEST(test_S23c_kind_dispatch);
    RUN_TEST(test_S23c_text_ref_bounds);
    RUN_TEST(test_S23d_rally_point_from_festpack);
    RUN_TEST(test_S23c_null_args);
    RUN_TEST(test_S23c_signal_lands_in_feed);
    RUN_TEST(test_S23c_private_signal_lands);
    RUN_TEST(test_S23d_rally_signal_festpack_sourced);
    RUN_TEST(test_S23d_rally_no_pack_sends_nothing);
    RUN_TEST(test_S23c_poke_refreshes_presence);
    return UNITY_END();
}
