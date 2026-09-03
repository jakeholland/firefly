/**
 * ff_rally.h — core/rally: quick-rally place naming, WHEN vocabulary, and
 * wire-name composition.
 *
 * This is a pure MOVE (tech-debt sprint, "rally place/name/when logic
 * moves from ff_shell into core ff_rally"; no protocol/behavior change).
 * The logic previously lived as `static` helpers in `firmware/app/ff_shell.c`
 * (S22 slice d's quick-rally naming, S24 slice d's WHERE list + WHEN
 * suffix); CLAUDE.md's "all logic goes in firmware/core/" — this was pure
 * domain policy over positions and small ints, not I/O, so it had no
 * business being a shell `static`.
 *
 * Spec: docs/specs/S22-signals-rework.md's "Questions" section (the quick
 * -rally place-naming SPEC GAP, closed by that section's Amendments) and
 * docs/specs/S24-signals-inbox.md slice (d) (the Rally screen's WHERE
 * list + WHEN-in-name send).
 *
 * ## Why this does NOT take an `fp_pack_t const *` (deviation from the
 * tech-debt brief's suggested signature)
 * `firmware/core` has a one-way dependency edge onto `firmware/festpack`
 * (docs/ARCHITECTURE.md): `ff-festpack` links `ff-core` (its parser calls
 * `ff_geo_project`), never the reverse. `ff_wall.h` documents the same
 * constraint for the same reason ("takes the offset as plain values
 * rather than an `fp_pack_t`... core keeps its zero-dependency,
 * no-festpack posture") and `firmware/core/tests/test_wall.c` calls it
 * "docs/ARCHITECTURE.md's one-way edge". Taking `fp_pack_t const *` here
 * would make `ff-core` depend on `ff-festpack`, which already depends on
 * `ff-core` — a cycle CMake cannot link. So every function below takes
 * plain values (a caller-built array of `ff_rally_landmark_t`, projected
 * east/north meters, small ints) instead of festpack types. The shell
 * keeps the one small loop that walks `sh->pack->landmarks[]` and calls
 * `ff_rally_landmark_displayable()` per entry — that walk is unavoidably
 * tied to `fp_pack_t`'s layout and is dispatch, not policy.
 *
 * Nearest-landmark search and place naming operate in the SAME projected
 * east/north space `fp_landmark_t` already stores (rather than re-deriving
 * lat/lon via `ff_geo_unproject` and re-projecting), so the math is
 * byte-identical to the original shell code — not merely equivalent.
 *
 * The WHEN vocabulary (`when_sel`: 0 = NOW, 1 = +15m, 2 = +30m) is passed
 * as a plain `int` rather than a new core enum: the UI-facing
 * `ff_rally_when_t` already lives in `firmware/app/include/ff_app_state.h`
 * (screen view-model territory, out of this PR's touch list) with those
 * exact numeric values, and duplicating an enum of the same name in core
 * would collide when a single translation unit (`ff_shell.c`) includes
 * both headers. Any caller passes `(int)`-cast values from that enum.
 *
 * Pure C11, no I/O, no allocation, no clock-reading.
 */
#ifndef FF_RALLY_H
#define FF_RALLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * FF_RALLY_LANDMARK_NEAR_M — S22 slice d SPEC-GAP MVP (see this header's
 * top comment and docs/specs/S22-signals-rework.md's "Questions"): a quick
 * RALLY names itself after a festpack landmark only when the sender is
 * genuinely near it. Naming a rally after a landmark the sender is nowhere
 * near would be dishonest, so the match is gated on real proximity; 120 m
 * ~ "you are effectively at it" at festival scale. The comparison is
 * STRICT (`<`): a landmark at EXACTLY this distance does not count as
 * near — ties at the boundary fall back to FF_RALLY_DEFAULT_NAME, never a
 * landmark.
 */
#define FF_RALLY_LANDMARK_NEAR_M 120.0f

/** Honest fallback rally place name: no pack, no landmark within
 * FF_RALLY_LANDMARK_NEAR_M, or the nearest one's name would not fit the
 * wire (`ff_proto.h`'s FF_PROTO_RALLY_NAME_MAX). Never a guess. */
#define FF_RALLY_DEFAULT_NAME "MY SPOT"

