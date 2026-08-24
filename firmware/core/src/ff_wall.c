#include "ff_wall.h"

#include <stddef.h> /* NULL */

/* S16 slice b0 — wall-clock derivation. See ff_wall.h for the placement
 * rationale, the honesty rule, and the three time bases. Pure integer
 * math: no libc time functions, no floating point, no allocation. */

#define FF_WALL_SECS_PER_DAY ((int64_t)86400)
#define FF_WALL_SECS_PER_MIN ((int64_t)60)

/* ---------------------------------------------------------------------
 * Civil-date math (proleptic Gregorian), Howard Hinnant's chrono
 * algorithms: an era is the 400-year Gregorian leap cycle (146097 days),
 * and shifting the epoch to 0000-03-01 puts the leap day at the end of
 * the year, which is what removes every special case. Integer only.
 * ------------------------------------------------------------------- */

/* days_from_civil — days since 1970-01-01 for a civil date.
 * `m` is 1..12, `d` is 1..31. */
static int64_t ff_wall_days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  /* [0, 399]   */
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1; /* [0, 365]   */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* ff_wall_year_from_days — the Gregorian year containing `z` (days since
 * 1970-01-01). Only the year is needed here: day-of-year then falls out
 * of a subtraction against that year's Jan 1, with no month table and no
 * leap-year branch of our own. */
static int64_t ff_wall_year_from_days(int64_t z)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                     /* [0, 146096] */
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399]    */
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);               /* [0, 365], from Mar 1 */
    int64_t mp = (5 * doy + 2) / 153;                                    /* [0, 11],  Mar = 0    */
    int64_t m = (mp < 10) ? mp + 3 : mp - 9;                             /* [1, 12]              */
    /* January and February belong to the following calendar year in the
     * March-based numbering. */
    return y + (m <= 2 ? 1 : 0);
}

/* Floor division / modulo — C's / and % truncate toward zero, which is
 * wrong for negative day numbers (pre-1970, or a local time pushed
 * before the epoch by a westward offset). Never reachable given
 * FF_WALL_EPOCH_FLOOR, but the date math is written to be correct on its
 * own terms rather than correct-by-luck. */
static int64_t ff_wall_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static bool ff_wall_offset_valid(int16_t off_min)
{
    return off_min >= FF_WALL_OFFSET_MIN_LO && off_min <= FF_WALL_OFFSET_MIN_HI;
}

/* ---------------------------------------------------------------------
 * Latch
 * ------------------------------------------------------------------- */

void ff_wall_init(ff_wall_state_t *st)
{
    if (st == NULL) {
        return;
    }
    st->latched = false;
    st->latch_unix_s = 0;
    st->latch_ms = 0;
}

/* Elapsed monotonic milliseconds since the latch, or false if the latch
 * is absent or has aged past the point where a uint32_t wrap could
 * masquerade as a small delta. Unsigned subtraction is wraparound-safe
 * (the idiom at targets/sim/live.c:245); a `now_ms` that went backwards
 * produces a near-2^32 delta and trips the same limit, degrading to
 * "unknown" rather than to a wrong time. */
static bool ff_wall_elapsed_ms(ff_wall_state_t const *st, uint32_t now_ms, uint32_t *out_ms)
{
    if (st == NULL || !st->latched) {
        return false;
    }
    uint32_t elapsed = now_ms - st->latch_ms;
    if (elapsed > FF_WALL_LATCH_MAX_AGE_MS) {
        return false;
    }
    *out_ms = elapsed;
    return true;
}

ff_wall_obs_t ff_wall_observe(ff_wall_state_t *st, int64_t unix_s, uint32_t rx_ms)
{
    if (st == NULL) {
        return FF_WALL_OBS_REJECTED;
    }

    /* Primary gate. Needs no pack and no festival data — which is what
     * lets this run during the want_config handshake. A rejection never
     * disturbs an existing good latch. */
    if (unix_s < FF_WALL_EPOCH_FLOOR) {
        return FF_WALL_OBS_REJECTED;
    }

    if (!st->latched) {
        st->latched = true;
        st->latch_unix_s = unix_s;
        st->latch_ms = rx_ms;
        return FF_WALL_OBS_LATCHED;
    }

    uint32_t elapsed_ms = 0;
    if (!ff_wall_elapsed_ms(st, rx_ms, &elapsed_ms)) {
        /* The latch expired (or the monotonic clock moved backwards):
         * the old reference can no longer be compared against, so this
         * plausible reading becomes the new one. */
        st->latch_unix_s = unix_s;
        st->latch_ms = rx_ms;
        return FF_WALL_OBS_RELATCHED;
    }

    int64_t predicted = st->latch_unix_s + (int64_t)(elapsed_ms / 1000u);
    int64_t delta = unix_s - predicted;
    if (delta < 0) {
        delta = -delta;
    }

    if (delta > FF_WALL_RELATCH_DELTA_S) {
        /* The comms brain's clock stepped — almost always GPS lock
         * correcting an uncorrected RTC. Take the new reading; keeping
         * the old one would mean asserting FF_WALL_MESH over a time we
         * now have direct evidence is wrong. */
        st->latch_unix_s = unix_s;
        st->latch_ms = rx_ms;
        return FF_WALL_OBS_RELATCHED;
    }

    /* Within tolerance: deliberately leave the latch alone. */
    return FF_WALL_OBS_AGREED;
}

