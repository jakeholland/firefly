/**
 * ff_idle.h — S26 slice (c): inactivity -> dim -> screen off.
 *
 * Spec: docs/specs/S26-device-lifecycle.md, "(c) Inactivity -> dim ->
 * screen off". Per this repo's house rule (CLAUDE.md: "all logic goes
 * in firmware/core/"), the DECISION of when the screen should dim or
 * turn off is pure function of (now_ms, input events, a keep-awake
 * predicate) and belongs here — the esp32s3 target only enacts it
 * (backlight percent + skipping face rebuilds; see
 * targets/esp32s3/main/app_main.c) and the sim's ctl loop mirrors the
 * same enact split for its own AC3 test
 * (targets/sim/tests/test_idle_render_skip.c).
 *
 * ## States
 *
 * ```
 * ACTIVE --(idle >= FF_IDLE_T_DIM_MS)--> DIM --(idle >= FF_IDLE_T_OFF_MS)--> OFF
 *    ^------------------------ ff_idle_input() from ANY state -------------^
 * ```
 *
 * "idle" is elapsed time since the last `ff_idle_input()` call (or
 * since init) — NOT time-in-DIM. Both thresholds are measured from the
 * SAME reference instant, so a single large `now_ms` jump with no
 * intervening ticks lands directly in the right state (OFF at 30000 ms
 * idle even if DIM's 15000 ms boundary was never separately ticked
 * through) — this is what AC1's boundary tests check.
 *
 * ## Keep-awake
 *
 * While the caller's `keep_awake` predicate is true, `ff_idle_tick`
 * forces (and holds) ACTIVE and continuously re-pins the idle reference
 * to `now_ms` — so no elapsed time accrues toward a future DIM/OFF
 * while it holds, and once it releases, idle time starts counting fresh
 * from that instant (not backdated to whenever the predicate first
 * became true). The three sources this spec names (flare takeover
 * pending, power menu open, touch calibration running) are combined
 * into one bool by the shell-level predicate `ff_shell_keep_awake`
 * (app/include/ff_shell.h) — this header takes the already-combined
 * bool so it stays free of any app/UI type.
 *
 * ## OFF is sticky
 *
 * Once OFF (whether reached by elapsed idle time or by
 * `ff_idle_force_off`), `ff_idle_tick` will not walk it back down to
 * DIM/ACTIVE on its own — only `ff_idle_input` (a real wake: touch, PWR
 * SHORT_PRESS while OFF, BOOT) or a newly-true `keep_awake` does that.
 * This is what makes `ff_idle_force_off` (PWR SHORT_PRESS while ACTIVE
 * -> "go OFF immediately", docs/specs/S26-device-lifecycle.md slice c)
 * stick even though the elapsed idle time at the moment of the press
 * may be far short of `FF_IDLE_T_OFF_MS`.
 *
 * Fully-defined (not opaque), same convention as `ff_flare_t` /
 * `ff_power_fsm_t`: safe on the stack or in a static; zero-initialise
 * (state reads ACTIVE, the enum's 0 value) or call `ff_idle_init()`
 * before first use.
 */
#ifndef FF_IDLE_H
#define FF_IDLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Idle time (since the last input / reset) at which the screen dims. */
#define FF_IDLE_T_DIM_MS ((uint32_t)15000u)

/** Idle time (since the last input / reset — NOT since entering DIM) at
 * which the screen turns off. */
#define FF_IDLE_T_OFF_MS ((uint32_t)30000u)

typedef enum {
    FF_IDLE_STATE_ACTIVE = 0,
    FF_IDLE_STATE_DIM,
    FF_IDLE_STATE_OFF,
} ff_idle_state_t;

typedef struct {
    ff_idle_state_t state;
    /** The instant idle time is measured from: the last `ff_idle_input`
     * call, or (while `keep_awake` holds) continuously re-pinned to the
     * current tick's `now_ms` — see this header's top comment. */
    uint32_t ref_ms;
} ff_idle_t;

/** Zero the FSM: ACTIVE, no idle time accrued. NULL-safe (no-op). */
void ff_idle_init(ff_idle_t *idle);

/**
 * ff_idle_tick — advance the FSM to `now_ms`. Call every tick regardless
 * of whether anything changed (same "always tick" contract as
 * `ff_flare_tick` / `ff_power_fsm_tick`). `keep_awake` is the
 * already-combined predicate (see this header's top comment) — while
 * true, forces and holds ACTIVE and prevents any idle time from
 * accruing; while false, elapsed idle time since the last
 * `ff_idle_input` (or since `keep_awake` last released) drives ACTIVE ->
 * DIM at `FF_IDLE_T_DIM_MS` and -> OFF at `FF_IDLE_T_OFF_MS`. OFF is
 * sticky (see top comment) — natural ticking never reverses it. Returns
 * the resulting state (also readable via `ff_idle_state`). Returns
 * `FF_IDLE_STATE_ACTIVE` for a NULL `idle` (safe default; no state is
 * mutated).
 */
ff_idle_state_t ff_idle_tick(ff_idle_t *idle, uint32_t now_ms, bool keep_awake);

/**
 * ff_idle_input — a real wake/reset event (touch, PWR SHORT_PRESS while
 * OFF, BOOT, and later a notification — docs/specs/S26-device-lifecycle.md
 * slice d) from ANY current state, including OFF. Sets state to ACTIVE
 * and pins the idle reference to `now_ms`, so the next `FF_IDLE_T_DIM_MS`
 * countdown starts fresh from this instant. NULL-safe (no-op).
 */
void ff_idle_input(ff_idle_t *idle, uint32_t now_ms);

/**
 * ff_idle_force_off — PWR SHORT_PRESS while ACTIVE: "go OFF immediately"
 * (docs/specs/S26-device-lifecycle.md slice c), independent of how much
 * idle time has actually elapsed. Sets state to OFF; the idle reference
 * (`ref_ms`) is left untouched, which is harmless — see this header's
 * "OFF is sticky" note: a subsequent `ff_idle_tick` with `keep_awake`
 * false will keep computing OFF (elapsed time only grows), so the forced
 * state never needs its own separate "why am I off" bit. NULL-safe
 * (no-op).
 */
void ff_idle_force_off(ff_idle_t *idle);

/** ff_idle_state — the current state. Returns `FF_IDLE_STATE_ACTIVE` for
 * a NULL `idle` (same safe-default convention as `ff_idle_tick`). */
ff_idle_state_t ff_idle_state(ff_idle_t const *idle);

#ifdef __cplusplus
}
#endif

#endif /* FF_IDLE_H */