/**
 * ff_rally_landmark_t — one candidate landmark for the quick-rally
 * nearest-search, in the SAME projected east/north space `fp_landmark_t`
 * stores (see this header's top comment for why this is a plain struct
 * and not `fp_landmark_t`).
 *
 *  - `name`   — non-NULL, NUL-terminated; pass "" (never NULL) for a
 *               landmark with no usable name.
 *  - `has_pos`— mirrors `fp_landmark_t.has_pos`: false means `east_m`/
 *               `north_m` are meaningless and the candidate is skipped.
 *  - `east_m`, `north_m` — projected position, same origin as the
 *               caller's `my_east_m`/`my_north_m`.
 */
typedef struct {
    char const *name;
    bool has_pos;
    float east_m;
    float north_m;
} ff_rally_landmark_t;

/**
 * ff_rally_nearest_landmark — find the index of the nearest USABLE
 * landmark in `landmarks[0..n)` to (`my_east_m`, `my_north_m`) strictly
 * within `near_m`. A landmark is usable iff `has_pos` is true, `name` is
 * non-empty, AND `name` is short enough to fit the wire
 * (`FF_PROTO_RALLY_NAME_MAX`, from `ff_proto.h`) — an unusable landmark is
 * skipped as if absent, never causing a truncated/misleading name.
 *
 * The boundary is EXCLUSIVE: a landmark at exactly `near_m` does not
 * qualify (`d^2 < near_m^2`, not `<=`) — see FF_RALLY_LANDMARK_NEAR_M's
 * doc comment.
 *
 * Returns true and writes `*out_index` (an index into `landmarks[]`) for
 * the closest qualifying landmark. Returns false — `*out_index`
 * unwritten — if `landmarks` is NULL, `n` is 0, or no landmark qualifies.
 */
bool ff_rally_nearest_landmark(ff_rally_landmark_t const *landmarks, uint8_t n, float my_east_m,
                                float my_north_m, float near_m, uint8_t *out_index);

/**
 * ff_rally_place_name — the quick-rally place name: the nearest qualifying
 * landmark's name (via ff_rally_nearest_landmark at FF_RALLY_LANDMARK_NEAR_M),
 * else the honest fallback FF_RALLY_DEFAULT_NAME. Never returns NULL.
 * The returned pointer aliases either the literal above or one of
 * `landmarks[].name` — valid exactly as long as the caller's array is.
 */
char const *ff_rally_place_name(ff_rally_landmark_t const *landmarks, uint8_t n, float my_east_m,
                                 float my_north_m);

/**
 * ff_rally_landmark_displayable — is this landmark a Rally WHERE row (S24
 * AC6)? True iff it has a known position AND a (possibly long — this
 * predicate does NOT check the wire-name budget, unlike the
 * nearest-search's usability filter) non-empty name. `name` must be
 * non-NULL (pass "" for none). The SAME predicate must gate both the
 * projected row list and the send-time landmark-by-index resolution, so a
 * selection index can never diverge between what the screen shows and
 * what the shell sends (S24's own invariant, preserved by this move).
 */
bool ff_rally_landmark_displayable(bool has_pos, char const *name);

/**
 * ff_rally_when_suffix — the suffix that rides in the rally NAME (S24 AC6:
 * `ff_proto_rally_t` has no time field, so WHEN travels as literal text in
 * the name). `when_sel`: 0 = NOW (no suffix), 1 = +15m, 2 = +30m — mirrors
 * `ff_app_state.h`'s `ff_rally_when_t` (`FF_RALLY_WHEN_NOW`/`_15`/`_30`);
 * any other value is treated as NOW. Kept well under
 * `FF_PROTO_RALLY_NAME_MAX` so a place name always has room.
 */
char const *ff_rally_when_suffix(int when_sel);

/**
 * ff_rally_when_echo — the WHEN chip's display word ("Now" / "+15m" /
 * "+30m"). Same `when_sel` vocabulary as ff_rally_when_suffix.
 */
char const *ff_rally_when_echo(int when_sel);

/**
 * ff_rally_compose_name — compose the wire rally NAME from a base place
 * name plus the WHEN suffix (ff_rally_when_suffix(when_sel)), writing into
 * `out` (capacity `out_cap`, which must be at least
 * `FF_PROTO_RALLY_NAME_MAX + 1`). The PLACE is truncated to fit; the
 * SUFFIX never is (S24 AC6: "truncate the PLACE, never the time suffix" —
 * a truncated time claim would be dishonest).
 *
 * Returns false — `out` left unwritten, the caller sends NOTHING — if
 * `place` is NULL/empty, `out_cap` is too small, or the suffix alone
 * would not leave room for at least one place byte (a rally with no
 * place at all is never sent).
 */
bool ff_rally_compose_name(char const *place, int when_sel, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* FF_RALLY_H */
