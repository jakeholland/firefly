/**
 * test_radar.c — S06 core/radar acceptance criteria (slice a).
 *
 * Test names follow docs/specs/S06-radar-face.md's numbered acceptance
 * criteria: S06_ACn_description. Covers AC1 (mode truth table), AC2
 * (smoothing step-response), AC3 (crew ring dots), and AC6
 * (allocation-free / fast) — AC4/AC5 are golden-PNG/input-injection render
 * criteria, out of scope for this compute-only slice (see the PR body).
 *
 * AC6's "allocation-free" half is a property enforced by construction, the
 * same way test_crew.c's AC8 is: ff_radar.c includes no <stdlib.h> and
 * calls no malloc/free/realloc anywhere (grep-able), and every struct here
 * (ff_radar_view_t, ff_radar_smooth_t) is fixed-size fields only, safe on
 * the stack. The runtime half (<1ms) is measured directly below.
 */
#include <stdlib.h> /* strtof — test-only, see this file's own top comment about ff_radar.c itself */
#include <string.h>
#include <time.h>

#include "unity.h"

#include "ff_radar.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* fixtures                                                              */
/* ------------------------------------------------------------------- */

/* One paired crew member ("DANA", node 1001), freshly upserted (no
 * position yet — has_pos is false until the caller sets one, matching
 * ff_crew_upsert's documented zeroed-slot contract). `c` is fully
 * initialized (ff_crew_init) and ready to use. */
static ff_crew_member_t *setup_selected_member(ff_crew_t *c)
{
    ff_crew_init(c, NULL); /* no clock needed: these tests never call ff_crew_on_rssi */
    ff_crew_member_t *m = ff_crew_upsert(c, 1001u);
    strncpy(m->name, "DANA", sizeof(m->name) - 1);
    m->initial = 'D';
    m->color_idx = 0;
    ff_crew_set_paired(c, 1001u, true);
    return m;
}

/* ------------------------------------------------------------------- */
/* AC1 — mode truth table (10 rows)                                     */
/* ------------------------------------------------------------------- */

static void S06_AC1_mode_nosel_no_paired_member(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL); /* nobody paired */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 1000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOSEL, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_EQUAL_STRING("", v.name);
    TEST_ASSERT_EQUAL_STRING("", v.dist_str);
    TEST_ASSERT_EQUAL_STRING("", v.age_str);
    TEST_ASSERT_EQUAL_INT(0, v.trend);
}

static void S06_AC1_mode_nofix_pos_invalid(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 1000u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

/* 2026-09-05 amendment: my_pos_ok true + heading invalid + the selected
 * member HAS a position used to resolve RADAR_NOFIX (the old, folded-in
 * behavior this test originally pinned) — it now resolves RADAR_NOHDG,
 * since distance/bearing to the member ARE honestly knowable even
 * without a heading (see ff_radar.h's mode-resolution doc comment). This
 * test (renamed from S06_AC1_mode_nofix_heading_invalid) now pins the
 * NEW behavior; S06_AC1_mode_nofix_heading_invalid_member_has_no_pos
 * immediately below covers the one case that DOES still stay NOFIX with
 * an invalid heading. */
static void S06_AC1_mode_nohdg_heading_invalid_member_has_pos(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 1000u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f /* invalid heading sentinel */, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_EQUAL_UINT8(0, v.n_dots); /* ring dots are heading-relative; never north-up fallback */
    TEST_ASSERT_TRUE(v.bearing_valid);    /* geometry alone (two known lat/lons) needs no heading */
}

/* The one case that DOES still resolve RADAR_NOFIX with an invalid
 * heading: the member has no position fix at all, so there is nothing
 * geometric whatsoever to show (no distance, no bearing) — RADAR_NOHDG
 * would have nothing honest to say either. */
static void S06_AC1_mode_nofix_heading_invalid_member_has_no_pos(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* has_pos stays false — never upserted a fix */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_FALSE(v.bearing_valid);
}

static void S06_AC1_mode_nofix_both_pos_and_heading_invalid(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 1000u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, false, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

/* NOFIX still tells the truth about the selected member's last known fix
 * age (that fact doesn't depend on *my* position/heading), but honestly
 * withholds distance (which does). */
static void S06_AC1_nofix_age_str_known_but_dist_str_unknown(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 8000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    /* age is KNOWN (non-empty) while dist is unknown — an 8s age reads the
     * steady sub-minute "now" (ff_fmt_age), not a per-second "8 SEC". */
    TEST_ASSERT_EQUAL_STRING("now", v.age_str);
    TEST_ASSERT_EQUAL_STRING("", v.dist_str);
}

static void S06_AC1_mode_close_by_distance(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.0001, 0.0}; /* ~11.1 m north: inside 30 m */
    m->pos_age_ms = 0u;                  /* fresh: would be LIVE if not CLOSE */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

static void S06_AC1_mode_close_by_rssi(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0}; /* ~1112 m: outside 30 m, distance leg false */
    m->pos_age_ms = 0u;
    m->rssi_dbm = -50; /* > -60 dBm threshold */
    m->rssi_age_ms = 5000u - 500u; /* age 500ms at now_ms=5000, inside 10s */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 5000u);

    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

static void S06_AC1_mode_live(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0}; /* far enough not to be CLOSE by distance */
    m->pos_age_ms = 0u;                /* age 0 at now_ms=0: LIVE */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v.mode);
    TEST_ASSERT_TRUE(v.arrow_valid);
    TEST_ASSERT_EQUAL_STRING("DANA", v.name);
}

static void S06_AC1_mode_stale(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u; /* age at now_ms=50000 is 50000ms: STALE (45s-600s) */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 50000u);

    TEST_ASSERT_EQUAL_INT(RADAR_STALE, v.mode);
    TEST_ASSERT_TRUE(v.arrow_valid);
}

static void S06_AC1_mode_lost(void)
{
    /* 2026-09-07 [api] presence-heard-vs-position: FF_CREW_LOST_MS
     * widened 10min -> 20min (ff_crew.h) — now_ms bumped from 700000 to
     * comfortably past the new 1200000ms threshold. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u; /* age at now_ms=1300000 is 1300000ms: LOST (>1200000ms) */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 1300000u);

    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v.mode);
    TEST_ASSERT_TRUE(v.arrow_valid); /* has_pos true: a real (old) bearing exists */
}

/* FF_FRESH_NEVER (paired, but never sent a position at all) folds into
 * RADAR_LOST (see ff_radar.h's doc comment) but MUST NOT claim a bearing
 * that doesn't exist — arrow_valid stays false, dist_str/age_str stay "". */
