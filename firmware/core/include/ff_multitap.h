/**
 * ff_multitap.h — S10 quick flare: a pure "N presses within a window,
 * no gap too long" counter FSM.
 *
 * Spec: docs/specs/S10-flare.md's Amendments (quick flare, 2026-09-03;
 * timing/robustness pass, `fix/quick-flare-detection`, 2026-09-03).
 * "press the HOME (BOOT, GPIO0) button 5 times quickly to flare to the
 * crew, no screen needed." This module is only the COUNTING decision —
 * "was that the 5th press of one real burst?" — fed press EDGES the
 * caller already has, each carrying ITS OWN timestamp of when that
 * physical press actually happened (see `ff_multitap_press`'s doc
 * comment — the `now_ms` parameter has always meant "this edge's own
 * instant," not "whatever tick is running right now"; the
 * `fix/quick-flare-detection` pass is what made every caller in this
 * tree actually pass a precise per-edge value instead of a debounce-
 * delayed tick timestamp — see `ff_shell_multitap_edge`,
 * app/include/ff_shell.h). It knows nothing about buttons, HOME,
 * flares, ISRs, or the shell; the shell (app/ff_shell.c,
 * `ff_shell_multitap_edge`) is what turns a true return from
 * `ff_multitap_press` into the actual flare-start action — that
 * composition is the shell's job, not this module's, so it can be
 * gesture-agnostic and unit-tested with bare integers, including a
 * "several edges delivered late in one batch" scenario (see
 * `ff_multitap_press`'s doc comment) that a tick-driven counter with no
 * per-edge timestamp could never pass.
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
 * run — a gap of AT LEAST this length (>= 700 ms, INCLUSIVE — the same
 * boundary convention `ff_time_reached` documents: "now_ms == deadline_ms
 * already reached") resets the count (the press that finds the gap too
 * long starts a fresh run of 1, it is not dropped).
 *
 * RELAXED 400 -> 700 ms, `fix/quick-flare-detection` (2026-09-03,
 * maintainer report: "the 5 click is a bit finicky"). See that PR's own
 * body for the full worst-case arithmetic, but the short version: the
 * device's BOOT sampling cadence (`app_main.c`'s render-loop tick, 20 ms
 * nominal) plus the 30 ms debounce hold (`ff_button.h`'s
 * `FF_BUTTON_DEBOUNCE_MS`) plus a slow frame (a face rebuild under
 * `ff_display_lock`) can each eat tens of ms off the *effective* gap
 * between two presses that were physically closer together than that —
 * 400 ms left very little headroom for a real thumb's cadence once that
 * jitter was subtracted. 700 ms is still comfortably shorter than an
 * idle thumb's accidental resting cadence near BOOT and short enough
 * that a pocket brushing the button a few times does not read as one
 * run, but it absorbs the jitter budget above with room to spare. */
#define FF_MULTITAP_MAX_GAP_MS ((uint32_t)700u)

/** The shortest gap between two consecutive presses that is believed to
 * be a GENUINE second press rather than mechanical/ISR contact bounce
 * of the SAME physical press. A gap strictly LESS than this (< 30 ms,
 * so exactly 30 ms itself already counts as genuine — `ff_time_reached`'s
 * same inclusive convention) is ignored entirely: no state is mutated
 * (`count`/`first_ms`/`last_ms` all left exactly as they were), the
 * edge is not counted toward the run, and it does not extend, reset, or
 * fire anything — the caller sees this exactly as if the edge had never
 * arrived at all (`fix/quick-flare-detection`, 2026-09-04, moved into
 * core after review: a device's GPIO ISR can legitimately fire more
 * than once for a single physical press, and de-duplicating that is
 * domain logic about "was this really a second press", which belongs
 * here alongside the gap/window rules, not bolted onto a caller).
 *
 * Independent of, and much smaller than, `FF_MULTITAP_MAX_GAP_MS` above
 * — the two constants answer opposite questions on the same axis ("how
 * short is too short to be real" vs. "how long is too long to still be
 * the same run"). 30 ms mirrors `ff_button.h`'s `FF_BUTTON_DEBOUNCE_MS`
 * BY VALUE (a mechanical button's real bounce settles in a similar
 * window regardless of which layer is doing the rejecting) — kept as
 * an independent constant rather than shared, same "zero dependency on
 * that header" reasoning `ff_button.h`'s own top comment gives for not
 * sharing ITS constant with `ff_power_fsm.h` either.
 *
 * Only applies once a run is already in progress (`count > 0`) — a
 * run's very first press has no prior press to bounce against and is
 * never rejected. */
