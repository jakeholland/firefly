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
#define FF_SETTINGS_FORMAT_VERSION ((uint16_t)6u)
/* v2: compass_cal_blob (opaque uint8_t[32]) -> compass_cal (ff_geo_cal_t),
 * per TODO(S01) in ff_settings.h. Same sizeof() risk the header comment
 * warns about (a reordering/retype can share sizeof() with the old
 * layout) doesn't apply numerically here — ff_geo_cal_t is 28 bytes vs.
 * the blob's 32 — but the version bump is required regardless: this is a
 * field type/semantics change, not just a size change. */
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

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size; /* sizeof(ff_settings_t) at write time */
} ff_settings_header_t;

#define FF_SETTINGS_BLOB_LEN (sizeof(ff_settings_header_t) + sizeof(ff_settings_t))

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
    /* touch_ax/bx/ay/by: left zeroed; touch_calibrated stays false ->
     * the device applies IDENTITY (raw touch passes through). S15d: a puck
     * that has never been calibrated corrects touch to nothing, not to a
     * garbage transform. */
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

    uint8_t buf[FF_SETTINGS_BLOB_LEN];
    int n = st->get(st->io, FF_SETTINGS_STORE_KEY, buf, sizeof(buf));
    if (n < 0 || (size_t)n != sizeof(buf)) {
        /* Missing, short, truncated, or oversized read -> defaults stand. */
        return;
    }

    ff_settings_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.magic != FF_SETTINGS_MAGIC) {
        return;
    }
    if (hdr.version != FF_SETTINGS_FORMAT_VERSION) {
        return;
    }
    if (hdr.payload_size != (uint16_t)sizeof(ff_settings_t)) {
        return;
    }

    memcpy(s, buf + sizeof(hdr), sizeof(*s));
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