static void S06_AC1_mode_lost_via_never_had_a_fix(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* has_pos left false: never fixed */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 123456u);

    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_EQUAL_STRING("", v.dist_str);
    TEST_ASSERT_EQUAL_STRING("", v.age_str);
}

/* ---------------------------------------------------------------------
 * S29 (docs/specs/S29-radio-only.md) — RADAR_SIGNAL mode resolution.
 * Mirrors the has_heard-true / has_heard-false arm of each of the three
 * branch points ff_radar.h's S29 amendment names.
 * ------------------------------------------------------------------- */

static void S29_nofix_falls_back_to_signal_when_heard(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    ff_crew_on_heard(&c, 1001u, 1500u, true); /* heard, direct */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_SIGNAL, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid); /* my own position unknown: no ghost possible */
    TEST_ASSERT_TRUE(v.signal_heard);
    (void)m;
}

static void S29_nofix_stays_nofix_when_never_heard(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* never heard */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.signal_heard);
}

static void S29_nohdg_no_member_position_falls_back_to_signal_when_heard(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* has_pos stays false */
    ff_crew_on_heard(&c, 1001u, 1500u, false); /* heard via relay */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f /* heading invalid */, my_pos, /*my_pos_ok=*/true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_SIGNAL, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_TRUE(v.signal_heard);
    TEST_ASSERT_TRUE(v.signal_via_relay);
}

static void S29_nohdg_no_member_position_stays_nofix_when_never_heard(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* has_pos stays false, never heard */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, /*my_pos_ok=*/true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.signal_heard);
}

static void S29_nohdg_with_member_position_is_unaffected_by_heard(void)
{
    /* NOHDG must still win when the member HAS a position, regardless of
     * has_heard — RADAR_SIGNAL never preempts a mode with real geometry. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 1000u;
    ff_crew_on_heard(&c, 1001u, 1500u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
}

static void S29_lost_falls_back_to_signal_when_heard(void)
{
    /* 2026-09-07 [api] presence-heard-vs-position: FF_CREW_LOST_MS
     * widened 10min -> 20min (ff_crew.h) — now_ms/heard-time bumped from
     * 700000/690000 to comfortably past the new 1200000ms threshold,
     * same convention as S06_AC1_mode_lost's own bump above. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u; /* age at now_ms=1300000 is 1300000ms: LOST (>1200000ms) */
    ff_crew_on_heard(&c, 1001u, 1290000u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 1300000u);

    TEST_ASSERT_EQUAL_INT(RADAR_SIGNAL, v.mode);
    /* Ghost path: a REAL fix exists (has_pos true), so the arrow is
     * still honestly valid, and dist_str/age_str are the "last known"
     * facts computed unconditionally earlier in ff_radar_compute. */
    TEST_ASSERT_TRUE(v.arrow_valid);
    TEST_ASSERT_TRUE(v.dist_str[0] != '\0');
    TEST_ASSERT_TRUE(v.age_str[0] != '\0');
    TEST_ASSERT_TRUE(v.signal_heard);
}

static void S29_lost_stays_lost_when_never_heard(void)
{
    /* Same 1300000ms bump as the sibling test above. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 1300000u);

    TEST_ASSERT_EQUAL_INT(RADAR_LOST, v.mode);
    TEST_ASSERT_FALSE(v.signal_heard);
}

/* Never-fixed (FF_FRESH_NEVER) member, but heard on the radio: RADAR_SIGNAL,
 * with NO ghost — arrow_valid stays false and dist_str/age_str stay ""
 * (no fabricated ghost for a member with no real fix on file at all). */
static void S29_never_fixed_but_heard_signal_has_no_ghost(void)
{
    ff_crew_t c;
    setup_selected_member(&c); /* has_pos left false: never fixed */
    ff_crew_on_heard(&c, 1001u, 122000u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 123456u);

    TEST_ASSERT_EQUAL_INT(RADAR_SIGNAL, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid); /* no real fix to point at */
    TEST_ASSERT_EQUAL_STRING("", v.dist_str);
    TEST_ASSERT_EQUAL_STRING("", v.age_str);
    TEST_ASSERT_TRUE(v.signal_heard);
    TEST_ASSERT_TRUE(v.signal_age_str[0] != '\0');
}

static void S29_close_and_live_stale_place_unaffected_by_heard(void)
{
    /* CLOSE/LIVE/STALE/PLACE must never be preempted by RADAR_SIGNAL even
     * when the member has_heard — RADAR_SIGNAL only fires where the old
     * logic had nothing left to say. Spot-check LIVE. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.001, 0.0}; /* far enough to avoid CLOSE-by-distance */
    m->pos_age_ms = 1000u;              /* fresh: LIVE */
    ff_crew_on_heard(&c, 1001u, 1500u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v.mode);
}

/* ---------------------------------------------------------------------
 * S29 — ff_radar_signal_tier boundary-exactness (both sides of each of
 * the three thresholds).
 * ------------------------------------------------------------------- */

static void S29_signal_tier_boundaries(void)
{
    /* Four non-overlapping, gap-free intervals (see ff_radar.h's
     * FF_SIGNAL_GOOD_MIN_DBM comment): STRONG (-80,+inf); GOOD [-95,-80]
     * (closed at both ends — -80 itself is GOOD, not STRONG); WEAK
     * [-110,-95); FAINT (-inf,-110). */
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_STRONG, ff_radar_signal_tier(-79));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_STRONG, ff_radar_signal_tier(-1));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_GOOD, ff_radar_signal_tier(-80)); /* exactly -80: NOT strong */
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_GOOD, ff_radar_signal_tier(-94));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_GOOD, ff_radar_signal_tier(-95)); /* exactly -95: still GOOD, not weak */
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_WEAK, ff_radar_signal_tier(-96));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_WEAK, ff_radar_signal_tier(-109));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_WEAK, ff_radar_signal_tier(-110)); /* exactly -110: still WEAK, not faint */
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_FAINT, ff_radar_signal_tier(-111));
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_FAINT, ff_radar_signal_tier(-140));
}

/* ---------------------------------------------------------------------
 * S29 — signal_dots[] computation.
 * ------------------------------------------------------------------- */

static void S29_signal_dots_unpaired_excluded(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_upsert(&c, 2001u); /* unpaired */
    ff_crew_on_heard(&c, 2001u, 1000u, true);
    ff_crew_on_rssi(&c, 2001u, -70);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_UINT8(0, v.n_signal_dots);
}

