/**
 * ff_dbgcmd.h — core/dbgcmd: the bench/debug console's line-command
 * PARSER (command policy, no I/O).
 *
 * Motivation: on 2026-09-05 every bench send test needed a human tap on
 * the puck. This module is the pure half of the fix — an opt-in
 * bench/debug console (`CONFIG_FF_DEBUG_CONSOLE`,
 * firmware/targets/esp32s3/main/Kconfig.projbuild) that lets an
 * end-to-end test drive the puck over its USB-Serial-JTAG port instead
 * of the touchscreen. See docs/hardware/comms-brain.md, "Bench console",
 * for the full command reference and an example session, and
 * `firmware/app/include/ff_debug_console.h` for the half that actually
 * DISPATCHES a parsed command against a live `ff_shell_t`.
 *
 * CLAUDE.md's placement rule puts this here, not in `firmware/app/`:
 * turning one line of ASCII into a structured, bounds-checked command is
 * command POLICY with zero I/O — no shell, no mc_client, no LVGL, no
 * serial port. It compiles and is unit-tested (Unity, firmware/core/
 * tests/test_dbgcmd.c) exactly like any other core module, on every
 * target, regardless of whether `CONFIG_FF_DEBUG_CONSOLE` is even a
 * thing in that build (the esp32s3 device target is the only build with
 * that Kconfig symbol at all; the sim/host build has no Kconfig and
 * always compiles this in — see ff_debug_console.h for where the
 * device-only Kconfig gate actually applies).
 *
 * ## Table-driven, bounded, CRLF-tolerant
 * `ff_dbgcmd_parse()` takes a raw byte buffer and an explicit LENGTH
 * (never a NUL-terminated C string — a line straight off a serial
 * driver is not guaranteed to be one, and reading past a caller-given
 * length is exactly the class of bug a 300-byte-line Unity test exists
 * to catch). It never calls `strlen`/`strcpy` on the input; every loop
 * is bounded by the passed-in `line_len` or by a `sizeof` of a
 * fixed-size local/output buffer. A line longer than
 * `FF_DBGCMD_LINE_MAX` bytes is rejected outright (`FF_DBGCMD_ERR_TOO_LONG`)
 * before a single byte is copied — never silently truncated, per this
 * repo's "honest data over pretty data" rule (CLAUDE.md): a bench
 * engineer who typed (or scripted) a too-long line should see it
 * rejected, not have it quietly clipped into a different, shorter
 * command.
 *
 * CRLF-tolerant: trailing `\r`, `\n`, or `\r\n` are trimmed before
 * anything else runs, so a command works whether the caller stripped
 * the line ending already (the esp32s3 driver does) or handed it over
 * verbatim (a unit test finding it more convenient to write `"me\r\n"`).
 *
 * ## Union validity — per kind, `ff_intent_t`'s own convention
 * Exactly the member(s) `kind` documents are meaningful on a
 * `FF_DBGCMD_OK` result; everything else in the struct is unspecified.
 * `ff_dbgcmd_parse` zero-initializes `*out` on every call (even a
 * rejected one) so a caller that reads a field it shouldn't gets
 * deterministic zeros, not stack garbage — but the CONTRACT is "per
 * kind", exactly like `ff_intent_t` (app/include/ff_intent.h).
 *
 * ## Command table (docs/hardware/comms-brain.md has the full reference)
 *   help                    — command list
 *   me                      — my node id / link / position / wall clock
 *   roster                  — paired crew: id, name, presence, position
 *   heard                   — heard-but-unpaired node ids
 *   send <text>             — crew broadcast (composer's SEND path)
 *   dm <node_hex> <text>    — addressed send to one node
 *   flare | flare cancel    — quick flare start/cancel
 *   wall                    — wall-clock latch dump
 *   i2c                     — shared I2C bus scan + one-shot compass status
 *   cal | cal start | cal finish | cal cancel | cal clear
 *                           — S12 step 3: the compass calibration ritual
 *   name | name <text>     — NAME in Settings: status / set + mesh push
 *   diag                    — DIAGNOSTICS: link/position/mesh/time/compass/
 *                             device facts, the same ones the Settings
 *                             DIAGNOSTICS page shows
 *   ping <node_hex>         — S29 PR2: one immediate bench PING, outside
 *                             the 10s/5min FIND session machinery
 *   find <node_hex>         — S29 PR2: start an ordinary FIND session on
 *                             that node, exactly as the UI gesture would
 *   find off                — S29 PR2: cancel the active FIND session
 * Anything else is `FF_DBGCMD_ERR_UNKNOWN` — the dispatcher's reply for
 * that is the fixed string `"dbg: ? try help"` (S16-style "the shell
 * decides", except here the deciding is this table).
 *
 * `i2c` is zero-arg like `me`/`roster`/`heard`/`wall` (this parser
 * rejects any trailing argument with `FF_DBGCMD_ERR_BAD_ARGS`, same as
 * those). It carries no I2C policy of its own — this module has zero
 * I/O per CLAUDE.md's placement rule — it only recognizes the verb; the
 * actual bus scan and compass read live behind a platform hook in
 * `firmware/app/include/ff_debug_console.h`, supplied by the device
 * target and left NULL on the sim (see that header for the "unavailable
 * on this target" honest-degrade contract).
 *
 * `cal` (S12 step 3, added alongside the compass calibration ritual)
 * follows `flare`/`flare cancel`'s exact shape — a bare verb plus one of
 * a small fixed set of sub-verbs, each its OWN `ff_dbgcmd_kind_t` (not a
 * single kind with a sub-command payload field, matching `FLARE`/
 * `FLARE_CANCEL`'s own precedent) — extended to four sub-verbs instead
 * of one:
 *   cal          — status: progress/sample count of an active session,
 *                  or the persisted calibration's valid/invalid state
 *   cal start    — begin a new session (a no-op if one is already active)
 *   cal finish   — attempt to end the session and persist the fit
 *   cal cancel   — abandon the session, persisted calibration untouched
 *   cal clear    — drop the PERSISTED calibration back to identity
 * This parser carries no calibration policy of its own (same "zero I/O,
 * zero policy" split every other verb here keeps) — the dispatcher
 * (`firmware/app/ff_debug_console.c`) routes all five straight through
 * `ff_shell_intent`/`ff_shell_compass_cal_status`, the SAME seam the
 * Settings ritual screen uses, never a second path into shell state.
 *
 * `name` (NAME in Settings) follows `send`'s exact shape (a bare verb
 * that also accepts a rest-of-line argument), not `cal`'s (a bare verb
 * plus one of a small fixed set of sub-verbs) — an arbitrary NAME has no
 * fixed vocabulary to enumerate:
 *   name          — status: stored puck name, mesh-reported name (if
 *                   any), confirmed/pending, plus the confirmation-fix
 *                   follow-up's own record of the CURRENT push (what was
 *                   pushed, its routing ack/nak, and any get_owner_
 *                   request reply) — see `ff_debug_console.c`'s
 *                   `dbgconsole_name_status` for the exact line format
 *   name <text>   — commit `<text>` through the SAME path the Settings
 *                   NAME row's DONE button uses (sanitize, persist,
 *                   push the Meshtastic owner update) — so the
 *                   coordinator can test the mesh push over USB without
 *                   the touchscreen
 * The rest-of-line argument is NOT trimmed/sanitized by this parser
 * (same "this module carries zero policy" rule every other verb here
 * keeps) — `<text>` is handed to the dispatcher verbatim, which routes
 * it through `FF_INTENT_SETTINGS_NAME_COMMIT`'s own path
 * (`ff_shell.c`'s `shell_apply_name_commit`, core's
 * `ff_meshname_sanitize`) exactly like a real T9-authored name would be.
 *
 * `diag` (DIAGNOSTICS) is zero-arg like `me`/`roster`/`heard`/`wall`/`i2c`
 * above — a bare status dump, no sub-verb, no argument. It carries no
 * projection policy of its own (same "zero I/O, zero policy" split every
 * verb here keeps): the dispatcher (`ff_debug_console.c`'s
 * `dbgconsole_diag`) reads the SAME `ff_app_diag_t` the Settings
 * DIAGNOSTICS page renders, via `ff_shell_diag_debug` (`ff_shell.h`) — one
 * projection, two presentations, never a second computation.
 *
 * `mic` (S30 mic bring-up, docs/specs/S30-audio-input.md) follows
 * `cal`'s exact shape — a bare verb plus one of a small fixed set of
 * sub-verbs, each its OWN `ff_dbgcmd_kind_t`:
 *   mic          — one-shot status + level (present/running/rms/peak/
 *                  envelope), all honest when absent — the bare status
 *                  a bench operator checks before/after `mic on`
 *   mic on       — start the I2S1 mic channel + reader task
 *   mic off      — stop them (channel disabled, task idle)
 *   mic watch <secs> — print RMS/peak/envelope once per 250ms for up to
 *                  30s, then stop; `<secs>` is 1-30 decimal, no prefix.
 *                  A value outside that range, or non-decimal, or a
 *                  missing/extra argument, is `FF_DBGCMD_ERR_BAD_ARGS` —
 *                  this parser enforces the 1-30 BOUND itself (not left
 *                  to the dispatcher) so a caller can never construct an
 *                  in-vocabulary `FF_DBGCMD_MIC_WATCH` with an out-of-
 *                  range duration in the first place.
 * This parser carries no mic policy of its own (same "zero I/O, zero
 * policy" split every other verb here keeps) — the dispatcher
 * (`firmware/app/ff_debug_console.c`) routes all four through a single
 * platform hook (`ff_dbgconsole_mic_fn`, `ff_debug_console.h`), honestly
 * unavailable on a target with no mic driver wired up (the sim).
 *
 * `mic dump <secs>` (2026-09-09 amendment, fix/s31-beat-real-audio,
 * docs/specs/S30-audio-input.md) is a FIFTH `mic` sub-verb, its own
 * `ff_dbgcmd_kind_t` (`FF_DBGCMD_MIC_DUMP`) alongside the four above —
 * same bare-verb-plus-fixed-sub-verb-plus-one-decimal-argument shape as
 * `mic watch <secs>`, reusing `parse_u32_dec` identically, just its own
 * (tighter — see `FF_DBGCMD_MIC_DUMP_MIN_S`/`_MAX_S`'s own doc comment)
 * range. Streams the reader task's raw 16kHz mono samples to the
 * console as base64 text (the coordinator's own capture workflow,
 * `tools/beat_replay.py`) — this parser still carries no mic policy of
 * its own; the actual ring-buffer/streaming plumbing lives entirely in
 * the esp32s3 target (`ff_mic.h`'s own doc comment).
 */
