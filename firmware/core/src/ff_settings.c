#include "ff_settings.h"

#include <string.h>

/* Persisted-blob framing: a small versioned header in front of the raw
 * ff_settings_t bytes so ff_settings_load can tell a valid blob from a
 * corrupt one or one written by an incompatible (old/new) struct layout,
 * and fall back to exact defaults rather than trusting garbage. */
#define FF_SETTINGS_STORE_KEY "ff.settings"
#define FF_SETTINGS_MAGIC ((uint32_t)0x46465331u) /* ASCII "FFS1" */
/* Bump on ANY change to ff_settings_t's layout — field add/remove/reorder/
 * retype — not just changes that alter sizeof(). The payload_size check
 * below only catches size mismatches; two different layouts can share the
 * same sizeof() (e.g. a reordering, or swapping a bool+uint8_t pair) and
 * would otherwise pass validation with silently corrupted semantics. */
#define FF_SETTINGS_FORMAT_VERSION ((uint16_t)8u)
/* v2: compass_cal_blob (opaque uint8_t[32]) -> compass_cal (ff_geo_cal_t).
 * Same sizeof() risk the header comment warns about (a reordering/retype
 * can share sizeof() with the old layout) doesn't apply numerically here
 * — ff_geo_cal_t is 28 bytes vs. the blob's 32 — but the version bump is
 * required regardless: this is a field type/semantics change, not just a
 * size change. */
/* v3: + utc_offset_min / utc_offset_set (S16 slice b0's [api] amendment
 * to S11 — see ff_settings.h). A v2 blob is rejected on load and the
 * full defaults apply, which is the honest outcome here: the new field
 * has no v2 representation to migrate from, and "unset" is exactly what
 * a v2 puck's real state was. Everything else in a v2 blob is user
 * preference that is cheap to re-set, and this is pre-v1 firmware with
 * no fielded devices. */
/* v4: + colorblind (S17 slice a's [api] amendment to S11 — see
 * ff_settings.h). Same rejection-not-migration policy as v3: a v3 blob
 * is refused outright and the full defaults apply (colorblind defaults
 * false, which is exactly what a pre-S17 puck's real state was — it had
 * no colorblind toggle at all, so "off" is the honest reading, not a
 * guess). Still pre-v1 firmware, no fielded devices to migrate. */
/* v5: + touch_ax/bx/ay/by + touch_calibrated (S15 slice d's [api]
 * amendment to S11 — the touch-calibration affine, see ff_settings.h and
 * ff_touchcal.h). Same rejection-not-migration policy as v3/v4: a v4 blob
 * is refused outright and the full defaults apply (touch_calibrated
 * defaults false -> identity transform, which is exactly what a pre-S15d
 * puck's real state was — it had no touch calibration at all, so
 * "uncalibrated / correct nothing" is the honest reading, not a guess).
 * Still pre-v1 firmware, no fielded devices to migrate. */
/* v6: + brightness_pct (#100 — the PWM-backlight brightness setting; see
 * ff_settings.h). Merges with v5 above: the combined-layout bump is required
 * because v5 (touch cal, already on main) and this brightness field are a
 * different struct layout — same version number would let a v5 blob be
 * misread, so v6 rejects it. brightness_pct defaults to
 * FF_BRIGHTNESS_DEFAULT_PCT (~70%), a sensible mid-bright start — a pre-#100
 * puck had a fixed full-on backlight and no brightness concept, so there is
 * no honest legacy value to migrate. Still pre-v1 firmware, no fielded
 * devices. */
