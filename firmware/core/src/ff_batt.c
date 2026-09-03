/**
 * ff_batt.c — see ff_batt.h for the contract this implements.
 */
#include "ff_batt.h"

#include <string.h>

#include "ff_clock.h" /* ff_time_reached — wraparound-safe deadline check */
#include "ff_radar.h" /* FF_BATT_LOW_PCT / ff_radar_batt_is_low — the ONE low-battery
                        * classification every renderer shares; the filter reuses it
                        * rather than re-typing the literal 15 (AGENTS.md standing
                        * brief: no second literal). */

/* ---------------------------------------------------------------------
 * ff_batt_pct_from_mv
 * ------------------------------------------------------------------- */

/* The OCV/SOC table — see ff_batt.h's doc comment for the source and
 * exact breakpoints. Ascending by mv, strictly monotonic in both
 * columns (required by the interpolation loop below). */
static const struct {
    uint16_t mv;
    int8_t   pct;
} k_ocv_table[] = {
    {3300, 0},
    {3500, 5},
    {3600, 10},
    {3700, 30},
    {3800, 50},
    {3900, 65},
    {4000, 80},
    {4100, 92},
    {4200, 100},
};
#define K_OCV_TABLE_N ((int)(sizeof(k_ocv_table) / sizeof(k_ocv_table[0])))

int8_t ff_batt_pct_from_mv(uint16_t pack_mv)
{
    if (pack_mv == 0u) {
        return -1;
    }
    if (pack_mv < FF_BATT_MV_PLAUSIBLE_MIN || pack_mv > FF_BATT_MV_PLAUSIBLE_MAX) {
        return -1;
    }

    /* Below the table's lowest point, still plausible: empty. */
    if (pack_mv <= k_ocv_table[0].mv) {
        return k_ocv_table[0].pct;
    }
    /* Above the table's highest point, still plausible: full. */
    if (pack_mv >= k_ocv_table[K_OCV_TABLE_N - 1].mv) {
        return k_ocv_table[K_OCV_TABLE_N - 1].pct;
    }

    for (int i = 0; i + 1 < K_OCV_TABLE_N; i++) {
        uint16_t const mv_lo = k_ocv_table[i].mv;
        uint16_t const mv_hi = k_ocv_table[i + 1].mv;
        if (pack_mv < mv_lo || pack_mv > mv_hi) {
            continue;
        }
        int32_t const pct_lo = k_ocv_table[i].pct;
        int32_t const pct_hi = k_ocv_table[i + 1].pct;
        int32_t const span_mv = (int32_t)mv_hi - (int32_t)mv_lo;   /* > 0, table is strictly ascending */
        int32_t const span_pct = pct_hi - pct_lo;                  /* >= 0, table is nondecreasing */
        int32_t const delta_mv = (int32_t)pack_mv - (int32_t)mv_lo; /* [0, span_mv] */
        /* Integer round-to-nearest: (delta_mv * span_pct) / span_mv,
         * rounded rather than truncated, so the midpoint of a segment
         * lands on the arithmetic midpoint of its two endpoints rather
         * than one below it. delta_mv/span_pct are both >= 0 here, so a
         * plain "+ half denominator" rounding is exact — no sign
         * handling needed. */
        int32_t const num = delta_mv * span_pct;
        int32_t const pct = pct_lo + (num + span_mv / 2) / span_mv;
        return (int8_t)pct;
    }

    /* Unreachable: pack_mv is bounded to [table[0].mv, table[N-1].mv]
     * by the two clamp checks above, so some segment above always
     * matches. Kept as an explicit, honest -1 rather than assuming the
     * loop always returns — a future table edit that leaves a gap must
     * fail toward "unknown", never toward a silently wrong percent. */
    return -1;
}


/* ---------------------------------------------------------------------
 * ff_batt_filter_t — moving-average + Schmitt-hysteresis
 * ------------------------------------------------------------------- */

void ff_batt_filter_init(ff_batt_filter_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->displayed_pct = -1;
}