#ifndef FF_DBGCMD_H
#define FF_DBGCMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max accepted raw line length in bytes, EXCLUDING any trailing CR/LF
 *  the caller included. A line at or under this is eligible to parse; a
 *  longer one is `FF_DBGCMD_ERR_TOO_LONG` regardless of content —
 *  checked before any CRLF trimming, so a 257-byte line with a trailing
 *  "\r\n" (259 bytes total) is still measured against the 256 that
 *  matter. */
#define FF_DBGCMD_LINE_MAX 256u

/** Max bytes (excluding the NUL terminator `ff_dbgcmd_t.text`/`u.dm.text`
 * always carries) for a SEND/DM message body. Comfortably under
 * `FF_DBGCMD_LINE_MAX` even behind the widest command header this parser
 * has (`"dm !aabbccdd "`, 13 bytes) — so for both SEND and DM this is
 * the bound that actually trips on an oversized body
 * (`FF_DBGCMD_ERR_BAD_ARGS`); the whole-line `FF_DBGCMD_LINE_MAX` gate
 * above exists independently, for a long line that is garbage for
 * other reasons (e.g. no recognizable command at all). */
#define FF_DBGCMD_TEXT_MAX 200u

/** `mic watch <secs>` bounds — see this header's top comment, "mic". */
#define FF_DBGCMD_MIC_WATCH_MIN_S 1u
#define FF_DBGCMD_MIC_WATCH_MAX_S 30u