/* v7: + clock_24h (S21 amendment — the Settings CLOCK 12H|24H toggle; see
 * ff_settings.h). UNLIKE v2-v6 above, this bump FORWARD-MIGRATES a v6 blob
 * instead of discarding it: S21 slice 4 (already merged) wired the real
 * NVS-backed store, so fielded pucks now hold a real v6 blob — brightness,
 * touch calibration, unit preference — and the pre-v1 "no fielded devices
 * to migrate" premise every earlier bump relied on no longer holds. This
 * repo's own honest-data ruling ("a settings change must never silently
 * wipe a unit's stored calibration") applies here for the first time.
 * clock_24h is a single bool appended at the very end of ff_settings_t —
 * every earlier field's offset is unchanged — so a v6 blob's payload is a
 * byte-for-byte prefix of a v7 one; ff_settings_load reads it with the
 * frozen ff_settings_v6_t shadow below and fills clock_24h in at its
 * honest default (false — a v6 puck genuinely had no clock-format toggle,
 * so 12-hour is the correct reading, not a guess). A blob OLDER than v6
 * (<=v5) still rejects outright: those pre-date the NVS store (S21 §4)
 * shipping at all, so there is no fielded device whose real persisted
 * state that rule could destroy — the "no fielded devices to migrate"
 * premise stated for v3-v6 above is still true for anything that old. */
/* v8: + screen_flip (maintainer ask, 2026-09-02 — the Fusion case's
 * SCREEN NORMAL|FLIPPED row; see ff_settings.h's doc comment on the
 * field for the hardware-mirror/touch-flip/glass-centre mechanism this
 * one flag drives). SAME forward-migration policy v7 established, one
 * hop further: fielded pucks now hold real v6 AND v7 blobs (v7 shipped
 * with this same NVS store, S21 §4), and this repo's honest-data ruling
 * ("never silently wipe a unit's stored calibration") applies to both.
 * screen_flip is a single bool appended at the very end of ff_settings_t
 * again, so — same "byte-for-byte prefix" fact v7's migration relied on
 * — a v7 blob's payload is a strict prefix of a v8 one, and (chained
 * through v7) so is a v6 blob's.
 *
 * ff_settings_load reads a v7 blob via the newly-frozen
 * `ff_settings_v7_t` shadow below and fills screen_flip in at its honest
 * default (false — a v7 puck genuinely had no flip toggle, so NORMAL is
 * the correct reading, not a guess). A v6 blob CHAINS: it is first
 * migrated into an `ff_settings_v7_t` (the exact same
 * `ff_settings_migrate_v6` this build already had, just retargeted to
 * write the v7 shadow instead of the live struct directly), and that
 * intermediate v7 shape is then run through the SAME v7->v8 step every
 * real v7 blob takes — one migration function per version hop, composed,
 * not a v6->v8 special case that could drift from what a real two-step
 * upgrade does. A blob OLDER than v6 (<=v5) still rejects outright, same
 * boundary as before: those pre-date the NVS store entirely, so no
 * fielded device holds one. */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size; /* sizeof(ff_settings_t) at write time for the
                             * CURRENT version, or the frozen size of
                             * whichever older vN shadow (ff_settings_v6_t /
                             * ff_settings_v7_t) a migrated blob was written
                             * as */
} ff_settings_header_t;

#define FF_SETTINGS_BLOB_LEN (sizeof(ff_settings_header_t) + sizeof(ff_settings_t))

/**
 * ff_settings_v6_t — a FROZEN, byte-for-byte mirror of ff_settings_t exactly
 * as it existed at format version 6 (i.e. every field ff_settings_t has
 * TODAY except the v7 amendment's trailing `clock_24h`, in the same order).
 * Exists ONLY so ff_settings_load can forward-migrate a real v6 blob — see
 * the v7 comment above for why that migration is required now (S21 §4's NVS
 * store means fielded pucks hold real v6 blobs today).
 *
 * Deliberately NOT derived from ff_settings_t (e.g. via some "all fields but
 * the last" trick) — it is its own independent, hand-written type. A LATER
 * reorder of ff_settings_t's live fields (however unlikely; nothing in this
 * codebase does that) must never silently change what this migration reads,
 * because this struct has to keep matching what a real v6 firmware actually
 * wrote to NVS, forever, regardless of what ff_settings_t looks like by
 * then. If a future version bump also needs a migration, that bump adds its
 * OWN frozen vN shadow the same way — this one never changes again. */
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
} ff_settings_v6_t;

