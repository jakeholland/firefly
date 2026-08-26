/**
 * ff_wall_window.c — S18 slice c (#40): pack -> plausibility window.
 * See ff_wall_window.h for the layering rationale and the honest-fallback
 * contract.
 */
#include "ff_wall_window.h"

#include <stddef.h> /* NULL */

#include "ff_wall.h" /* ff_wall_unix_from_doy — the shared civil-date math */

/* Day-of-year is 1..366 (366 for a leap year's Dec 31). */
#define FF_WALL_WINDOW_DOY_MAX 366
#define FF_WALL_WINDOW_SECS_PER_DAY ((int64_t)86400)

bool ff_wall_window_from_pack(fp_pack_t const *pack, int64_t *out_floor, int64_t *out_ceiling)
{
    if (pack == NULL || out_floor == NULL || out_ceiling == NULL) {
        return false;
    }

    /* Honest-fallback guards: any of these means the pack cannot honestly
     * pin the window to a festival, so the caller must keep the fixed
     * bootstrap window rather than narrow to a fabricated one. year == 0
     * or a zero doy is exactly what fp_parse leaves for an absent/null
     * date; the range and ordering checks reject a corrupt pack. */
    if (pack->year == 0) {
        return false;
    }
    if (pack->start_doy == 0 || pack->end_doy == 0) {
        return false;
    }
    if (pack->start_doy > FF_WALL_WINDOW_DOY_MAX || pack->end_doy > FF_WALL_WINDOW_DOY_MAX) {
        return false;
    }
    if (pack->end_doy < pack->start_doy) {
        return false;
    }

    /* Both bounds against the SAME integer civil-date math the wall clock
     * itself uses (ff_wall_unix_from_doy), so the window can't drift from
     * the gate it feeds. The ceiling reaches the start of the day AFTER
     * end_doy, so the full last festival day is inside the window before
     * the margin is added — ff_wall_unix_from_doy extends past a year's
     * length linearly, so end_doy + 1 needs no rollover handling. */
    int64_t const start_s = ff_wall_unix_from_doy((int64_t)pack->year, (int64_t)pack->start_doy);
    int64_t const end_next_s =
        ff_wall_unix_from_doy((int64_t)pack->year, (int64_t)pack->end_doy) + FF_WALL_WINDOW_SECS_PER_DAY;

    *out_floor = start_s - FF_WALL_WINDOW_MARGIN_S;
    *out_ceiling = end_next_s + FF_WALL_WINDOW_MARGIN_S;
    return true;
}
