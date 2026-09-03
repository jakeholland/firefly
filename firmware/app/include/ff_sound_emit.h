/**
 * ff_sound_emit.h — the screens-level TAP sound seam (S27 sounds).
 *
 * Spec: docs/specs/S27-sounds.md, "Shell seam". Mirrors `ff_intent.h`'s
 * process-global emit/bind seam ON PURPOSE — same problem shape, same
 * answer: the shell does not see raw button presses (only screens'
 * `ff_scr_button_create` base does, via LVGL's `LV_EVENT_PRESSED`), so a
 * screen-layer press has nowhere to report "a tap just happened" except a
 * process-global sink, exactly like a screen-layer tap has nowhere to
 * report "the user asked to go back" except `ff_intent_emit`.
 *
 * ## Why a SECOND seam instead of folding TAP into `ff_intent_t`
 * `ff_intent_t` is a SEMANTIC action a screen chooses to perform ("open
 * the composer", "send this text") — the shell decides what happens.
 * FF_SOUND_TAP is not a decision at all: `ff_scr_button_create`
 * (scr_nav.h/.c) is the ONE choke point every button in the app already
 * funnels through, so wiring the tick there fires it for literally every
 * button, on every screen, with no per-screen code to write or forget —
 * exactly the coverage a UI tick needs (spec: "UI tick on button press").
 * Routing that through `ff_intent_t` would mean inventing a payload-less
 * `FF_INTENT_TAP` intent whose only purpose is to be immediately
 * re-translated into a sound event on the other side — an extra hop for
 * no gain, and a `ff_intent_kind_t` member that means something
 * different in kind from every semantic action around it. A second,
 * narrowly-typed seam keeps that translation out of the intent vocabulary
 * altogether.
 *
 * ## Where the policy decision happens (say what you chose)
 * `ff_sound_emit(ev)` is called UNCONDITIONALLY from the button base —
 * same "the screen doesn't decide, it just reports" discipline
 * `ff_intent_emit` already has. The GATING (sounds_on && ui_ticks &&
 * !quiet) happens on the RECEIVING end: `ff_shell_sound_sink`
 * (ff_shell.h/.c) is the function the target binds via
 * `ff_sound_emit_bind(ff_shell_sound_sink, sh)`, mirroring
 * `ff_intent_emit_bind(ff_shell_intent_sink, sh)` exactly — it asks
 * `ff_shell_should_tap_sound(sh)` (the shell owns settings + the wall
 * clock, so it is the one place that CAN answer that) and only then
 * calls the shell's injected `play_sound` hook. This reuses the SAME
 * per-shell `play_sound` hook every other sound event calls, so the
 * device HAL sees one uniform call site regardless of which of the six
 * events fired it.
 *
 * Deliberately NOT `ff_shell_should_tap_sound(sh)` called directly from
 * the button base (the other option S27's brief offered): screens never
 * include `ff_shell.h` (docs/ARCHITECTURE.md's one-way layering —
 * `ff_wiring.c` and `ff_shell.c` are the only two files allowed to see
 * core+meshclient+app together, and `ff_shell.h` itself documents that
 * "screen files include [ff_intent.h] to build intents" is the
 * layering-safe shape) — a screen file cannot hold a `ff_shell_t*` at
 * all without breaking that invariant.
 *
 * Same triviality/lifetime/threading contract as ff_intent.h: unbound is
 * a safe no-op (headless/golden rendering never binds anything), single
 * process-global sink (one display, one shell, one seam), not
 * thread-safe by design (LVGL's own one-UI-thread assumption), and the
 * caller must unbind before the bound object goes away.
 */
#ifndef FF_SOUND_EMIT_H
#define FF_SOUND_EMIT_H

#include "ff_sound.h" /* ff_sound_event_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The sink shape `ff_sound_emit_bind` accepts. `user` is the bound
 * context, passed back untouched; `ev` is always `FF_SOUND_TAP` today
 * (the only event this seam carries — see this header's top comment for
 * why the other five events go straight through `ff_shell.c`'s own
 * `shell_sound` helper instead of this seam), but the callback takes the
 * full `ff_sound_event_t` rather than a bare void so the seam is not
 * artificially narrowed if a future screens-only sound joins it.
 */
typedef void (*ff_sound_emit_fn)(void *user, ff_sound_event_t ev);

/**
 * ff_sound_emit_bind — bind (or, with fn == NULL, unbind) the process
 * sink screens emit into. Called once by the target at startup, after
 * its shell exists (same call site as `ff_intent_emit_bind`); tests bind
 * a spy. Rebinding replaces the previous sink.
 *
 * LIFETIME: same warning as `ff_intent_emit_bind` — the seam holds
 * `user` as a raw pointer. Unbind (or rebind) BEFORE the bound object
 * goes away.
 */
void ff_sound_emit_bind(ff_sound_emit_fn fn, void *user);

/**
 * ff_sound_emit — hand a sound event up from a screen (today, always
 * FF_SOUND_TAP, from `ff_scr_button_create`'s press handler). Forwards
 * `ev` to the bound sink; a safe no-op when nothing is bound (headless/
 * golden rendering) — no policy is applied here, that is entirely the
 * bound sink's job (see this header's top comment).
 */
void ff_sound_emit(ff_sound_event_t ev);

#ifdef __cplusplus
}
#endif

#endif /* FF_SOUND_EMIT_H */