static void S29_signal_dots_member_with_position_excluded(void)
{
    /* A member with a position is already on the ordinary ring
     * (dots[]), never both — mutual exclusivity. */
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_member_t *m = ff_crew_upsert(&c, 2001u);
    ff_crew_set_paired(&c, 2001u, true);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 1000u;
    ff_crew_on_heard(&c, 2001u, 1500u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_UINT8(0, v.n_signal_dots);
    TEST_ASSERT_EQUAL_UINT8(1, v.n_dots); /* placed on the ordinary ring instead */
}

static void S29_signal_dots_never_heard_excluded(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_crew_upsert(&c, 2001u);
    ff_crew_set_paired(&c, 2001u, true); /* paired, no position, never heard */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_UINT8(0, v.n_signal_dots);
}

static void S29_signal_dots_sort_order_mixed_tier(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);

    /* Roster order: WEAK, STRONG, FAINT-via-relay, GOOD. Expected sorted
     * order: STRONG, GOOD, WEAK, NONE(via relay) — stable within a tier
     * by roster order (no ties here, but exercised anyway). */
    ff_crew_member_t *w = ff_crew_upsert(&c, 1u);
    w->initial = 'W';
    ff_crew_set_paired(&c, 1u, true);
    ff_crew_on_heard(&c, 1u, 1000u, true);
    ff_crew_on_rssi(&c, 1u, -100); /* WEAK */

    ff_crew_member_t *s = ff_crew_upsert(&c, 2u);
    s->initial = 'S';
    ff_crew_set_paired(&c, 2u, true);
    ff_crew_on_heard(&c, 2u, 1000u, true);
    ff_crew_on_rssi(&c, 2u, -50); /* STRONG */

    ff_crew_member_t *r = ff_crew_upsert(&c, 3u);
    r->initial = 'R';
    ff_crew_set_paired(&c, 3u, true);
    ff_crew_on_heard(&c, 3u, 1000u, false); /* via relay: no licensed RSSI */

    ff_crew_member_t *g = ff_crew_upsert(&c, 4u);
    g->initial = 'G';
    ff_crew_set_paired(&c, 4u, true);
    ff_crew_on_heard(&c, 4u, 1000u, true);
    ff_crew_on_rssi(&c, 4u, -90); /* GOOD */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 2000u);

    TEST_ASSERT_EQUAL_UINT8(4, v.n_signal_dots);
    TEST_ASSERT_EQUAL_INT('S', v.signal_dots[0].initial);
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_STRONG, v.signal_dots[0].tier);
    TEST_ASSERT_EQUAL_INT('G', v.signal_dots[1].initial);
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_GOOD, v.signal_dots[1].tier);
    TEST_ASSERT_EQUAL_INT('W', v.signal_dots[2].initial);
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_WEAK, v.signal_dots[2].tier);
    TEST_ASSERT_EQUAL_INT('R', v.signal_dots[3].initial);
    TEST_ASSERT_EQUAL_INT(FF_SIGNAL_NONE, v.signal_dots[3].tier);
    TEST_ASSERT_TRUE(v.signal_dots[3].via_relay);
}

/* ---------------------------------------------------------------------
 * AC1 — mode-priority interaction cases (PR #13 review finding #1).
 *
 * The rows above each hold every OTHER input fixed at a value that can't
 * trip an earlier-priority check, so none of them actually pin the
 * check ORDER itself: e.g. every CLOSE row above uses a fresh
 * (would-be-LIVE) position, so swapping the CLOSE check and the
 * freshness switch wouldn't change their result. These two do pin the
 * order — each is constructed so a specific pair of checks disagree,
 * and only wins if the higher-priority one runs first.
 * ------------------------------------------------------------------- */

static void S06_AC1_close_by_rssi_wins_over_stale_gps(void)
{
    /* 2026-09-07 [api] presence-heard-vs-position: FF_CREW_LOST_MS
     * widened 10min -> 20min (ff_crew.h) — now_ms bumped from 700000 to
     * comfortably past the new 1200000ms threshold so this fix is still
     * genuinely LOST on its own. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};  /* ~1112 m: outside 30 m, distance leg false */
    m->pos_age_ms = 0u;                 /* age at now_ms=1300000 is 1300000ms: LOST on its own */
    m->rssi_dbm = -50;                  /* > -60 dBm threshold */
    m->rssi_age_ms = 1300000u - 500u;   /* age 500ms at now_ms=1300000: inside the 10s window */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 1300000u);

    /* This member's GPS fix is 1300s old — comfortably past the 1200s
     * LOST threshold, so freshness alone says LOST. If the CLOSE check
     * ran AFTER the freshness switch instead of before it, this would
     * resolve RADAR_LOST. Pins the priority order CLOSE > freshness
     * (S06 spec: "CLOSE per S02 predicate; else LIVE/STALE/LOST from
     * freshness" — CLOSE is checked first, unconditionally). */
    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

static void S06_AC1_nofix_beats_close_by_rssi(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;
    m->rssi_dbm = -50;             /* > -60 dBm threshold: RSSI-close on its own */
    m->rssi_age_ms = 5000u - 500u; /* age 500ms at now_ms=5000: inside the 10s window */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* my_pos_ok=false: this member is RSSI-close (ff_crew_close_range
     * would return true on its own), but if the CLOSE check ran BEFORE
     * the NOFIX check instead of after it, that RSSI-close reading
     * would win and produce RADAR_CLOSE. Pins the priority order
     * NOFIX > CLOSE (S06 spec lists NOFIX before CLOSE). */
    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 5000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

/* ---------------------------------------------------------------------
 * 2026-09-05 amendment — RADAR_NOHDG (docs/specs/S06-radar-face.md).
 * ------------------------------------------------------------------- */

/* Bearing/distance for the exact bench coordinates cited in the
 * amendment: a member ~150 m due south of me. Pins both the numeric
 * bearing (180 deg, i.e. true south) and the distance, with no heading
 * involved at all. */
static void S06_NOHDG_bearing_and_distance_for_bench_coords(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){47.705785, -122.2820993};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {47.707135, -122.2820993};

    ff_radar_compute(&v, &sm, &c, -1.0f /* no heading */, my_pos, true, false, 1000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
    TEST_ASSERT_TRUE(v.bearing_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 180.0f, v.bearing_deg);
    TEST_ASSERT_EQUAL_UINT8(0, v.n_dots);

    /* ~150 m — parse the formatted distance string back rather than
     * asserting on ff_geo_distance_m directly, so this also exercises
     * the same ff_fmt_distance path the render layer reads. */
    float parsed_m = strtof(v.dist_str, NULL);
    TEST_ASSERT_TRUE_MESSAGE(parsed_m > 100.0f && parsed_m < 200.0f, v.dist_str);
}

/* NOHDG outranks CLOSE (by RSSI) and outranks the freshness switch: a
 * compass-less puck reports NOHDG for its selection even when the
 * member is simultaneously RSSI-close AND has a fresh (would-be-LIVE)
 * GPS fix — see ff_radar.h's mode-resolution doc comment's explicit
 * ranking. Constructed so both alternate checks would fire if NOHDG's
 * early return didn't happen first. */
static void S06_NOHDG_beats_close_and_freshness(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.0001, 0.0}; /* ~11.1 m: inside 30 m CLOSE-by-distance */
    m->pos_age_ms = 0u;                  /* fresh: would be LIVE by freshness alone */
    m->rssi_dbm = -50;                   /* > -60 dBm: RSSI-close on its own too */
    m->rssi_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f /* no heading */, my_pos, true, false, 1000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_FALSE(v.arrow_valid);
}

