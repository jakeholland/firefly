/**
 * fixture.h — ff_app_state_t JSON fixture loader (S13 slice b).
 *
 * Loads the JSON schema documented in tests/fixtures/README.md into a
 * caller-owned ff_app_state_t. Platform code (file I/O), lives under
 * targets/sim/ rather than core/ or app/ — see docs/ARCHITECTURE.md
 * principle 4 ("platform code lives only under targets/").
 *
 * Zero dynamic allocation (fixed jsmn token arena, same pattern as
 * firmware/festpack/src/fp_pack.c). Not reentrant — one fixture load at a
 * time, matching every caller's actual usage (ffsim loads at most one
 * fixture per process invocation).
 */
#ifndef FF_FIXTURE_H
#define FF_FIXTURE_H

#include <stddef.h>

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FF_FIXTURE_OK = 0,
    FF_FIXTURE_ERR_IO,      /* file missing/unreadable, or too large */
    FF_FIXTURE_ERR_JSON,    /* malformed/truncated/non-object JSON */
    FF_FIXTURE_ERR_TOO_BIG, /* input exceeded the parser's size/token budget,
                                OR a section's array (radar.dots/now.rows/
                                now.lineup/signals.items) exceeded its
                                documented cap: FF_CREW_MAX
                                (core/include/ff_crew.h) for radar.dots,
                                FF_APP_NOW_MAX_ROWS/FF_APP_NOW_MAX_LINEUP/
                                FF_APP_SIGNALS_MAX_ITEMS (ff_app_state.h)
                                for the other three.
                                Fail-loud by design (PR #12 review ruling):
                                an over-cap array is rejected outright, not
                                silently truncated, so a fixture that grows
                                past a cap gets a loud, attributable failure
                                instead of a quietly-dropped entry that only
                                surfaces later as an unrelated-looking
                                golden diff. */
    FF_FIXTURE_ERR_BAD_ENUM, /* an enum-string key (`face`, `radar.mode`,
                                `now.state`, `signals.items[].kind`,
                                `compose.mode`, `settings.share_mode`) is
                                PRESENT but its value isn't one of that
                                key's documented strings (or isn't a JSON
                                string at all). Fail-loud by design
                                (issue #28, orchestrator ruling): fixtures
                                feed the golden suite, so a typo'd enum
                                that silently defaulted rendered a
                                DIFFERENT state and then committed it as
                                the golden — a test green forever about
                                the wrong screen (the exact vacuous-test
                                failure mode PR #36's memset/renumbering
                                incident hit). The loader prints a
                                one-line stderr diagnostic naming the bad
                                key and value. An ABSENT enum key is NOT
                                this error — it takes its documented
                                default (absent != malformed; in
                                particular `face` omitted defaults to
                                `radar`, PR #36's deliberate exception). */
} ff_fixture_result_t;

/**
 * ff_fixture_load_json — parse `json[0..len)` into `*out`.
 *
 * On any return other than FF_FIXTURE_OK, `*out` is left zeroed, not
 * partially populated (matches fp_parse()'s contract). Fields the JSON
 * omits are left at their zero value in `*out` — every section is
 * optional (see tests/fixtures/README.md).
 */
ff_fixture_result_t ff_fixture_load_json(char const *json, size_t len, ff_app_state_t *out);

/**
 * ff_fixture_load_file — read `path` and parse it via
 * ff_fixture_load_json(). FF_FIXTURE_ERR_IO if the file can't be opened,
 * is empty, or exceeds the loader's input-size budget.
 */
ff_fixture_result_t ff_fixture_load_file(char const *path, ff_app_state_t *out);

/**
 * ff_fixture_stem — the filename component of `path` with its directory
 * and a trailing ".json" extension stripped, e.g.
 * "tests/fixtures/radar_live.json" -> "radar_live". Used to name the
 * headless renderer's output PNG (`<fixture-stem>.png`). Always
 * NUL-terminates `out` (truncating if `out_sz` is too small); writes ""
 * if `path` is NULL.
 */
void ff_fixture_stem(char const *path, char *out, size_t out_sz);

/* -----------------------------------------------------------------------
 * S13c — the inverse direction: ff_app_state_t -> JSON.
 *
 * Written for the ctl socket's `{"cmd":"state"}` response (docs/specs/
 * S13-sim-target.md slice c: "dump the current ff_app_state_t as JSON,
 * the same schema the fixture loader reads, so dumps round-trip as
 * fixtures"). Emits exactly the schema documented in
 * tests/fixtures/README.md/parsed by ff_fixture_load_json above — every
 * field this file's loader understands, the dumper writes, so
 * `ff_fixture_load_json(dump(s)) == s` for any `s` reachable through the
 * loader (dump -> load round-trips; the reverse isn't guaranteed, since a
 * hand-authored fixture may omit fields that then read back as their
 * documented zero default, which is exactly what gets dumped for them
 * too, so it still round-trips — the only source of divergence would be a
 * fixture deliberately relying on "key entirely absent" vs "key present
 * with the zero value", which the loader treats identically).
 * --------------------------------------------------------------------- */

/* Output budget for ff_fixture_dump_json: generous enough for a
 * maximally-populated state (8 radar dots, 3 now rows, 8 signal items,
 * every string field at its cap) with headroom — see test_fixture.c's
 * `dump_max_populated_state_fits_budget` for the actual worst-case size
 * check. Matches the same "fixed arena, no allocation surprises" budget
 * discipline as FIX_MAX_JSON_LEN in fixture.c (kept here, not there,
 * because callers sizing a response/scratch buffer need it without
 * pulling in fixture.c's internals). */
#define FF_FIXTURE_DUMP_MAX (8u * 1024u)

/**
 * ff_fixture_dump_json — serialize `*s` into `buf` as a single JSON
 * object (no trailing newline), matching the schema ff_fixture_load_json
 * parses. Every string field is JSON-escaped (quotes/backslashes/control
 * bytes) — state can carry live mesh data (node long/short names, free
 * text messages) that is not developer-authored and must not be able to
 * break the response framing.
 *
 * Returns the number of bytes written (excluding the NUL terminator, always
 * NUL-terminated on success) on success, or -1 if the serialized form
 * would not fit in `buf_sz` (buf is left in an unspecified, NOT
 * necessarily NUL-terminated state in that case — callers must check the
 * return value before using `buf`) or if `s`/`buf` is NULL/`buf_sz` is 0.
 */
int ff_fixture_dump_json(ff_app_state_t const *s, char *buf, size_t buf_sz);

#ifdef __cplusplus
}
#endif

#endif /* FF_FIXTURE_H */
