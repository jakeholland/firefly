/**
 * test_demo.c — S20 demo mode: ff_demo_seed produces the expected crew
 * states, feed and wall clock, all computed honestly through core.
 *
 * These assertions read the REAL projection (ff_shell_view) and the real
 * core accessors after seeding — nothing is asserted by hand-poking state.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_crew.h"
#include "ff_demo.h"
#include "ff_shell.h"

#ifndef FF_DEMO_PACK_PATH
#error "FF_DEMO_PACK_PATH must be defined (path to firefly-fields.festpack.json)"
#endif

/* A monotonic clock reading a single counter ff_demo_seed pins. */
static uint32_t s_clock_ms;
static uint32_t clock_now_ms(void *user)
{
    (void)user;
    return s_clock_ms;
}

static char s_json[64 * 1024];
static size_t s_json_len;

static ff_shell_t s_shell;
static fp_pack_t s_pack;
static ff_clock_t s_clock;

void setUp(void) {}
void tearDown(void) {}

static size_t load_pack_bytes(void)
{
    FILE *f = fopen(FF_DEMO_PACK_PATH, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot open demo festpack — FF_DEMO_PACK_PATH wrong?");
    size_t n = fread(s_json, 1, sizeof(s_json), f);
    fclose(f);
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_LESS_THAN_UINT(sizeof(s_json), n); /* fully read, not truncated */
    return n;
}

/* Bring up a fresh no-transport shell and seed it with `primary`. Returns
 * the projected view after one tick. */
static ff_app_state_t const *seed(uint32_t primary)
{
    s_clock.now_ms = clock_now_ms;
    s_clock.user = NULL;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &s_clock;
    cfg.store = NULL;
    cfg.pack = &s_pack;
    /* cfg.transport zeroed => no transport, the documented test seam. */

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&s_shell, &cfg));
    TEST_ASSERT_EQUAL_INT(0, ff_demo_seed(&s_shell, s_json, s_json_len, &s_clock_ms, primary));
    (void)ff_shell_tick(&s_shell, s_clock_ms);
    return ff_shell_view(&s_shell);
}

static ff_crew_member_t const *crew_find(uint32_t node)
{
    return ff_crew_find(ff_shell_crew(&s_shell), node);
}

/* S20_AC2 — the wall clock latches to Sat 2026-09-05 21:30 local. */
void test_S20_wall_latched_to_saturday_2130(void)
{
    (void)seed(0);
    ff_wall_t w = ff_shell_wall(&s_shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);   /* not UNKNOWN */
    TEST_ASSERT_EQUAL_UINT16(248, w.day_doy);     /* doy(2026-09-05) */
    TEST_ASSERT_EQUAL_INT16(21 * 60 + 30, w.now_min); /* 21:30 local */
    TEST_ASSERT_FALSE(w.offset_assumed);          /* pack STATES utc_offset_min */
    ff_shell_close(&s_shell);
}

/* S20_AC1 — the four Radar states, each spotlit as the default selection.
 * mode is read from the real projection, so it flows through
 * ff_crew_freshness / ff_crew_close_range / ff_radar_compute. */
void test_S20_radar_dana_live_arrow(void)
{
    ff_app_state_t const *v = seed(FF_DEMO_NODE_DANA);
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v->radar.mode);
    TEST_ASSERT_TRUE(v->radar.arrow_valid);
    ff_shell_close(&s_shell);
}

void test_S20_radar_riley_close_range(void)
{
    ff_app_state_t const *v = seed(FF_DEMO_NODE_RILEY);
    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v->radar.mode);
    ff_shell_close(&s_shell);
}

void test_S20_radar_maya_stale_last_seen(void)
{
    ff_app_state_t const *v = seed(FF_DEMO_NODE_MAYA);
    /* A real past fix, ~25 min old: LOST band, but a non-empty age_str, so
     * the radar shows "LAST SEEN 25 MIN" rather than "NO FIX YET"
     * (ff_radar.h's renderer contract). */
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v->radar.mode);
    TEST_ASSERT_NOT_EQUAL('\0', v->radar.age_str[0]);
    ff_shell_close(&s_shell);
}

void test_S20_radar_sam_no_fix(void)
{
    ff_app_state_t const *v = seed(FF_DEMO_NODE_SAM);
    /* Paired, never sent a fix: LOST mode but EMPTY age_str => "NO FIX
     * YET", the honest no-fix state. */
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v->radar.mode);
    TEST_ASSERT_EQUAL_CHAR('\0', v->radar.age_str[0]);
    ff_shell_close(&s_shell);
}

