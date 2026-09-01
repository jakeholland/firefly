/**
 * ff_power_fsm.h — S26 slice (b) / S25 slice (b): the PWR-button press
 * FSM and the reboot BOOT-release guard.
 *
 * Spec: docs/specs/S26-device-lifecycle.md, "(b) Power button -> power
 * menu -> soft power-off"; hardware contract in
 * docs/specs/S25-power-latch.md (GPIO7 = SYS_EN hold, GPIO6 = PWR key).
 *
 * Per this repo's house rule (CLAUDE.md: "all logic goes in
 * firmware/core/"), the press-timing DECISION — is this a short tap, a
 * long hold, or a released long hold — is domain logic, not I/O: it is
 * pure function of (now_ms, a debounced boolean level) and belongs here,
 * fed by `ff_power_fsm_tick()`. The esp32s3 target's job is only to
 * sample GPIO6 (`ff_power_pwr_pressed()`) and hand the raw level to this
 * FSM every tick — see targets/esp32s3/main/app_main.c.
 *
 * ## Debounce
 * A raw level change is only believed once it has held steady for
 * `FF_POWER_FSM_DEBOUNCE_MS` (30 ms). This is symmetric — both the
 * press edge and the release edge debounce the same way — matching a
 * mechanical button's real bounce behaviour on either transition, not
 * just the press.
 *
 * ## Events
 * `ff_power_fsm_tick()` returns AT MOST one event per call (the same
 * "one result struct per call" shape `ff_flare_tick()` uses):
 *
 *  - `FF_POWER_FSM_EVENT_LONG_PRESS` — fires EXACTLY ONCE per press
 *    cycle, on the tick where a debounced-held press first reaches
 *    `FF_POWER_FSM_LONG_MS` (1500 ms) — well under the panel's ~6 s
 *    hardware force-off (docs/specs/S25-power-latch.md). The button is
 *    still down when this fires; the caller (the shell, via
 *    FF_INTENT_POWER_MENU_OPEN) reacts immediately rather than waiting
 *    for release, so the power menu opens the instant the hold is long
 *    enough, not after the finger lifts.
 *  - `FF_POWER_FSM_EVENT_SHORT_PRESS` — fires on a debounced RELEASE
 *    that never reached the long threshold during this press cycle.
 *  - `FF_POWER_FSM_EVENT_RELEASE` — fires on a debounced RELEASE that
 *    DID already emit LONG_PRESS earlier in the same press cycle. This
 *    is the rule under test as "a held press emits LONG exactly once;
 *    release after LONG does NOT also emit SHORT" — SHORT_PRESS and
 *    RELEASE are mutually exclusive outcomes of the same release edge,
 *    distinguished by whether LONG_PRESS already fired for this press.
 *
 * A press that is still debouncing, or a steady level with no edge and
 * no threshold newly crossed, returns `FF_POWER_FSM_EVENT_NONE`.
 *
 * ## Reboot BOOT-release guard
 * GPIO0 (BOOT) held LOW across a reset enters the ROM bootloader
 * (download mode) rather than running the app — a real hazard if
 * `esp_restart()` is called while the user's OTHER thumb happens to be
 * resting on BOOT (S26's nav model makes BOOT the home button, so this
 * is not a hypothetical). `ff_power_fsm_request_reboot()` arms a pending
 * reboot; `ff_power_fsm_reboot_ready()`, called every tick with the live
 * BOOT level, reports ready (true) exactly once, on the first tick BOOT
 * reads released, and clears the pending flag (one-shot) so the caller's
 * `esp_restart()` fires exactly once. Independent of the press FSM above
 * — the two share no state — because a reboot can be requested from the
 * power menu regardless of what PWR is doing at that instant.
 */
#ifndef FF_POWER_FSM_H
#define FF_POWER_FSM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Debounce window: a raw GPIO6 level change must hold steady this long
 * before it is believed (both edges). */
#define FF_POWER_FSM_DEBOUNCE_MS ((uint32_t)30u)

/** Long-press threshold: a debounced-held press reaching this duration
 * fires LONG_PRESS. Well under the board's ~6 s hardware force-off
 * (docs/specs/S25-power-latch.md). */
#define FF_POWER_FSM_LONG_MS ((uint32_t)1500u)

typedef enum {
    FF_POWER_FSM_EVENT_NONE = 0,
    FF_POWER_FSM_EVENT_SHORT_PRESS,
    FF_POWER_FSM_EVENT_LONG_PRESS,
    FF_POWER_FSM_EVENT_RELEASE,
} ff_power_fsm_event_t;

/**
 * The whole FSM state. Fully-defined (not opaque), same convention as
 * `ff_flare_t` — safe on the stack or in a static, zero-initialise or
 * call `ff_power_fsm_init()` before first use.
 */
typedef struct {
    /* Debounce bookkeeping for the PWR level. */
    bool     raw_level;       /* last raw level observed (pre-debounce) */
    bool     raw_pending;     /* true while raw_level differs from debounced_pressed and the debounce window is still running */
    uint32_t raw_change_ms;   /* when raw_level last changed (debounce window start) */

    /* The debounced press state. */
    bool     debounced_pressed;
    uint32_t press_start_ms;  /* valid iff debounced_pressed */
    bool     long_fired;      /* LONG_PRESS already emitted this press cycle */

    /* Reboot BOOT-release guard — independent of the press state above. */
    bool     reboot_pending;
} ff_power_fsm_t;

/** Zero the FSM: no press in progress, nothing debouncing, no reboot
 * pending. NULL-safe (no-op). */
void ff_power_fsm_init(ff_power_fsm_t *fsm);

/**
 * ff_power_fsm_tick — feed one raw GPIO6 sample. Call every tick
 * regardless of whether the level changed (same "always tick" contract
 * as `ff_flare_tick`) — the debounce timing and the long-press threshold
 * are both measured against `now_ms`, not against how often this is
 * called. Returns at most one event; see this header's top comment for
 * the full rule set. Wraparound-safe against `now_ms` rollover (the
 * same `(int32_t)(now-deadline) >= 0` convention `ff_flare.c` documents).
 * Returns FF_POWER_FSM_EVENT_NONE for a NULL `fsm`.
 */
ff_power_fsm_event_t ff_power_fsm_tick(ff_power_fsm_t *fsm, uint32_t now_ms, bool pwr_pressed);

/**
 * ff_power_fsm_request_reboot — arm a pending reboot (the power menu's
 * Reboot action). Idempotent: calling it again while already pending
 * changes nothing. NULL-safe (no-op).
 */
void ff_power_fsm_request_reboot(ff_power_fsm_t *fsm);

/**
 * ff_power_fsm_reboot_ready — call every tick (independent of, and at
 * whatever cadence relative to, `ff_power_fsm_tick`) with the live BOOT
 * (GPIO0) level once a reboot has been requested. Returns true EXACTLY
 * ONCE — on the tick `boot_pressed` first reads false (released) after
 * `ff_power_fsm_request_reboot()` — and clears the pending flag in the
 * same call, so a caller that reacts to `true` by calling
 * `esp_restart()` cannot double-fire. Returns false when no reboot is
 * pending, when BOOT is still held (would enter the ROM bootloader on
 * reset), or `fsm` is NULL.
 */
bool ff_power_fsm_reboot_ready(ff_power_fsm_t *fsm, bool boot_pressed);

#ifdef __cplusplus
}
#endif

#endif /* FF_POWER_FSM_H */
