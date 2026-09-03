/**
 * ff_face_dispatch.h — app/: the ONE face-dispatch mapping shared by
 * targets/sim/face_dispatch.c and targets/esp32s3/main/ff_face.c.
 *
 * debt/shared-face-dispatch: targets/sim/face_dispatch.c and
 * targets/esp32s3/main/ff_face.c independently implemented the identical
 * app-layer logic — the "takeover overrides active_face" effective-face
 * computation, the #bug4 Settings-scroll-reset-on-fresh-entry rule (each
 * kept its own file-`static s_prev_face`), the takeover-first early
 * return, and the same 5-way `active_face` -> screen-builder if/else-if
 * chain in the same order — with only ff_face.c's own header comment
 * ("Mirrors targets/sim/face_dispatch.c's mapping ... the two must
 * agree") standing in for a compiler-enforced guarantee. This file IS
 * that guarantee now: one function, one chain, both targets call it.
 *
 * Per-target divergence (small, and it stays real per-target code — NOT
 * folded into this shared function):
 *  - the sim applies a fixture-only Settings scroll hint after the build
 *    (so a golden can capture a scrolled Settings state), and falls back
 *    to fixture_view.h's S13 placeholder for any face with no real
 *    screen;
 *  - the device has no fixture concept (no scroll hint) and no
 *    placeholder — the shell only ever projects a face with a real
 *    screen, so an unknown face there is a bug, logged rather than
 *    papered over.
 * Both deltas are optional hooks (`ff_face_dispatch_hooks_t`, NULL-safe
 * throughout: a NULL `hooks` pointer, or any NULL member of one that IS
 * supplied, is a plain no-op) so neither target has to reimplement the
 * chain around them.
 *
 * The #bug4 prev-face memory used to be a file `static` in each copy —
 * moved here into an explicit `ff_face_dispatch_ctx_t` the CALLER owns
 * (each target keeps exactly one static instance, the same lifetime the
 * old file static had — see targets/sim/face_dispatch.c / targets/
 * esp32s3/main/ff_face.c) instead of a static inside this shared
 * function, so a unit test can construct its own ctx and reset it
 * between cases rather than fighting a process-wide static (app/tests/
 * test_face_dispatch.c does exactly that).
 */
#ifndef FF_APP_FACE_DISPATCH_H
#define FF_APP_FACE_DISPATCH_H

#include <stdint.h>

#include "ff_app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_face_dispatch_ctx_t — owns the #bug4 fresh-entry memory across
 * calls. A zeroed instance (`= {0}`, or FF_FACE_DISPATCH_CTX_INIT) means
 * "no prior face" — `FF_APP_FACE_NONE` is 0, so the first Settings entry
 * is always treated as fresh, matching the old static's start value.
 * One ctx per "logical display" (one real caller owns one ctx for its
 * lifetime) — pass the SAME ctx on every call that is meant to observe
 * face-to-face transitions.
 */
typedef struct ff_face_dispatch_ctx {
    ff_app_face_t prev_face;
} ff_face_dispatch_ctx_t;

#define FF_FACE_DISPATCH_CTX_INIT \
    {                             \
        .prev_face = FF_APP_FACE_NONE, \
    }

/**
 * ff_face_dispatch_ctx_reset — resets `*ctx` to the "no prior face"
 * state, so the next Settings entry through it is always treated as
 * fresh. Same effect as FF_FACE_DISPATCH_CTX_INIT, offered as a
 * callable so tests (and a target reinitializing its display) don't
 * need to reach into the struct directly. `ctx == NULL` is a no-op.
 */
void ff_face_dispatch_ctx_reset(ff_face_dispatch_ctx_t *ctx);

/**
 * ff_face_dispatch_hooks_t — the two per-target deltas described above.
 * Every member is individually optional (NULL = no-op); `hooks` itself
 * may also be NULL. `user_data` is passed through unchanged — neither
 * hook receives any other context.
 */
typedef struct ff_face_dispatch_hooks {
    /**
     * settings_scroll_hint — called once, AFTER a SETTINGS build (and
     * after any fresh-entry scroll reset), with `state->
     * ui_settings_scroll_y`. Sim-only: forwards to
     * `ff_scr_settings_apply_scroll_hint` so a golden can capture a
     * scrolled Settings state. Never called for any other face.
     */
    void (*settings_scroll_hint)(void *user_data, int32_t y);

    /**
     * unknown_face — called INSTEAD of a build when `state->active_face`
     * matches none of the faces this dispatcher knows how to build. The
     * sim falls back to the S13 fixture placeholder
     * (`ff_fixture_view_build`); the device logs a warning and builds
     * nothing (an unknown face on a real shell projection is a bug, not
     * a state to paper over).
     */
    void (*unknown_face)(void *user_data, ff_app_state_t const *state);

    void *user_data;
} ff_face_dispatch_hooks_t;

/**
 * ff_face_dispatch_build — builds whichever screen matches `*state` on
 * the current default display's active screen (same calling convention
 * every screen builder in this codebase already uses: a display must
 * already be the LVGL default). `state == NULL` is a no-op.
 *
 * `ctx` carries the #bug4 fresh-entry memory across calls — see
 * `ff_face_dispatch_ctx_t`'s doc comment. `ctx == NULL` is accepted (a
 * local, always-"no prior face" context is used for that one call, so
 * behavior degrades to "every Settings entry is fresh" rather than
 * crashing) but every real caller should own a persistent ctx instead.
 *
 * `hooks` may be NULL (both per-target deltas become no-ops).
 *
 * S10 slice b: a pending receive takeover
 * (`state->flare.takeover_active`) is checked FIRST and, if true, is the
 * ONLY thing built — per spec ("full-screen takeover regardless of
 * current face"), it replaces whatever face would otherwise show;
 * `active_face` is not consulted at all on that path.
 *
 * Otherwise: the five base faces — RADAR, LINEUP (Now), INBOX (Signals),
 * MAP, and SETTINGS — go through the shared nav shell (`ff_scr_nav_build`,
 * which builds the active face's content). SETTINGS additionally gets
 * the fresh-entry scroll reset (before the build) and the
 * `settings_scroll_hint` hook (after it). COMPOSE, POWER_MENU, and
 * LAUNCHER are the three full-screen modals (`ff_scr_compose_build`/
 * `ff_scr_power_menu_build`/`ff_scr_launcher_build`) — none is a base
 * face reached through nav. Anything else invokes `hooks->unknown_face`
 * (a no-op if `hooks` is NULL or that member is NULL).
 */
void ff_face_dispatch_build(ff_app_state_t const *state, ff_face_dispatch_ctx_t *ctx,
                             ff_face_dispatch_hooks_t const *hooks);

#ifdef __cplusplus
}
#endif

#endif /* FF_APP_FACE_DISPATCH_H */