/* place/stale (2026-09-05): an ASSERTED (landmark) selection with an
 * unknown heading still resolves RADAR_NOHDG (the KNOWN INTERACTION
 * ff_radar.h's mode-resolution doc comment records), but `place` reports
 * the underlying fact honestly so the renderer can skip the "aging" rim
 * treatment for it. */
static void S06_NOHDG_place_flag_true_for_asserted_member(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_asserted = true;
    m->pos_age_ms = 999999u; /* irrelevant for an asserted position */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, true, false, 1000000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_TRUE(v.place);
    TEST_ASSERT_FALSE(v.stale);
}

/* place/stale: an ordinary (non-asserted) member whose fix has aged past
 * the LIVE window reports stale==true while still in RADAR_NOHDG — the
 * "freshness still picks the rim colour... mode stays NOHDG" ruling. */
static void S06_NOHDG_stale_flag_true_for_aged_member(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* now_ms far enough past pos_age_ms to land in the STALE band (past
     * FF_CREW_LIVE_MS, at/before FF_CREW_LOST_MS). CLOSE is irrelevant
     * here regardless of distance/RSSI: the !heading_ok branch returns
     * before ff_crew_close_range is ever consulted. */
    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, true, false, 90000u);

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_FALSE(v.place);
    TEST_ASSERT_TRUE(v.stale);
}

/* place/stale: a freshly-live member (not asserted, not aged) reports
 * neither flag while in RADAR_NOHDG — no rim tint to draw. */
static void S06_NOHDG_neither_flag_for_live_member(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, -1.0f, my_pos, true, false, 1000u); /* age 1s: well inside LIVE */

    TEST_ASSERT_EQUAL_INT(RADAR_NOHDG, v.mode);
    TEST_ASSERT_FALSE(v.place);
    TEST_ASSERT_FALSE(v.stale);
}

/* ---------------------------------------------------------------------
 * clock_str/batt_pct/mesh_ok left untouched (PR #13 review finding #3).
 *
 * These three are caller-owned — RTC/battery/mesh-link inputs
 * ff_radar_compute never receives (see ff_radar.h's deviation note) —
 * so it must leave them bit-for-bit as it found them. Whole-struct
 * 0xAA poisoning (same model as
 * S06_AC3_dots_empty_when_my_pos_or_heading_invalid) makes an
 * accidental write to any of the three fail loudly, regardless of what
 * value it happened to write.
 * ------------------------------------------------------------------- */

static void S06_AC1_compute_leaves_clock_batt_mesh_untouched(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0xAA, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    char poisoned_clock[FF_RADAR_CLOCK_LEN];
    memset(poisoned_clock, 0xAA, sizeof(poisoned_clock));
    TEST_ASSERT_EQUAL_MEMORY(poisoned_clock, v.clock_str, sizeof(poisoned_clock));
    TEST_ASSERT_EQUAL_INT8((int8_t)0xAA, v.batt_pct);

    unsigned char mesh_ok_byte;
    memcpy(&mesh_ok_byte, &v.mesh_ok, sizeof(mesh_ok_byte));
    TEST_ASSERT_EQUAL_UINT8(0xAA, mesh_ok_byte);
}

/* ------------------------------------------------------------------- */
/* debt/batt-low-core — ff_radar_batt_is_low / ff_radar_batt_icon.      */
/* Not a numbered S06 AC (this PR predates any spec numbering for it);  */
/* named S06_ to sit next to the rest of this file's battery coverage.  */
/* ------------------------------------------------------------------- */

static void S06_batt_low_boundary_at_threshold_is_low(void)
{
    /* FF_BATT_LOW_PCT == 15 (S06's documented "Status bar alert color"
     * paragraph): AT the threshold counts as low, mutation-guard against
     * `<=` silently becoming `<`. */
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(FF_BATT_LOW_PCT));
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(15));
}

static void S06_batt_low_boundary_one_above_threshold_is_not_low(void)
{
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(FF_BATT_LOW_PCT + 1));
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(16));
}

static void S06_batt_low_unknown_neg1_is_not_low(void)
{
    /* Honest data: "we don't know" never escalates to an alarm. */
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(-1));
}

static void S06_batt_low_zero_is_low(void)
{
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(0));
}

static void S06_batt_low_full_is_not_low(void)
{
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(100));
}

/* Icon ladder boundaries (scr_launcher.c's status row) — pinned by
 * literal so a future edit to the ladder is a deliberate, reviewed
 * change to this test, not a silent renderer-side drift. */
static void S06_batt_icon_ladder_boundaries(void)
{
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_UNKNOWN, ff_radar_batt_icon(-1));

    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_EMPTY, ff_radar_batt_icon(0));
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_EMPTY, ff_radar_batt_icon(FF_BATT_ICON_1_MIN_PCT - 1));
    TEST_ASSERT_EQUAL_INT(11, FF_BATT_ICON_1_MIN_PCT - 1); /* pins the literal, not just the relation */

    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_1, ff_radar_batt_icon(FF_BATT_ICON_1_MIN_PCT));
    TEST_ASSERT_EQUAL_INT(12, FF_BATT_ICON_1_MIN_PCT);
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_1, ff_radar_batt_icon(FF_BATT_ICON_2_MIN_PCT - 1));

    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_2, ff_radar_batt_icon(FF_BATT_ICON_2_MIN_PCT));
    TEST_ASSERT_EQUAL_INT(37, FF_BATT_ICON_2_MIN_PCT);
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_2, ff_radar_batt_icon(FF_BATT_ICON_3_MIN_PCT - 1));

    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_3, ff_radar_batt_icon(FF_BATT_ICON_3_MIN_PCT));
    TEST_ASSERT_EQUAL_INT(62, FF_BATT_ICON_3_MIN_PCT);
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_3, ff_radar_batt_icon(FF_BATT_ICON_FULL_MIN_PCT - 1));

    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_FULL, ff_radar_batt_icon(FF_BATT_ICON_FULL_MIN_PCT));
    TEST_ASSERT_EQUAL_INT(87, FF_BATT_ICON_FULL_MIN_PCT);
    TEST_ASSERT_EQUAL_INT(FF_BATT_ICON_FULL, ff_radar_batt_icon(100));
}

