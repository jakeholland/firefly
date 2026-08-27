/**
 * test_settings.c — S11 slice a: store seam + settings + logic.
 *
 * Criteria covered here (see docs/specs/S11-settings.md):
 *   AC1 — load-with-defaults (empty store, corrupt blob).
 *   AC2 — round-trip save/load equality, including calibration.
 *   AC3 — ff_quiet_now table, incl. midnight wraparound, inclusive/exclusive.
 *   AC4 — water-nudge tick: fires at interval, resets on settings change,
 *         silent in quiet hours.
 *   AC6 — store mock records exactly one write per save.
 *
 * AC5 (golden settings.json vs mockup) is UI (slice b) — out of scope here.
 *
 * The store used in this file is a small in-memory mock (below), not the
 * sim's real file-backed store — that lives in targets/sim/store_file.c
 * and has its own test (targets/sim/tests/test_store_file.c), including
 * an end-to-end round trip of ff_settings_t through the real file store.
 */
#include <string.h>

#include "unity.h"

#include "ff_geo.h"
#include "ff_settings.h"
#include "ff_store.h"

void setUp(void) {}
void tearDown(void) {}

/* ---------------------------------------------------------------------
 * Mock ff_store_t: a single fixed-size slot plus a write counter, so
 * tests can assert both persisted content and write amplification
 * (AC6) without any real I/O.
 * ------------------------------------------------------------------- */

#define MOCK_SLOT_CAP 256

typedef struct {
    bool has_value;
    size_t len;
    uint8_t data[MOCK_SLOT_CAP];
    char key[64];
    int set_calls;
    int get_calls;
} mock_store_io_t;

static void mock_store_reset(mock_store_io_t *m)
{
    memset(m, 0, sizeof(*m));
}

static int mock_get(void *io, char const *key, void *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    m->get_calls++;
    if (!m->has_value || strcmp(m->key, key) != 0) {
        return -1;
    }
    if (n < m->len) {
        return -1; /* buffer too small */
    }
    memcpy(buf, m->data, m->len);
    return (int)m->len;
}

static int mock_set(void *io, char const *key, void const *buf, size_t n)
{
    mock_store_io_t *m = (mock_store_io_t *)io;
    m->set_calls++;
    if (n > MOCK_SLOT_CAP) {
        return -1;
    }
    strncpy(m->key, key, sizeof(m->key) - 1);
    m->key[sizeof(m->key) - 1] = '\0';
    memcpy(m->data, buf, n);
    m->len = n;
    m->has_value = true;
    return (int)n;
}

static ff_store_t mock_store_vtable(mock_store_io_t *m)
{
    ff_store_t st;
    st.get = mock_get;
    st.set = mock_set;
    st.io = m;
    return st;
}

/* ---------------------------------------------------------------------
 * AC1 — load with defaults.
 * ------------------------------------------------------------------- */

