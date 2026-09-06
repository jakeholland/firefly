/**
 * ff_debug_console.h — the bench/debug console's DISPATCH half
 * (CONFIG_FF_DEBUG_CONSOLE).
 *
 * Spec: this feature has no docs/specs/SXX of its own (assigned
 * directly, not claimed from the spec backlog) — see
 * docs/hardware/comms-brain.md, "Bench console", for the command
 * reference this module implements, and this header's own doc comments
 * for the handful of interpretation calls made along the way.
 *
 * Pairs with `firmware/core/include/ff_dbgcmd.h` (the pure line PARSER)
 * to make the whole console: a device driver (or a test) reads a line,
 * hands it to `ff_dbgconsole_handle_line` here, and gets back zero or
 * more `"dbg: "`-prefixed reply lines through a caller-supplied
 * callback — never printf/ESP_LOG directly, so this file stays as
 * target-agnostic as the rest of `firmware/app/` and is exercised by
 * `firmware/app/tests/test_debug_console.c` against a REAL `ff_shell_t`
 * (unity + ctest), exactly like `test_shell.c` does for the intent seam.
 *
 * ## The seam discipline this module exists to enforce
 * Every command that ACTS goes through exactly one of:
 *   - `ff_shell_intent()` — `flare`/`flare cancel` dispatch
 *     `FF_INTENT_QUICK_FLARE`/`FF_INTENT_FLARE_END`, the SAME intents
 *     the physical 5-tap gesture and the sender overlay's CANCEL button
 *     use, chosen because both are already documented (ff_intent.h) to
 *     work "from any state, screen off included" / ungated on a
 *     takeover — exactly the "works no matter what the glass is
 *     showing" property a bench command needs;
 *   - `ff_shell_debug_send_text()` — `send`/`dm` dispatch this
 *     debug-only seam (ff_shell.h) rather than
 *     `ff_shell_intent(FF_INTENT_SEND_TEXT)`, because that intent is
 *     real Compose-modal UI navigation (pushes/pops the modal, consumes
 *     the live T9 draft, rejected under a takeover) and firing it from
 *     a bench command would both fight whatever the glass is actually
 *     showing and make a send's success depend on unrelated UI state.
 *     See that function's own doc comment for the full rationale.
 * Every command that only READS uses a public getter (`ff_shell_crew`,
 * `ff_shell_heard`, `ff_shell_link`, `ff_shell_my_node_id`,
 * `ff_shell_wall`/`ff_shell_wall_debug`, `ff_shell_wall_unix_now`,
 * `ff_shell_my_pos_debug`) — this file never reaches into `shell_t`
 * (it cannot; that struct is private to ff_shell.c) and never fabricates
 * a field a getter does not honestly provide (an unknown reads as
 * "?"/"unknown" in the reply, never a guess).
 *
 * ## Interpretation calls (see the PR body for the short version)
 *  - `roster` filters `ff_shell_crew()` to `paired == true` rows only —
 *    the struct's own doc comment allows a slot to exist unpaired (a
 *    member once paired, then unpaired), and "roster" means the crew a
 *    bench engineer actually cares about testing against.
 *  - `me`'s wall-clock line reports `ff_shell_wall_debug()`'s
 *    trust/source bookkeeping, which this feature ADDS (ff_shell.c's
 *    `has_last_wall_obs` doc comment) because nothing tracked it before
 *    — an honest new getter over honestly-recorded data, not a
 *    fabrication.
 *  - `send`/`dm` reject empty text and oversized text/lines at the
 *    PARSER (ff_dbgcmd.h) — by the time a command reaches this
 *    dispatcher it is already known-valid, so this file has no text-
 *    length policy of its own to restate.
 */
#ifndef FF_DEBUG_CONSOLE_H
#define FF_DEBUG_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#include "ff_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compiled in for a sim build unconditionally (so this dispatcher is
 * unit-testable against a real ff_shell_t) and for a device build only
 * when CONFIG_FF_DEBUG_CONSOLE=y — same gate, same rationale, as the
 * ff_shell.h bench/debug console section this file's dispatch logic
 * calls into exclusively. See that header's doc comment for the full
 * "compiled out, not defaulted off" contract. */
#if defined(FF_TARGET_SIM) || defined(CONFIG_FF_DEBUG_CONSOLE)

/**
 * ff_dbgconsole_reply_fn — one reply LINE, without a trailing newline
 * (the caller's transport owns line termination — CRLF over a serial
 * port, a plain `\n` appended by a test's capture buffer, whatever fits
 * the medium). Called once per line; a dump command (`roster`/`heard`)
 * calls it once per row plus a header/count line. `user` is the bound
 * context, passed back untouched, the same convention as
 * `ff_intent_emit_fn` (app/include/ff_intent.h).
 */