/* ------------------------------------------------------------------- */
/* AC2 — arrow smoothing                                                */
/* ------------------------------------------------------------------- */

static void S06_AC2_smoothing_reaches_at_least_81deg_by_600ms_no_overshoot(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.0, 1.0}; /* due east of origin: bearing ~90 deg */
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* t=0, heading=90 -> target = wrap(90-90) = 0. First-ever call: snaps
     * straight to the target (no prior smoothing state to blend from). */
    ff_radar_compute(&v, &sm, &c, 90.0f, my_pos, true, false, 0u);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, v.arrow_deg);

    /* t=600ms, heading swings to 0 -> target jumps to 90 deg. tau=250ms:
     * alpha = 1 - exp(-600/250) = 1 - exp(-2.4) ~= 0.9093, so the smoothed
     * value should land at ~81.8 deg — comfortably clearing the spec's
     * ">=81 deg at 600ms" bar with no overshoot past 90. */
    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 600u);

    TEST_ASSERT_TRUE_MESSAGE(v.arrow_deg >= 81.0f, "expected arrow_deg >= 81 deg at t=600ms");
    TEST_ASSERT_TRUE_MESSAGE(v.arrow_deg <= 90.5f, "arrow_deg overshot the 90 deg target");
}

static void S06_AC2_smoothing_350_to_10_wraps_through_zero_not_180(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){1.0, 0.0}; /* due north of origin: bearing ~0 deg */
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* heading=10 -> target = wrap(0-10) = 350. First call snaps there. */
    ff_radar_compute(&v, &sm, &c, 10.0f, my_pos, true, false, 0u);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 350.0f, v.arrow_deg);

    /* 100ms later, heading=350 -> target = wrap(0-350) = 10. Shortest path
     * 350->10 is +20 deg through 0 (ff_geo_angdiff_deg), never through 180.
     * alpha at dt=100ms (tau=250) = 1-exp(-0.4) ~= 0.3297; smoothed lands
     * at ~356.6 deg — just past 350, wrapping toward 0 — not anywhere near
     * the 180 deg the wrong-direction path would produce. */
    ff_radar_compute(&v, &sm, &c, 350.0f, my_pos, true, false, 100u);

    TEST_ASSERT_FLOAT_WITHIN(1.5f, 356.6f, v.arrow_deg);
    TEST_ASSERT_TRUE_MESSAGE(v.arrow_deg > 340.0f, "smoothed value took the long way around through ~180 deg");
}

/* ------------------------------------------------------------------- */
/* AC3 — crew ring dots                                                 */
/* ------------------------------------------------------------------- */

static void S06_AC3_dots_bearings_colors_stale_flags_unpaired_excluded(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* A: paired, LIVE, due north (bearing 0). */
    ff_crew_member_t *a = ff_crew_upsert(&c, 1u);
    a->initial = 'A';
    a->color_idx = 0;
    a->has_pos = true;
    a->pos = (ff_latlon_t){1.0, 0.0};
    a->pos_age_ms = 0u;
    ff_crew_set_paired(&c, 1u, true);

    /* B: paired, STALE, due east (bearing 90). */
    ff_crew_member_t *b = ff_crew_upsert(&c, 2u);
    b->initial = 'B';
    b->color_idx = 1;
    b->has_pos = true;
    b->pos = (ff_latlon_t){0.0, 1.0};
    b->pos_age_ms = 0u;
    ff_crew_set_paired(&c, 2u, true);

    /* C: paired, LOST, due south (bearing 180). */
    ff_crew_member_t *cc = ff_crew_upsert(&c, 3u);
    cc->initial = 'C';
    cc->color_idx = 2;
    cc->has_pos = true;
    cc->pos = (ff_latlon_t){-1.0, 0.0};
    cc->pos_age_ms = 0u;
    ff_crew_set_paired(&c, 3u, true);

    /* D: paired, LIVE, due west (bearing 270). */
    ff_crew_member_t *d = ff_crew_upsert(&c, 4u);
    d->initial = 'D';
    d->color_idx = 3;
    d->has_pos = true;
    d->pos = (ff_latlon_t){0.0, -1.0};
    d->pos_age_ms = 0u;
    ff_crew_set_paired(&c, 4u, true);

    /* E: has a fix but is NOT paired — must be excluded. */
    ff_crew_member_t *e = ff_crew_upsert(&c, 5u);
    e->initial = 'E';
    e->has_pos = true;
    e->pos = (ff_latlon_t){0.5, 0.5};
    e->pos_age_ms = 0u;
    /* deliberately not paired */

    /* F: paired, but never sent a fix — excluded too (no honest bearing). */
    ff_crew_member_t *f = ff_crew_upsert(&c, 6u);
    f->initial = 'F';
    ff_crew_set_paired(&c, 6u, true);
    (void)f;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);

    /* now_ms chosen so B is STALE (age 50s) and C is LOST (age 700s) while
     * A/D stay LIVE (age 0) — see setup above, all pos_age_ms == 0. */
    uint32_t now_ms = 700000u;
    /* Re-stamp A/B/D so their intended freshness holds at this now_ms. */
    a->pos_age_ms = now_ms;         /* age 0: LIVE */
    b->pos_age_ms = now_ms - 50000u; /* age 50s: STALE */
    /* c->pos_age_ms stays 0: age 700000ms: LOST */
    d->pos_age_ms = now_ms; /* age 0: LIVE */

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, now_ms);

    TEST_ASSERT_EQUAL_UINT8(4, v.n_dots); /* E (unpaired) and F (no fix) excluded */

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, v.dots[0].ring_deg);
    TEST_ASSERT_EQUAL_CHAR('A', v.dots[0].initial);
    TEST_ASSERT_EQUAL_UINT8(0, v.dots[0].color_idx);
    TEST_ASSERT_FALSE(v.dots[0].stale);
    TEST_ASSERT_FALSE(v.dots[0].imprecise); /* issue #74: no precision bits set anywhere in this fixture */

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, v.dots[1].ring_deg);
    TEST_ASSERT_EQUAL_CHAR('B', v.dots[1].initial);
    TEST_ASSERT_EQUAL_UINT8(1, v.dots[1].color_idx);
    TEST_ASSERT_TRUE(v.dots[1].stale);
    TEST_ASSERT_FALSE(v.dots[1].imprecise);

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 180.0f, v.dots[2].ring_deg);
    TEST_ASSERT_EQUAL_CHAR('C', v.dots[2].initial);
    TEST_ASSERT_EQUAL_UINT8(2, v.dots[2].color_idx);
    TEST_ASSERT_TRUE(v.dots[2].stale);
    TEST_ASSERT_FALSE(v.dots[2].imprecise);

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 270.0f, v.dots[3].ring_deg);
    TEST_ASSERT_EQUAL_CHAR('D', v.dots[3].initial);
    TEST_ASSERT_EQUAL_UINT8(3, v.dots[3].color_idx);
    TEST_ASSERT_FALSE(v.dots[3].stale);
    TEST_ASSERT_FALSE(v.dots[3].imprecise);
}