static void ff_assert_defaults(ff_settings_t const *s)
{
    TEST_ASSERT_TRUE(s->imperial);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_LIVE, s->share_mode);
    TEST_ASSERT_TRUE(s->haptics);
    TEST_ASSERT_TRUE(s->night_glow);
    TEST_ASSERT_EQUAL_UINT16(90, s->water_min);
    TEST_ASSERT_EQUAL_UINT16(240, s->quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(600, s->quiet_to_min);
    /* UTC offset defaults to UNSET (S16 slice b0): 0 is legitimately
     * UTC, so the flag — not the value — encodes absence. */
    TEST_ASSERT_FALSE(s->utc_offset_set);
    TEST_ASSERT_EQUAL_INT16(0, s->utc_offset_min);
    TEST_ASSERT_EQUAL_STRING("", s->my_name);
    TEST_ASSERT_FALSE(s->cal_valid);
    /* S17 slice a: default false — "not colorblind by default, keep the
     * brand colours" (docs/specs/S17-usability-hardening.md's scoping note). */
    TEST_ASSERT_FALSE(s->colorblind);
    /* #100: brightness defaults to a sensible mid-bright ~70% (never 0). */
    TEST_ASSERT_EQUAL_UINT8(FF_BRIGHTNESS_DEFAULT_PCT, s->brightness_pct);
    /* S21 §5: the default touch cal is now a representative Waveshare-1.46
     * panel prior (the affine measured on board 2), not identity — a fresh
     * puck of this panel model is roughly right out of the box, refined
     * per-device by the in-app CALIBRATE TOUCH row + NVS. touch_calibrated is
     * true (a usable transform is installed), and the four params are the
     * documented board-2 measurement. See ff_settings.c's
     * ff_settings_apply_defaults for the provenance + honesty note. */
    TEST_ASSERT_TRUE(s->touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(0.885060f, s->touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(15.5352f, s->touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(0.902172f, s->touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(8.2439f, s->touch_by);

    ff_geo_cal_t zero_cal;
    memset(&zero_cal, 0, sizeof(zero_cal));
    TEST_ASSERT_EQUAL_MEMORY(&zero_cal, &s->compass_cal, sizeof(ff_geo_cal_t));
}

static void S11_AC1_load_with_empty_store_yields_exact_defaults(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s)); /* poison, to prove load fully overwrites it */
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S11_AC1_load_with_null_store_yields_exact_defaults(void)
{
    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, NULL);

    ff_assert_defaults(&s);
}

static void S11_AC1_load_with_wrong_size_blob_yields_defaults(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    /* Simulate a blob from an older/incompatible layout: too short. */
    uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    st.set(st.io, "ff.settings", garbage, sizeof(garbage));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S11_AC1_load_with_bad_magic_yields_defaults(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    /* Write a real settings blob via save(), then flip a magic byte to
     * corrupt it in place. */
    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = false;
    saved.water_min = 45;
    ff_settings_save(&saved, &st);

    TEST_ASSERT_TRUE(m.has_value);
    m.data[0] ^= 0xFF; /* corrupt the magic's first byte */

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S11_AC1_load_with_wrong_version_yields_defaults(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = true;
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Header layout: magic(4) | version(2) | payload_size(2). Bump the
     * version field past what this build understands. */
    uint16_t bumped_version = 0xFFFF;
    memcpy(m.data + 4, &bumped_version, sizeof(bumped_version));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S11_AC1_load_with_v2_blob_yields_defaults_not_a_migration(void)
{
    /* The specific transition this build performs, pinned rather than
     * covered incidentally by the generic 0xFFFF case above (PR #37
     * review, D6): v2 -> v3 added ff_settings_t's UTC-offset field, and
     * there is deliberately NO migration — a v2 blob is discarded whole
     * and the full defaults stand. That resets compass calibration along
     * with everything else, which is the part a user would notice; the
     * decision and its blast radius are recorded in
     * docs/specs/S11-settings.md's ## Amendments. When the next bump
     * lands, this is the test that should be updated to name it. */
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = false;
    saved.water_min = 45;
    saved.cal_valid = true; /* the field whose loss actually costs a user */
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    uint16_t v2 = 2;
    memcpy(m.data + 4, &v2, sizeof(v2));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
    TEST_ASSERT_FALSE(s.cal_valid); /* discarded, not carried across */
}

/* S17 slice a: the transition this build actually performs, same "pin it,
 * don't rely on the generic 0xFFFF case" reasoning as the v2 test above —
 * v3 -> v4 added ff_settings_t.colorblind (docs/specs/S17-usability-hardening.md),
 * again with NO migration: a v3 blob is discarded whole and the full
 * defaults stand (colorblind included — it lands false, which is honest:
 * a v3 puck never had this field at all). */
static void S11_AC1_load_with_v3_blob_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = false;
    saved.water_min = 45;
    saved.cal_valid = true; /* the field whose loss actually costs a user */
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    uint16_t v3 = 3;
    memcpy(m.data + 4, &v3, sizeof(v3));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
    TEST_ASSERT_FALSE(s.cal_valid);   /* discarded, not carried across */
    TEST_ASSERT_FALSE(s.colorblind); /* the new field: false is the honest default, not a guess */
}

/* Same policy for v4 -> v5 (S15 slice d added the touch-cal fields): a v4
 * blob is discarded whole and the full defaults stand — the saved value is
 * NOT migrated across an incompatible layout. Post-S21 the touch-cal default
 * is the documented board-2 panel prior (not identity), so "reject, don't
 * migrate" now shows as: the loaded touch_ax is the DEFAULT prior, never the
 * v4 blob's own saved value. */
static void S11_AC1_load_with_v4_blob_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = false;
    saved.water_min = 45;
    saved.touch_calibrated = true;
    saved.touch_ax = 1.0123f; /* a value distinct from the default prior, to prove it's not carried across */
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    uint16_t v4 = 4;
    memcpy(m.data + 4, &v4, sizeof(v4));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
    /* Reject-not-migrate: the v4 blob's saved 1.0123 is discarded; the
     * default board-2 prior stands instead (asserted by ff_assert_defaults
     * above, which now checks touch_ax == 0.885060). */
    TEST_ASSERT_EQUAL_FLOAT(0.885060f, s.touch_ax);
}

/* ---------------------------------------------------------------------
 * AC2 — round-trip save/load equality, including calibration.
 * ------------------------------------------------------------------- */

static void S11_AC2_round_trip_save_load_is_exact_including_calibration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t out;
    memset(&out, 0, sizeof(out));
    out.imperial = false;
    out.share_mode = FF_SHARE_GHOST;
    out.haptics = false;
    out.night_glow = false;
    out.water_min = 45;
    out.quiet_from_min = 1380; /* 23:00 */
    out.quiet_to_min = 120;    /* 02:00 */
    out.utc_offset_min = -420; /* MDT (S16 slice b0) */
    out.utc_offset_set = true;
    out.colorblind = true; /* S17 slice a: a real change from the default, not left at its zero value */
    out.brightness_pct = 55; /* #100: a real change from the default, so the round-trip actually proves it persists */
    strncpy(out.my_name, "Dana", sizeof(out.my_name) - 1);
    out.cal_valid = true;
    out.compass_cal.hard_offset = (ff_vec3_t){12.5f, -3.25f, 0.75f};
    out.compass_cal.soft_scale[0] = 1.04f;
    out.compass_cal.soft_scale[1] = 0.97f;
    out.compass_cal.soft_scale[2] = 1.11f;
    out.compass_cal.declination_deg = -6.5f; /* e.g. Columbus, OH */
    /* S15 slice d: real touch-cal params, a change from the zero default. */
    out.touch_calibrated = true;
    out.touch_ax = 1.0123f;
    out.touch_bx = -14.0f;
    out.touch_ay = 1.0123f;
    out.touch_by = -18.0f;

    ff_settings_save(&out, &st);

    ff_settings_t in;
    memset(&in, 0xAA, sizeof(in));
    ff_settings_load(&in, &st);

    TEST_ASSERT_EQUAL_MEMORY(&out, &in, sizeof(ff_settings_t));
}

static void S11_AC2_round_trip_preserves_exact_defaults(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t defaults;
    ff_settings_load(&defaults, &st); /* empty store -> defaults */
    ff_settings_save(&defaults, &st);

    ff_settings_t reloaded;
    memset(&reloaded, 0xAA, sizeof(reloaded));
    ff_settings_load(&reloaded, &st);

    TEST_ASSERT_EQUAL_MEMORY(&defaults, &reloaded, sizeof(ff_settings_t));
}

/* ---------------------------------------------------------------------
 * AC3 — ff_quiet_now table, including wraparound.
 * ------------------------------------------------------------------- */

static void S11_AC3_quiet_now_same_day_window_is_inclusive_exclusive(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.quiet_from_min = 240; /* 4:00a */
    s.quiet_to_min = 600;   /* 10:00a */

    TEST_ASSERT_FALSE(ff_quiet_now(&s, 239)); /* just before start */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 240));  /* start: inclusive */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 599));  /* just before end */
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 600)); /* end: exclusive */
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 0));   /* well outside */
}

