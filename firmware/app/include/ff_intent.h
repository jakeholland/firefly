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
 * This file includes only the freestanding C standard headers <stdint.h>
 * and <stdbool.h> (fixed-width ints and `bool`). That is load-bearing, not
 * tidiness:
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

#include <stdbool.h> /* #bug1 — the setting payload's `transient` flag */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Canned replies (S08: OMW / 5 MIN chips). Defined here — see this
 * header's top comment — consumed by `ff_wiring_send_canned_reply`
 * (app/ff_wiring.h), which is why it keeps the ff_wiring_ prefix.
 * (2026-09-02: a third member, FF_WIRING_REPLY_PULSE, is retired along
 * with the rest of the PULSE wire type — see ff_proto.h's RESERVED_01
 * section and ff_wiring.h's header note.)
 */
typedef enum {
    FF_WIRING_REPLY_OMW,
    FF_WIRING_REPLY_5MIN,
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
    /* NOTE: there is deliberately no FF_INTENT_OPEN_MAP. Map used to be a
     * modal opened by a vertical top-swipe (S09 [api]); the
     * horizontal-carousel rework made it an ordinary swipe face between
     * Signals and Settings, reached by FF_INTENT_SWIPE like any other
     * neighbour, so a Map-specific open intent no longer exists. */
    FF_INTENT_CANNED_REPLY, FF_INTENT_SEND_TEXT, FF_INTENT_MARK_FEED_READ,
    FF_INTENT_SELECT_CREW, FF_INTENT_SELECT_RALLY,
    FF_INTENT_T9_KEY, FF_INTENT_T9_SPACE, FF_INTENT_T9_BACKSPACE,
    FF_INTENT_T9_MODE, FF_INTENT_T9_INSERT,     /* SYM shortcuts, ff_t9_insert_text */
    /* [api] S08 predictive addendum — the two predictive-T9 controls the
     * maintainer chose (cycle + tap-to-select). Both are handled only in
     * FF_APP_COMPOSE_PRED mode; a no-op otherwise. Appended, so no existing
     * intent's numeric value moves. */
    FF_INTENT_T9_CYCLE,                         /* advance the candidate selection (the › chip) */
    FF_INTENT_T9_SELECT,                        /* select a candidate by index — reuses u.t9_key as that index */
    FF_INTENT_FLARE_START, FF_INTENT_FLARE_END,
    FF_INTENT_TAKEOVER_GO, FF_INTENT_TAKEOVER_DISMISS, FF_INTENT_RELEASE_LOCK,
    FF_INTENT_SETTING_SET,
    /* [api] S21 §3 — run the touch-calibration crosshair flow from the
     * Settings "CALIBRATE TOUCH" row. Shell-owned, one seam, mirroring the
     * other setting intents: no payload. The shell invokes its injected
     * device calibrate hook (ff_shell_cfg_t.calibrate_touch), which on
     * device runs ff_display_run_calibration -> ff_display_touch_set_cal and
     * returns the solved transform; the shell writes it into ff_settings and
     * persists. On a target with no touch panel (the sim) the hook is NULL
     * and this intent is a safe no-op — the row renders, nothing happens,
     * goldens/tests stay green.
     *
     * (#105's FF_INTENT_SETTINGS_PAGE was removed here: S21 replaced the
     * paginated Settings with one scrolling list, so there is no page to
     * cycle — see scr_settings.c and the scroll-aware sweep in
     * test_face_hit_targets.c.) */
    FF_INTENT_CALIBRATE_TOUCH,
    /* [api] S22 slice b — the reworked Signals face (docs/specs/S22-signals-rework.md).
     * The S24 inbox rework retired the S22 screen these were built for, but
     * the S22(d) send machinery they drive stayed (see ff_shell.h /
     * ff_app_state.h's "what of S22 carries over" notes); these five
     * intents are still the whole seam between the (now S24) screen and
     * the shell's send-target state. Appended, so no existing intent's
     * numeric value moves.
     *
     * SIG_SELECT_MEMBER / SIG_CLEAR_TARGET mutate the shell's persistent
     * target holders (`inbox_target_kind`/`inbox_target_node` in
     * ff_shell.c) NOW (S22 AC3). The shell is the only place the roster is
     * consulted to validate a selection — SELECT is checked against
     * `shell_member_paired` at the point of the intent, and the same
     * revalidation runs again every projection tick (a member who
     * unpaired/left honestly falls back to WHOLE_CREW). SELECT carries the
     * tapped member's node id in `u.node_id` (a paired-crew node by
     * construction — every selectable row has a known identity); CLEAR has
     * no payload.
     *
     * SIG_RALLY / SIG_COMPOSE were originally three action buttons with
     * SIG_PULSE alongside them (RALLY encodes an `ff_proto` RALLY, PULSE
     * encoded an `ff_proto` PULSE), dispatched over FF_PORTNUM to the
     * current target (WHOLE_CREW broadcast vs a member's addressed send),
     * then the target resets to WHOLE_CREW (S22 AC3). RALLY to WHOLE_CREW
     * is the one loud broadcast, so its first tap ARMS a confirm (the
     * shell mirrors its `inbox_rally_armed` state machine directly into
     * the view's `confirm_armed` display field — `view.inbox.rally_confirm_armed`
     * and `view.inbox.rally.confirm_armed` — each tick, and the button
     * renders from that) and only a second tap within a short window sends
     * (S22 AC4); a member rally sends on the first tap. COMPOSE opens the
     * composer with its TO set to the current target and switches face —
     * the composer's own SEND does the text send. No payload — each acts
     * on the current target, which the shell reads from its own
     * `inbox_target_kind`/`inbox_target_node` holder, never from the
     * screen (a pure renderer must not carry the target itself).
     * (2026-09-02: FF_INTENT_INBOX_PULSE, the SIG_PULSE member,
     * is retired — see the FLARE note further below, where its sibling
     * FF_INTENT_INBOX_POPUP_PULSE is also retired; removing an enum member
     * here renumbers everything after it, which is fine — intents are
     * never persisted or encoded by number anywhere.) */
    FF_INTENT_INBOX_SELECT_MEMBER, FF_INTENT_INBOX_CLEAR_TARGET,
    FF_INTENT_INBOX_RALLY, FF_INTENT_INBOX_COMPOSE,
    /* [api] S24 slice b — the Signals inbox -> thread navigation seam
     * (docs/specs/S24-signals-inbox.md). The Signals face is now the S24
     * INBOX (a projection of `ff_inbox_t`); these three intents are its
     * whole navigation seam. Appended, so no existing intent's numeric
     * value moves. All three carry a conversation key in `u.node_id`
     * where noted: 0 = the CREW conversation, nonzero = that member's
     * node id — the ff_inbox convention (a member conversation never has
     * node 0), so no second enum crosses this layering-free header.
     *
     * INBOX_OPEN_THREAD — a conversation row tap. The shell switches the
     * Signals sub-view to THREAD scoped to that conversation, marks that
     * conversation's items read (`ff_inbox_mark_thread_read` — S24 AC4's
     * mark-read-on-open lives on this transition), and sets the S22(d)
     * send target to the thread's scope ("the open thread IS the send
     * scope"). Payload: u.node_id.
     *
     * INBOX_NEW — the inbox's `+` FAB. Opens the RECIPIENT PICKER
     * sub-view (the spec's "inbox + first opens a scope step"). No
     * payload.
     *
     * INBOX_PICK — a recipient-picker row tap (CREW or one member).
     * Payload: u.node_id. In slice (d) this routes to the action popup
     * scoped to the pick; until then the shell routes it to the thread
     * for that scope (same handling as OPEN_THREAD), so the picker is a
     * real, honest navigation today and only the shell-side routing
     * changes when the popup lands — the emit site does not. */
    FF_INTENT_INBOX_OPEN_THREAD, FF_INTENT_INBOX_NEW, FF_INTENT_INBOX_PICK,
    /* [api] S24 slice (d) — the action popup + Rally screen seam
     * (docs/specs/S24-signals-inbox.md, AC5/AC6). Appended, so no existing
     * intent's numeric value moves. The popup + Rally are shell-owned
     * sub-views over the open thread's scope ("the open thread IS the send
     * scope"); these intents carry NO payload except RALLY_SELECT_PLACE —
     * each acts on the scope the shell already holds, never one the screen
     * claims (a pure renderer must not carry the scope).
     *
     * POPUP_COMPOSE — the popup's Compose row: opens the S08 composer with
     *   its TO set to the scope, dropping back to the thread underneath so
     *   SEND/BACK land there.
     * POPUP_RALLY — the popup's Rally row: opens the Rally sub-view.
     * RALLY_SELECT_PLACE — a WHERE radio-row tap. `u.rally_idx` is the
     *   selection: 0 = On Me, 1..place_count = landmark (idx - 1). An On Me
     *   pick while my position is unknown is rejected (the row is disabled).
     * RALLY_CYCLE_WHEN — the WHEN chip: Now -> +15m -> +30m -> Now.
     * RALLY_SEND — the Send button: encodes+sends the rally to the scope at
     *   the selected place/when, then pops to the thread. A crew-wide rally
     *   arms on the first tap and sends on the second (S22 AC4).
     * (2026-09-02: the popup's third row used to be POPUP_PULSE — see the
     * FLARE note just below, retired along with it.) */
    FF_INTENT_INBOX_POPUP_COMPOSE, FF_INTENT_INBOX_POPUP_RALLY,
    FF_INTENT_RALLY_SELECT_PLACE, FF_INTENT_RALLY_CYCLE_WHEN, FF_INTENT_RALLY_SEND,
    /* The OUTBOUND quick signal is a FLARE ("come find me"), not a PULSE
     * (empty ping) — the maintainer's "in send to crew we should have flare
     * not pulse". Both intents encode an `ff_proto` FLARE
     * (`ff_proto_encode_flare`, FF_FLARE_DEFAULT_DUR_S) and dispatch it over
     * FF_PORTNUM to the current scope through the SAME S22(d)/S24(d) send
     * seam PULSE used to. Appended, so no existing intent's numeric value
     * moves.
     *
     * SIG_FLARE — the 1:1 thread's quick chip (was the PULSE chip): flares
     *   the open thread's scope, then (no thread open) resets the target to
     *   WHOLE_CREW, exactly as the retired SIG_PULSE did.
     * INBOX_POPUP_FLARE — the action popup's third row (was the Pulse row):
     *   flares the scope immediately, then pops the popup back to the thread
     *   (the OUT flare shows there).
     *
     * 2026-09-02, PULSE retired end to end: FF_INTENT_INBOX_PULSE and
     * FF_INTENT_INBOX_POPUP_PULSE — the outbound-pulse programmatic seam
     * this comment used to say were DELIBERATELY kept for the shell's unit
     * tests, since no screen emitted them — are now removed outright along
     * with `shell_pulse_to_scope` (ff_shell.c) and `FF_WIRING_REPLY_PULSE`
     * (this header, above): the device has no notion of a pulse left to
     * program against, so keeping a dead send path alive under test bought
     * nothing. Removing them DOES renumber the intents below, which is
     * fine — nothing persists or encodes an `ff_intent_kind_t` by number
     * (contrast `ff_proto_type_t`, which travels over RF and is why
     * FF_PROTO_TYPE_RESERVED_01 stays a reserved slot instead). The
     * INCOMING pulse — the feed kind, the "DANA pulsed you" wording, the
     * demo generator — is ALSO retired now (this was the surviving piece
     * when only the outbound send path was retired, PR #129); see
     * ff_feed.h's FEED_PULSE removal and scr_inbox.c's rendering. */
    FF_INTENT_INBOX_FLARE, FF_INTENT_INBOX_POPUP_FLARE,
    /* [api] S26 slice b — PWR button -> power menu -> soft power-off
     * (docs/specs/S26-device-lifecycle.md). Appended, so no existing
     * intent's numeric value moves. No payload on any of the four — each
     * acts on state the shell already owns (the route's modal, the
     * injected power_off/power_reboot hooks), never on anything the
     * screen or the hardware sampler carries.
     *
     * POWER_MENU_OPEN — NOT emitted by a screen. The esp32s3 target's
     *   app_main calls `ff_shell_intent()` with this directly (the same
     *   pattern FF_INTENT_CALIBRATE_TOUCH's in-app re-emit already uses,
     *   ff_shell.c) when its `ff_power_fsm_t` reports LONG_PRESS —
     *   app_main forwards the FSM's decision; it makes none of its own
     *   (CLAUDE.md's "no `if` about behavior in app_main"). Pushes the
     *   FF_APP_FACE_POWER_MENU modal (rejected, silently, exactly like
     *   any other push_modal call, if a takeover is up or another modal
     *   is already open — Compose's draft is never interrupted by a
     *   PWR long-press).
     * POWER_OFF — the menu's "Power off" button: calls the injected
     *   `ff_shell_cfg_t.power_off` hook (NULL on a target with no power
     *   latch — the sim — a safe no-op) and pops the modal. On device the
     *   hook drives GPIO7 low (`ff_power_off`) and the backlight to 0
     *   (`ff_display_set_brightness(0)`, called from app_main's hook —
     *   NOT from inside `ff_power`, which stays a pure two-pin GPIO HAL).
     * POWER_REBOOT — the menu's "Reboot" button: calls the injected
     *   `power_reboot` hook (arms the target's own `ff_power_fsm_t`
     *   reboot-BOOT-release guard; app_main's tick loop is what actually
     *   calls `esp_restart()` once that guard reports ready) and pops the
     *   modal.
     * POWER_CANCEL — the menu's "Cancel" button: pops the modal. The same
     *   10 s auto-dismiss the shell applies on a timeout uses this exact
     *   pop, not a fourth intent. */
    FF_INTENT_POWER_MENU_OPEN, FF_INTENT_POWER_OFF, FF_INTENT_POWER_REBOOT, FF_INTENT_POWER_CANCEL,
    /* [api] S26 slice d — the message banner's tap (docs/specs/
     * S26-device-lifecycle.md "(d) ff_notify + message banner"). Appended,
     * so no existing intent's numeric value moves. No payload: `scr_banner.c`
     * is a pure renderer of `ff_app_state_t.banner` (the projected HEAD of
     * the shell's own `ff_notify` queue) and must not carry the scope
     * itself — same "acts on state the shell already owns" convention as
     * the popup/power-menu intents above. The shell routes to the banner's
     * sender's thread (S24 `ff_inbox` thread), marks it read, and dismisses
     * the head banner — see `ff_shell_intent`'s case for the exact steps. */
    FF_INTENT_BANNER_OPEN,
    /* [api] S26 slice e — the BOOT-button home model + the launcher's
     * own tap (docs/specs/S26-device-lifecycle.md "(e) Home button +
     * launcher"), AMENDED 2026-09-01 (the launcher IS home now — see
     * `ff_route.h`'s header note for the full model change). Appended,
     * so no existing intent's numeric value moves. This slice also
     * RETIRES `FF_INTENT_SWIPE` as a live navigation mechanism — no
     * screen emits it any more (scr_nav.c's gesture handler is gone) and
     * `ff_shell.c`'s own case is now a no-op — but the member itself
     * stays (removing it would renumber every intent after it) and its
     * union payload is untouched.
     *
     * HOME — NOT emitted by a screen, the same way POWER_MENU_OPEN
     *   above is not: the esp32s3 target's app_main debounces GPIO0
     *   through the new core `ff_button` (reusing `ff_power_fsm`'s
     *   debounce shape — see that header) and forwards one HOME per
     *   physical press. No payload — the shell owns the whole decision
     *   in `ff_route_home` (app/include/ff_route.h): set `base` to the
     *   launcher, a no-op if it already is; suppressed under a takeover
     *   or a live modal (Compose, Power menu) exactly like every other
     *   nav intent.
     * LAUNCHER_SELECT — a tap on one of the launcher's app circles
     *   (`scr_launcher.c`). Payload: `u.launcher_idx`, an index into the
     *   launcher's own fixed circle order — as of this amendment, FIVE
     *   circles (0=Radar, 1=Now, 2=Signals, 3=Map, 4=Settings; Radar is
     *   an ordinary circle now, no longer excluded) — a small int, not a
     *   domain enum, so this dependency-free header (see its top
     *   comment) needs no `ff_app_state.h` include for `ff_app_face_t`;
     *   the shell maps the index to a face and calls
     *   `ff_route_launcher_select`. Only meaningful while the launcher
     *   is showing with nothing over it — a no-op otherwise
     *   (`ff_route_launcher_select`'s own guard). */
    FF_INTENT_HOME, FF_INTENT_LAUNCHER_SELECT,
    /* [api] S10 quick flare (docs/specs/S10-flare.md's Amendments,
     * 2026-09-03 maintainer decision — "press the HOME (BOOT, GPIO0)
     * button 5 times quickly to flare to the crew, no screen needed").
     * Appended, so no existing intent's numeric value moves. No payload:
     * same "acts on state the shell already owns" convention as
     * POWER_MENU_OPEN/HOME above.
     *
     * QUICK_FLARE — NOT emitted by a screen, and not even by a target's
     *   main loop directly: `ff_shell_home_press` (app/include/ff_shell.h)
     *   is the ONE place that constructs this, on the tick a physical
     *   HOME/BOOT press edge completes the shell's own multitap FSM
     *   (`core/include/ff_multitap.h`) — both targets forward every
     *   debounced press edge into `ff_shell_home_press`; the shell alone
     *   decides whether that edge was the 5th of one continuous burst.
     *   Handling mirrors FF_INTENT_FLARE_START exactly (same
     *   `ff_flare_send_begin` call, same default 300s duration, same
     *   wire-send path — see that intent's own case in ff_shell.c and
     *   this PR's body for the one known gap both share) with three
     *   differences: it is idempotent against a flare already in flight
     *   (no second SEND_FLARE, no restarted timer — the spec's explicit
     *   "if already flaring, a 5-tap does nothing"); it is deliberately
     *   NOT gated on a visible takeover the way FLARE_START is (routing
     *   rule 4 exists because FLARE_START is an on-screen Radar-face
     *   button, invisible during a takeover; quick flare is a hardware
     *   gesture that must work "from any state, screen off included"
     *   per the feature brief, takeover included — the flare still
     *   STARTS under a takeover, it is simply invisible until the
     *   takeover clears, at which point whatever's underneath renders
     *   it unassisted); and if a COMPOSE or POWER_MENU modal is up (and
     *   no takeover), it POPS that modal first — neither modal
     *   composites the sender overlay, so sending would otherwise be
     *   silently invisible for as long as the modal stayed open; see
     *   this intent's own case in ff_shell.c for the full three-case
     *   writeup (takeover / modal / neither). */
    FF_INTENT_QUICK_FLARE,
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
    /* [api] S17 slice a — the colorblind toggle (docs/specs/S17-usability-hardening.md).
     * bool-backed, same "nonzero is true" int payload convention as
     * IMPERIAL/HAPTICS/NIGHT_GLOW above. */
    FF_SETTING_COLORBLIND,
    /* [api] #100 — display brightness percent. Int payload, clamped by the
     * shell to [FF_BRIGHTNESS_MIN_PCT, FF_BRIGHTNESS_MAX_PCT] (ff_settings.h)
     * — the floor is non-zero on purpose (never a black, unrecoverable
     * screen). */
    FF_SETTING_BRIGHTNESS,
    /* [api] S21 amendment — the Settings CLOCK 12H|24H toggle
     * (ff_settings_t.clock_24h). Bool-backed, same "nonzero is true" int
     * payload convention as IMPERIAL/HAPTICS/NIGHT_GLOW/COLORBLIND above. */
    FF_SETTING_CLOCK_24H,
    /* [api] format v8 amendment (maintainer ask, 2026-09-02) — the
     * Settings SCREEN NORMAL|FLIPPED toggle (ff_settings_t.screen_flip).
     * Bool-backed, same "nonzero is true" int payload convention as every
     * other two-state row above — no range to reject. Drives a HARDWARE
     * 180° panel mirror + a touch-coordinate flip + a mirrored glass
     * centre; see ff_settings.h's doc comment on the field for the full
     * mechanism. */
    FF_SETTING_SCREEN_FLIP,
} ff_setting_id_t;

typedef struct {
    ff_intent_kind_t kind;
    union {
        /** SWIPE. A ROUTE direction, not a finger direction: -1 toward
         *  RADAR, +1 toward SIGNALS — a rightward finger drag maps to -1
         *  (ff_route.h's `ff_route_swipe` doc has the full warning). */
        int8_t swipe_dir;
        ff_wiring_canned_reply_t reply;         /* CANNED_REPLY */
        /** SELECT_CREW, OPEN_COMPOSE, SIG_SELECT_MEMBER,
         *  INBOX_OPEN_THREAD, INBOX_PICK (S24: a conversation key —
         *  0 = CREW, nonzero = that member's node id). For OPEN_COMPOSE:
         *  an explicit destination, or 0 = none given, which the shell
         *  resolves per S08's Behavior ("TO = selected crew member") — the
         *  currently selected paired member if there is one, else broadcast.
         *  See ff_shell_intent's doc in ff_shell.h for the exact rule. For
         *  SIG_SELECT_MEMBER (S22): the tapped Signals row's crew node id,
         *  which the shell validates against the roster (`shell_member_paired`)
         *  before it becomes the send target. */
        uint32_t node_id;
        uint8_t rally_idx;                      /* SELECT_RALLY; RALLY_SELECT_PLACE
                                                 * (S24 d: 0 = On Me, 1.. = landmark) */
        uint8_t launcher_idx;                   /* LAUNCHER_SELECT (S26e, amended
                                                 * 2026-09-01): 0=Radar, 1=Now,
                                                 * 2=Signals, 3=Map, 4=Settings —
                                                 * the launcher's own fixed circle
                                                 * order (scr_launcher.c); the shell
                                                 * maps this to an ff_app_face_t. */
        uint8_t t9_key;                         /* T9_KEY: 0-9. REUSED by T9_SELECT
                                                 * as the candidate index to select
                                                 * (0-based, into the shown chips) —
                                                 * the maintainer's "reuse the t9 int
                                                 * field" choice, no new union member. */
        char const *text;                       /* T9_INSERT (NOT owned; copied — see top comment) */
        struct { ff_setting_id_t id;            /* SETTING_SET */
                 union { int32_t i; char const *s; } v;
                 /* [api] #bug1 — a TRANSIENT setting is a live preview the
                  * shell applies to its in-memory state (so a projection
                  * consumer like the device backlight follows it) but does
                  * NOT persist. Only the brightness slider uses it, emitting
                  * transient on every VALUE_CHANGED during a drag and a final
                  * NON-transient (committed) value on RELEASED, so a drag
                  * writes NVS exactly once instead of on every step. Defaults
                  * to false via every emit site's `{...}`/`.u = {0}`
                  * zero-init, so every existing emitter persists unchanged;
                  * handlers that do not opt in ignore it. */
                 bool transient; } setting;
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
