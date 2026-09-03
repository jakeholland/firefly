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
 * ff_batt_filter_t
 * ------------------------------------------------------------------- */

void ff_batt_filter_init(ff_batt_filter_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
    f->displayed_pct = -1;
}

/* Median of the `n` values in `vals` (n >= 1, n <= FF_BATT_FILTER_WINDOW).
 * Sorts a small scratch COPY (insertion sort — n is at most 5, nothing
 * here benefits from anything fancier) and returns the middle element;
 * for an even `n` the two middle elements are averaged (rounded), so
 * every call during window warm-up (count growing 1..FF_BATT_FILTER_WINDOW)
 * has a well-defined answer, not just the steady-state odd-window case. */
static int8_t batt_median(int8_t const *vals, uint8_t n)
{
    int8_t sorted[FF_BATT_FILTER_WINDOW];
    for (uint8_t i = 0; i < n; i++) {
        sorted[i] = vals[i];
    }
    for (uint8_t i = 1; i < n; i++) {
        int8_t const v = sorted[i];
        uint8_t j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    if (n % 2u == 1u) {
        return sorted[n / 2u];
    }
    int32_t const a = sorted[n / 2u - 1u];
    int32_t const b = sorted[n / 2u];
    /* Round-to-nearest average of the two middle values (a, b >= 0,
     * since ff_batt_pct_from_mv's valid range is [0, 100]). */
    return (int8_t)((a + b + 1) / 2);
}

int8_t ff_batt_filter_push(ff_batt_filter_t *f, uint16_t pack_mv, uint32_t now_ms)
{
    if (f == NULL) {
        return -1;
    }

    int8_t const raw_pct = ff_batt_pct_from_mv(pack_mv);
    if (raw_pct < 0) {
        /* An unreadable/implausible sample is never folded into the
         * history — see ff_batt.h's doc comment. Report whatever is
         * already displayed (honestly -1 if nothing ever has been). */
        f->has_last_push = true;
        f->last_push_ms = now_ms;
        return f->displayed_pct;
    }

    if (f->has_last_push && ff_time_reached(now_ms, f->last_push_ms + FF_BATT_FILTER_STALE_GAP_MS)) {
        /* A long gap since the last push: start the window fresh
         * rather than blend a possibly stale pre-gap reading with the
         * new one (ff_batt.h's FF_BATT_FILTER_STALE_GAP_MS doc comment). */
        f->count = 0;
        f->next = 0;
    }
    f->has_last_push = true;
    f->last_push_ms = now_ms;

    f->history[f->next] = raw_pct;
    f->next = (uint8_t)((f->next + 1u) % FF_BATT_FILTER_WINDOW);
    if (f->count < FF_BATT_FILTER_WINDOW) {
        f->count++;
    }

    int8_t const median = batt_median(f->history, f->count);

    if (!f->has_displayed) {
        /* First real sample ever: show immediately, no hysteresis, no
         * warm-up wait (ff_batt.h's doc comment). */
        f->has_displayed = true;
        f->displayed_pct = median;
        return f->displayed_pct;
    }

    int16_t const delta = (int16_t)median - (int16_t)f->displayed_pct;
    int16_t const abs_delta = (delta < 0) ? (int16_t)(-delta) : delta;
    bool const crosses_low = ff_radar_batt_is_low((int8_t)median) != ff_radar_batt_is_low(f->displayed_pct);

    if (crosses_low || abs_delta >= FF_BATT_HYSTERESIS_PCT) {
        f->displayed_pct = median;
    }
    return f->displayed_pct;
}