/** `mic dump <secs>` bounds (2026-09-09 amendment, fix/s31-beat-real-
 *  audio) — see this header's top comment, "mic dump", just below.
 *  Capped at 10s (not `mic watch`'s 30s): a dump streams every raw
 *  sample as base64 text, not one summary line every 250ms, so the
 *  console/USB-serial-JTAG bandwidth budget is a lot tighter per second
 *  of capture — 10s is comfortably enough to catch a handful of beats
 *  at any real-world tempo without threatening to run away. */
#define FF_DBGCMD_MIC_DUMP_MIN_S 1u
#define FF_DBGCMD_MIC_DUMP_MAX_S 10u

/** Every line this parser recognizes. `FF_DBGCMD_NONE` is the zero value
 *  used for "nothing parsed yet" / a rejected line; it is never a
 *  successful parse's `kind`. */
typedef enum {
    FF_DBGCMD_NONE = 0,
    FF_DBGCMD_HELP,
    FF_DBGCMD_ME,
    FF_DBGCMD_ROSTER,
    FF_DBGCMD_HEARD,
    FF_DBGCMD_SEND,         /* u.text: crew broadcast body */
    FF_DBGCMD_DM,           /* u.dm.dest_node + u.dm.text */
    FF_DBGCMD_FLARE,        /* start a quick flare */
    FF_DBGCMD_FLARE_CANCEL, /* "flare cancel" */
    FF_DBGCMD_WALL,
    FF_DBGCMD_I2C,          /* I2C bus scan + one-shot compass status */
    FF_DBGCMD_CAL,          /* "cal" bare — compass-cal status */
    FF_DBGCMD_CAL_START,    /* "cal start" */
    FF_DBGCMD_CAL_FINISH,   /* "cal finish" */
    FF_DBGCMD_CAL_CANCEL,   /* "cal cancel" */
    FF_DBGCMD_CAL_CLEAR,    /* "cal clear" */
    FF_DBGCMD_NAME,         /* "name" bare — stored/mesh/confirmed status */
    FF_DBGCMD_NAME_SET,     /* "name <text>": u.text — commit + mesh push */
    FF_DBGCMD_DIAG,         /* DIAGNOSTICS: link/position/mesh/time/compass/device dump */
    FF_DBGCMD_PERF,         /* 2026-09-08 QA hardening — frame/flush timing, heap, stack high-water dump */
    FF_DBGCMD_PING,         /* S29 PR2: "ping <node_hex>" — u.node: one immediate bench PING */
    FF_DBGCMD_FIND,         /* S29 PR2: "find <node_hex>" — u.node: start a FIND session */
    FF_DBGCMD_FIND_OFF,     /* S29 PR2: "find off" — cancel the active FIND session */
    FF_DBGCMD_MIC,          /* S30: "mic" bare — one-shot status + level */
    FF_DBGCMD_MIC_ON,       /* S30: "mic on" */
    FF_DBGCMD_MIC_OFF,      /* S30: "mic off" */
    FF_DBGCMD_MIC_WATCH,    /* S30: "mic watch <secs>" — u.mic_watch_secs, 1-30 */
    FF_DBGCMD_MIC_DUMP,     /* 2026-09-09 amendment: "mic dump <secs>" — u.mic_dump_secs, 1-10 */
    FF_DBGCMD_MUSIC,        /* S31: "music" bare — source/loudness/bpm-estimate */
    FF_DBGCMD_MUSIC_SEED,   /* S31: "music seed <n>" — u.music_seed, bench-determinism reseed */
} ff_dbgcmd_kind_t;