typedef void (*ff_dbgconsole_reply_fn)(void *user, char const *line);

/**
 * ff_dbgconsole_i2c_scan_fn / ff_dbgconsole_compass_status_fn — the
 * `i2c` command's platform hooks (docs/hardware/comms-brain.md, "Bench
 * console"). This file is app-layer, target-agnostic C — it cannot
 * touch I2C itself (CLAUDE.md's placement rule: I/O lives in the
 * target, not `firmware/app/`) — so the actual bus scan and compass
 * read are supplied by the CALLER as a pair of callbacks, exactly the
 * seam `ff_dbgconsole_reply_fn` already establishes for output:
 *
 *   - `i2c_scan` sweeps the shared I2C bus and writes a single
 *     human-readable line body (no "dbg: i2c " prefix — this file adds
 *     that) into `out`, NUL-terminated, at most `cap` bytes including
 *     the NUL. Returns 0 on success; a negative value means the scan
 *     could not run at all (e.g. the bus was never brought up) and
 *     produces the honest `"dbg: i2c scan failed"` reply instead of
 *     whatever partial/stale text might be sitting in `out`.
 *   - `compass_status` writes a one-shot compass status line body the
 *     same way (mag/imu presence, last heading, calibration state) —
 *     independent of whether the scan itself succeeded, since it comes
 *     from the compass driver's own state, not the bus sweep.
 *
 * The esp32s3 target supplies both (app_main.c) so a bench engineer
 * gets the same "is my magnetometer even wired up" answer the
 * coordinator's one-off bench patch gave, permanently and without
 * touching the touchscreen. The sim build has no I2C bus at all and
 * passes NULL for both — see `ff_dbgconsole_handle_line`'s own doc
 * comment for the resulting "unavailable on this target" reply, which
 * this module states honestly rather than fabricating a scan result.
 */
typedef int (*ff_dbgconsole_i2c_scan_fn)(void *user, char *out, size_t cap);
typedef int (*ff_dbgconsole_compass_status_fn)(void *user, char *out, size_t cap);

/**
 * ff_dbgconsole_handle_line — parse one raw line (via
 * `ff_dbgcmd_parse`) and dispatch it against `sh`, emitting zero or
 * more `"dbg: "`-prefixed reply lines through `reply`.
 *
 * `line`/`line_len` follow `ff_dbgcmd_parse`'s own contract exactly:
 * `line` need not be NUL-terminated, and exactly `line_len` bytes are
 * read. A blank/whitespace-only line (`FF_DBGCMD_ERR_EMPTY`) produces
 * NO reply at all — pressing Enter on an empty line is not an error.
 * Every other rejection (`TOO_LONG`/`UNKNOWN_CMD`/`BAD_ARGS`) produces
 * exactly one reply line: `"dbg: ? try help"` — the command table
 * doesn't distinguish WHY a line was bad in its reply text, on purpose:
 * a bench script parsing for the literal string `"dbg: ? try help"` has
 * one thing to check, not four.
 *
 * `sh == NULL` or `reply == NULL` is a safe no-op. `now_ms` is the
 * shell's own clock reading (mirrors every other call site in this
 * codebase — never `lv_tick_get()`/a raw platform tick — see
 * `ff_shell_now_ms`), used only where a command needs "now" for an age
 * computation that isn't already folded into a getter.
 *
 * `i2c_scan`/`compass_status` are the `i2c` command's platform hooks
 * (see their own typedefs' doc comment just above) — pass NULL for
 * either (or both) when the target has no I2C bus to scan (the sim) or
 * no compass driver built in (`CONFIG_FF_COMPASS=n`); `i2c_scan ==
 * NULL` alone makes the WHOLE `i2c` command reply with the single line
 * `"dbg: i2c unavailable on this target"` (no second compass line —
 * there is nothing to scan, so there is nothing to follow up on
 * either). `compass_status == NULL` with `i2c_scan` present still
 * prints the scan line; it just omits the compass line, honestly,
 * rather than printing one with fields it cannot answer.
 */
void ff_dbgconsole_handle_line(ff_shell_t *sh, char const *line, size_t line_len, uint32_t now_ms,
                                ff_dbgconsole_reply_fn reply, void *user, ff_dbgconsole_i2c_scan_fn i2c_scan,
                                ff_dbgconsole_compass_status_fn compass_status);

#endif /* FF_TARGET_SIM || CONFIG_FF_DEBUG_CONSOLE */

#ifdef __cplusplus
}
#endif

#endif /* FF_DEBUG_CONSOLE_H */