/* issue #74 (S17 slice a): dot.imprecise is set PER MEMBER, independent of
 * stale/place — a mutation that folds this into "always false" or "copies
 * .stale" would still pass every other dots test in this file (none of
 * them set precision bits), so this is a dedicated, from-scratch fixture
 * rather than an addition to the test above. */
static void S17a_AC4_dot_imprecise_flag_is_set_per_member_independent_of_stale(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* A: paired, LIVE, precise fix (no precision_bits stated at all —
     * issue #47's documented "didn't say is not evidence of degraded"
     * asymmetry: this must NOT read as imprecise). */
    ff_crew_member_t *a = ff_crew_upsert(&c, 1u);
    a->initial = 'A';
    a->color_idx = 0;
    a->has_pos = true;
    a->pos = (ff_latlon_t){1.0, 0.0};
    a->pos_age_ms = 700000u;
    ff_crew_set_paired(&c, 1u, true);

    /* B: paired, LIVE, degraded precision — the one dot that must render
     * imprecise. */
    ff_crew_member_t *b = ff_crew_upsert(&c, 2u);
    b->initial = 'B';
    b->color_idx = 1;
    b->has_pos = true;
    b->pos = (ff_latlon_t){0.0, 1.0};
    b->pos_age_ms = 700000u;
    b->has_precision_bits = true;
    b->precision_bits = (uint8_t)(FF_CREW_POS_PRECISION_MIN_BITS - 1u); /* below threshold: degraded */
    ff_crew_set_paired(&c, 2u, true);

    /* C: paired, LIVE, precision stated but AT the threshold — the
     * documented boundary (S47's own "< MIN_BITS", strict) must NOT flip
     * this one imprecise either. */
    ff_crew_member_t *cc = ff_crew_upsert(&c, 3u);
    cc->initial = 'C';
    cc->color_idx = 2;
    cc->has_pos = true;
    cc->pos = (ff_latlon_t){-1.0, 0.0};
    cc->pos_age_ms = 700000u;
    cc->has_precision_bits = true;
    cc->precision_bits = (uint8_t)FF_CREW_POS_PRECISION_MIN_BITS;
    ff_crew_set_paired(&c, 3u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 700000u);

    TEST_ASSERT_EQUAL_UINT8(3, v.n_dots);
    TEST_ASSERT_EQUAL_CHAR('A', v.dots[0].initial);
    TEST_ASSERT_FALSE(v.dots[0].imprecise);
    TEST_ASSERT_EQUAL_CHAR('B', v.dots[1].initial);
    TEST_ASSERT_TRUE(v.dots[1].imprecise);
    TEST_ASSERT_EQUAL_CHAR('C', v.dots[2].initial);
    TEST_ASSERT_FALSE(v.dots[2].imprecise);
}

static void S06_AC3_dots_empty_when_my_pos_or_heading_invalid(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){1.0, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    ff_radar_smooth_t sm;
    ff_latlon_t my_pos = {0.0, 0.0};

    memset(&v, 0xAA, sizeof(v));
    ff_radar_smooth_reset(&sm);
    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, /*my_pos_ok=*/false, false, 0u);
    TEST_ASSERT_EQUAL_UINT8(0, v.n_dots);

    memset(&v, 0xAA, sizeof(v));
    ff_radar_smooth_reset(&sm);
    ff_radar_compute(&v, &sm, &c, -1.0f /* invalid heading */, my_pos, true, false, 0u);
    TEST_ASSERT_EQUAL_UINT8(0, v.n_dots);
}

/* ------------------------------------------------------------------- */
/* issue #33 — RADAR_PLACE: mode, age_str, and the freshness-axis        */
/* exclusion measured through ff_radar_compute (not just ff_crew_freshness) */
/* ------------------------------------------------------------------- */

static void S33_mode_place_for_asserted_position(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0}; /* far enough not to be CLOSE by distance */
    m->pos_age_ms = 0u;
    m->pos_asserted = true;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_PLACE, v.mode);
    TEST_ASSERT_TRUE(v.arrow_valid); /* a real coordinate exists to point at */
    TEST_ASSERT_EQUAL_STRING("", v.age_str); /* never a fabricated "LAST SEEN" */
    TEST_ASSERT_NOT_EQUAL_INT(0, strcmp("", v.dist_str)); /* the coordinate itself is honest */
}

/* Mutation-conscious: elapsed time must NEVER move an asserted member off
 * RADAR_PLACE, at every named freshness boundary and far beyond. A
 * mutant that deleted the ASSERTED check ahead of the freshness switch
 * would pass S33_mode_place_for_asserted_position (age 0) but fail every
 * row here. */
static void S33_mode_place_never_ages_at_any_boundary(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;
    m->pos_asserted = true;

    ff_radar_view_t v;
    ff_radar_smooth_t sm;
    ff_latlon_t my_pos = {0.0, 0.0};
    uint32_t const ages[] = {0u, FF_CREW_LIVE_MS, FF_CREW_LOST_MS, FF_CREW_LOST_MS * 100u};

    for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); i++) {
        memset(&v, 0, sizeof(v));
        ff_radar_smooth_reset(&sm);
        ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, ages[i]);
        TEST_ASSERT_EQUAL_INT(RADAR_PLACE, v.mode);
        TEST_ASSERT_EQUAL_STRING("", v.age_str);
    }
}

/* A precise asserted position is still a real coordinate: CLOSE-by-
 * distance can fire honestly for it (ff_radar.h's RADAR_CLOSE priority
 * paragraph) — proximity is a geometric fact an assertion doesn't taint,
 * only the position's AGE is unknowable. */