static void S11_AC3_quiet_now_wraps_past_midnight_inclusive_exclusive(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.quiet_from_min = 1380; /* 23:00 */
    s.quiet_to_min = 120;    /* 02:00 */

    TEST_ASSERT_FALSE(ff_quiet_now(&s, 1379)); /* just before start */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 1380));  /* start: inclusive */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 1439));  /* 23:59, pre-midnight */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 0));     /* midnight, mid-window */
    TEST_ASSERT_TRUE(ff_quiet_now(&s, 119));   /* just before end */
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 120));  /* end: exclusive */
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 700));  /* well outside, afternoon */
}

static void S11_AC3_quiet_now_equal_bounds_is_always_off(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.quiet_from_min = 0;
    s.quiet_to_min = 0;

    TEST_ASSERT_FALSE(ff_quiet_now(&s, 0));
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 700));
    TEST_ASSERT_FALSE(ff_quiet_now(&s, 1439));
}

/* ---------------------------------------------------------------------
 * AC4 — water-nudge tick.
 * ------------------------------------------------------------------- */

static void S11_AC4_water_tick_fires_at_interval(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90;
    s.quiet_from_min = 240;
    s.quiet_to_min = 600; /* quiet 4a-10a; test runs entirely in daytime */

    ff_water_state_t st;
    ff_water_state_init(&st);

    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 700)); /* priming tick: never fires */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 730)); /* +30 -> 30 elapsed */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 760)); /* +30 -> 60 elapsed */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 789)); /* +29 -> 89 elapsed */
    TEST_ASSERT_TRUE(ff_water_tick(&st, &s, 790));  /* +1 -> 90 elapsed: fires */

    /* Timer restarts after firing. */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 800));
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 879));
    TEST_ASSERT_TRUE(ff_water_tick(&st, &s, 880)); /* another 90 elapsed */
}

