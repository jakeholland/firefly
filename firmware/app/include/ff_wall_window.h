/**
 * ff_wall_window.h — S18 slice c (#40): derive the wall-clock plausibility
 * window from a loaded festpack.
 *
 * Spec: docs/specs/S18-wall-clock-trust.md, "Slice c".
 *
 * ---------------------------------------------------------------------
 * WHY THIS LIVES IN app/ AND NOT core/
 * ---------------------------------------------------------------------
 * The honest bound on "is this a plausible time" is "is it near the
 * festival we're at". That bound is festival data, so deriving it needs
 * an `fp_pack_t` (festpack) — and core/ deliberately does not, and must
 * not, depend on festpack (docs/ARCHITECTURE.md's one-way edge; see
 * ff_wall.h's placement note). So the derivation lives HERE, at the
 * shell/app boundary that is allowed to see both a pack and core.
 *
 * `ff_wall` core takes the resulting window as plain int64_t VALUES
 * through `ff_wall_set_window` — it never sees the pack. This file is the
 * only new festpack->window seam; the shell calls
 * ff_wall_window_from_pack on load and hands the result to
 * ff_wall_set_window.
 *
 * The function is a PURE function over the pack's dates (year / start_doy
 * / end_doy) and holds no state, so it is unit-testable with a plain
 * fp_pack_t and no shell — see firmware/app/tests/test_wall_window.c.
 */
#ifndef FF_WALL_WINDOW_H
#define FF_WALL_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "fp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * FF_WALL_WINDOW_MARGIN_S — the slack added on each side of the festival's
 * own dates: 14 days. Covers early-entry (crews arrive days before the
 * gates), teardown, and the sloppiness of a pack that dates only the main
 * days. Also comfortably subsumes any UTC offset: the window bounds below
 * are computed in UTC from day-of-year, and a festival's few-hour offset
 * from UTC is dwarfed by a two-week margin, so no timezone correction is
 * applied (nor would one be honest to invent from dates alone).
 */
#define FF_WALL_WINDOW_MARGIN_S ((int64_t)1209600) /* 14 days */

/**
 * ff_wall_window_from_pack — derive the tightened plausibility window
 * [*out_floor, *out_ceiling) from `pack`'s festival dates.
 *
 *   *out_floor   = 00:00 UTC of start_doy        - FF_WALL_WINDOW_MARGIN_S
 *   *out_ceiling = 00:00 UTC of (end_doy + 1 day) + FF_WALL_WINDOW_MARGIN_S
 *
 * The ceiling reaches the day AFTER end_doy so the whole final festival
 * day (and any after-midnight sets rolling into the next calendar day) is
 * inside the window before the margin is even added.
 *
 * Returns true and writes both outputs only for a pack with USABLE dates.
 * Returns false — writing nothing — for the honest-fallback cases, so the
 * caller keeps the fixed bootstrap window rather than a fabricated tight
 * one:
 *   - `pack` NULL, or either output pointer NULL;
 *   - year == 0 (absent);
 *   - start_doy == 0 or end_doy == 0 (a null-dated pack: fp_parse leaves
 *     these zero when the pack omits or nulls its dates);
 *   - start_doy or end_doy out of the 1..366 day-of-year range (corrupt);
 *   - end_doy < start_doy (inverted/corrupt span).
 *
 * A false return is NOT an error the caller must surface — it is the
 * documented "this pack cannot tighten the window, stay on the fixed one"
 * signal. Never invents a tight window from missing data (the S18
 * standing-brief honest-data rule).
 */
bool ff_wall_window_from_pack(fp_pack_t const *pack, int64_t *out_floor, int64_t *out_ceiling);

#ifdef __cplusplus
}
#endif

#endif /* FF_WALL_WINDOW_H */