/** Why a line failed to become a command. `FF_DBGCMD_ERR_EMPTY` is not
 *  really an error — a blank line (or one that is CRLF/whitespace only)
 *  is the console's own "just pressed Enter" case, and the dispatcher's
 *  contract (ff_debug_console.h) is to print nothing for it, not "dbg: ?
 *  try help". Every other value IS a rejection the dispatcher reports. */
typedef enum {
    FF_DBGCMD_ERR_OK = 0,
    FF_DBGCMD_ERR_EMPTY,
    FF_DBGCMD_ERR_TOO_LONG,
    FF_DBGCMD_ERR_UNKNOWN_CMD,
    FF_DBGCMD_ERR_BAD_ARGS,
} ff_dbgcmd_status_t;

/**
 * One parsed line. Validity is per-`kind`, exactly `ff_intent_t`'s
 * convention (app/include/ff_intent.h) — `u.text` is meaningful for
 * `FF_DBGCMD_SEND` AND `FF_DBGCMD_NAME_SET` (the same field, reused —
 * both are "the rest of the line is a text body" shapes with nothing
 * else to disambiguate on), `u.dm` only for `FF_DBGCMD_DM`, `u.node`
 * only for `FF_DBGCMD_PING`/`FF_DBGCMD_FIND` (S29 PR2 — a bare hex node
 * id, `parse_node_hex`'s exact shape, no text body), `u.mic_watch_secs`
 * only for `FF_DBGCMD_MIC_WATCH` (S30 — already validated into
 * `[FF_DBGCMD_MIC_WATCH_MIN_S, FF_DBGCMD_MIC_WATCH_MAX_S]` by this
 * parser), `u.music_seed` only for `FF_DBGCMD_MUSIC_SEED` (S31 — "music
 * seed <n>", a plain decimal via the same `parse_u32_dec` helper `mic
 * watch` uses, capped at that helper's own 3-digit/999 ceiling — no
 * further range check needed, unlike `mic watch`'s duration, since any
 * u32 is a legal PRNG seed); every other kind (including `FF_DBGCMD_
 * FIND_OFF`, `FF_DBGCMD_MIC`, `FF_DBGCMD_MIC_ON`, `FF_DBGCMD_MIC_OFF`,
 * `FF_DBGCMD_MUSIC`) carries no payload at all.
 */