bool ff_wall_unix_now(ff_wall_state_t const *st, uint32_t now_ms, int64_t *out_unix_s)
{
    uint32_t elapsed_ms = 0;
    if (out_unix_s == NULL || !ff_wall_elapsed_ms(st, now_ms, &elapsed_ms)) {
        return false;
    }
    *out_unix_s = st->latch_unix_s + (int64_t)(elapsed_ms / 1000u);
    return true;
}

/* ---------------------------------------------------------------------
 * Offset resolution
 * ------------------------------------------------------------------- */

bool ff_wall_resolve_offset(ff_wall_offset_cfg_t const *cfg, int16_t *out_offset_min, bool *out_assumed)
{
    if (cfg == NULL || out_offset_min == NULL || out_assumed == NULL) {
        return false;
    }

    bool pack_usable = cfg->pack_loaded && ff_wall_offset_valid(cfg->pack_offset_min);

    /* 1. A STATED pack offset — the festival says what time zone it is in. */
    if (pack_usable && !cfg->pack_offset_assumed) {
        *out_offset_min = cfg->pack_offset_min;
        *out_assumed = false;
        return true;
    }

    /* 2. A deliberately configured settings offset. Ranked above the
     * pack's parser default on purpose: quiet hours is a settings
     * feature with no festival dependency, and a value the user set
     * must not be outranked by fp_parse's -240 fallback. */
    if (cfg->settings_offset_set && ff_wall_offset_valid(cfg->settings_offset_min)) {
        *out_offset_min = cfg->settings_offset_min;
        *out_assumed = false;
        return true;
    }

    /* 3. The pack's assumed default — usable, but flagged as a guess so
     * the UI can say so. */
    if (pack_usable) {
        *out_offset_min = cfg->pack_offset_min;
        *out_assumed = true;
        return true;
    }

    /* 4. Nothing. FF_WALL_UNKNOWN, not a defaulted guess. */
    return false;
}

/* ---------------------------------------------------------------------
 * unix -> local -> (day_doy, now_min)
 * ------------------------------------------------------------------- */

bool ff_wall_split_local(int64_t unix_s, int16_t utc_offset_min, uint16_t *out_day_doy, int16_t *out_now_min)
{
    if (out_day_doy == NULL || out_now_min == NULL) {
        return false;
    }
    if (unix_s < FF_WALL_EPOCH_FLOOR || !ff_wall_offset_valid(utc_offset_min)) {
        return false;
    }

    int64_t local_s = unix_s + (int64_t)utc_offset_min * FF_WALL_SECS_PER_MIN;

    /* ff_sched.h's contract: the festival day rolls at 06:00 local, not
     * at midnight. Shifting the local clock back by that boundary makes
     * the festival day an ordinary calendar day in the shifted frame, so
     * a single floor-division yields the right date with no
     * before-06:00 special case:
     *
     *   01:00 local  ->  shifted to 19:00 of the PREVIOUS date
     *                ->  day_doy = previous date, now_min = 1500
     *   06:00 local  ->  shifted to 00:00 of the same date
     *                ->  day_doy = that date,     now_min = 360
     *   05:59 local  ->  day_doy = previous date, now_min = 1799
     *
     * now_min is then measured from midnight of day_doy's date (NOT from
     * the shifted origin), which is what puts it in [360, 1800). */
    int64_t fest_s = local_s - (int64_t)FF_WALL_DAY_START_MIN * FF_WALL_SECS_PER_MIN;
    int64_t days = ff_wall_floor_div(fest_s, FF_WALL_SECS_PER_DAY);

    int64_t now_min = (local_s - days * FF_WALL_SECS_PER_DAY) / FF_WALL_SECS_PER_MIN;

    int64_t year = ff_wall_year_from_days(days);
    int64_t doy = days - ff_wall_days_from_civil(year, 1, 1) + 1; /* 1..366 */

    *out_day_doy = (uint16_t)doy;
    *out_now_min = (int16_t)now_min;
    return true;
}

ff_wall_t ff_wall_now(ff_wall_state_t const *st, uint32_t now_ms, ff_wall_offset_cfg_t const *cfg)
{
    /* Start from UNKNOWN with every other field zeroed, and return early
     * from any step that cannot be honestly completed. There is
     * deliberately no `else` anywhere below that produces a time. */
    ff_wall_t w;
    w.src = FF_WALL_UNKNOWN;
    w.day_doy = 0;
    w.now_min = 0;
    w.offset_assumed = false;

    int64_t unix_s = 0;
    if (!ff_wall_unix_now(st, now_ms, &unix_s)) {
        return w;
    }

    int16_t offset_min = 0;
    bool assumed = false;
    if (!ff_wall_resolve_offset(cfg, &offset_min, &assumed)) {
        return w;
    }

    uint16_t day_doy = 0;
    int16_t now_min = 0;
    if (!ff_wall_split_local(unix_s, offset_min, &day_doy, &now_min)) {
        return w;
    }

    w.src = FF_WALL_MESH;
    w.day_doy = day_doy;
    w.now_min = now_min;
    w.offset_assumed = assumed;
    return w;
}
