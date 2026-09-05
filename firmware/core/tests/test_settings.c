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

#include "support/mock_store.h"

void setUp(void) {}
void tearDown(void) {}

/* Mock ff_store_t (mock_store_io_t/mock_store_reset/mock_store_vtable):
 * shared support/mock_store.h (debt/test-harness PR) — a single
 * fixed-size slot plus get/set write counters, so tests can assert both
 * persisted content and write amplification (AC6) without any real I/O.
 * Previously hand-rolled here and independently in test_wall.c, which
 * had already diverged from this file's own shape (no counters, a
 * different key-copy call, a different return convention) — see the
 * header's own doc comment for the superset reasoning. */

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
    /* S21 §5: the default touch cal is IDENTITY (correct nothing) with
     * touch_calibrated=false — a fresh puck genuinely has NOT been calibrated,
     * so "uncalibrated / correct nothing" is the honest default. We do NOT bake
     * any specific unit's measured affine in as everyone's default; the owner
     * runs the in-app CALIBRATE TOUCH row to install a per-unit fit (persisted
     * to NVS). See ff_settings.c's ff_settings_apply_defaults for the rationale. */
    TEST_ASSERT_FALSE(s->touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s->touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s->touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s->touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s->touch_by);

    ff_geo_cal_t zero_cal;
    memset(&zero_cal, 0, sizeof(zero_cal));
    TEST_ASSERT_EQUAL_MEMORY(&zero_cal, &s->compass_cal, sizeof(ff_geo_cal_t));

    /* S21 amendment: default is 12-hour (the design vocabulary's mockup
     * form, e.g. "9:46 pm"), not 24-hour. */
    TEST_ASSERT_FALSE(s->clock_24h);

    /* format v8 amendment: default is NORMAL (not FLIPPED) — a fresh puck's
     * case orientation is unknown until the owner sets it. */
    TEST_ASSERT_FALSE(s->screen_flip);

    /* S27 sounds (format v9): sounds_on defaults TRUE (opt-out — the
     * one field on this struct whose honest default differs from "off",
     * see ff_settings.h's doc comment); ui_ticks defaults FALSE (opt-in). */
    TEST_ASSERT_TRUE(s->sounds_on);
    TEST_ASSERT_FALSE(s->ui_ticks);

    /* S12/S04 (format v10): the persisted crew roster defaults to the
     * EMPTY list — a fresh puck has paired no one yet. */
    TEST_ASSERT_EQUAL_UINT8(0, s->paired_count);
    for (size_t i = 0; i < FF_CREW_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, s->paired_ids[i]);
    }
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
 * NOT migrated across an incompatible layout. The touch-cal default is
 * identity (1·raw+0), so "reject, don't migrate" shows as: the loaded touch_ax
 * is the identity DEFAULT, never the v4 blob's own saved value. */
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
    saved.touch_ax = 1.0123f; /* a value distinct from the identity default, to prove it's not carried across */
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    uint16_t v4 = 4;
    memcpy(m.data + 4, &v4, sizeof(v4));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
    /* Reject-not-migrate: the v4 blob's saved 1.0123 is discarded; the
     * identity default stands instead (asserted by ff_assert_defaults above,
     * which now checks touch_ax == 1.0). */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.touch_ax);
}

/* v5-or-older still rejects outright (S21 amendment's v7 forward-migrates
 * ONLY v6 — see ff_settings.c's v7 comment for why the cutoff is exactly
 * there: v6 is the layout the real NVS store, S21 §4, actually shipped
 * with; anything older pre-dates that store entirely, so no fielded
 * device holds one and there is nothing genuine to preserve). Same
 * reject-not-migrate shape as the v2/v3/v4 tests above, one version
 * closer to the new v6 boundary — the case most likely to accidentally
 * slip through the v6-migration branch if its size/version guard were
 * ever loosened. */
static void S11_AC1_load_with_v5_blob_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t saved;
    memset(&saved, 0, sizeof(saved));
    saved.imperial = false;
    saved.water_min = 45;
    saved.touch_calibrated = true;
    saved.touch_ax = 1.0123f; /* a value distinct from the identity default, to prove it's not carried across */
    ff_settings_save(&saved, &st);
    TEST_ASSERT_TRUE(m.has_value);

    uint16_t v5 = 5;
    memcpy(m.data + 4, &v5, sizeof(v5));

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.touch_ax); /* discarded, not carried across */
}