/**
 * ff_settings_v7_t — a FROZEN, byte-for-byte mirror of ff_settings_t
 * exactly as it existed at format version 7 (every field ff_settings_v6_t
 * has, plus v7's own trailing `clock_24h`; every field ff_settings_t has
 * TODAY except the v8 amendment's trailing `screen_flip`). Same rationale
 * and same rule as ff_settings_v6_t above: deliberately its own
 * independent, hand-written type (not derived from the live struct), so
 * a later reorder of ff_settings_t's live fields can't silently change
 * what a v7-blob migration reads — this struct has to keep matching what
 * real v7 firmware actually wrote to NVS, forever. It doubles as the
 * INTERMEDIATE shape a v6 blob is migrated through on its way to v8 (see
 * ff_settings_migrate_v6 below) — the one v7 shape, never duplicated for
 * "a v6 blob's v7 stage" vs. "a real v7 blob". If a future version bump
 * needs its own migration, it adds its own frozen vN shadow the same
 * way; this one never changes again. */
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
} ff_settings_v7_t;

static void ff_settings_apply_defaults(ff_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->imperial = true;
    s->share_mode = FF_SHARE_LIVE;
    s->haptics = true;
    s->night_glow = true;
    s->water_min = 90;
    s->quiet_from_min = 240; /* 4:00a */
    s->quiet_to_min = 600;   /* 10:00a */
    s->brightness_pct = FF_BRIGHTNESS_DEFAULT_PCT; /* #100: ~70%, a sensible mid-bright default */
    /* utc_offset_min / utc_offset_set: left zeroed -> UNSET. Deliberately
     * not defaulted to any real zone: an unset offset makes the wall
     * clock read FF_WALL_UNKNOWN (honest), while a defaulted one would
     * silently outrank nothing and quietly produce a wrong local time on
     * a puck that was never configured. See ff_wall.h. */
    /* my_name: left zeroed -> empty string. */
    /* compass_cal: left zeroed -> identity ff_geo_cal_t; cal_valid stays false. */
    /* colorblind: left zeroed -> false (brand palette). S17's own scoping
     * note: "not colorblind by default — keep the brand colours". */

    /* Touch calibration (S21 §5) — the default is IDENTITY (correct nothing),
     * and touch_calibrated is false, because a freshly-flashed puck genuinely
     * has NOT been calibrated: "uncalibrated / correct nothing" is the honest
     * reading, not a guess. We deliberately do NOT bake a specific unit's
     * measured affine (e.g. board 2's) in as everyone's default — that would
     * make touch_calibrated=true a lie ("a usable transform is installed" is
     * not "THIS unit was calibrated"), and would apply one panel's correction
     * to a possibly-different panel, which is worse than honest raw passthrough.
     *
     * Raw coordinates are close enough to operate the UI (the observed panel
     * skew is a ~15px offset, well inside a 44px hit target), so the owner can
     * reach the S21 in-app CALIBRATE TOUCH row (FF_INTENT_CALIBRATE_TOUCH) and
     * run the crosshair flow if/when they want a refined fit; NVS then persists
     * that unit's own transform. The reject-not-migrate policy still applies —
     * a stale/foreign blob falls back to THIS identity default (honest raw),
     * never to a migrated guess from an incompatible layout. */
    s->touch_ax = 1.0f;
    s->touch_bx = 0.0f;
    s->touch_ay = 1.0f;
    s->touch_by = 0.0f;
    s->touch_calibrated = false;

    /* clock_24h: left zeroed -> false (12-hour, with a lowercase am/pm
     * suffix — the design vocabulary's mockup form). See ff_settings.h's
     * doc comment on the field. */

    /* screen_flip: left zeroed -> false (NORMAL). See ff_settings.h's
     * doc comment on the field. */
}

