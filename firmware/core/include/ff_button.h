/**
 * ff_button.h — a small, generic debounced push-button (S26 slice e:
 * docs/specs/S26-device-lifecycle.md "(e) Home button + launcher").
 *
 * The BOOT (GPIO0) home button needs none of `ff_power_fsm.h`'s
 * short/long-press distinction — the nav model fires on a single PRESS,
 * nothing else — so rather than bolt a second, unrelated channel onto
 * that FSM (which already carries the PWR short/long state machine AND
 * the independent reboot BOOT-release guard), this is a standalone
 * module reusing exactly its DEBOUNCE shape: a raw level change is only
 * believed once it has held steady for `FF_BUTTON_DEBOUNCE_MS`
 * (`ff_power_fsm.h`'s own `FF_POWER_FSM_DEBOUNCE_MS`, 30 ms — kept as a
 * separate constant here rather than shared so this module has zero
 * `ff_power_fsm.h` dependency and can debounce any button, not just
 * BOOT).
 *
 * Per this repo's house rule (CLAUDE.md: "all logic goes in
 * firmware/core/"), the debounce DECISION is domain logic and belongs
 * here; the esp32s3 target's job is only to sample the raw pin
 * (`ff_power_boot_pressed()`, already exists) and hand the level to
 * `ff_button_tick()` every tick — see targets/esp32s3/main/app_main.c.
 */
#ifndef FF_BUTTON_H
#define FF_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Debounce window: a raw level change must hold steady this long
 * before it is believed. Same value as `ff_power_fsm.h`'s
 * `FF_POWER_FSM_DEBOUNCE_MS` (a mechanical button's real bounce
 * settles in a similar window regardless of which pin it's on), kept
 * as an independent constant so this header has no dependency on that
 * one. */
#define FF_BUTTON_DEBOUNCE_MS ((uint32_t)30u)

/**
 * The whole debouncer state. Fully-defined (not opaque), same
 * convention as `ff_power_fsm_t`: safe on the stack or in a static;
 * zero-initialise or call `ff_button_init()` before first use.
 */
typedef struct {
    bool     raw_level;     /* last raw level observed (pre-debounce) */
    bool     raw_pending;   /* true while raw_level differs from debounced_pressed and the debounce window is still running */
    uint32_t raw_change_ms; /* when raw_level last changed (debounce window start) */
    bool     debounced_pressed;
} ff_button_t;

/** Zero the debouncer: not pressed, nothing debouncing. NULL-safe
 * (no-op). */
void ff_button_init(ff_button_t *b);

/**
 * ff_button_tick — feed one raw pin sample. Call every tick regardless
 * of whether the level changed (same "always tick" contract as
 * `ff_power_fsm_tick`/`ff_flare_tick`) — the debounce timing is
 * measured against `now_ms`, not against how often this is called.
 *
 * Returns true EXACTLY ONCE per physical press: on the tick where a
 * debounced-held press first registers (a raw level change that holds
 * steady for `FF_BUTTON_DEBOUNCE_MS`). A held press, and every tick
 * before the debounce window elapses, returns false; the button must
 * debounce-release (hold `level == false` for the same window) before
 * another press can fire again — so a single physical press yields
 * exactly one `true`, never a stream of them while held, and a bounce
 * shorter than the debounce window (e.g. a 20 ms blip) never fires at
 * all. Wraparound-safe against `now_ms` rollover (the same
 * `(int32_t)(now-deadline) >= 0` convention `ff_power_fsm.c`
 * documents). Returns false for a NULL `b`.
 */
bool ff_button_tick(ff_button_t *b, uint32_t now_ms, bool level);

#ifdef __cplusplus
}
#endif

#endif /* FF_BUTTON_H */
