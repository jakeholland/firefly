/**
 * ff_intent.h — the intent seam: how screens talk to the shell (S16
 * slice c1).
 *
 * Spec: docs/specs/S16-app-shell.md, "Intents". This header is the seam
 * that replaces every stub callback in issue #23: screens stay pure
 * renderers — they emit a *semantic* intent ("open the composer", "go
 * back") and the shell decides what, if anything, happens. No screen
 * ever includes ff_shell.h, touches an `ff_route_t`, or mutates core
 * state directly.
 *
 * ## Layering — why this header depends on nothing
 * This file includes only <stdint.h>. That is load-bearing, not tidiness:
 * screen files (app/screens/) include it to *build* intents, and pulling
 * ff_wiring.h in here would transitively hand every screen mc_client.h —
 * exactly the core+meshclient+app inclusion that only `ff_wiring.c` and
 * `ff_shell.c` are allowed (docs/ARCHITECTURE.md; ff_wiring.h's header
 * comment). Which is also why:
 *
 * ## `ff_wiring_canned_reply_t` is DEFINED here, not in ff_wiring.h
 * The spec's intent union carries `ff_wiring_canned_reply_t reply` by
 * that exact name. Defining the intent struct in terms of ff_wiring.h
 * breaks the layering above; mirroring the enum under a second name
 * re-opens the DRIFT GUARD problem ff_app_state.h:23-37 records paying
 * for once already. So the *definition* moved to this dependency-free
 * header and ff_wiring.h includes it — same members, same order, same
 * name, zero drift, and every existing `ff_wiring.h` consumer compiles
 * unchanged. The `ff_wiring_` prefix is kept deliberately: the type is
 * still ff_wiring's vocabulary (`ff_wiring_send_canned_reply` consumes
 * it); only its textual home moved. `[api]` — flagged in the PR title.
 *
 * ## Payload ownership — NOT OWNED; COPIED
 * The two pointer payloads (`u.text`, `u.setting.v.s`) are BORROWED for
 * the duration of the `ff_shell_intent()` / sink call only. The shell
 * copies whatever it keeps before returning; the caller may (and screens
 * do — every emit site builds its `ff_intent_t` on the stack) invalidate
 * the pointer and the struct the instant the call returns. A shell-side
 * handler that stores one of these pointers is a bug against this
 * contract, not an implementation choice. (S16, "Intents": "not owned;
 * copied".)
 *
 * ## Union validity — per kind, ff_flare_result_t's convention
 * Exactly the member the `kind` names is meaningful; everything else in
 * `u` is garbage and must not be read. Emit sites zero-initialize the
 * struct anyway (`ff_intent_t in = {...}`) so an accidental read is
 * deterministic garbage, but the contract is "per kind", not "zeroed".
 *
 * ## The emit seam (`ff_intent_emit` / `ff_intent_emit_bind`)
 * One process-global sink, bound once by the target at startup
 * (`ff_intent_emit_bind(ff_shell_intent_sink, &shell)` — see
 * ff_shell.h), emitted into by screens (`ff_intent_emit(&in)`). Unbound,
 * every emit is a safe no-op — which is exactly what golden/headless
 * rendering wants: `--headless --screenshot` never binds a shell, never
 * fires a click, and keeps producing byte-identical frames.
 *
 * A global, rather than an emit context threaded through every screen
 * builder's signature, is **the design, not a stopgap** (PR #54 review
 * asked which — this is the answer, recorded where it binds: the
 * global does NOT get replaced when slice c2 rewrites the builder
 * signatures for its own reasons). Why it is the right permanent shape:
 *  - the screens are process-singletons already, by construction — they
 *    hold file-static LVGL state (`scr_compose.c`'s `static ff_t9_t
 *    s_t9`, its label pointers) and render to the one default display,
 *    so "which instance's sink?" has exactly one answer: one display,
 *    one shell, one seam. A second simultaneous sink would mean a
 *    second display and a second shell, which is a different device;
 *  - LVGL event `user_data` slots are already carrying per-key payloads
 *    (scr_compose.c's key indices), so per-callback context would be a
 *    second mechanism alongside this one anyway.
 * (That c1 threading a context would also have collided with c2's
 * five-signature `[api]` change was sequencing luck, not the rationale
 * — the rationale is the two bullets above, and it outlives c2.)
 * Single-threaded by the same assumption LVGL itself imposes on every
 * caller in this repo (one UI thread); no locking.
 */
#ifndef FF_INTENT_H
#define FF_INTENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Canned replies (S08: OMW / 5 MIN / PULSE chips). Defined here — see
 * this header's top comment — consumed by `ff_wiring_send_canned_reply`
 * (app/ff_wiring.h), which is why it keeps the ff_wiring_ prefix.
 */
typedef enum {
    FF_WIRING_REPLY_OMW,
    FF_WIRING_REPLY_5MIN,
    FF_WIRING_REPLY_PULSE,
} ff_wiring_canned_reply_t;

/**
 * Every semantic intent a screen can emit. The full S16 contract is
 * defined in this slice (c1) so later slices extend *handling*, never
 * this enum's shape; per-kind handling lands with its owning slice
 * (navigation: c1 · core-mutating: c2 · T9/compose: c3 · settings
 * write-through: e).
 */
