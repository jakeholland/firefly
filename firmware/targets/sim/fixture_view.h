/**
 * fixture_view.h — S13 slice b's placeholder "fixture debug view".
 *
 * Real screens (radar/now/signals/settings) arrive with S06+ (Wave 3).
 * Until then, this is what a headless fixture render produces: enough of
 * a deterministic, legible face to prove the fixture-load -> render -> PNG
 * pipeline end to end and give golden-screenshot tooling (S14 slice b)
 * something stable to diff against. No domain logic — pure rendering of
 * whatever ff_app_state_t it's given.
 */
#ifndef FF_FIXTURE_VIEW_H
#define FF_FIXTURE_VIEW_H

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_fixture_view_build — builds the debug face on the current default
 * display's active screen (same calling convention as main.c's
 * ff_build_boot_screen(): a display must already be the LVGL default).
 *
 * Renders: a dark puck circle (matches the boot placeholder's palette),
 * the fixture name, and a text dump of the active face's key fields —
 * whichever section of `state` matches `state->active_face`. Purely
 * text/shape rendering: no animation, no font other than LVGL's built-in
 * default, no randomness — required for S13 AC2 (identical PNG across
 * two runs of the same fixture).
 */
void ff_fixture_view_build(ff_app_state_t const *state);

#ifdef __cplusplus
}
#endif

#endif /* FF_FIXTURE_VIEW_H */