/* v6 -> v7 FORWARD MIGRATION (S21 amendment, reviewer finding on the first
 * version of this PR): unlike every earlier bump, a v6 blob is NOT
 * discarded — S21 §4's real NVS store means fielded pucks hold a genuine
 * v6 blob today (brightness, touch calibration, unit preference, ...),
 * and this repo's own honest-data ruling ("a settings change must never
 * silently wipe a unit's stored calibration") applies. Builds a hand-made
 * v6-SHAPED blob (own local mirror of the frozen ff_settings_v6_t layout
 * — see ff_settings.c) with every field set to a real, non-default value,
 * loads it under v7, and asserts EVERY ONE of those values survives, plus
 * the field v6 never had (clock_24h) lands at its honest default. */
static void S21_v6_blob_forward_migrates_preserving_every_value(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    /* Establish a real header (correct magic) via the actual save() path —
     * same "don't hardcode the encoding" technique the wrong-version test
     * above uses — then overwrite version/payload_size and replace the
     * payload with a v6-SIZED (not v7-sized) one, exactly matching what a
     * real v6 firmware actually wrote to NVS. */
    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Independently declared here (not shared with ff_settings.c's private
     * ff_settings_v6_t) — a byte-for-byte mirror of that frozen v6 layout,
     * so a migration bug in EITHER copy is caught by comparing this test's
     * expectations against the production loader's real behavior, rather
     * than both silently drifting the same wrong way together. */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
    } v6_mirror_t;

    v6_mirror_t v6;
    memset(&v6, 0, sizeof(v6));
    v6.imperial = true; /* units: imperial */
    v6.share_mode = FF_SHARE_GHOST;
    v6.haptics = false;
    v6.night_glow = false;
    v6.water_min = 45;         /* != the 90 default */
    v6.quiet_from_min = 120;
    v6.quiet_to_min = 480;
    v6.utc_offset_min = -420;
    v6.utc_offset_set = true;
    v6.colorblind = true;
    v6.brightness_pct = 35;    /* != FF_BRIGHTNESS_DEFAULT_PCT */
    strncpy(v6.my_name, "Dana", sizeof(v6.my_name) - 1);
    v6.compass_cal.hard_offset = (ff_vec3_t){12.5f, -3.25f, 0.75f};
    v6.compass_cal.soft_scale[0] = 1.04f;
    v6.compass_cal.soft_scale[1] = 0.97f;
    v6.compass_cal.soft_scale[2] = 1.11f;
    v6.compass_cal.declination_deg = -6.5f;
    v6.cal_valid = true;
    v6.touch_calibrated = true; /* a real, calibrated unit */
    v6.touch_ax = 1.0123f;      /* non-identity affine */
    v6.touch_bx = -14.0f;
    v6.touch_ay = 1.0087f;
    v6.touch_by = -18.5f;

    /* Header layout: magic(4) | version(2) | payload_size(2) — see the
     * wrong-version test above. Overwrite version -> 6 and payload_size ->
     * this v6-shaped payload's own size, then replace the payload bytes,
     * and SHRINK the mock store's recorded length to match — a real v6 NVS
     * record was genuinely shorter than a v7 one (this is the "a v6 blob
     * is a strict byte-prefix of v7" fact the migration relies on). */
    uint16_t const v6_version = 6;
    memcpy(m.data + 4, &v6_version, sizeof(v6_version));
    uint16_t const v6_payload_size = (uint16_t)sizeof(v6);
    memcpy(m.data + 6, &v6_payload_size, sizeof(v6_payload_size));
    memcpy(m.data + 8, &v6, sizeof(v6));
    m.len = 8 + sizeof(v6);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    /* Every v6 value survives... */
    TEST_ASSERT_TRUE(s.imperial);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_GHOST, s.share_mode);
    TEST_ASSERT_FALSE(s.haptics);
    TEST_ASSERT_FALSE(s.night_glow);
    TEST_ASSERT_EQUAL_UINT16(45, s.water_min);
    TEST_ASSERT_EQUAL_UINT16(120, s.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(480, s.quiet_to_min);
    TEST_ASSERT_EQUAL_INT16(-420, s.utc_offset_min);
    TEST_ASSERT_TRUE(s.utc_offset_set);
    TEST_ASSERT_TRUE(s.colorblind);
    TEST_ASSERT_EQUAL_UINT8(35, s.brightness_pct);
    TEST_ASSERT_EQUAL_STRING("Dana", s.my_name);
    TEST_ASSERT_TRUE(s.cal_valid);
    TEST_ASSERT_EQUAL_MEMORY(&v6.compass_cal, &s.compass_cal, sizeof(ff_geo_cal_t));
    TEST_ASSERT_TRUE(s.touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.0123f, s.touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(-14.0f, s.touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(1.0087f, s.touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(-18.5f, s.touch_by);

    /* ...and the fields v6 never had land at their honest defaults —
     * clock_24h (the v6->v7 step), screen_flip (the v7->v8 step), and
     * sounds_on/ui_ticks (the v8->v9 step, S27 sounds) — a v6 blob chains
     * through ALL THREE steps to reach the live v9 struct. sounds_on is
     * the one field whose honest default is TRUE, not false — see
     * ff_settings.c's v9 migration comment. */
    TEST_ASSERT_FALSE(s.clock_24h);
    TEST_ASSERT_FALSE(s.screen_flip);
    TEST_ASSERT_TRUE(s.sounds_on);
    TEST_ASSERT_FALSE(s.ui_ticks);
}

/* v7 -> v8 FORWARD MIGRATION (format v8 amendment, maintainer ask,
 * 2026-09-02): same shape as the v6->v7 test above, one version hop
 * later. A v7 blob (the format S21's clock_24h amendment shipped, and
 * the one every fielded puck flashed since holds) is NOT discarded —
 * every v7 value must survive, and only the field v7 never had
 * (screen_flip) lands at its honest default (false / NORMAL). Builds a
 * hand-made v7-SHAPED blob (own local mirror of the frozen
 * ff_settings_v7_t layout — see ff_settings.c) with every field set to a
 * real, non-default value. */
static void S21_v7_blob_forward_migrates_preserving_every_value(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Independently declared here (not shared with ff_settings.c's private
     * ff_settings_v7_t) — same "two independent copies must agree" reasoning
     * as the v6 mirror above. */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
    } v7_mirror_t;

    v7_mirror_t v7;
    memset(&v7, 0, sizeof(v7));
    v7.imperial = false;
    v7.share_mode = FF_SHARE_ZONES;
    v7.haptics = true;
    v7.night_glow = true;
    v7.water_min = 60;
    v7.quiet_from_min = 60;
    v7.quiet_to_min = 300;
    v7.utc_offset_min = 330;
    v7.utc_offset_set = true;
    v7.colorblind = false;
    v7.brightness_pct = 88;
    strncpy(v7.my_name, "Kai", sizeof(v7.my_name) - 1);
    v7.compass_cal.hard_offset = (ff_vec3_t){-2.0f, 4.5f, 0.1f};
    v7.compass_cal.soft_scale[0] = 0.99f;
    v7.compass_cal.soft_scale[1] = 1.02f;
    v7.compass_cal.soft_scale[2] = 0.95f;
    v7.compass_cal.declination_deg = 3.25f;
    v7.cal_valid = true;
    v7.touch_calibrated = true;
    v7.touch_ax = 0.994f;
    v7.touch_bx = 6.0f;
    v7.touch_ay = 1.006f;
    v7.touch_by = 9.5f;
    v7.clock_24h = true; /* a real, non-default value only v7+ could hold */

    uint16_t const v7_version = 7;
    memcpy(m.data + 4, &v7_version, sizeof(v7_version));
    uint16_t const v7_payload_size = (uint16_t)sizeof(v7);
    memcpy(m.data + 6, &v7_payload_size, sizeof(v7_payload_size));
    memcpy(m.data + 8, &v7, sizeof(v7));
    m.len = 8 + sizeof(v7);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    /* Every v7 value survives... */
    TEST_ASSERT_FALSE(s.imperial);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_ZONES, s.share_mode);
    TEST_ASSERT_TRUE(s.haptics);
    TEST_ASSERT_TRUE(s.night_glow);
    TEST_ASSERT_EQUAL_UINT16(60, s.water_min);
    TEST_ASSERT_EQUAL_UINT16(60, s.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(300, s.quiet_to_min);
    TEST_ASSERT_EQUAL_INT16(330, s.utc_offset_min);
    TEST_ASSERT_TRUE(s.utc_offset_set);
    TEST_ASSERT_FALSE(s.colorblind);
    TEST_ASSERT_EQUAL_UINT8(88, s.brightness_pct);
    TEST_ASSERT_EQUAL_STRING("Kai", s.my_name);
    TEST_ASSERT_TRUE(s.cal_valid);
    TEST_ASSERT_EQUAL_MEMORY(&v7.compass_cal, &s.compass_cal, sizeof(ff_geo_cal_t));
    TEST_ASSERT_TRUE(s.touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(0.994f, s.touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, s.touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(1.006f, s.touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(9.5f, s.touch_by);
    TEST_ASSERT_TRUE(s.clock_24h);

    /* ...and the fields v7 never had land at their honest defaults —
     * screen_flip (the v7->v8 step) and sounds_on/ui_ticks (the v8->v9
     * step, S27 sounds; a v7 blob chains through both). */
    TEST_ASSERT_FALSE(s.screen_flip);
    TEST_ASSERT_TRUE(s.sounds_on);
    TEST_ASSERT_FALSE(s.ui_ticks);
}

/* ---------------------------------------------------------------------
 * v6/v7 guard coverage (debt/test-harness PR): the forward-migration
 * branches above are gated on BOTH `hdr.payload_size == sizeof(vN)` AND
 * `got == sizeof(hdr) + sizeof(vN)` (ff_settings.c's own comment calls
 * this "each gated on BOTH its own version number and its own exact
 * documented payload size" — reject, not guess). Deleting either guard
 * term still passes every v2-v5/v6/v7-forward-migrate test above (none
 * of them exercises a version/size MISMATCH on the v6/v7 branches
 * specifically), so it's a live coverage gap a mutation on those two
 * `&&` terms would slip through undetected. The four tests below close
 * it, one per guard term per version, same "wrong payload_size" /
 * "truncated buffer" shape as each other:
 *   - wrong payload_size: the STORE genuinely holds a full, correctly-
 *     sized vN payload (`got` is right), but the header's OWN
 *     payload_size field lies about it — isolates the payload_size
 *     term, since the got-length term alone would (wrongly) let this
 *     through if that term were the only guard left.
 *   - truncated buffer: the header's payload_size field correctly
 *     names sizeof(vN), but the store actually returns fewer bytes than
 *     that (a short/corrupt read) — isolates the got-length term the
 *     same way in reverse.
 * All four must yield the full default struct (ff_assert_defaults),
 * never a migrated blob — reject-not-migrate, same as the v2-v5 tests.
 * Named `S21_...` (no AC number), matching this file's own two sibling
 * v6/v7-migration tests just above: S21-settings-rework.md's amendments
 * (2026-09-02, clock_24h format v6->v7 / screen_flip format v7->v8) are
 * what introduced these two guarded branches and their "reject anything
 * that doesn't match" rule in the first place — S11-settings.md's own
 * AC1 covers load-with-defaults in general (the v2-v5 tests above are
 * S11_AC1_...) but does not itself enumerate the v6/v7 forward-migration
 * guard, so following the sibling tests' precedent rather than
 * retrofitting an S11 AC number that doesn't name this behavior. */
static void S21_v7_blob_wrong_payload_size_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Same v7_mirror_t shape as S21_v7_blob_forward_migrates_preserving_
     * every_value above (independently declared, same "two independent
     * copies must agree" reasoning). Filled with a non-zero/non-default
     * byte pattern throughout: if the payload_size guard were missing
     * and this were wrongly migrated, every field would read back as
     * this pattern rather than the honest defaults, so the assertion
     * below is a real proof, not a proxy that would pass even on a
     * partially-broken migration. */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
    } v7_mirror_t;

    v7_mirror_t v7;
    memset(&v7, 0x55, sizeof(v7));

    uint16_t const v7_version = 7;
    memcpy(m.data + 4, &v7_version, sizeof(v7_version));
    /* The lie: payload_size does NOT equal sizeof(v7_mirror_t), even
     * though the store below genuinely holds a full sizeof(v7_mirror_t)
     * payload (m.len matches it exactly) — isolates the payload_size
     * guard term from the got-length term. */
    uint16_t const wrong_payload_size = (uint16_t)(sizeof(v7) - 1u);
    memcpy(m.data + 6, &wrong_payload_size, sizeof(wrong_payload_size));
    memcpy(m.data + 8, &v7, sizeof(v7));
    m.len = 8 + sizeof(v7);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S21_v7_blob_truncated_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
    } v7_mirror_t;

    v7_mirror_t v7;
    memset(&v7, 0x66, sizeof(v7));

    uint16_t const v7_version = 7;
    memcpy(m.data + 4, &v7_version, sizeof(v7_version));
    /* The header's payload_size field is HONEST (names the real
     * sizeof(v7_mirror_t)) — isolates the got-length guard term: the
     * store below returns fewer bytes than that honest claim (a
     * short/corrupt read), which the got-length term must catch on its
     * own even though payload_size alone looks fine. */
    uint16_t const v7_payload_size = (uint16_t)sizeof(v7);
    memcpy(m.data + 6, &v7_payload_size, sizeof(v7_payload_size));
    memcpy(m.data + 8, &v7, sizeof(v7));
    m.len = 8 + sizeof(v7) - 4; /* short by 4 bytes — a truncated/corrupt read */

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S21_v6_blob_wrong_payload_size_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Same v6_mirror_t shape as S21_v6_blob_forward_migrates_preserving_
     * every_value above (independently declared). */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
    } v6_mirror_t;

    v6_mirror_t v6;
    memset(&v6, 0x55, sizeof(v6));

    uint16_t const v6_version = 6;
    memcpy(m.data + 4, &v6_version, sizeof(v6_version));
    uint16_t const wrong_payload_size = (uint16_t)(sizeof(v6) - 1u);
    memcpy(m.data + 6, &wrong_payload_size, sizeof(wrong_payload_size));
    memcpy(m.data + 8, &v6, sizeof(v6));
    m.len = 8 + sizeof(v6);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S21_v6_blob_truncated_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
    } v6_mirror_t;

    v6_mirror_t v6;
    memset(&v6, 0x66, sizeof(v6));

    uint16_t const v6_version = 6;
    memcpy(m.data + 4, &v6_version, sizeof(v6_version));
    uint16_t const v6_payload_size = (uint16_t)sizeof(v6);
    memcpy(m.data + 6, &v6_payload_size, sizeof(v6_payload_size));
    memcpy(m.data + 8, &v6, sizeof(v6));
    m.len = 8 + sizeof(v6) - 4; /* short by 4 bytes — a truncated/corrupt read */

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

/* v8 -> v9 FORWARD MIGRATION (S27 sounds, docs/specs/S27-sounds.md): same
 * shape as the v6->v7 / v7->v8 tests above, one version hop later. A v8
 * blob (the format S21's screen_flip amendment shipped, and the one every
 * fielded puck flashed since holds) is NOT discarded — every v8 value
 * must survive, and only the two fields v8 never had (sounds_on/ui_ticks)
 * land at THEIR OWN honest defaults (true/false respectively — see
 * ff_settings.h's doc comments and ff_settings.c's v9 migration comment
 * for why sounds_on differs from every earlier migrated-in field's
 * "lands at false"). Builds a hand-made v8-SHAPED blob (own local mirror
 * of the frozen ff_settings_v8_t layout — see ff_settings.c) with every
 * field set to a real, non-default value. */
static void S27_v8_blob_forward_migrates_preserving_every_value(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Independently declared here (not shared with ff_settings.c's private
     * ff_settings_v8_t) — same "two independent copies must agree"
     * reasoning as the v6/v7 mirrors above. */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
    } v8_mirror_t;

    v8_mirror_t v8;
    memset(&v8, 0, sizeof(v8));
    v8.imperial = true;
    v8.share_mode = FF_SHARE_LIVE;
    v8.haptics = false;
    v8.night_glow = true;
    v8.water_min = 120;
    v8.quiet_from_min = 90;
    v8.quiet_to_min = 400;
    v8.utc_offset_min = 60;
    v8.utc_offset_set = true;
    v8.colorblind = true;
    v8.brightness_pct = 42;
    strncpy(v8.my_name, "Riley", sizeof(v8.my_name) - 1);
    v8.compass_cal.hard_offset = (ff_vec3_t){1.0f, -2.0f, 3.0f};
    v8.compass_cal.soft_scale[0] = 1.01f;
    v8.compass_cal.soft_scale[1] = 0.98f;
    v8.compass_cal.soft_scale[2] = 1.02f;
    v8.compass_cal.declination_deg = 2.0f;
    v8.cal_valid = true;
    v8.touch_calibrated = true;
    v8.touch_ax = 1.02f;
    v8.touch_bx = 3.0f;
    v8.touch_ay = 0.99f;
    v8.touch_by = -1.5f;
    v8.clock_24h = true;
    v8.screen_flip = true; /* a real, non-default value only v8+ could hold */

    uint16_t const v8_version = 8;
    memcpy(m.data + 4, &v8_version, sizeof(v8_version));
    uint16_t const v8_payload_size = (uint16_t)sizeof(v8);
    memcpy(m.data + 6, &v8_payload_size, sizeof(v8_payload_size));
    memcpy(m.data + 8, &v8, sizeof(v8));
    m.len = 8 + sizeof(v8);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    /* Every v8 value survives... */
    TEST_ASSERT_TRUE(s.imperial);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_LIVE, s.share_mode);
    TEST_ASSERT_FALSE(s.haptics);
    TEST_ASSERT_TRUE(s.night_glow);
    TEST_ASSERT_EQUAL_UINT16(120, s.water_min);
    TEST_ASSERT_EQUAL_UINT16(90, s.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(400, s.quiet_to_min);
    TEST_ASSERT_EQUAL_INT16(60, s.utc_offset_min);
    TEST_ASSERT_TRUE(s.utc_offset_set);
    TEST_ASSERT_TRUE(s.colorblind);
    TEST_ASSERT_EQUAL_UINT8(42, s.brightness_pct);
    TEST_ASSERT_EQUAL_STRING("Riley", s.my_name);
    TEST_ASSERT_TRUE(s.cal_valid);
    TEST_ASSERT_EQUAL_MEMORY(&v8.compass_cal, &s.compass_cal, sizeof(ff_geo_cal_t));
    TEST_ASSERT_TRUE(s.touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.02f, s.touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(0.99f, s.touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, s.touch_by);
    TEST_ASSERT_TRUE(s.clock_24h);
    TEST_ASSERT_TRUE(s.screen_flip);

    /* ...and the two fields v8 never had land at THEIR OWN honest
     * defaults — sounds_on=TRUE (opt-out), ui_ticks=FALSE (opt-in). This
     * is the mutation target (task's mutation (c)): drop the ui_ticks
     * default in the v9 migration and this assertion fails. */
    TEST_ASSERT_TRUE(s.sounds_on);
    TEST_ASSERT_FALSE(s.ui_ticks);
}

/* v6/v7/v8 guard coverage (same "each branch gated on BOTH its own
 * version number and its own exact documented payload size" reasoning
 * the v6/v7 guard tests above already establish) — the two tests below
 * close the same live coverage gap for the NEW v8->v9 branch. */
static void S27_v8_blob_wrong_payload_size_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
    } v8_mirror_t;

    v8_mirror_t v8;
    memset(&v8, 0x55, sizeof(v8));

    uint16_t const v8_version = 8;
    memcpy(m.data + 4, &v8_version, sizeof(v8_version));
    /* The lie: payload_size does NOT equal sizeof(v8_mirror_t), even
     * though the store below genuinely holds a full sizeof(v8_mirror_t)
     * payload — isolates the payload_size guard term from the
     * got-length term. */
    uint16_t const wrong_payload_size = (uint16_t)(sizeof(v8) - 1u);
    memcpy(m.data + 6, &wrong_payload_size, sizeof(wrong_payload_size));
    memcpy(m.data + 8, &v8, sizeof(v8));
    m.len = 8 + sizeof(v8);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S27_v8_blob_truncated_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
    } v8_mirror_t;

    v8_mirror_t v8;
    memset(&v8, 0x66, sizeof(v8));

    uint16_t const v8_version = 8;
    memcpy(m.data + 4, &v8_version, sizeof(v8_version));
    /* The header's payload_size field is HONEST (names the real
     * sizeof(v8_mirror_t)) — isolates the got-length guard term: the
     * store below returns fewer bytes than that honest claim. */
    uint16_t const v8_payload_size = (uint16_t)sizeof(v8);
    memcpy(m.data + 6, &v8_payload_size, sizeof(v8_payload_size));
    memcpy(m.data + 8, &v8, sizeof(v8));
    m.len = 8 + sizeof(v8) - 4; /* short by 4 bytes — a truncated/corrupt read */

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