/* The underlying freshness/position facts, asserted directly on core. */
void test_S20_crew_freshness_states(void)
{
    (void)seed(0);
    uint32_t const now = s_clock_ms;

    ff_crew_member_t const *dana = crew_find(FF_DEMO_NODE_DANA);
    ff_crew_member_t const *riley = crew_find(FF_DEMO_NODE_RILEY);
    ff_crew_member_t const *maya = crew_find(FF_DEMO_NODE_MAYA);
    ff_crew_member_t const *sam = crew_find(FF_DEMO_NODE_SAM);
    TEST_ASSERT_NOT_NULL(dana);
    TEST_ASSERT_NOT_NULL(riley);
    TEST_ASSERT_NOT_NULL(maya);
    TEST_ASSERT_NOT_NULL(sam);

    TEST_ASSERT_TRUE(dana->paired);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(dana, now));

    /* RILEY is close-range (fresh DIRECT RSSI), and by distance too. */
    TEST_ASSERT_TRUE(ff_crew_close_range(riley, -1.0f, now));

    /* MAYA: a real fix, ~25 min old => LOST, but has_pos is true. */
    TEST_ASSERT_TRUE(maya->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, ff_crew_freshness(maya, now));

    /* SAM: paired, no fix ever => NEVER. */
    TEST_ASSERT_TRUE(sam->paired);
    TEST_ASSERT_FALSE(sam->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(sam, now));

    ff_shell_close(&s_shell);
}

/* The seeded fix ages did NOT drag the wall clock backwards: MAYA's older,
 * disagreeing timestamp was offered at BOOTSTRAP tier (un-paired at the
 * time) and refused. If it had re-latched, now_min would not be 21:30. */
void test_S20_stale_fix_did_not_move_the_clock(void)
{
    (void)seed(0);
    /* MAYA's 25-min-old, disagreeing timestamp was offered at BOOTSTRAP
     * tier and refused a re-latch (the guard that keeps a stale fix from
     * dragging the clock back). That refusal is bench-visible: */
    TEST_ASSERT_EQUAL_UINT32(1, ff_shell_wall_rejected_relatches(&s_shell));
    /* And the clock still reads the seeded instant. */
    ff_wall_t w = ff_shell_wall(&s_shell);
    TEST_ASSERT_EQUAL_INT16(21 * 60 + 30, w.now_min);
    ff_shell_close(&s_shell);
}

/* S20_AC1 — the Signals feed carries the seeded chatter. */
void test_S20_feed_seeded(void)
{
    ff_app_state_t const *v = seed(0);
    ff_feed_t const *feed = ff_shell_feed(&s_shell);
    TEST_ASSERT_EQUAL_UINT8(5, ff_feed_count(feed));   /* status, pulse, rally, omw, message */
    /* S24 — the Signals inbox model projects those into conversation
     * traffic. Sum item_count across conversations (the seeded chatter
     * must actually reach the model the screen renders, not only the raw
     * feed). */
    unsigned items = 0;
    for (uint8_t i = 0; i < v->signals.inbox.conv_count; i++) {
        items += v->signals.inbox.convs[i].item_count;
    }
    TEST_ASSERT_GREATER_THAN_UINT(0, items);
    ff_shell_close(&s_shell);
}

/* S20_AC1 — the Now face reads NOW_LIVE with FIREFLY mid-set and a starred
 * countdown ticking. */
void test_S20_now_live_with_starred_countdown(void)
{
    ff_app_state_t const *v = seed(0);
    TEST_ASSERT_EQUAL_INT(NOW_LIVE, v->now.state);
    TEST_ASSERT_GREATER_THAN_UINT8(0, v->now.n_rows);

    /* FIREFLY is on The Beacon and currently playing. */
    bool firefly_live = false;
    for (uint8_t i = 0; i < v->now.n_rows; i++) {
        if (strcmp(v->now.rows[i].artist, "FIREFLY") == 0) firefly_live = true;
    }
    TEST_ASSERT_TRUE(firefly_live);

    /* A starred set is coming up (TWILIGHT FUNCTION), so the countdown is live. */
    TEST_ASSERT_TRUE(v->now.next.valid);
    TEST_ASSERT_GREATER_THAN_INT(0, v->now.next.mins_until);
    ff_shell_close(&s_shell);
}

/* S20_AC1 — the Map is populated: features (stages+landmarks) + crew dots. */
void test_S20_map_populated(void)
{
    ff_app_state_t const *v = seed(0);
    TEST_ASSERT_GREATER_THAN_UINT8(0, v->map.n_features);
    TEST_ASSERT_EQUAL_UINT8(4, v->map.n_crew); /* DANA/KEV/RILEY/MAYA have positions; SAM doesn't */
    TEST_ASSERT_TRUE(v->map.you_has_pos);
    ff_shell_close(&s_shell);
}

int main(void)
{
    s_json_len = load_pack_bytes();

    UNITY_BEGIN();
    RUN_TEST(test_S20_wall_latched_to_saturday_2130);
    RUN_TEST(test_S20_radar_dana_live_arrow);
    RUN_TEST(test_S20_radar_riley_close_range);
    RUN_TEST(test_S20_radar_maya_stale_last_seen);
    RUN_TEST(test_S20_radar_sam_no_fix);
    RUN_TEST(test_S20_crew_freshness_states);
    RUN_TEST(test_S20_stale_fix_did_not_move_the_clock);
    RUN_TEST(test_S20_feed_seeded);
    RUN_TEST(test_S20_now_live_with_starred_countdown);
    RUN_TEST(test_S20_map_populated);
    return UNITY_END();
}