static void S33_close_by_distance_still_wins_over_place(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.0001, 0.0}; /* ~11.1 m: inside 30 m close range */
    m->pos_age_ms = 0u;
    m->pos_asserted = true;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v.mode);
}

/* ------------------------------------------------------------------- */
/* issue #47 — degraded precision: area-scale dist_str, threshold        */
/* boundary, and the CLOSE-by-distance gate                             */
/* ------------------------------------------------------------------- */

static void S47_degraded_precision_sets_dist_imprecise_and_area_string(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0}; /* far enough not to be CLOSE by distance */
    m->pos_age_ms = 0u;
    m->has_precision_bits = true;
    m->precision_bits = 13; /* issue #47's own hardware measurement */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v.mode); /* freshness is untouched by precision */
    TEST_ASSERT_TRUE(v.dist_imprecise);
    TEST_ASSERT_EQUAL_CHAR('~', v.dist_str[0]); /* never a bare metre-looking number */
}

/* Absent precision (has_precision_bits == false) renders exactly as the
 * ordinary case — the documented asymmetry (mc_client.h): "didn't say"
 * is not evidence of a degraded fix. */
static void S47_absent_precision_is_not_treated_as_degraded(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;
    /* has_precision_bits left false by setup_selected_member's zeroed slot */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_FALSE(v.dist_imprecise);
    TEST_ASSERT_NOT_EQUAL('~', v.dist_str[0]);
}

/* The exact FF_CREW_POS_PRECISION_MIN_BITS fencepost, driven through
 * ff_radar_compute (test_crew.c pins the same boundary against the raw
 * grid-size formula; this pins it against the actual consuming code
 * path, which is the thing a `<` vs `<=` mutation in ff_radar.c itself
 * would actually break). */
static void S47_precision_threshold_boundary_through_compute(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;
    m->has_precision_bits = true;

    ff_radar_view_t v;
    ff_radar_smooth_t sm;
    ff_latlon_t my_pos = {0.0, 0.0};

    m->precision_bits = (uint8_t)(FF_CREW_POS_PRECISION_MIN_BITS - 1u);
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_reset(&sm);
    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);
    TEST_ASSERT_TRUE_MESSAGE(v.dist_imprecise, "one bit below threshold must be imprecise");

    m->precision_bits = (uint8_t)FF_CREW_POS_PRECISION_MIN_BITS;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_reset(&sm);
    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);
    TEST_ASSERT_FALSE_MESSAGE(v.dist_imprecise, "the threshold's own bit count must be precise");
}

/* The gate this issue exists for: a degraded fix that WOULD be inside the
 * 30 m close-range distance leg must not trigger RADAR_CLOSE off that
 * leg — a coordinate that could be kilometers off cannot honestly support
 * "you are standing next to them". No RSSI sample exists here (rssi_dbm
 * stays INT16_MIN, the "never direct" sentinel — setup_selected_member's
 * zeroed slot), so if the distance leg fires, CLOSE is entirely on the
 * back of the untrustworthy coordinate. */
static void S47_close_by_distance_gated_off_when_imprecise(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.0001, 0.0}; /* ~11.1 m: inside 30 m close range */
    m->pos_age_ms = 0u;
    m->has_precision_bits = true;
    m->precision_bits = 13;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_NOT_EQUAL_INT(RADAR_CLOSE, v.mode);
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v.mode); /* falls through to ordinary freshness */
}

/* The RSSI leg is untouched by precision — it is measured by our own
 * radio and carries no coordinate dependency at all, so CLOSE can still
 * fire through it even while the position itself is degraded. */
static void S47_close_by_rssi_unaffected_by_imprecise_position(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0}; /* ~1112 m: outside 30 m, distance leg false */
    m->pos_age_ms = 0u;
    m->has_precision_bits = true;
    m->precision_bits = 13;
    m->rssi_dbm = -50; /* > -60 dBm threshold */
    m->rssi_age_ms = 5000u - 500u; /* age 500ms at now_ms=5000: inside 10s */

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 5000u);

    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, v.mode);
}

/* ------------------------------------------------------------------- */
/* AC3 extension — ring dots: place vs stale (issue #33)                */
/* ------------------------------------------------------------------- */

static void S33_AC3_dot_place_flag_set_and_mutually_exclusive_with_stale(void)
{
    ff_crew_t c;
    ff_crew_init(&c, NULL);
    ff_latlon_t my_pos = {0.0, 0.0};

    /* A: paired, asserted, due north. */
    ff_crew_member_t *a = ff_crew_upsert(&c, 1u);
    a->initial = 'A';
    a->color_idx = 0;
    a->has_pos = true;
    a->pos = (ff_latlon_t){1.0, 0.0};
    a->pos_age_ms = 0u;
    a->pos_asserted = true;
    ff_crew_set_paired(&c, 1u, true);

    /* B: paired, ordinary LIVE, due east — the regression guard: an
     * unrelated live member must not pick up `place` by accident. */
    ff_crew_member_t *b = ff_crew_upsert(&c, 2u);
    b->initial = 'B';
    b->color_idx = 1;
    b->has_pos = true;
    b->pos = (ff_latlon_t){0.0, 1.0};
    b->pos_age_ms = 0u;
    ff_crew_set_paired(&c, 2u, true);

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_UINT8(2, v.n_dots);
    TEST_ASSERT_TRUE(v.dots[0].place);
    TEST_ASSERT_FALSE(v.dots[0].stale); /* mutually exclusive with place */
    TEST_ASSERT_FALSE(v.dots[1].place);
    TEST_ASSERT_FALSE(v.dots[1].stale);
}

/* ------------------------------------------------------------------- */
/* 2026-09-06 [api] crew long names — headline uses ff_crew_display_name */
/* ------------------------------------------------------------------- */

static void LONGNAME_headline_uses_long_name_when_known(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c); /* name "DANA" */
    strncpy(m->long_name, "Dana", sizeof(m->long_name) - 1);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, v.mode);
    TEST_ASSERT_EQUAL_STRING("Dana", v.name); /* long name, not the short "DANA" */
}

static void LONGNAME_headline_falls_back_to_short_when_long_unknown(void)
{
    /* setup_selected_member never sets long_name — this pins that the
     * pre-existing (short-name) behavior is unchanged when no long name
     * has ever arrived. Same scenario S06_AC1_mode_live already covers;
     * kept as its own named test so it reads as a crew-long-names AC,
     * not an incidental assertion buried in an unrelated mode test. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c); /* name "DANA", long_name "" */
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_STRING("DANA", v.name);
}