/* v9 -> v10 FORWARD MIGRATION (S12/S04 — the persisted crew roster;
 * docs/specs/S04-firefly-protocol.md's "pairing v1 = channel membership +
 * explicit crew list"). Same shape as the v7->v8 / v8->v9 tests above, one
 * version hop later. A v9 blob (the format S27 sounds shipped, and the one
 * every fielded puck flashed since holds) is NOT discarded — every v9
 * value must survive, and the two fields v9 never had (paired_ids/
 * paired_count) land at the EMPTY list — a v9 puck genuinely never
 * persisted a crew, so "nobody yet" is the honest reading, not a guess. */
static void S12_v9_blob_forward_migrates_preserving_every_value(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    /* Independently declared here (not shared with ff_settings.c's private
     * ff_settings_v9_t) — same "two independent copies must agree"
     * reasoning as the v6/v7/v8 mirrors above. */
    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
        bool sounds_on;
        bool ui_ticks;
    } v9_mirror_t;

    v9_mirror_t v9;
    memset(&v9, 0, sizeof(v9));
    v9.imperial = false;
    v9.share_mode = FF_SHARE_GHOST;
    v9.haptics = true;
    v9.night_glow = false;
    v9.water_min = 45;
    v9.quiet_from_min = 10;
    v9.quiet_to_min = 500;
    v9.utc_offset_min = -300;
    v9.utc_offset_set = true;
    v9.colorblind = true;
    v9.brightness_pct = 88;
    strncpy(v9.my_name, "Dana", sizeof(v9.my_name) - 1);
    v9.compass_cal.hard_offset = (ff_vec3_t){0.5f, 0.25f, -0.1f};
    v9.compass_cal.soft_scale[0] = 1.0f;
    v9.compass_cal.soft_scale[1] = 1.0f;
    v9.compass_cal.soft_scale[2] = 1.0f;
    v9.cal_valid = true;
    v9.touch_calibrated = true;
    v9.touch_ax = 1.1f;
    v9.touch_bx = 2.2f;
    v9.touch_ay = 0.9f;
    v9.touch_by = -3.3f;
    v9.clock_24h = true;
    v9.screen_flip = true;
    v9.sounds_on = false; /* a real, non-default value only v9+ could hold */
    v9.ui_ticks = true;   /* a real, non-default value only v9+ could hold */

    uint16_t const v9_version = 9;
    memcpy(m.data + 4, &v9_version, sizeof(v9_version));
    uint16_t const v9_payload_size = (uint16_t)sizeof(v9);
    memcpy(m.data + 6, &v9_payload_size, sizeof(v9_payload_size));
    memcpy(m.data + 8, &v9, sizeof(v9));
    m.len = 8 + sizeof(v9);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    /* Every v9 value survives... */
    TEST_ASSERT_FALSE(s.imperial);
    TEST_ASSERT_EQUAL_UINT8(FF_SHARE_GHOST, s.share_mode);
    TEST_ASSERT_TRUE(s.haptics);
    TEST_ASSERT_FALSE(s.night_glow);
    TEST_ASSERT_EQUAL_UINT16(45, s.water_min);
    TEST_ASSERT_EQUAL_UINT16(10, s.quiet_from_min);
    TEST_ASSERT_EQUAL_UINT16(500, s.quiet_to_min);
    TEST_ASSERT_EQUAL_INT16(-300, s.utc_offset_min);
    TEST_ASSERT_TRUE(s.utc_offset_set);
    TEST_ASSERT_TRUE(s.colorblind);
    TEST_ASSERT_EQUAL_UINT8(88, s.brightness_pct);
    TEST_ASSERT_EQUAL_STRING("Dana", s.my_name);
    TEST_ASSERT_TRUE(s.cal_valid);
    TEST_ASSERT_EQUAL_MEMORY(&v9.compass_cal, &s.compass_cal, sizeof(ff_geo_cal_t));
    TEST_ASSERT_TRUE(s.touch_calibrated);
    TEST_ASSERT_EQUAL_FLOAT(1.1f, s.touch_ax);
    TEST_ASSERT_EQUAL_FLOAT(2.2f, s.touch_bx);
    TEST_ASSERT_EQUAL_FLOAT(0.9f, s.touch_ay);
    TEST_ASSERT_EQUAL_FLOAT(-3.3f, s.touch_by);
    TEST_ASSERT_TRUE(s.clock_24h);
    TEST_ASSERT_TRUE(s.screen_flip);
    TEST_ASSERT_FALSE(s.sounds_on);
    TEST_ASSERT_TRUE(s.ui_ticks);

    /* ...and the two fields v9 never had land at the EMPTY list — this is
     * the mutation target: drop the zeroing in ff_settings_migrate_v9 (or
     * skip it entirely) and this assertion fails. */
    TEST_ASSERT_EQUAL_UINT8(0, s.paired_count);
    for (size_t i = 0; i < FF_CREW_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, s.paired_ids[i]);
    }
}