/* v6 -> v7 forward migration step (see the v8 comment above
 * `ff_settings_header_t` for why this now targets the v7 SHADOW rather
 * than the live struct directly — it's the first half of a chained v6 ->
 * v7 -> v8 upgrade). Copies every v6 field across EXPLICITLY, field by
 * field — not a single memcpy of the whole struct — so a future reorder
 * of either struct's field layout can't silently misalign this
 * migration; the compiler catches a missing/renamed field as a normal
 * member-access error at the call site below. clock_24h, which v6 never
 * had, is set to its honest default; screen_flip does not exist at this
 * shape yet (it's ff_settings_migrate_v7's field to fill, one step
 * later). */
static void ff_settings_migrate_v6(ff_settings_v7_t *v7, ff_settings_v6_t const *v6)
{
    v7->imperial = v6->imperial;
    v7->share_mode = v6->share_mode;
    v7->haptics = v6->haptics;
    v7->night_glow = v6->night_glow;
    v7->water_min = v6->water_min;
    v7->quiet_from_min = v6->quiet_from_min;
    v7->quiet_to_min = v6->quiet_to_min;
    v7->utc_offset_min = v6->utc_offset_min;
    v7->utc_offset_set = v6->utc_offset_set;
    v7->colorblind = v6->colorblind;
    v7->brightness_pct = v6->brightness_pct;
    memcpy(v7->my_name, v6->my_name, sizeof(v7->my_name));
    v7->compass_cal = v6->compass_cal;
    v7->cal_valid = v6->cal_valid;
    v7->touch_ax = v6->touch_ax;
    v7->touch_bx = v6->touch_bx;
    v7->touch_ay = v6->touch_ay;
    v7->touch_by = v6->touch_by;
    v7->touch_calibrated = v6->touch_calibrated;
    v7->clock_24h = false; /* v6 never had this field: false (12-hour) is the
                             * honest reading of "this puck never had the
                             * toggle", not a guess — see ff_settings.h. */
}

/* v7 -> v8 forward migration step (see the v8 comment above
 * `ff_settings_header_t`). Same explicit field-by-field convention as
 * ff_settings_migrate_v6 above, same reason. Used for BOTH a real v7
 * blob (read straight off the store) and a v6 blob already lifted to
 * v7 shape by ff_settings_migrate_v6 above — one v7->v8 step, run once
 * either way, never duplicated. screen_flip, which v7 never had, is set
 * to its honest default. */
static void ff_settings_migrate_v7(ff_settings_t *s, ff_settings_v7_t const *v7)
{
    s->imperial = v7->imperial;
    s->share_mode = v7->share_mode;
    s->haptics = v7->haptics;
    s->night_glow = v7->night_glow;
    s->water_min = v7->water_min;
    s->quiet_from_min = v7->quiet_from_min;
    s->quiet_to_min = v7->quiet_to_min;
    s->utc_offset_min = v7->utc_offset_min;
    s->utc_offset_set = v7->utc_offset_set;
    s->colorblind = v7->colorblind;
    s->brightness_pct = v7->brightness_pct;
    memcpy(s->my_name, v7->my_name, sizeof(s->my_name));
    s->compass_cal = v7->compass_cal;
    s->cal_valid = v7->cal_valid;
    s->touch_ax = v7->touch_ax;
    s->touch_bx = v7->touch_bx;
    s->touch_ay = v7->touch_ay;
    s->touch_by = v7->touch_by;
    s->touch_calibrated = v7->touch_calibrated;
    s->clock_24h = v7->clock_24h;
    s->screen_flip = false; /* v7 never had this field: false (NORMAL) is the
                              * honest reading of "this puck never had the
                              * toggle", not a guess — see ff_settings.h. */
}

