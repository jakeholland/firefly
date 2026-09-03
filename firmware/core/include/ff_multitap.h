/**
 * ff_multitap.h — S10 quick flare: a pure "N presses within a window,
 * no gap too long" counter FSM.
 *
 * Spec: docs/specs/S10-flare.md's Amendments (quick flare, 2026-09-03):
 * "press the HOME (BOOT, GPIO0) button 5 times quickly to flare to the
 * crew, no screen needed." This module is only the COUNTING decision —
 * "was that the 5th press of one real burst?" — fed by debounced press
 * EDGES the caller already has (`ff_button_tick`'s return, S26 slice e:
 * true exactly once per physical press, never a stream while held). It
 * knows nothing about buttons, HOME, flares, or the shell; the shell
 * (app/ff_shell.c, `ff_shell_home_press`) is what turns a true return
 * from `ff_multitap_press` into the actual flare-start action, and what
 * decides whether THIS press edge should also do the ordinary HOME
 * (navigate / wake) thing — that composition is the shell's job, not
 * this module's, so it can be gesture-agnostic and unit-tested with
 * bare integers.
 *
 * Pure C11, no I/O, no allocation, no clock-reading of its own — same
 * "explicit now_ms in, no hidden clock" shape as `ff_flare.h`/
 * `ff_button.h`. Safe on the stack or in a static; zero-initialize or
 * call `ff_multitap_init()` before first use.
 */
#ifndef FF_MULTITAP_H
#define FF_MULTITAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many presses complete the gesture. Pinned by a literal test
 * (`S10_multitap_count_is_5`, test_multitap.c) per this repo's proxy-
 * check failure mode (AGENTS.md item 6) — a test that only ever compares
 * against this macro symbolically would still pass if the macro's value
 * silently changed underneath a mutation. */
#define FF_MULTITAP_COUNT ((uint8_t)5u)

/** The longest gap allowed between two consecutive presses within one
 * run — a gap STRICTLY LONGER than this resets the count (the press that
 * finds the gap too long starts a fresh run of 1, it is not dropped).
 * 400 ms is comfortably longer than any real double-tap cadence but
 * short enough that an idle thumb resting near BOOT cannot accidentally
 * accumulate a run across unrelated presses minutes apart. */
#define FF_MULTITAP_MAX_GAP_MS ((uint32_t)400u)

/** The longest the WHOLE run (first press to Nth) may span, measured
 * from the run's first press. Independent of, and in addition to, the
 * per-gap bound above: five presses each exactly at the per-gap limit
 * would otherwise total `4 * FF_MULTITAP_MAX_GAP_MS` = 1600 ms, comfortably
 * inside this window, but a slower, still-individually-legal cadence
 * must not be allowed to stretch the whole gesture out indefinitely — a
 * "quick flare" that takes visibly too long to complete stops reading as
 * one deliberate burst. */
#define FF_MULTITAP_WINDOW_MS ((uint32_t)2500u)

/**
 * The whole FSM state. Fully-defined (not opaque), same convention as
 * `ff_button_t`/`ff_flare_t`: safe on the stack or in a static;
 * zero-initialize or call `ff_multitap_init()` before first use (zero
 * reads as "idle, no run in progress" — `count == 0`).
 */
typedef struct {
    uint8_t  count;     /* presses counted in the CURRENT run; 0 = idle, no run in progress */
    uint32_t first_ms;  /* when the run's first press landed; meaningful iff count > 0 */
    uint32_t last_ms;   /* when the run's most recently counted press landed; meaningful iff count > 0 */
} ff_multitap_t;

/** Zero the FSM: idle, no run in progress. NULL-safe (no-op). */
void ff_multitap_init(ff_multitap_t *m);

/**
 * ff_multitap_press — feed one debounced press EDGE (the caller has
 * already debounced — see `ff_button_tick`; this is called only on that
 * function's `true` return, never once per tick). Returns true EXACTLY
 * on the tick this press is counted as the `FF_MULTITAP_COUNT`th (5th)
 * press of one continuous run — i.e. exactly once per completed
 * gesture, never on the 4th or the 6th.
 *
 * Rules, in the order they're evaluated:
 *  1. If a run is already in progress (`count > 0`) and either the gap
 *     since the last press exceeds `FF_MULTITAP_MAX_GAP_MS`, OR the
 *     total span since the run's first press would exceed
 *     `FF_MULTITAP_WINDOW_MS`, the run resets — THIS press starts a
 *     brand-new run of 1 (it is the first press of the new run, not
 *     dropped and not counted toward the old one).
 *  2. Otherwise this press extends the current run (or starts one, if
 *     `count == 0`): `count` increments, `last_ms` (and `first_ms` if
 *     this is press 1) update to `now_ms`.
 *  3. If `count` has just reached `FF_MULTITAP_COUNT`, the run resets to
 *     idle (`count = 0`) — a 6th press starts a brand-new run of 1, per
 *     the spec's "a 6th tap starts a new count" — and this call returns
 *     true. Every other case returns false.
 *
 * Wraparound-safe against `now_ms` rollover (`ff_time_reached`,
 * platform/include/ff_clock.h — the same twos-complement-subtraction
 * convention every other core FSM in this repo uses). Returns false for
 * a NULL `m` (no state mutated).
 */
bool ff_multitap_press(ff_multitap_t *m, uint32_t now_ms);

/**
 * ff_multitap_pending — is a run currently in progress AND still live
 * (not yet expired by either the per-gap or whole-window bound at
 * `now_ms`)? For the caller's `keep_awake` input (S26 slice c,
 * `ff_shell_keep_awake`/`ff_idle_tick`): while a quick-flare sequence is
 * mid-burst, the puck must not fall back asleep between taps.
 *
 * Deliberately a PURE query — it never mutates `*m` (unlike
 * `ff_multitap_press`, which both decides and advances state) — because
 * the caller polls this every tick (S26's "call every tick" convention)
 * and a run's own natural expiry is decided and applied exactly once,
 * on the NEXT real press attempt via `ff_multitap_press`'s reset rule
 * above; this function just answers "as of now, would that reset have
 * already applied" without pre-emptively clearing state a following
 * press might still legitimately extend... except it never could: once
 * either bound has elapsed, `ff_multitap_press` always resets on the
 * next call anyway (rule 1 above), so this function's answer and that
 * eventual reset are always consistent — this is stated only to be
 * explicit that "pure query, no mutation" was a deliberate simplicity
 * choice and not a correctness gap.
 *
 * True iff `count > 0` AND `now_ms` has not reached either
 * `last_ms + FF_MULTITAP_MAX_GAP_MS` or `first_ms + FF_MULTITAP_WINDOW_MS`.
 * False for a NULL `m` (nothing pending) or an idle FSM (`count == 0`).
 */
bool ff_multitap_pending(ff_multitap_t const *m, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_MULTITAP_H */