static void S12_v9_blob_wrong_payload_size_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
        bool sounds_on;
        bool ui_ticks;
    } v9_mirror_t;

    v9_mirror_t v9;
    memset(&v9, 0x55, sizeof(v9));

    uint16_t const v9_version = 9;
    memcpy(m.data + 4, &v9_version, sizeof(v9_version));
    uint16_t const wrong_payload_size = (uint16_t)(sizeof(v9) - 1u);
    memcpy(m.data + 6, &wrong_payload_size, sizeof(wrong_payload_size));
    memcpy(m.data + 8, &v9, sizeof(v9));
    m.len = 8 + sizeof(v9);

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

static void S12_v9_blob_truncated_yields_defaults_not_a_migration(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t seed;
    memset(&seed, 0, sizeof(seed));
    ff_settings_save(&seed, &st);
    TEST_ASSERT_TRUE(m.has_value);

    typedef struct {
        bool imperial;
        uint8_t share_mode;
        bool haptics;
        bool night_glow;
        uint16_t water_min;
        uint16_t quiet_from_min;
        uint16_t quiet_to_min;
        int16_t utc_offset_min;
        bool utc_offset_set;
        bool colorblind;
        uint8_t brightness_pct;
        char my_name[FF_SETTINGS_NAME_LEN];
        ff_geo_cal_t compass_cal;
        bool cal_valid;
        float touch_ax;
        float touch_bx;
        float touch_ay;
        float touch_by;
        bool touch_calibrated;
        bool clock_24h;
        bool screen_flip;
        bool sounds_on;
        bool ui_ticks;
    } v9_mirror_t;

    v9_mirror_t v9;
    memset(&v9, 0x66, sizeof(v9));

    uint16_t const v9_version = 9;
    memcpy(m.data + 4, &v9_version, sizeof(v9_version));
    uint16_t const v9_payload_size = (uint16_t)sizeof(v9);
    memcpy(m.data + 6, &v9_payload_size, sizeof(v9_payload_size));
    memcpy(m.data + 8, &v9, sizeof(v9));
    m.len = 8 + sizeof(v9) - 4; /* short by 4 bytes — a truncated/corrupt read */

    ff_settings_t s;
    memset(&s, 0xAA, sizeof(s));
    ff_settings_load(&s, &st);

    ff_assert_defaults(&s);
}

