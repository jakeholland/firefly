/**
 * ff_idle.h — S26 slices (c)+(f): inactivity -> dim -> screen off ->
 * timer-based light sleep.
 *
 * Spec: docs/specs/S26-device-lifecycle.md, "(c) Inactivity -> dim ->
 * screen off" and "(f) Light sleep — timer-based". Per this repo's
 * house rule (CLAUDE.md: "all logic goes in firmware/core/"), the
 * DECISION of when the screen should dim, turn off, or the device
 * should light-sleep is a pure function of (now_ms, input events, a
 * keep-awake predicate) and belongs here — the esp32s3 target only
 * enacts it (backlight percent + skipping face rebuilds +
 * `esp_light_sleep_start()`; see targets/esp32s3/main/app_main.c) and
 * the sim's ctl loop mirrors the same enact split for its own AC3 test
 * (targets/sim/tests/test_idle_render_skip.c).
 *
 * ## States
 *
 * ```
 * ACTIVE --(idle>=T_DIM_MS)--> DIM --(idle>=T_OFF_MS)--> OFF --(idle>=T_OFF_MS+T_SLEEP_MS, !sleep_inhibit)--> SLEEP
 *    ^-------------------------------- ff_idle_input() from ANY state ------------------------------------------^
 * ```
 *
 * "idle" is elapsed time since the last `ff_idle_input()` call (or
 * since init) — NOT time-in-DIM or time-in-OFF. Every threshold is
 * measured from the SAME reference instant, so a single large `now_ms`
 * jump with no intervening ticks lands directly in the right state
 * (SLEEP at `FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS` idle even if the
 * DIM/OFF boundaries were never separately ticked through) — this is
 * what AC1's boundary tests check. Slice (f)'s AC names this as "after
 * OFF for FF_IDLE_T_SLEEP_MS", which reads the same way in the only
 * path that reaches SLEEP by natural ticking: OFF is itself entered at
 * exactly `ref_ms + FF_IDLE_T_OFF_MS`, so "T_SLEEP_MS after OFF" and
 * "T_OFF_MS + T_SLEEP_MS after the reference instant" are the same
 * instant. (The one case they can diverge is `ff_idle_force_off` from
 * ACTIVE well before `FF_IDLE_T_OFF_MS` of real idle time has elapsed —
 * see that function's doc comment: it deliberately leaves `ref_ms`
 * untouched, so SLEEP in that case is measured from the same
 * still-old reference, i.e. from the LAST REAL INPUT, not from the
 * forced-OFF instant. Flagged as this slice's interpretation call,
 * PR body — the common case (PWR short-press soon after the last
 * touch) makes the two reference points coincide in practice.)
 *
 * ## Keep-awake
 *
 * While the caller's `keep_awake` predicate is true, `ff_idle_tick`
 * forces (and holds) ACTIVE and continuously re-pins the idle reference
 * to `now_ms` — so no elapsed time accrues toward a future DIM/OFF/SLEEP
 * while it holds, and once it releases, idle time starts counting fresh
 * from that instant (not backdated to whenever the predicate first
 * became true). The three sources this spec names (flare takeover
 * pending, power menu open, touch calibration running) are combined
 * into one bool by the shell-level predicate `ff_shell_keep_awake`
 * (app/include/ff_shell.h) — this header takes the already-combined
 * bool so it stays free of any app/UI type. `ff_idle_short_press` below
 * takes the same already-combined bool for the identical reason. S26
 * slice (f) AC2 ("not entered while any keep-awake holds") needs no new
 * code: `keep_awake` unconditionally forces ACTIVE every tick
 * regardless of the CURRENT state, so SLEEP (like OFF before it) can
 * never be computed while it holds — this is the same mechanism slice
 * (c) already relied on for OFF, just inherited by the new state.
 *
 * ## OFF and SLEEP are sticky
 *
 * Once OFF (whether reached by elapsed idle time or by
 * `ff_idle_force_off`), `ff_idle_tick` will not walk it back down to
 * DIM/ACTIVE on its own — only `ff_idle_input` (a real wake: touch, PWR
 * SHORT_PRESS while OFF, BOOT, a GPIO/timer light-sleep wake carrying
 * real input) or a newly-true `keep_awake` does that. This is what
 * makes `ff_idle_force_off` (PWR SHORT_PRESS while ACTIVE -> "go OFF
 * immediately", docs/specs/S26-device-lifecycle.md slice c) stick even
 * though the elapsed idle time at the moment of the press may be far
 * short of `FF_IDLE_T_OFF_MS`. SLEEP is sticky the same way once
 * reached: a bare timer wake with no real input keeps `ff_idle_tick`
 * reporting SLEEP so the target's enact loop goes straight back to
 * sleep (S26 slice f: "if no input arrived, ff_idle stays SLEEP").
 *
 * ## Sleep inhibit (USB connected)
 *
 * `ff_idle_tick` takes a second, independent bool: `sleep_inhibit`.
 * Maintainer decision, dated 2026-09-02 (docs/specs/S26-device-lifecycle.md
 * slice (f) amendment): the ESP32-S3's native USB-Serial/JTAG powers down
 * during light sleep, so the puck vanishes from the host the moment the
 * screen sleeps — every dev/flash session over USB breaks. USB-powered
 * operation is also not battery-limited, so there is no cost to staying
 * awake. `sleep_inhibit` is NOT `keep_awake`: it blocks ONLY the OFF ->
 * SLEEP transition. DIM and OFF still happen exactly as today (the screen
 * still goes dark on schedule; only entering light sleep itself is
 * withheld) — a USB-tethered puck sitting idle on a desk still dims and
 * blanks its screen, it just never stops answering the host. Unlike
 * `keep_awake`, `sleep_inhibit` does NOT re-pin `ref_ms` — it is not a
 * wake or an activity signal, so DIM/OFF timings are completely unchanged
 * by it (a version that folded `sleep_inhibit` into `keep_awake` would
 * make USB itself a keep-awake source and the screen would never dim,
 * which is not what's asked). When `sleep_inhibit` releases, SLEEP is
 * computed the same way it always is: entered the moment
 * `FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS` has elapsed from the SAME
 * unmoved reference — immediately, on the very next tick, if that instant
 * is already in the past (the common case: USB was unplugged well after
 * the screen had gone OFF), or after the remaining time if not. No extra
 * state is needed for this — inhibit only ever gates the one comparison
 * that would set SLEEP, so the FSM just resumes evaluating it normally
 * once the gate lifts. The esp32s3 target is the only caller that ever
 * passes `true` here (`usb_serial_jtag_is_connected()`, sampled once per
 * tick — see app_main.c); the sim's ctl loop always passes `false` (no
 * light sleep on host, nothing to inhibit).
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

/** Additional idle time, on top of `FF_IDLE_T_OFF_MS` (i.e. measured from
 * the same reference instant as every other threshold here — see this
 * header's top comment), at which the device enters timer-based light
 * sleep (S26 slice f). Literal-pinned in a unit test
 * (`S26f_AC1_thresholds_pinned_to_spec_literals`, core/tests/test_idle.c)
 * per this repo's proxy-check failure mode (AGENTS.md item 6, bug
 * #135/#136): a test that only ever compares against this macro
 * symbolically would still pass if the macro's value silently changed. */