void ff_settings_load(ff_settings_t *s, ff_store_t const *st)
{
    if (s == NULL) {
        return;
    }

    ff_settings_apply_defaults(s);

    if (st == NULL || st->get == NULL) {
        return;
    }

    /* Sized to the LARGEST blob this build can read (today, v8's). A v6 or
     * v7 blob is smaller, so either fits the same buffer; `get` (ff_store_t's
     * documented contract) returns the value's ACTUAL stored length, which
     * can legitimately be shorter than the buffer's capacity — this is not
     * "short read" in the corrupt-data sense, it's a real, older, still-
     * valid record. See the three explicit per-version branches below; no
     * length is accepted silently, each is checked against the exact size
     * its claimed version is documented to be. */
    uint8_t buf[FF_SETTINGS_BLOB_LEN];
    int n = st->get(st->io, FF_SETTINGS_STORE_KEY, buf, sizeof(buf));
    if (n < 0 || (size_t)n < sizeof(ff_settings_header_t)) {
        /* Missing, failed, or too short to even hold a header -> defaults. */
        return;
    }
    size_t const got = (size_t)n;

    ff_settings_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != FF_SETTINGS_MAGIC) {
        return;
    }

    /* Explicit per-version step table (AGENTS.md: "no silent 'accept any
     * size'") — exactly three versions this build knows how to read, each
     * gated on BOTH its own version number and its own exact documented
     * payload size. Anything else (an unknown/future version, a <=v5 blob,
     * or a version/size combination that doesn't match any row) falls
     * through to the defaults ff_settings_apply_defaults already applied
     * above — reject, not guess. */
    if (hdr.version == FF_SETTINGS_FORMAT_VERSION && hdr.payload_size == (uint16_t)sizeof(ff_settings_t) &&
        got == sizeof(hdr) + sizeof(ff_settings_t)) {
        memcpy(s, buf + sizeof(hdr), sizeof(*s));
        return;
    }
    if (hdr.version == 7u && hdr.payload_size == (uint16_t)sizeof(ff_settings_v7_t) &&
        got == sizeof(hdr) + sizeof(ff_settings_v7_t)) {
        ff_settings_v7_t v7;
        memcpy(&v7, buf + sizeof(hdr), sizeof(v7));
        ff_settings_migrate_v7(s, &v7);
        return;
    }
    if (hdr.version == 6u && hdr.payload_size == (uint16_t)sizeof(ff_settings_v6_t) &&
        got == sizeof(hdr) + sizeof(ff_settings_v6_t)) {
        ff_settings_v6_t v6;
        memcpy(&v6, buf + sizeof(hdr), sizeof(v6));
        /* Chained: v6 -> v7 (shadow) -> v8 (live) — one migration step
         * each, composed, not a v6->v8 special case (see the v8 comment
         * above `ff_settings_header_t`). */
        ff_settings_v7_t v7;
        ff_settings_migrate_v6(&v7, &v6);
        ff_settings_migrate_v7(s, &v7);
        return;
    }

    /* <=v5 (pre-dates the NVS store, S21 §4 — no fielded device holds one),
     * or a version/size mismatch (corrupt or foreign): defaults stand. */
}

void ff_settings_save(ff_settings_t const *s, ff_store_t const *st)
{
    if (s == NULL || st == NULL || st->set == NULL) {
        return;
    }

    uint8_t buf[FF_SETTINGS_BLOB_LEN];
    ff_settings_header_t hdr;
    hdr.magic = FF_SETTINGS_MAGIC;
    hdr.version = FF_SETTINGS_FORMAT_VERSION;
    hdr.payload_size = (uint16_t)sizeof(ff_settings_t);

    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), s, sizeof(*s));

    /* Exactly one write per save — no read-modify-write loop, no retries. */
    st->set(st->io, FF_SETTINGS_STORE_KEY, buf, sizeof(buf));
}