static void LONGNAME_headline_long_name_near_full_budget_not_truncated(void)
{
    /* FF_RADAR_NAME_LEN == FF_CREW_LONG_NAME_LEN (40): a long name right
     * at the wire's own 39-byte-plus-NUL bound must survive intact
     * through ff_radar_compute's copy, not get clipped by a stale 16-byte
     * assumption left over from the short-name era. */
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    char const *long39 = "Bartholomew Montgomery-Fitzgeraldxx"; /* 35 chars */
    strncpy(m->long_name, long39, sizeof(m->long_name) - 1);
    m->has_pos = true;
    m->pos = (ff_latlon_t){0.01, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, 0u);

    TEST_ASSERT_EQUAL_STRING(long39, v.name);
}

/* ------------------------------------------------------------------- */
/* AC6 — allocation-free (by construction, see file header) and fast    */
/* ------------------------------------------------------------------- */

static void S06_AC6_compute_runs_well_under_1ms(void)
{
    ff_crew_t c;
    ff_crew_member_t *m = setup_selected_member(&c);
    m->has_pos = true;
    m->pos = (ff_latlon_t){1.0, 0.0};
    m->pos_age_ms = 0u;

    ff_radar_view_t v;
    memset(&v, 0, sizeof(v));
    ff_radar_smooth_t sm;
    ff_radar_smooth_reset(&sm);
    ff_latlon_t my_pos = {0.0, 0.0};

    enum { N = 1000 };
    clock_t start = clock();
    for (int i = 0; i < N; i++) {
        ff_radar_compute(&v, &sm, &c, 0.0f, my_pos, true, false, (uint32_t)i);
    }
    clock_t end = clock();

    double total_ms = ((double)(end - start) / (double)CLOCKS_PER_SEC) * 1000.0;
    double ms_per_call = total_ms / (double)N;
    TEST_ASSERT_TRUE_MESSAGE(ms_per_call < 1.0, "ff_radar_compute averaged >= 1ms/call");
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S06_AC1_mode_nosel_no_paired_member);
    RUN_TEST(S06_AC1_mode_nofix_pos_invalid);
    RUN_TEST(S06_AC1_mode_nohdg_heading_invalid_member_has_pos);
    RUN_TEST(S06_AC1_mode_nofix_heading_invalid_member_has_no_pos);
    RUN_TEST(S06_AC1_mode_nofix_both_pos_and_heading_invalid);
    RUN_TEST(S06_AC1_nofix_age_str_known_but_dist_str_unknown);
    RUN_TEST(S06_AC1_mode_close_by_distance);
    RUN_TEST(S06_AC1_mode_close_by_rssi);
    RUN_TEST(S06_AC1_mode_live);
    RUN_TEST(S06_AC1_mode_stale);
    RUN_TEST(S06_AC1_mode_lost);
    RUN_TEST(S06_AC1_mode_lost_via_never_had_a_fix);

    RUN_TEST(S29_nofix_falls_back_to_signal_when_heard);
    RUN_TEST(S29_nofix_stays_nofix_when_never_heard);
    RUN_TEST(S29_nohdg_no_member_position_falls_back_to_signal_when_heard);
    RUN_TEST(S29_nohdg_no_member_position_stays_nofix_when_never_heard);
    RUN_TEST(S29_nohdg_with_member_position_is_unaffected_by_heard);
    RUN_TEST(S29_lost_falls_back_to_signal_when_heard);
    RUN_TEST(S29_lost_stays_lost_when_never_heard);
    RUN_TEST(S29_never_fixed_but_heard_signal_has_no_ghost);
    RUN_TEST(S29_close_and_live_stale_place_unaffected_by_heard);
    RUN_TEST(S29_signal_tier_boundaries);
    RUN_TEST(S29_signal_dots_unpaired_excluded);
    RUN_TEST(S29_signal_dots_member_with_position_excluded);
    RUN_TEST(S29_signal_dots_never_heard_excluded);
    RUN_TEST(S29_signal_dots_sort_order_mixed_tier);

    RUN_TEST(S06_AC1_close_by_rssi_wins_over_stale_gps);
    RUN_TEST(S06_AC1_nofix_beats_close_by_rssi);
    RUN_TEST(S06_AC1_compute_leaves_clock_batt_mesh_untouched);

    RUN_TEST(S06_NOHDG_bearing_and_distance_for_bench_coords);
    RUN_TEST(S06_NOHDG_beats_close_and_freshness);
    RUN_TEST(S06_NOHDG_place_flag_true_for_asserted_member);
    RUN_TEST(S06_NOHDG_stale_flag_true_for_aged_member);
    RUN_TEST(S06_NOHDG_neither_flag_for_live_member);

    RUN_TEST(S06_batt_low_boundary_at_threshold_is_low);
    RUN_TEST(S06_batt_low_boundary_one_above_threshold_is_not_low);
    RUN_TEST(S06_batt_low_unknown_neg1_is_not_low);
    RUN_TEST(S06_batt_low_zero_is_low);
    RUN_TEST(S06_batt_low_full_is_not_low);
    RUN_TEST(S06_batt_icon_ladder_boundaries);

    RUN_TEST(S06_AC2_smoothing_reaches_at_least_81deg_by_600ms_no_overshoot);
    RUN_TEST(S06_AC2_smoothing_350_to_10_wraps_through_zero_not_180);

    RUN_TEST(S06_AC3_dots_bearings_colors_stale_flags_unpaired_excluded);
    RUN_TEST(S06_AC3_dots_empty_when_my_pos_or_heading_invalid);
    RUN_TEST(S17a_AC4_dot_imprecise_flag_is_set_per_member_independent_of_stale);

    RUN_TEST(S33_mode_place_for_asserted_position);
    RUN_TEST(S33_mode_place_never_ages_at_any_boundary);
    RUN_TEST(S33_close_by_distance_still_wins_over_place);
    RUN_TEST(S33_AC3_dot_place_flag_set_and_mutually_exclusive_with_stale);

    RUN_TEST(S47_degraded_precision_sets_dist_imprecise_and_area_string);
    RUN_TEST(S47_absent_precision_is_not_treated_as_degraded);
    RUN_TEST(S47_precision_threshold_boundary_through_compute);
    RUN_TEST(S47_close_by_distance_gated_off_when_imprecise);
    RUN_TEST(S47_close_by_rssi_unaffected_by_imprecise_position);

    RUN_TEST(S06_AC6_compute_runs_well_under_1ms);

    RUN_TEST(LONGNAME_headline_uses_long_name_when_known);
    RUN_TEST(LONGNAME_headline_falls_back_to_short_when_long_unknown);
    RUN_TEST(LONGNAME_headline_long_name_near_full_budget_not_truncated);

    return UNITY_END();
}