static void S11_AC4_water_tick_off_never_fires(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 0;
    s.quiet_from_min = 240;
    s.quiet_to_min = 600;

    ff_water_state_t st;
    ff_water_state_init(&st);

    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 700));
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 1000));
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 2000 % 1440));
}

static void S11_AC4_water_tick_resets_on_interval_change(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90;
    s.quiet_from_min = 240;
    s.quiet_to_min = 600;

    ff_water_state_t st;
    ff_water_state_init(&st);

    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 700)); /* prime */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 780)); /* 80 elapsed, close to firing */

    s.water_min = 45; /* settings change mid-flight */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 781)); /* reset tick: never fires */

    /* New interval counts from the reset point, not from the stale 80. */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 800)); /* +19 -> 19 elapsed of 45 */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 825)); /* +25 -> 44 elapsed */
    TEST_ASSERT_TRUE(ff_water_tick(&st, &s, 826));  /* +1 -> 45 elapsed: fires */
}

static void S11_AC4_water_tick_silent_during_quiet_hours(void)
{
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90;
    s.quiet_from_min = 240; /* 4:00a */
    s.quiet_to_min = 600;   /* 10:00a */

    ff_water_state_t st;
    ff_water_state_init(&st);

    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 300)); /* prime, inside quiet */

    /* 295 minutes of WALL-CLOCK time elapse (> water_min=90), entirely
     * inside quiet hours -> zero awake minutes accrued -> never fires,
     * no matter how long the quiet span runs. */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 595)); /* +295, still quiet (300-595 < 600) */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 600)); /* +5, still quiet (599 < 600) */

    /* Counting only starts once minutes are genuinely awake; it fires
     * after exactly water_min awake minutes, not sooner. */
    TEST_ASSERT_TRUE(ff_water_tick(&st, &s, 690)); /* +90, fully awake -> fires */
}