/* S12/S04 AC — the v10 round trip: paired_ids/paired_count survive a
 * plain save/load through the CURRENT (v10) format, in order, with no
 * truncation up to FF_CREW_MAX. This is the mutation target for "drop the
 * boot re-pair" style regressions one layer down (ff_shell) — this test
 * only proves the persistence layer itself carries the list faithfully. */
static void S12_AC_v10_round_trip_preserves_paired_list(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t out;
    memset(&out, 0, sizeof(out));
    out.paired_count = FF_CREW_MAX;
    for (uint8_t i = 0; i < FF_CREW_MAX; i++) {
        out.paired_ids[i] = 1000u + i;
    }
    ff_settings_save(&out, &st);

    ff_settings_t in;
    memset(&in, 0xAA, sizeof(in));
    ff_settings_load(&in, &st);

    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, in.paired_count);
    for (uint8_t i = 0; i < FF_CREW_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(1000u + i, in.paired_ids[i]);
    }
}

/**
 * S12/S04 — a CORRUPT (but otherwise well-formed: right magic/version/
 * exact payload size) v10 blob whose `paired_count` claims more than
 * FF_CREW_MAX must be CLAMPED, not trusted verbatim — `paired_ids` is a
 * fixed FF_CREW_MAX-element array, and any consumer that loops
 * `for (i = 0; i < s->paired_count; i++) s->paired_ids[i]` (ff_shell.c's
 * boot re-pair does exactly this) would read past the array's own bound
 * on a corrupt count this size never trusted. Directly at the settings
 * layer, not through the shell: a shell-level test alone does not kill
 * this mutation, because `ff_crew_upsert` independently refuses a 9th+
 * distinct id once the roster is full — the roster still ends up at the
 * correct FF_CREW_MAX size even while the shell's snapshot copy is being
 * read out of bounds one element at a time, so the visible SYMPTOM this
 * test targets never surfaces one layer up (the AGENTS.md "proxy check"
 * lesson, applied here rather than found the expensive way later).
 */