typedef struct {
    ff_dbgcmd_kind_t kind;
    union {
        char text[FF_DBGCMD_TEXT_MAX + 1]; /* SEND: NUL-terminated body */
        struct {
            uint32_t dest_node;
            char     text[FF_DBGCMD_TEXT_MAX + 1]; /* NUL-terminated body */
        } dm;
        uint32_t node;             /* S29 PR2: PING/FIND target node id */
        uint32_t mic_watch_secs;   /* S30: "mic watch <secs>" duration, already bounds-checked */
        uint32_t mic_dump_secs;    /* 2026-09-09 amendment: "mic dump <secs>" duration, already bounds-checked */
        uint32_t music_seed;       /* S31: "music seed <n>" — the swarm PRNG seed to apply next build */
    } u;
} ff_dbgcmd_t;

/**
 * ff_dbgcmd_parse — parse one line into `*out`.
 *
 * `line` need not be NUL-terminated; exactly `line_len` bytes of it are
 * read, never more (the 300-byte-line Unity test exists to confirm this
 * — a buffer that ends exactly at `line_len` with no NUL byte anywhere
 * in it must not be over-read). `line_len == 0` is treated the same as
 * an all-whitespace line (`FF_DBGCMD_ERR_EMPTY`).
 *
 * `*out` is zero-initialized on every call, including a rejected one,
 * before this function does anything else — a caller that reads a field
 * it should not (violating the "per kind" contract above) gets
 * deterministic zeros, never stack garbage from a previous call.
 *
 * `line == NULL` or `out == NULL` returns `FF_DBGCMD_ERR_BAD_ARGS`
 * (`out == NULL` obviously cannot also be zeroed first).
 *
 * Returns `FF_DBGCMD_ERR_OK` iff `out->kind` is now a real command
 * (never `FF_DBGCMD_NONE` on that return value). Every other return
 * value leaves `out->kind == FF_DBGCMD_NONE`.
 */
ff_dbgcmd_status_t ff_dbgcmd_parse(char const *line, size_t line_len, ff_dbgcmd_t *out);

/**
 * ff_dbgcmd_kind_name / ff_dbgcmd_status_name — short, stable, all-caps
 * names for logging and test failure messages (mirrors this codebase's
 * existing `ff_*_name` convention, e.g. `ff_link_state_name`). Never
 * NULL; an out-of-range value maps to "?".
 */
char const *ff_dbgcmd_kind_name(ff_dbgcmd_kind_t kind);
char const *ff_dbgcmd_status_name(ff_dbgcmd_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* FF_DBGCMD_H */