static void S11_AC4_quiet_minutes_do_not_accrue(void)
{
    /* Regression for the reviewer-reported gap: elapsed_min must only
     * count AWAKE minutes, not be gated solely at the fire instant.
     * water_min=90, quiet 4a-10a (240-600): prime@550 (quiet), tick@600
     * (+50, all quiet -> 0 awake accrued), tick@640 (+40, all awake ->
     * 40 accrued) must NOT fire (40 < 90). The pre-fix implementation
     * banked the 50 quiet minutes too and fired here at 90/90. */
    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.water_min = 90;
    s.quiet_from_min = 240;
    s.quiet_to_min = 600;

    ff_water_state_t st;
    ff_water_state_init(&st);

    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 550)); /* prime, inside quiet */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 600)); /* +50, all quiet -> 0 awake accrued */
    TEST_ASSERT_FALSE(ff_water_tick(&st, &s, 640)); /* +40, all awake -> 40 accrued; must NOT fire */
}

/* ---------------------------------------------------------------------
 * AC6 — store mock records a single write per save.
 * ------------------------------------------------------------------- */

static void S11_AC6_save_issues_exactly_one_store_write(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t s;
    memset(&s, 0, sizeof(s));
    s.imperial = true;
    s.water_min = 90;

    ff_settings_save(&s, &st);
    TEST_ASSERT_EQUAL_INT(1, m.set_calls);

    ff_settings_save(&s, &st);
    TEST_ASSERT_EQUAL_INT(2, m.set_calls); /* one more save -> exactly one more write */
}

static void S11_AC6_load_does_not_write_to_store(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t s;
    ff_settings_load(&s, &st);

    TEST_ASSERT_EQUAL_INT(0, m.set_calls);
    TEST_ASSERT_EQUAL_INT(1, m.get_calls);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S11_AC1_load_with_empty_store_yields_exact_defaults);
    RUN_TEST(S11_AC1_load_with_null_store_yields_exact_defaults);
    RUN_TEST(S11_AC1_load_with_wrong_size_blob_yields_defaults);
    RUN_TEST(S11_AC1_load_with_bad_magic_yields_defaults);
    RUN_TEST(S11_AC1_load_with_wrong_version_yields_defaults);
    RUN_TEST(S11_AC1_load_with_v2_blob_yields_defaults_not_a_migration);
    RUN_TEST(S11_AC1_load_with_v3_blob_yields_defaults_not_a_migration);
    RUN_TEST(S11_AC1_load_with_v4_blob_yields_defaults_not_a_migration);

    RUN_TEST(S11_AC2_round_trip_save_load_is_exact_including_calibration);
    RUN_TEST(S11_AC2_round_trip_preserves_exact_defaults);

    RUN_TEST(S11_AC3_quiet_now_same_day_window_is_inclusive_exclusive);
    RUN_TEST(S11_AC3_quiet_now_wraps_past_midnight_inclusive_exclusive);
    RUN_TEST(S11_AC3_quiet_now_equal_bounds_is_always_off);

    RUN_TEST(S11_AC4_water_tick_fires_at_interval);
    RUN_TEST(S11_AC4_water_tick_off_never_fires);
    RUN_TEST(S11_AC4_water_tick_resets_on_interval_change);
    RUN_TEST(S11_AC4_water_tick_silent_during_quiet_hours);
    RUN_TEST(S11_AC4_quiet_minutes_do_not_accrue);

    RUN_TEST(S11_AC6_save_issues_exactly_one_store_write);
    RUN_TEST(S11_AC6_load_does_not_write_to_store);

    return UNITY_END();
}