#define FF_MULTITAP_BOUNCE_MS ((uint32_t)30u)

/** The longest the WHOLE run (first press to Nth) may span, measured
 * from the run's first press. Independent of, and in addition to, the
 * per-gap bound above: five presses each exactly at the per-gap limit
 * would otherwise total `4 * FF_MULTITAP_MAX_GAP_MS` = 2800 ms (at the
 * relaxed 700 ms gap bound below), comfortably inside this window, but
 * a slower, still-individually-legal cadence must not be allowed to
 * stretch the whole gesture out indefinitely — a "quick flare" that
 * takes visibly too long to complete stops reading as one deliberate
 * burst.
 *
 * RELAXED 2500 -> 4000 ms, `fix/quick-flare-detection` (2026-09-03,
 * same pass as the gap bound above) — scaled up roughly in step with
 * the gap bound (4000 / 700 leaves more headroom than 2500 / 400 did,
 * deliberately: a human doing a slower, still-clearly-deliberate 5-tap
 * burst at a relaxed pace should not have the window bound fire ahead
 * of the gap bound). At these constants a run with every gap exactly at
 * the per-gap limit still spans only 2800 ms, comfortably under 4000 ms,
 * so — as before this change — the window bound in practice never fires
 * ahead of the per-gap bound; it exists as an independent backstop, not
 * because it is expected to be the thing that resets a real run. */
#define FF_MULTITAP_WINDOW_MS ((uint32_t)4000u)

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
 * ff_multitap_press — feed one press EDGE, timestamped at `now_ms` —
 * the instant THAT EDGE actually happened, not necessarily "now" in any
 * wall-clock sense at the moment this function is called. This is what
 * makes the FSM robust to a caller that cannot always call it promptly:
 * a device that captured several real edges (say, in an ISR-fed ring
 * buffer, `ff_power_boot_take_edges`) and only got around to draining
 * and delivering them on a later, slower tick can call this once per
 * drained edge, each with THAT edge's own recorded timestamp, and the
 * gap/window math below comes out identical to if each had been
 * delivered the instant it happened — a "5 edges recorded at 0/250/
 * 500/750/1000 ms but all drained and delivered together on one later
 * tick" batch counts exactly the same as if delivered live (see
 * `test_multitap.c`'s `S10_multitap_late_batch_delivery_...` tests).
 * This function DOES detect and discard closely-spaced duplicate edges
 * itself (`fix/quick-flare-detection`, 2026-09-04 — moved into core
 * after review; a real press is expected to appear here as ONE edge,
 * not a burst of them a few ms apart — see `FF_MULTITAP_BOUNCE_MS`'s
 * own doc comment for why this belongs here). Returns true EXACTLY on
 * the edge counted as the `FF_MULTITAP_COUNT`th (5th) press of one
 * continuous run — i.e. exactly once per completed gesture, never on
 * the 4th or the 6th.
 *
 * Rules, in the order they're evaluated:
 *  0. If a run is already in progress (`count > 0`) and the gap since
 *     the last press has NOT yet reached `FF_MULTITAP_BOUNCE_MS` (30 ms
 *     — i.e. this edge is `< 30 ms` after the last one), this edge is
 *     bounce: return false immediately, mutating NOTHING (`count`,
 *     `first_ms`, `last_ms` all stay exactly as they were) — the caller
 *     cannot even tell this call happened by inspecting the struct
 *     afterward.
 *  1. Otherwise, if a run is already in progress (`count > 0`) and
 *     either the gap since the last press has REACHED
 *     `FF_MULTITAP_MAX_GAP_MS` (i.e. is `>=`, not `>` —
 *     `ff_time_reached`'s inclusive boundary), OR the total span since
 *     the run's first press has likewise reached `FF_MULTITAP_WINDOW_MS`,
 *     the run resets — THIS press starts a brand-new run of 1 (it is
 *     the first press of the new run, not dropped and not counted
 *     toward the old one).
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