#define FF_IDLE_T_SLEEP_MS ((uint32_t)120000u)

typedef enum {
    FF_IDLE_STATE_ACTIVE = 0,
    FF_IDLE_STATE_DIM,
    FF_IDLE_STATE_OFF,
    /** S26 slice f: OFF for `FF_IDLE_T_SLEEP_MS` with no keep-awake
     * source holding — the esp32s3 target enacts this with
     * `esp_light_sleep_start()` (timer + GPIO wake); the sim has nothing
     * to enact (no sleep on host) and treats it like OFF for its
     * rebuild-skip gate (targets/sim/ctl_loop.c). */
    FF_IDLE_STATE_SLEEP,
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
 * DIM at `FF_IDLE_T_DIM_MS`, -> OFF at `FF_IDLE_T_OFF_MS`, and -> SLEEP
 * at `FF_IDLE_T_OFF_MS + FF_IDLE_T_SLEEP_MS` (S26 slice f) — UNLESS
 * `sleep_inhibit` is true, in which case that last transition (OFF ->
 * SLEEP only) is withheld; see this header's "Sleep inhibit" section
 * above for the full contract (it does not touch DIM/OFF and does not
 * re-pin the idle reference). OFF and SLEEP are both sticky (see top
 * comment) — natural ticking never reverses either back down; OFF can
 * still advance forward into SLEEP once `sleep_inhibit` is false and the
 * threshold has elapsed. Returns the resulting state (also readable via
 * `ff_idle_state`). Returns `FF_IDLE_STATE_ACTIVE` for a NULL `idle`
 * (safe default; no state is mutated).
 */
ff_idle_state_t ff_idle_tick(ff_idle_t *idle, uint32_t now_ms, bool keep_awake, bool sleep_inhibit);

/**
 * ff_idle_input — a real wake/reset event (touch, PWR SHORT_PRESS while
 * OFF, BOOT, a light-sleep GPIO wake carrying real input (S26 slice f),
 * and later a notification — docs/specs/S26-device-lifecycle.md slice d)
 * from ANY current state, including OFF and SLEEP. Sets state to ACTIVE
 * and pins the idle reference to `now_ms`, so the next `FF_IDLE_T_DIM_MS`
 * countdown starts fresh from this instant. NULL-safe (no-op).
 */
void ff_idle_input(ff_idle_t *idle, uint32_t now_ms);

/**
 * ff_idle_force_off — PWR SHORT_PRESS while ACTIVE: "go OFF immediately"
 * (docs/specs/S26-device-lifecycle.md slice c), independent of how much
 * idle time has actually elapsed. Sets state to OFF; the idle reference
 * (`ref_ms`) is left untouched, which is harmless — see this header's
 * "OFF and SLEEP are sticky" note: a subsequent `ff_idle_tick` with
 * `keep_awake` false will keep computing OFF (elapsed time only grows,
 * eventually advancing into SLEEP too), so the forced state never needs
 * its own separate "why am I off" bit. NULL-safe (no-op).
 */
void ff_idle_force_off(ff_idle_t *idle);

/**
 * ff_idle_short_press — the WHOLE PWR SHORT_PRESS decision
 * (docs/specs/S26-device-lifecycle.md slice c: "PWR SHORT_PRESS while
 * OFF = wake; while ACTIVE = go OFF immediately — this is where (b)'s
 * reserved SHORT_PRESS lands"), so the caller (app_main.c /
 * ff_power_fsm_event handling) never itself branches on `ff_idle_state`
 * — CLAUDE.md's "no `if` about domain behavior outside core" house
 * rule:
 *
 *  - `keep_awake` true (the same already-combined predicate `ff_idle_tick`
 *    takes): a no-op. A `keep_awake` source dominates ff_idle_tick's own
 *    next call regardless (it forces ACTIVE), so acting here would only
 *    produce a one-tick OFF-then-ACTIVE flicker with no spec guidance
 *    either way — an explicit no-op is simpler and avoids it.
 *  - ACTIVE: `ff_idle_force_off` — go OFF immediately, per the spec.
 *  - DIM, OFF, or SLEEP: `ff_idle_input` — a wake. The spec's AC only
 *    names OFF explicitly; DIM and SLEEP are extended to the same
 *    "wake" behaviour here (interpretation call, PR body) since "not
 *    fully off yet" / "off but sleeping" both read as a wake, not a
 *    no-op — this is also how a PWR press reaching app_main.c's loop on
 *    a light-sleep GPIO wake (S26 slice f) lands back in ACTIVE.
 *
 * NULL-safe (no-op, mirrors ff_idle_input/ff_idle_force_off).
 */
void ff_idle_short_press(ff_idle_t *idle, uint32_t now_ms, bool keep_awake);

/** ff_idle_state — the current state. Returns `FF_IDLE_STATE_ACTIVE` for
 * a NULL `idle` (same safe-default convention as `ff_idle_tick`). */
ff_idle_state_t ff_idle_state(ff_idle_t const *idle);

/**
 * ff_idle_brightness_pct — the WHOLE brightness-enact decision (S26
 * slice c, AC2: "wake restores exactly the pre-dim brightness_pct"), so
 * the caller (app_main.c) never itself branches on `ff_idle_state` to
 * pick a percent — the same house-rule reasoning as
 * `ff_idle_short_press` above. `stored_pct` is the shell's projected
 * `settings.brightness_pct` (the persisted setting — never touched by
 * dimming/off); the return value is what the target should actually
 * drive the backlight to:
 *
 *  - ACTIVE: `stored_pct`, UNCHANGED — so a wake restores the EXACT
 *    pre-dim value, never a hardcoded one.
 *  - DIM: `FF_BRIGHTNESS_MIN_PCT` (core/ff_settings.h — the same
 *    constant the stored setting itself is floored to, NOT a
 *    display-layer macro: this file has no display/ include).
 *  - OFF or SLEEP: `0` — a TRUE zero (S26 slice f: the backlight is
 *    already off from the OFF state that preceded SLEEP; light sleep
 *    does not change what gets driven to it). The caller is expected to
 *    route a `0` result to a true-off backlight call (e.g. esp32s3's
 *    `ff_display_backlight_off()`) and any nonzero result to a normal
 *    percent-set call — this function never needs to know which
 *    concrete HAL calls those are (no display/ include here either).
 */
uint8_t ff_idle_brightness_pct(ff_idle_state_t state, uint8_t stored_pct);

/**
 * ## Wake-only touch/button gate (amendment, docs/specs/S26-device-
 * lifecycle.md "(c) Inactivity -> dim -> screen off", dated 2026-09-02,
 * maintainer decision): "a touch or button press that begins while the
 * screen is not ACTIVE is a wake-only input and is never delivered to
 * the UI." On-glass bug this closes: a tap on a DIM/OFF/SLEEP screen
 * both woke the device AND landed on whatever was under the finger
 * (an unintended button press, launcher navigation, etc.) — the wake
 * should be the ENTIRE effect of that tap.
 *
 * `ff_idle_touch_gate_t` is a PER-INPUT-SOURCE latch: one instance per
 * physical input (the touch panel, BOOT, ...) so two input sources'
 * gestures are never confused with each other (e.g. a touch mid-gesture
 * and a BOOT press starting a moment later each get their own
 * press-begin/deliver decision) — pass a fresh instance (or a per-input
 * `static`) for each one, never share a single instance across inputs.
 * Zero-initialise or call `ff_idle_touch_gate_init()` before first use.
 */
typedef struct {
    bool was_pressed; /* raw level on the PREVIOUS call — detects the false->true press-begin edge */
    bool swallowing;  /* latched at press-begin; true = every sample of this gesture is withheld until release */
} ff_idle_touch_gate_t;

/** Zero the gate: not pressed, nothing swallowing. NULL-safe (no-op). */
void ff_idle_touch_gate_init(ff_idle_touch_gate_t *gate);

/**
 * ff_idle_touch_gate — decide whether THIS sample of a press/release
 * gesture may be delivered to the UI (see this header's "Wake-only
 * touch/button gate" section above for the amendment this implements).
 * Input-agnostic: the esp32s3 target calls this from BOTH the touch
 * indev read path (`ff_display.c`) and the BOOT-as-home debounce
 * (`app_main.c`) — pass the raw physical level (finger down / button
 * held), not a debounced click — with a SEPARATE `ff_idle_touch_gate_t`
 * instance per input source (see that type's doc comment).
 *
 * Semantics, sampled once per tick (same "call every tick, even when
 * nothing changed" contract as `ff_idle_tick`):
 *
 *  - `pressed` transitions false -> true (a press BEGINS) while
 *    `ff_idle_state(idle)` is NOT ACTIVE: this is a WAKE. Fires
 *    `ff_idle_input(idle, now_ms)` (the wake itself — the same call
 *    every other input source on this device makes), latches
 *    `gate->swallowing = true`, and returns false (not delivered) —
 *    for this sample and every subsequent sample of the SAME gesture,
 *    until release.
 *  - `pressed` transitions false -> true while ACTIVE: delivered
 *    normally. Returns true; `gate->swallowing` stays false.
 *  - `pressed` continues true (the gesture is still held): returns
 *    whatever was decided AT PRESS-BEGIN (`!gate->swallowing`), even if
 *    `idle`'s own state changes mid-gesture (e.g. ACTIVE -> DIM while a
 *    finger is still down, held there by this very gesture's own
 *    keep-awake feed racing a slow caller) — state matters only at
 *    press START, never for the rest of the gesture; that is what keeps
 *    a legitimate long-press/drag from being cut off mid-way.
 *  - `pressed` is false (released, or never pressed): resets the latch
 *    (`gate->was_pressed = gate->swallowing = false` — "release always
 *    resets the latch") and returns false (there is no press to
 *    deliver).
 *
 * NULL-safe: a NULL `idle` or `gate` fails OPEN — returns `pressed`
 * unchanged (deliver whatever the caller asked), the same "never
 * silently block input over a wiring bug" convention as the rest of
 * this header (see e.g. `ff_idle_tick`'s NULL default).
 */
bool ff_idle_touch_gate(ff_idle_t *idle, ff_idle_touch_gate_t *gate, uint32_t now_ms, bool pressed);

#ifdef __cplusplus
}
#endif

#endif /* FF_IDLE_H */