typedef enum {
    FF_INTENT_SWIPE, FF_INTENT_BACK, FF_INTENT_OPEN_COMPOSE, FF_INTENT_OPEN_SETTINGS,
    /* S09 [api] — opens the Map face as a modal over the current base
     * (ff_app_state.h's FF_APP_FACE_MAP comment has the routing
     * rationale). No payload: unlike OPEN_COMPOSE there is no
     * destination to resolve. */
    FF_INTENT_OPEN_MAP,
    FF_INTENT_CANNED_REPLY, FF_INTENT_SEND_TEXT, FF_INTENT_MARK_FEED_READ,
    FF_INTENT_SELECT_CREW, FF_INTENT_SELECT_RALLY,
    FF_INTENT_T9_KEY, FF_INTENT_T9_SPACE, FF_INTENT_T9_BACKSPACE,
    FF_INTENT_T9_MODE, FF_INTENT_T9_INSERT,     /* SYM shortcuts, ff_t9_insert_text */
    FF_INTENT_FLARE_START, FF_INTENT_FLARE_END,
    FF_INTENT_TAKEOVER_GO, FF_INTENT_TAKEOVER_DISMISS, FF_INTENT_RELEASE_LOCK,
    FF_INTENT_SETTING_SET,
} ff_intent_kind_t;

/**
 * One member per mutable ff_settings_t field. Split int/string because
 * `my_name` is char[16] and compass_cal is a struct — an int32_t-only
 * payload could not carry the one setting users actually type.
 *
 * `compass_cal`/`cal_valid` are deliberately absent: calibration is
 * written by S12's calibration ritual, not by a settings toggle, and a
 * generic setter would let any caller assert a calibration it never
 * performed. (S16, "Intents".)
 */
typedef enum {
    FF_SETTING_IMPERIAL, FF_SETTING_SHARE_MODE, FF_SETTING_HAPTICS,
    FF_SETTING_NIGHT_GLOW, FF_SETTING_WATER_MIN,
    FF_SETTING_QUIET_FROM_MIN, FF_SETTING_QUIET_TO_MIN,
    FF_SETTING_UTC_OFFSET_MIN,   /* the field S16's wall-clock section added to S11 */
    FF_SETTING_MY_NAME,          /* string payload */
} ff_setting_id_t;

typedef struct {
    ff_intent_kind_t kind;
    union {
        /** SWIPE. A ROUTE direction, not a finger direction: -1 toward
         *  RADAR, +1 toward SIGNALS — a rightward finger drag maps to -1
         *  (ff_route.h's `ff_route_swipe` doc has the full warning). */
        int8_t swipe_dir;
        ff_wiring_canned_reply_t reply;         /* CANNED_REPLY */
        /** SELECT_CREW, OPEN_COMPOSE. For OPEN_COMPOSE: an explicit
         *  destination, or 0 = none given, which the shell resolves per
         *  S08's Behavior ("TO = selected crew member") — the currently
         *  selected paired member if there is one, else broadcast. See
         *  ff_shell_intent's doc in ff_shell.h for the exact rule. */
        uint32_t node_id;
        uint8_t rally_idx;                      /* SELECT_RALLY */
        uint8_t t9_key;                         /* T9_KEY: 0-9 */
        char const *text;                       /* T9_INSERT (NOT owned; copied — see top comment) */
        struct { ff_setting_id_t id;            /* SETTING_SET */
                 union { int32_t i; char const *s; } v; } setting;
    } u; /* validity per kind, ff_flare_result_t convention */
} ff_intent_t;

/**
 * The sink shape `ff_intent_emit_bind` accepts. `user` is the bound
 * context, passed back untouched; `in` is borrowed for the call only
 * (see "Payload ownership" above) and is never NULL when called through
 * `ff_intent_emit`.
 */
typedef void (*ff_intent_emit_fn)(void *user, ff_intent_t const *in);

/**
 * ff_intent_emit_bind — bind (or, with fn == NULL, unbind) the process
 * sink screens emit into. Called once by the target at startup, after
 * its shell exists; tests bind a spy. Rebinding replaces the previous
 * sink. Not thread-safe by design — one UI thread, LVGL's own standing
 * assumption.
 *
 * LIFETIME: the seam holds `user` as a raw pointer and cannot know when
 * it dies. Unbind (or rebind) BEFORE the bound object goes away — in
 * particular before `ff_shell_close()` on a bound shell, and before a
 * test's stack-allocated shell or spy leaves scope. An emit through a
 * stale binding is a use-after-free, not a no-op. (`ff_shell_close`
 * cannot do this for you: the shell does not know it is the sink.)
 */
void ff_intent_emit_bind(ff_intent_emit_fn fn, void *user);

/**
 * ff_intent_emit — hand an intent up from a screen. Forwards `in` to the
 * bound sink; a safe no-op when nothing is bound (headless/golden
 * rendering) or `in` is NULL. The struct and any pointer payload are
 * borrowed for the duration of the call only.
 */
void ff_intent_emit(ff_intent_t const *in);

#ifdef __cplusplus
}
#endif

#endif /* FF_INTENT_H */