static void S12_load_clamps_a_corrupt_paired_count_past_FF_CREW_MAX(void)
{
    mock_store_io_t m;
    mock_store_reset(&m);
    ff_store_t st = mock_store_vtable(&m);

    ff_settings_t out;
    memset(&out, 0, sizeof(out));
    for (uint8_t i = 0; i < FF_CREW_MAX; i++) {
        out.paired_ids[i] = 5000u + i;
    }
    out.paired_count = 200u; /* corrupt: claims far more than the array holds */
    ff_settings_save(&out, &st);

    ff_settings_t in;
    memset(&in, 0xAA, sizeof(in));
    ff_settings_load(&in, &st);

    /* The mutation target: drop ff_settings_load's clamp and this fails
     * (in.paired_count reads back as the corrupt 200, not 8). */
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, in.paired_count);
    for (uint8_t i = 0; i < FF_CREW_MAX; i++) {
        TEST_ASSERT_EQUAL_UINT32(5000u + i, in.paired_ids[i]);
    }
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
    out.clock_24h = true; /* S21 amendment: a real change from the default, not left at its zero value */
    out.screen_flip = true; /* format v8 amendment: a real change from the default, not left at its zero value */
    out.sounds_on = false; /* S27 (format v9): a real change from the default (true), not left at its zero value */
    out.ui_ticks = true;   /* S27 (format v9): a real change from the default (false), not left at its zero value */
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
    RUN_TEST(S11_AC1_load_with_v5_blob_yields_defaults_not_a_migration);
    RUN_TEST(S21_v6_blob_forward_migrates_preserving_every_value);
    RUN_TEST(S21_v7_blob_forward_migrates_preserving_every_value);
    RUN_TEST(S21_v7_blob_wrong_payload_size_yields_defaults_not_a_migration);
    RUN_TEST(S21_v7_blob_truncated_yields_defaults_not_a_migration);
    RUN_TEST(S21_v6_blob_wrong_payload_size_yields_defaults_not_a_migration);
    RUN_TEST(S21_v6_blob_truncated_yields_defaults_not_a_migration);
    RUN_TEST(S27_v8_blob_forward_migrates_preserving_every_value);
    RUN_TEST(S27_v8_blob_wrong_payload_size_yields_defaults_not_a_migration);
    RUN_TEST(S27_v8_blob_truncated_yields_defaults_not_a_migration);
    RUN_TEST(S12_v9_blob_forward_migrates_preserving_every_value);
    RUN_TEST(S12_v9_blob_wrong_payload_size_yields_defaults_not_a_migration);
    RUN_TEST(S12_v9_blob_truncated_yields_defaults_not_a_migration);
    RUN_TEST(S12_AC_v10_round_trip_preserves_paired_list);
    RUN_TEST(S12_load_clamps_a_corrupt_paired_count_past_FF_CREW_MAX);

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