/* Normalizes any int16_t minute-of-day value (including negatives) into
 * [0, 1440). */
static int32_t ff_norm_min(int32_t m)
{
    int32_t r = m % 1440;
    if (r < 0) {
        r += 1440;
    }
    return r;
}

bool ff_quiet_now(ff_settings_t const *s, int16_t now_min)
{
    if (s == NULL) {
        return false;
    }

    int32_t from = ff_norm_min(s->quiet_from_min);
    int32_t to = ff_norm_min(s->quiet_to_min);
    int32_t now = ff_norm_min(now_min);

    if (from == to) {
        /* Empty window: quiet hours off. */
        return false;
    }

    if (from < to) {
        /* Same-day window: [from, to). */
        return now >= from && now < to;
    }

    /* Wraps past midnight: [from, 1440) union [0, to). */
    return now >= from || now < to;
}

void ff_water_state_init(ff_water_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

/* Counts how many of the `delta` minutes starting at `start_min`
 * (normalized 0..1439) are NOT quiet hours, i.e. awake minutes, per the
 * spec's "every water_min while awake". `delta` is bounded to < 1440 by
 * ff_water_tick (see ff_norm_min's wraparound), so this walks at most
 * one lap of the clock — a plain per-minute scan, since ticks are an
 * infrequent, human-timescale event (not a hot loop). */
static uint16_t ff_awake_minutes_in_window(ff_settings_t const *s, int32_t start_min, int32_t delta)
{
    uint16_t awake = 0;
    for (int32_t i = 0; i < delta; i++) {
        int32_t minute = (start_min + i) % 1440;
        if (!ff_quiet_now(s, (int16_t)minute)) {
            awake++;
        }
    }
    return awake;
}

bool ff_water_tick(ff_water_state_t *state, ff_settings_t const *s, int16_t now_min)
{
    if (state == NULL || s == NULL) {
        return false;
    }

    int32_t nm = ff_norm_min(now_min);

    if (!state->primed || state->configured_water_min != s->water_min) {
        /* First tick ever, or the interval changed underneath us: reset
         * the timer and start counting fresh from this call. */
        state->primed = true;
        state->configured_water_min = s->water_min;
        state->elapsed_min = 0;
        state->last_now_min = (int16_t)nm;
        return false;
    }

    int32_t start = state->last_now_min;
    int32_t delta = nm - start;
    if (delta < 0) {
        delta += 1440;
    }
    state->last_now_min = (int16_t)nm;

    if (s->water_min == 0) {
        /* Off. */
        state->elapsed_min = 0;
        return false;
    }

    /* Only minutes spent awake count toward the interval — quiet-hours
     * minutes are never banked, per spec ("every water_min while
     * awake", suppressed in quiet hours). Saturating add: elapsed_min
     * can never usefully exceed water_min (max preset 120), so this
     * only guards against a corrupt/out-of-range water_min ever making
     * the accumulator overflow instead of just plateauing. */
    uint16_t awake_delta = ff_awake_minutes_in_window(s, start, delta);
    uint32_t sum = (uint32_t)state->elapsed_min + (uint32_t)awake_delta;
    state->elapsed_min = (uint16_t)(sum > UINT16_MAX ? UINT16_MAX : sum);

    if (state->elapsed_min < s->water_min) {
        return false;
    }

    if (ff_quiet_now(s, (int16_t)nm)) {
        /* Defensive boundary case: accrual only ever counts awake
         * minutes now, so this should be unreachable in practice — but
         * if `nm` itself (the tick instant, exclusive upper bound of
         * the window just scanned) lands exactly on a quiet-hours
         * boundary, don't deliver the nudge into quiet hours. Consumed,
         * not deferred (same policy as the whole-interval-during-quiet
         * case above), so this can't compound into a double-length
         * silent window either. */
        state->elapsed_min = 0;
        return false;
    }

    state->elapsed_min = 0;
    return true;
}
