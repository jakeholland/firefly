/**
 * scr_compose.h — app/screens: the Compose face (S08 slice d).
 *
 * The T9 keypad screen: a message bubble (recipient + live text, with the
 * pending multi-tap character visually distinguished) driving the merged
 * `core/include/ff_t9.h` engine, plus a mode-paged keypad — ABC (letters,
 * multi-tap) / 123 (literal digits) / SYM (curated ASCII shortcuts) — per
 * docs/specs/S08-signals-t9.md's Amendments (2026-08-23 owner ruling).
 * See scr_compose.c's header comment for the full render-vs-live-input
 * split and which emoji tier this PR ships.
 */
#ifndef FF_SCR_COMPOSE_H
#define FF_SCR_COMPOSE_H

#include "ff_app_state.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_scr_compose_build — builds the Compose screen on the current default
 * display's active screen (own puck/top-level screen, same calling
 * convention as `ff_scr_nav_build` — Compose is reached from Signals'
 * "+", not a tileview swipe tile, so it isn't nested inside scr_nav.c's
 * shell).
 *
 * The initial paint is a pure function of `*compose` (matching every
 * other screen in this codebase — same static-golden-render guarantee as
 * `ff_scr_radar_build`/`ff_scr_signals_build`). Real keypad input then
 * drives a live, internal `ff_t9_t` — see scr_compose.c's header comment
 * for exactly when rendering switches from "the fixture snapshot" to
 * "the live engine's own text".
 */
void ff_scr_compose_build(ff_app_compose_t const *compose);

#ifdef __cplusplus
}
#endif

#endif /* FF_SCR_COMPOSE_H */
