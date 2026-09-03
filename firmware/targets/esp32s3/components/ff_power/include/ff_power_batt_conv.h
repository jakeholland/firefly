/**
 * ff_power_batt_conv.h — S25 slice c: the pure pin-mV -> pack-mV
 * conversion arithmetic, hoisted out of ff_power.c into its own
 * header-only, ESP-free file so a HOST test (targets/sim/tests/
 * test_batt_pack_mv.c) can pin it by literal — no ADC, no gpio.h, no
 * esp_adc, nothing that only compiles under `idf.py build`.
 *
 * ## The conversion arithmetic (cited)
 * Composes two factors into ONE named integer constant:
 *   1. the board's 1:3 resistive divider — multiply by 3;
 *   2. Waveshare's own measured correction on top of that (their
 *      BAT_Driver.c, https://github.com/yaosy1997/ESP32-S3-Touch-LCD-1.46-Test/blob/main/main/BAT_Driver/BAT_Driver.c,
 *      divides the naive ×3 result by 0.990476 — the silicon's actual
 *      divider ratio is not exactly 3:1).
 * `3.0 / 0.990476 = 3.02884673631668...` (full double precision),
 * scaled by 1e6 and rounded to the nearest integer: `round(3.02884673631668
 * * 1e6) = 3028847`. (Review fix: an earlier revision truncated this to
 * 3028835 — the 1e6-scaled value at only 6 significant figures of the
 * ratio, one part in ~33000 low. 3028847 is correct at full double
 * precision.)
 */
#pragma once

#include <stdint.h>

#define FF_BATT_PACK_MV_PER_CAL_MV_X1E6 ((uint32_t)3028847u) /* round(3.0 / 0.990476 * 1e6) at full precision — see this file's top comment */

/**
 * ff_power_batt_pack_mv_unclamped — the conversion BEFORE the uint16_t
 * clamp `ff_power_batt_pack_mv_from_cal_mv` applies, as a uint64_t.
 * Exposed (not folded into the clamped function) specifically so a test
 * can observe round-half-up AT an exact tie: this constant's unique
 * exact-.5 input (verified: `cal_mv` such that `cal_mv *
 * FF_BATT_PACK_MV_PER_CAL_MV_X1E6 mod 1e6 == 500000`) is `cal_mv ==
 * 500000` — far outside any physically plausible pin reading and past
 * `UINT16_MAX`, so the public clamped function alone cannot distinguish
 * a correct round-up from an incorrect floor at that tie (both clamp to
 * 65535). This function has no such blind spot. `cal_mv` is
 * deliberately NOT range-checked here (or in the clamped function below)
 * — plausibility is `core/include/ff_batt.h`'s job one layer up, not
 * this pure arithmetic's.
 *
 * `pack_mv = round(cal_mv * 3.0 / 0.990476)`, computed as
 * `(cal_mv * FF_BATT_PACK_MV_PER_CAL_MV_X1E6 + 500000) / 1000000` — a
 * `uint64_t` intermediate is required at the multiply: the largest
 * plausible calibrated ADC reading (~1533 mV, `ff_batt.h`'s ~4.6V pack
 * plausibility ceiling / 3) times the scaled ratio is ~4.64e9, which
 * overflows a 32-bit product (UINT32_MAX is ~4.29e9).
 */
static inline uint64_t ff_power_batt_pack_mv_unclamped(uint32_t cal_mv)
{
    uint64_t const scaled = (uint64_t)cal_mv * FF_BATT_PACK_MV_PER_CAL_MV_X1E6 + 500000ULL;
    return scaled / 1000000ULL;
}

/**
 * ff_power_batt_pack_mv_from_cal_mv — the conversion `ff_power_batt_mv`
 * (ff_power.c) actually uses: `ff_power_batt_pack_mv_unclamped` above,
 * clamped to `uint16_t` (this function's return type, and
 * `ff_power_batt_mv`'s) rather than silently wrapping. A calibrated
 * reading that would overflow it is well outside any plausible pack
 * voltage and `ff_batt.h`'s own plausibility gate ([2500, 4600] mV)
 * rejects it as unknown either way — this clamp only protects the
 * narrowing cast itself from undefined behavior.
 */
static inline uint16_t ff_power_batt_pack_mv_from_cal_mv(uint32_t cal_mv)
{
    uint64_t const pack_mv64 = ff_power_batt_pack_mv_unclamped(cal_mv);
    return (pack_mv64 > (uint64_t)UINT16_MAX) ? UINT16_MAX : (uint16_t)pack_mv64;
}