int8_t ff_batt_filter_push(ff_batt_filter_t *f, uint16_t pack_mv, uint32_t now_ms)
{
    if (f == NULL) {
        return -1;
    }

    int8_t const raw_pct = ff_batt_pct_from_mv(pack_mv);
    if (raw_pct < 0) {
        /* An unreadable/implausible sample is never folded into the
         * window — see ff_batt.h's doc comment. Track consecutive bad
         * samples for the dead-sensor revert (FF_BATT_FILTER_DEAD_AFTER). */
        f->has_last_push = true;
        f->last_push_ms = now_ms;
        if (f->consecutive_bad < UINT8_MAX) {
            f->consecutive_bad++;
        }
        if (f->consecutive_bad >= FF_BATT_FILTER_DEAD_AFTER) {
            /* The sense line has gone dead: an old percent frozen from
             * before it died is a stale-shown-as-current honesty
             * violation (CLAUDE.md). Revert to unknown and clear the
             * window so the next valid reading is a fresh first-ever
             * sample (immediate reveal, no residual weight). */
            f->has_displayed = false;
            f->displayed_pct = -1;
            f->filled = false;
            f->next = 0;
        }
        return f->displayed_pct;
    }

    /* Valid reading: a real sample clears the consecutive-bad run. */
    f->consecutive_bad = 0;

    if (f->has_last_push && ff_time_reached(now_ms, f->last_push_ms + FF_BATT_FILTER_STALE_GAP_MS)) {
        /* A long gap since the last push: start the window fresh
         * rather than blend a possibly stale pre-gap reading with the
         * new one (ff_batt.h's FF_BATT_FILTER_STALE_GAP_MS doc comment). */
        f->filled = false;
        f->next = 0;
    }
    f->has_last_push = true;
    f->last_push_ms = now_ms;

    if (!f->filled) {
        /* Pre-fill: every slot starts as this one reading, not a
         * window of 1 grown one sample at a time — see
         * FF_BATT_FILTER_WINDOW's doc comment for why that avoids a
         * variable-weight warm-up transient. */
        for (uint8_t i = 0; i < FF_BATT_FILTER_WINDOW; i++) {
            f->history[i] = pack_mv;
        }
        f->next = 0;
        f->filled = true;
    } else {
        f->history[f->next] = pack_mv;
        f->next = (uint8_t)((f->next + 1u) % FF_BATT_FILTER_WINDOW);
    }

    uint32_t sum_mv = 0;
    for (uint8_t i = 0; i < FF_BATT_FILTER_WINDOW; i++) {
        sum_mv += f->history[i];
    }
    /* Round-to-nearest mean, integer-only (both operands non-negative,
     * so truncating division after adding half the divisor rounds
     * correctly — same convention ff_batt_pct_from_mv's own
     * interpolation uses). */
    uint16_t const mean_mv = (uint16_t)((sum_mv + FF_BATT_FILTER_WINDOW / 2u) / FF_BATT_FILTER_WINDOW);
    int8_t const filtered = ff_batt_pct_from_mv(mean_mv);
    if (filtered < 0) {
        /* Defensive only: a mean of plausible readings should itself
         * be plausible. If it somehow isn't, don't let a single bad
         * arithmetic corner silently move the display — hold what's
         * already shown rather than guess. */
        return f->displayed_pct;
    }

    if (!f->has_displayed) {
        /* First-ever real sample: shows immediately (a freshly
         * pre-filled window's mean equals that one sample), no
         * hysteresis, no warm-up wait. */
        f->has_displayed = true;
        f->displayed_pct = filtered;
        return f->displayed_pct;
    }

    bool const displayed_low = ff_radar_batt_is_low(f->displayed_pct);
    bool const filtered_low = ff_radar_batt_is_low(filtered);
    int16_t const delta = (int16_t)filtered - (int16_t)f->displayed_pct;
    int16_t const abs_delta = (delta < 0) ? (int16_t)(-delta) : delta;

    bool update;
    if (!displayed_low && filtered_low) {
        /* Downward crossing into low: always promote, no hysteresis
         * check — the low-battery alert must never be delayed. */
        update = true;
    } else if (displayed_low && !filtered_low) {
        /* Upward exit from low: requires clearing the boundary by a
         * full hysteresis margin (asymmetric on purpose — see
         * ff_batt_filter_push's doc comment). */
        update = (filtered >= FF_BATT_LOW_PCT + FF_BATT_HYSTERESIS_PCT);
    } else {
        /* Ordinary case: both low, or both not low. */
        update = (abs_delta >= FF_BATT_HYSTERESIS_PCT);
    }

    if (update) {
        f->displayed_pct = filtered;
    }
    return f->displayed_pct;
}
