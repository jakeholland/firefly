/**
 * ff_shell.h — the running application (S16 slice b1).
 *
 * Spec: docs/specs/S16-app-shell.md, "App: the shell".
 *
 * `ff_shell_t` is the object that owns a live `mc_client_t`, the core
 * domain state (crew / heard / feed / flare / settings / wall clock) and
 * the `ff_app_state_t` the screens render — together, across time. It is
 * target-agnostic: `targets/sim` and `targets/esp32s3` both drive the
 * same object, and neither of them owns any domain logic.
 *
 * This slice (b1) is the skeleton plus the inbound half:
 *   - lifecycle (`ff_shell_init` / `ff_shell_tick` / `ff_shell_view` /
 *     `ff_shell_close`),
 *   - the core -> view projection and its dirty bit,
 *   - all SEVEN `mc_events_t` callbacks, carrying the roster trust
 *     policy (the spec's slice table said five; see S16's Amendments),
 *   - haptic / quiet-hours composition,
 *   - wall-clock accessor over `core/include/ff_wall.h` (slice b0).
 *
 * Slice b2 retired `targets/sim/live.{c,h}` and repointed `--connect` at
 * this object: `targets/sim/main.c` now drives an `ff_shell_t` over a
 * `mc_tcp_t` transport, and added the sim-only dev affordances at the
 * foot of this header (`ff_shell_dev_*`, compiled out of device builds).
 *
 * Slice c1 adds the intent seam: `ff_intent_t` (app/include/ff_intent.h),
 * `ff_shell_intent` / `ff_shell_intent_sink` below, and the navigation
 * intent handling (SWIPE / BACK / OPEN_COMPOSE / OPEN_SETTINGS, plus the
 * takeover-decision trio GO / DISMISS / RELEASE_LOCK, which the S10
 * Ruling 3 distinctness test needs observable). Core-mutating intents
 * (canned replies, FLARE_START/END) are slice c2; SETTING_SET
 * write-through is slice e — both remain deliberate no-ops here until
 * their slice lands, alongside MARK_FEED_READ (already handled, just not
 * through this intent — S08 AC3) and SELECT_CREW/SELECT_RALLY (the
 * latter has no core entry point to call yet at all).
 *
 * Slice c3 moves the composer's DRAFT in (`compose_draft`, an `ff_t9_t`
 * — `scr_compose.c` no longer holds one) and wires the five T9 intents
 * (T9_KEY / T9_SPACE / T9_BACKSPACE / T9_MODE / T9_INSERT) plus
 * SEND_TEXT, which actually sends now, via `ff_wiring_ctx_t.sender` — the
 * same seam the canned replies use. All six are rejected while a
 * takeover is up (AC3b), leaving the draft untouched rather than
 * partially consumed.
 *
 * Slice e wires `FF_INTENT_SETTING_SET` (validated per `ff_setting_id_t`,
 * applied to `ff_settings_t`, persisted via `cfg->store` on change only —
 * AC8) and makes link state (`ff_shell_link_t`) reachable end to end: the
 * transport layer can now redial a dropped connection
 * (`meshclient/include/mc_transport_tcp.h`), so "reconnecting" is a
 * state `mc_client`'s own auto-reconnect can actually recover from,
 * closing the pre-existing gap PR #56 flagged. `ff_shell_link` was
 * already correct against `mc_state_t` since slice b1 — see the
 * `ff_shell_link_t` doc below.
 *
 * Deliberately NOT here yet:
 *   - build-once/update-in-place render lifecycle (slice d).
 *
 * ---------------------------------------------------------------------
 * LAYERING — a correction, not an exception
 * ---------------------------------------------------------------------
 * `app/ff_wiring.h` used to describe itself as "deliberately the ONE file
 * allowed to include core + meshclient + app together". `ff_shell.h`
 * breaks that by construction (`ff_shell_cfg_t` holds an
 * `mc_transport_t`), which S16 anticipates and rules on: the invariant
 * becomes **`ff_wiring.c` and `ff_shell.c` are the two such files**.
 * `ff_wiring.h`'s comment is updated in this slice rather than left
 * asserting something the design no longer holds.
 *
 * ---------------------------------------------------------------------
 * THE ROSTER TRUST POLICY (inherited verbatim from ff_wiring.h)
 * ---------------------------------------------------------------------
 *   > Inbound radio traffic never grows the paired roster. Unknown
 *   > senders go to `ff_heard` (bounded, LRU-evictable). Only an explicit
 *   > user pairing action adds a roster slot.
 *
 * **The rule is about effect, not about which function is called.** Four
 * core entry points find-or-CREATE internally and therefore may never be
 * reached from an inbound callback without a prior `ff_crew_find`:
 * `ff_crew_upsert`, `ff_crew_set_paired`, `ff_crew_on_position` and
 * `ff_crew_on_rssi`. Calling `ff_crew_on_position` straight from
 * `on_position` satisfies the letter of "don't call `ff_crew_upsert`"
 * while committing the exact roster-exhaustion bug the rule exists to
 * prevent (S16 AC5c). Every inbound path in `ff_shell.c` therefore looks
 * the sender up read-only first and drops it — noting it in `ff_heard` —
 * when it is not already a roster member.
 *
 * `ff_shell_pair()` is the one entry point that may grow the roster, and
 * it is not reachable from the radio — with ONE deliberate, sim-only,
 * compile-gated exception: `ff_shell_dev_trust_all` (S16 AC6, slice b2)
 * makes an inbound NodeInfo auto-pair its sender. That affordance does
 * not exist in a device build at all (`#if FF_TARGET_SIM` — the branch,
 * the field and the setter are compiled out, not defaulted off), so on
 * device the sentence above holds without exception. See the "Sim-only
 * dev affordances" section at the foot of this header.
 *
 * ---------------------------------------------------------------------
 * POSITION AGES — never "now"
 * ---------------------------------------------------------------------
 * `mc_client` auto-reconnects and restarts `want_config`, which replays
 * the cached node DB: a burst of **`on_node`** callbacks carrying old
 * positions that arrive now. Stamping those with the local clock makes
 * every crew member read LIVE off cached data the instant the link
 * flaps. So (S16 AC9):
 *
 *   - a position arriving over the air (`on_position`) is aged from
 *     `mc_position_t.rx_time`;
 *   - a position arriving in a NodeInfo replay (`on_node`) is aged from
 *     `mc_nodeinfo_t.last_heard` — `mc_client.c:222` hardcodes
 *     `has_rx_time = false` on that path, so there is no rx_time to use;
 *   - if neither is available the age is **unknown**, which is
 *     `FF_FRESH_NEVER`, not fresh. The shell does not record the position
 *     at all in that case — `ff_crew_on_position` sets `has_pos`, and a
 *     recorded position with a fabricated age is exactly the lie this
 *     rule forbids.
 *
 * Consequence worth stating plainly: **until the wall clock latches, no
 * position is recorded.** Unix timestamps off the wire cannot be related
 * to the monotonic clock without a latch, and there is no honest
 * fallback. In practice the latch happens during the want_config
 * handshake, from the same NodeInfo burst (see ff_wall.h).
 *
 * And the rule that closes the third incarnation of this defect (PR #46
 * review, D1):
 *
 *   > **A timestamp may not age a fix if that same timestamp is what
 *   > defines the clock the age is measured against.**
 *
 * `on_node` latches the wall from `last_heard` and then wants to age
 * that node's cached position from the same `last_heard`. When the
 * observation latches or re-latches, `ff_wall_unix_now()` returns
 * exactly that value, so the age is **zero by construction rather than
 * by measurement** — every cold boot stamps its cached positions LIVE,
 * and whichever node carries the greatest `last_heard` in a burst always
 * does, whatever the nodeDB order. So a NodeInfo that moved the latch
 * does not age its own position: it reads `FF_FRESH_NEVER`, which the
 * radar renders as "NO FIX YET" rather than a fabricated "LAST SEEN"
 * (ff_radar.h's renderer contract). Nodes later in the same burst with
 * older timestamps do not move the latch and ARE aged, against the
 * running maximum — the best estimate of "now" available.
 *
 * `on_position` deliberately does NOT carry the same guard, and the
 * asymmetry is the point rather than an oversight: `rx_time` is *this
 * packet's local receive time*, so "when did this arrive" and "what time
 * is it" genuinely coincide, and age ~0 is a measurement. `last_heard`
 * is *the node's contact history* — a lower bound on now and the receive
 * time of a possibly much older cached fix, which coincide only by
 * accident. The assumption `on_position` rests on, stated so it can be
 * checked: `mc_client` delivers MeshPackets as they arrive, and a
 * post-disconnect backlog is replayed as NodeInfo rather than as
 * MeshPackets, so a live `rx_time` is never long-stale.
 *
 * ---------------------------------------------------------------------
 * HAPTICS AND QUIET HOURS
 * ---------------------------------------------------------------------
 * `ff_flare_result_t.should_alert` **overrides quiet hours** and is
 * honoured unconditionally — never re-gated through `ff_quiet_now`.
 * Feed-push haptics ARE quiet-hours gated. Getting this backwards
 * silences a flare at 3 a.m., which is the one alert that must always
 * land (S16 AC11).
 *
 * Both are gated on `ff_settings_t.haptics`, the user's master switch:
 * "overrides quiet hours" is a statement about the quiet-hours window,
 * not about a user who has turned buzzing off entirely. Interpretation
 * noted in the PR body per CLAUDE.md.
 *
 * When the wall clock is `FF_WALL_UNKNOWN` there is no honest `now_min`,
 * so `ff_quiet_now` is not evaluated at all (ff_wall.h forbids it) and
 * nothing is suppressed. Under-suppressing is the safe direction: a
 * surprise buzz is recoverable, a swallowed flare is not.
 *
 * ---------------------------------------------------------------------
 * `active_face` IS NEVER `FF_APP_FACE_FLARE`
 * ---------------------------------------------------------------------
 * That member exists so `ff_route_visible()` has something to return —
 * an input-dispatch answer. The takeover stays `ff_flare_t`'s single
 * fact, projected as `ff_app_state_t.flare.takeover_active`, and
 * `targets/sim/face_dispatch.c` keeps dispatching on it. The projection
 * writes `modal ? modal : base` into `active_face` and never consults
 * `ff_route_visible()` (S16 AC13).
 */
#ifndef FF_SHELL_H
#define FF_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_app_state.h"
#include "ff_clock.h"
#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_flare.h"
#include "ff_heard.h"
#include "ff_intent.h"
#include "ff_latlon.h"
#include "ff_settings.h"
#include "ff_store.h"
#include "ff_touchcal.h" /* S21 §3 — ff_touchcal_t, returned by the calibrate hook */
#include "ff_wall.h"

#include "fp_pack.h"

#include "ff_wiring.h" /* ff_wiring_sender_t — the outbound send vtable ff_shell_set_sender overrides */

#include "mc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Link state — first-class, per S16's "Behavior" section. A stale view
 * during reconnect must not present itself as live.
 *
 * Three values, the spec's own vocabulary. The mapping from
 * `mc_state_t`:
 *
 *   MC_STATE_READY        -> FF_SHELL_LINK_CONNECTED
 *   MC_STATE_HANDSHAKE    -> FF_SHELL_LINK_RECONNECTING
 *   MC_STATE_DISCONNECTED -> FF_SHELL_LINK_RECONNECTING once the link has
 *                            ever been up, else FF_SHELL_LINK_NONE
 *
 * `mc_client` schedules a retry on every failure and never gives up
 * (`mc_fail_and_schedule_reconnect`), so after the first successful
 * handshake the link genuinely is only ever connected or reconnecting —
 * FF_SHELL_LINK_NONE after that would claim a finality the client does
 * not have. Before the first success, "no link" is the honest reading.
 */
typedef enum {
    FF_SHELL_LINK_NONE = 0,     /* never connected, or no transport attached */
    FF_SHELL_LINK_RECONNECTING, /* handshake in flight, or dropped and retrying */
    FF_SHELL_LINK_CONNECTED,    /* want_config complete, mc_client READY */
} ff_shell_link_t;

/**
 * ff_shell_cfg_t — everything the shell needs from its target.
 *
 * Every pointer must outlive the shell. `transport` is copied by value
 * (it is itself a vtable + `io` pointer, and the `io` object must
 * outlive the shell).
 */
typedef struct {
    ff_clock_t const *clock; /* monotonic; wall time is derived, see ff_wall.h */
    ff_store_t const *store; /* settings persistence (S11); NULL = defaults, no persistence */

    /** UART on device, TCP in sim. A transport whose `read`/`write` are
     *  both NULL is treated as "no transport": `ff_shell_init` skips
     *  `mc_connect`, the link stays FF_SHELL_LINK_NONE, and events can
     *  still be injected through `ff_shell_events()`. That is the test
     *  seam, and the same one `ff_wiring.h` documents for its own
     *  handlers — no radio, no handshake, no socket. */
    mc_transport_t transport;

    /** Fired for feed pushes (quiet-hours gated) and flare alerts
     *  (unconditional). NULL = no haptics. */
    void (*haptic)(void *user);
    void *haptic_user;

    /** S21 §3 — device touch-calibration hook, invoked by
     *  FF_INTENT_CALIBRATE_TOUCH (the Settings "CALIBRATE TOUCH" row). On a
     *  target with a touch panel (esp32s3) this runs the crosshair capture,
     *  applies the solved transform to the live touch path, and writes it to
     *  `*out_cal` returning true; the shell then persists it into
     *  ff_settings so the calibration survives reboot via the store. NULL on
     *  targets with no touch panel (the sim), where the intent is a safe
     *  no-op. Like `haptic`, a device-IO seam the pure shell never performs
     *  itself. */
    bool (*calibrate_touch)(void *user, ff_touchcal_t *out_cal);
    void *calibrate_touch_user;

    /** S26 slice b — the power-menu "Power off" hook, invoked by
     *  FF_INTENT_POWER_OFF (docs/specs/S26-device-lifecycle.md). Same
     *  injected-device-IO shape as `calibrate_touch`/`haptic`: NULL on a
     *  target with no power latch (the sim) is a safe no-op — the modal
     *  still pops, nothing fires below the shell. On device this calls
     *  `ff_power_off()` (GPIO7 low) AND `ff_display_set_brightness(0)`
     *  from app_main's own callback — deliberately NOT from inside
     *  `ff_power`, which stays a pure two-pin GPIO HAL with no display
     *  dependency (S25/S26 hardware contract). */
    void (*power_off)(void *user);
    void *power_off_user;

    /** S26 slice b — the power-menu "Reboot" hook, invoked by
     *  FF_INTENT_POWER_REBOOT. NULL on the sim (safe no-op — the modal
     *  still pops). On device this arms the target's OWN `ff_power_fsm_t`
     *  reboot-BOOT-release guard (`ff_power_fsm_request_reboot`) and
     *  returns immediately — it does NOT call `esp_restart()` itself.
     *  app_main's tick loop is what polls `ff_power_fsm_reboot_ready()`
     *  every tick and calls `esp_restart()` once BOOT (GPIO0) reads
     *  released, so a reboot requested while BOOT happens to be held
     *  never risks entering the ROM bootloader (S26 AC4). */
    void (*power_reboot)(void *user);
    void *power_reboot_user;

    /**
     * WHERE `fp_pack_t` LIVES — the decision S16 leaves to the
     * implementer, made here explicitly rather than left implied.
     *
     * **Beside the shell, not inside it.** The target owns pack storage
     * and hands the shell a pointer; `ff_shell_load_pack` parses into
     * it. NULL means this target has no pack support at all, and
     * `ff_shell_load_pack` fails rather than silently doing nothing.
     *
     * Why beside:
     *  - `fp_pack_t` carries a ~48 KB budget of its own (fp_pack.h),
     *    four times everything else in the shell put together. Folding
     *    it in would make `ff_shell_t`'s stated footprint one field's
     *    footprint, and the `_Static_assert` below would stop being a
     *    guard against runaway growth in the shell — which is the only
     *    thing it is for.
     *  - On device the two objects want different memory. The shell is
     *    touched every tick and belongs in internal SRAM; the pack is
     *    read a few times a minute and belongs in PSRAM
     *    (`EXT_RAM_BSS_ATTR`), which is exactly the placement fp_pack.h
     *    already assumes ("must live comfortably in ESP32-S3 PSRAM").
     *    One combined object has to go wherever the larger half needs
     *    to, and that is the wrong place for the hot half.
     *  - Pack lifetime is then the target's, which is where it already
     *    is: the target reads the file, under its own documented read
     *    budget, and the shell only parses bytes.
     *
     * The cost, stated: `ff_shell_cfg_t` gains a field the spec's own
     * sketch does not have, and a target that wants a pack must
     * allocate one. Noted in the PR body as a deviation.
     */
    fp_pack_t *pack;

    /**
     * S26 slice (a) — the jsmn token scratch `ff_shell_load_pack` hands
     * to `fp_parse` (fp_pack.h). Same "beside the shell, not inside it"
     * reasoning as `pack` above, for the same reason `fp_parse` stopped
     * owning a static arena: this is ~128KB at FP_MAX_TOKENS, and the
     * target is the one that knows whether it can afford that in
     * internal RAM or should put it in PSRAM.
     *
     * RETAINED, not borrowed: `ff_shell_init` copies `toks`/`ntoks` into
     * the shell (`sh->toks`/`sh->ntoks`), and EVERY subsequent
     * `ff_shell_load_pack` call reuses that same stored pointer —
     * `ff_shell_load_pack` is the shell's real pack-load path (used
     * whenever a fresh/updated festpack arrives, not a one-shot
     * boot-time helper), so it is called more than once over a shell's
     * life. The caller must keep this buffer valid for as long as the
     * shell itself lives, exactly like `pack` above. On the esp32s3
     * target this is a PSRAM allocation that is deliberately never
     * freed (see `app_main.c`); on host/sim, a static or heap buffer
     * that outlives the shell. Freeing it after only the first
     * `ff_shell_load_pack` call is a use-after-free on every call after
     * that — see `S26_ff_shell_load_pack_twice_same_shell_same_toks_buffer`
     * (`test_shell.c`) for the test that guards this.
     *
     * `toks` must point to at least `ntoks` writable `jsmntok_t` slots;
     * pass `FP_MAX_TOKENS` (fp_pack.h) for `ntoks` unless sizing smaller
     * deliberately. NULL `toks` (or `ntoks <= 0`) means this target has
     * no pack-parsing support, matching `pack == NULL` above —
     * `ff_shell_load_pack` fails cleanly rather than crashing.
     */
    jsmntok_t *toks;
    int ntoks;
} ff_shell_cfg_t;

/* ---------------------------------------------------------------------
 * ff_shell_t — opaque, caller-allocated
 * ------------------------------------------------------------------- */

/**
 * FF_SHELL_BYTES — the shell's stated footprint budget.
 *
 * `ff_shell_t` is **opaque**: the real layout lives in `ff_shell.c` and
 * no field of it is public. But the shell also allocates nothing (core
 * and the libraries are zero-heap, and the device target has no business
 * malloc'ing its own application object at boot), so the caller has to
 * be able to allocate it — hence a public, opaque byte budget rather
 * than a public struct. Targets do `static ff_shell_t s_shell;` and hold
 * a pointer, exactly as S16 describes.
 *
 * `ff_shell.c` static-asserts that the real struct fits, so this is a
 * hard build-time guard and not merely documentation: a slice that grows
 * the shell past its budget fails to compile with a named error, the
 * same discipline `ff_app_state.h` and `fp_pack.h` apply to their own
 * structs.
 *
 * RAISED 16 KB -> 22 KB in S09's PR #73 fix round, deliberately, per
 * this comment's own instruction above ("raise the budget deliberately
 * and say why"): `ff_app_state_t.map`'s two feature-array caps
 * (`FF_APP_MAP_MAX_FEATURES`/`FF_APP_MAP_MAX_POLY_PTS`, `ff_app_state.h`)
 * grew from 8/12 to 20/16 to close a real silent-truncation defect found
 * rendering the actual, currently-merged Lost Lands pack — see that
 * header's own doc comment for the full story. Two `ff_app_state_t`
 * copies (the view + the previous frame's render key) means that growth
 * lands here TWICE. Measured, not estimated: `sizeof(shell_t)` is
 * 19,936 B against the OLD 16,384 B budget (a hard compile failure, not
 * a close call). 22 KB (22,528 B) is the smallest round-KB number that
 * (a) clears 19,936 with real headroom (~2.6 KB) for the next slice's
 * own small change, and (b) stays UNDER `sizeof(fp_pack_t)` (23,696 B
 * measured) — `test_shell.c`'s `S16_b1_shell_footprint_excludes_the_pack`
 * pins the shell staying meaningfully smaller than the pack as a
 * red-flag check against accidentally folding a pack in, and this
 * growth has nothing to do with that, so it's kept true rather than
 * loosened. Note that `fp_pack_t` is NOT in here — see
 * `ff_shell_cfg_t.pack`.
 *
 * RAISED 22 KB -> 23 KB in the S08 predictive-composer PR, deliberately,
 * per this comment's own instruction: the predictive composer adds the
 * shell's FESTPACK WORD TABLE — `compose_extra[FF_COMPOSE_EXTRA_MAX]`, 280
 * `char const *` (all of a pack's set/stage/landmark names, the maintainer's
 * "all names" budget) = 2,240 B — plus the predictive session and the two
 * `ff_app_state_t` copies' grown `compose` section. Measured, not estimated:
 * `sizeof(shell_t)` is 23,104 B against the old 22,528 B budget (a hard
 * compile failure). 23 KB (23,552 B) is the smallest round-KB number that
 * clears 23,104 with headroom (~448 B) AND stays UNDER `sizeof(fp_pack_t)`
 * (23,696 B measured) so `test_shell.c`'s
 * `S16_b1_shell_footprint_excludes_the_pack` red-flag check stays true —
 * the festpack word table ALIASES into the pack (fp_t9words_collect copies
 * nothing), so this growth is 280 pointers, not a folded-in pack.
 *
 * RAISED 23 KB -> 25 KB in S22 slice b, deliberately, per this comment's
 * own instruction — and this one crosses a line the earlier raises did not:
 * the shell now exceeds `sizeof(fp_pack_t)`. The reworked Signals face
 * renders the core view-model `ff_sigview_t` DIRECTLY (docs/specs/S22, and
 * ff_app_state.h's signals doc), so `ff_app_state_t` embeds it by value, and
 * the shell holds TWO of it — `view` and the `prev_key` render-key copy —
 * plus a tiny 8-byte send-target HOLDER (sig_target_kind/node) rather than a
 * third full view-model: the rows are built straight into `view.signals`
 * each tick (the RADAR precedent, `ff_radar_compute(&sh->view.radar, ...)`
 * beside a small `ff_radar_smooth_t`), and only the target survives the
 * per-tick view memset. `ff_sigview_t` is 1,816 B (FF_SIGVIEW_MAX_ROWS=41
 * fixed rows), so the two embedded copies add ~2 KB. Measured, not
 * estimated: sizeof(shell_t) is 25,136 B against the old 23,552 budget (a
 * hard compile failure). 25 KB (25,600 B) clears it with ~464 B headroom,
 * the same tight round-KB headroom the earlier raises used. This growth is
 * REAL view-model state, not a folded-in pack — but it does mean the old
 * "shell < pack" red-flag proxy (test_shell.c's
 * S16_b1_shell_footprint_excludes_the_pack) no longer holds and was updated
 * there to a check that still catches a pack-embed (a pack would add ~23.7 KB
 * more, far past any view-copy count). On the S3's 512 KB SRAM a ~25 KB
 * static shell object is comfortable; the budget stays a runaway-growth
 * tripwire, not a hardware limit.
 *
 * S24 slice b (no raise needed): the view's `signals` section became
 * `ff_app_signals_t` — the embedded `ff_inbox_t` conversation model plus
 * sub-view/thread/target fields — which is a little SMALLER than the
 * `ff_sigview_t` it replaced (9 conversations vs 41 rows), and the shell
 * gained only two small persistent holders (sig_subview/sig_thread_node)
 * on the S22 target-holder precedent. The budget stood unchanged.
 *
 * RAISED 25 KB -> 32.5 KB in S24 slice c, deliberately, per this
 * comment's own instruction: the thread screens render the core
 * `ff_inbox_thread_t` DIRECTLY (`ff_app_signals_t.thread`,
 * ff_app_state.h — the same embed-the-real-view-model resolution as
 * `inbox`/`radar`/`ff_sigview_t` before it), and `ff_inbox_thread_t` is
 * 3,460 B (FF_INBOX_MAX_MSGS = FF_FEED_CAP = 32 messages, each carrying
 * its FF_FEED_TEXT_LEN text + joined identity), landing TWICE via the
 * `view` + `prev_key` copies. Measured, not estimated: sizeof(shell_t)
 * is 32,056 B against the old 25,600 B budget (a hard compile failure).
 * 32.5 KB (33,280 B) clears it with ~1.2 KB headroom — same
 * tight-round-number discipline as the earlier raises. Still REAL
 * view-model state (a pack-embed would add ~23.7 KB more at once — the
 * test_shell.c red-flag check still catches that), and a ~32 KB static
 * shell remains comfortable in the S3's 512 KB SRAM; this stays a
 * runaway-growth tripwire, not a hardware limit.
 */
#define FF_SHELL_BYTES 33280u

/** Alignment of the opaque payload. 8 covers every member the shell
 *  holds today (the widest are `double` inside `ff_latlon_t` and
 *  pointers); `ff_shell.c` static-asserts the real struct's `_Alignof`
 *  against it, so an over-aligned member added later fails the build
 *  rather than mis-aligning at runtime. */
#define FF_SHELL_ALIGN 8u

/**
 * The shell object. **Treat as opaque** — the byte array is storage, not
 * a field, and reading it is not supported. Allocate it (static, or on a
 * host stack in tests), pass `&it` to `ff_shell_init`, keep the pointer.
 *
 * NOT RELOCATABLE after `ff_shell_init`: the shell holds interior
 * pointers into itself (the `ff_wiring_ctx_t` it drives its feed through,
 * and `mc_events_t.user`). `memcpy`ing an initialised shell to another
 * address produces an object whose callbacks write into the old one.
 * Move the pointer, never the bytes.
 */
typedef struct ff_shell {
    _Alignas(FF_SHELL_ALIGN) unsigned char opaque[FF_SHELL_BYTES];
} ff_shell_t;

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/**
 * ff_shell_init — bring the shell up.
 *
 * Zeroes `*sh`, initialises every core module it owns, loads settings
 * from `cfg->store` (defaults if absent or corrupt — see
 * `ff_settings_load`), wires all seven `mc_events_t` callbacks, and — if
 * `cfg->transport` has a read or a write — calls `mc_connect` to start
 * the want_config handshake.
 *
 * Returns 0 on success, negative on failure (`sh` or `cfg` NULL, or
 * `cfg->clock` NULL — the shell cannot honestly do anything without a
 * clock). 0/negative convention as S16 specifies for every int return
 * here (inherited from the retired live.c's ff_live_load_pack).
 */
int ff_shell_init(ff_shell_t *sh, ff_shell_cfg_t const *cfg);

/**
 * ff_shell_load_pack — parse `len` bytes of festpack JSON into the
 * caller-provided `cfg->pack`, using the caller-provided `cfg->toks`
 * scratch (S26 slice a — see `ff_shell_cfg_t.toks` above).
 *
 * Bytes, not a path: the device has no filesystem the way the sim does,
 * and `fp_parse` is already bytes-based. The target reads the file under
 * its own documented budget; the shell parses.
 *
 * Returns 0 on success, negative on failure (no pack storage or no
 * token scratch was configured, NULL/empty input, or `fp_parse`
 * rejected it — including a too-small `cfg->ntoks`). On failure the
 * shell's "a pack is loaded" flag is cleared — `fp_parse` zeroes `*out`
 * on any error, and a zeroed `fp_pack_t` reads as a deliberately STATED
 * UTC offset of 0, which would silently outrank the user's configured
 * offset (see `ff_wall_offset_cfg_t`).
 *
 * Deliberately does NOT adopt the pack's venue origin as "my position",
 * as the retired `targets/sim/live.c` used to. That is a dev-harness
 * affordance: the venue centre is not where the wearer is standing, and
 * asserting it as a fix would fabricate a position (CLAUDE.md). A target
 * that wants that behaviour for development calls `ff_shell_set_my_pos`
 * itself, visibly — see `targets/sim/main.c`'s --pack block.
 */
int ff_shell_load_pack(ff_shell_t *sh, char const *json, size_t len);

/**
 * ff_shell_tick — pump the client, expire the flare timers, rebuild the
 * projection. The shell is passive: it never sleeps, blocks, or owns a
 * thread. The target picks the cadence (SDL callback in sim, an ESP-IDF
 * task on device); `mc_client.h` asks for ~50 Hz.
 *
 * Returns **true iff the RENDERED projection changed** since the last
 * tick — the input signal the slice-(d) render lifecycle runs on.
 *
 * "Rendered", not "raw", is the whole point. Several `ff_app_state_t`
 * fields are pure functions of elapsed time and change every tick by
 * construction (`send_expires_in_ms`, `takeover_expires_in_ms`,
 * `locked_expires_in_ms`) or every frame by asymptotic convergence
 * (`radar.arrow_deg`, exponentially smoothed). A whole-struct `memcmp`
 * would pass an idle test and still return true on every single frame in
 * the field, and slice (d) would buy nothing. The comparison is made
 * against a *render key*: the projection with exactly those fields
 * coarsened to the granularity that actually reaches the screen —
 * countdowns to whole seconds (what `flare_fmt` prints), the arrow to
 * 0.1 degrees (LVGL's own rotation unit). Every other field is compared
 * verbatim.
 *
 * The key is built by copying the whole projection and coarsening the
 * named fields, never by listing the fields that matter. That direction
 * is deliberate: a view field added by a later slice is automatically in
 * the key, so the failure mode of forgetting to update this is a
 * redundant repaint, never a stale screen.
 *
 * The first tick after `ff_shell_init` always returns true.
 */
bool ff_shell_tick(ff_shell_t *sh, uint32_t now_ms);

/**
 * ff_shell_view — the current projection, rebuilt by the last
 * `ff_shell_tick`. Valid until the next tick. NULL if `sh` is NULL.
 *
 * Before the first tick this is the zeroed struct, whose `active_face`
 * is `FF_APP_FACE_NONE` — not renderable, exactly as `ff_app_state.h`
 * documents for a failed fixture load. Tick before you render.
 */
ff_app_state_t const *ff_shell_view(ff_shell_t const *sh);

/**
 * ff_shell_wall — the wall clock now.
 *
 * A thin accessor over `ff_wall_now()` (slice b0): the latch, the
 * plausibility gate and the festival-day mapping all live in
 * `core/ff_wall.c`; this only composes the latch with the offset sources
 * (pack, then settings, in `ff_wall_resolve_offset`'s order).
 *
 * Reads the injected clock at the moment of the call rather than
 * reporting the last tick's answer — the shell's own quiet-hours gate
 * runs inside inbound callbacks, between ticks, and "is it quiet right
 * now" must not be answered from a tick that happened before the window
 * opened. (The projection is the exception and passes its own tick time
 * internally, so one frame describes one instant.)
 *
 * Reports `FF_WALL_UNKNOWN` — with every other field zeroed — until a
 * plausible mesh timestamp has latched, and says so rather than guessing.
 */
ff_wall_t ff_shell_wall(ff_shell_t const *sh);

/**
 * ff_shell_wall_rejected_relatches — S18 slice a, AC5: how many
 * disagreeing wall-clock re-latches the trust gate has refused over this
 * shell's lifetime — a source below TRUSTED (an unpaired/unknown node)
 * tried to move an already-established latch by more than
 * FF_WALL_RELATCH_DELTA_S and was refused (`ff_wall_observe` returned
 * FF_WALL_OBS_REJECTED for that reason specifically; plausibility-window
 * rejections do not count — see ff_wall_state_t's field doc). 0 if `sh`
 * is NULL. Monotonically increasing; there is no reset short of
 * `ff_shell_init`.
 *
 * Exists so a stranger attempting to move the clock is bench-visible —
 * `targets/sim/ctl_loop.c`'s `state` dump surfaces this in the `"wall"`
 * object — rather than a silent no-op (issue #49).
 */
uint32_t ff_shell_wall_rejected_relatches(ff_shell_t const *sh);

/**
 * ff_shell_replay_overflow_count — S18 slice b, AC4: how many buffered
 * cold-boot replay positions this shell has had to DROP because the
 * fixed FF_CREW_MAX-bounded settle buffer was full when another arrived.
 *
 * The buffer defers aging of the want_config replay's cached positions
 * until the wall latch settles (issue #50); it is bounded and allocates
 * nothing, so an overrun drops the oldest entry rather than growing. That
 * drop is a lost freshness recovery, so it is surfaced here (and in
 * `targets/sim/ctl_loop.c`'s `state` dump, beside `rejected_relatches`)
 * rather than being silent. 0 if `sh` is NULL. Monotonically increasing;
 * no reset short of `ff_shell_init`.
 */
uint32_t ff_shell_replay_overflow_count(ff_shell_t const *sh);

/** ff_shell_close — tear down: stops driving the client and marks the
 *  shell detached. Does NOT close the transport — the target opened it
 *  and owns it (the shell was handed a vtable, not a socket). Safe on a
 *  never-initialised or already-closed shell.
 *
 *  If the emit seam is bound to this shell
 *  (`ff_intent_emit_bind(ff_shell_intent_sink, sh)`), the caller must
 *  `ff_intent_emit_bind(NULL, NULL)` BEFORE closing or reusing the
 *  shell's storage — the seam holds a raw pointer this function cannot
 *  reach, and an emit through the stale binding is a use-after-free
 *  (ff_intent.h, "LIFETIME"). */
void ff_shell_close(ff_shell_t *sh);

/* ---------------------------------------------------------------------
 * Inbound seam
 * ------------------------------------------------------------------- */

/**
 * ff_shell_events — the `mc_events_t` the shell wires into its client,
 * with `user` already bound to `sh`.
 *
 * Two uses, same as `ff_wiring.h`'s handlers:
 *  - a target driving its own `mc_client_t` can attach these directly;
 *  - a test can call them with synthetic values as a mock event
 *    injector, with no transport, socket or handshake anywhere. That is
 *    how every AC5/AC9/AC11/AC13 test in `app/tests/test_shell.c` drives
 *    the shell.
 *
 * Returns a zeroed `mc_events_t` for a NULL `sh`.
 */
mc_events_t ff_shell_events(ff_shell_t *sh);

/**
 * ff_shell_pair — the explicit user pairing action, and **the only entry
 * point that may grow the paired roster**.
 *
 * Not reachable from the radio: nothing in `ff_shell.c`'s seven inbound
 * callbacks calls this — except, in a SIM BUILD ONLY, the
 * `ff_shell_dev_trust_all` auto-pair branch, which is compiled out of
 * device builds entirely (see the dev-affordances section below). On
 * device that makes the roster-trust policy a property of the code
 * rather than a comment. The pairing UI (S12) and `--dev-trust-all`
 * are its callers.
 *
 * Returns true if `node_id` now has the requested paired state; false if
 * `sh` is NULL, or the roster is full (`FF_CREW_MAX`, no eviction in v1)
 * and `node_id` is not already in it.
 */
bool ff_shell_pair(ff_shell_t *sh, uint32_t node_id, bool paired);

/* ---------------------------------------------------------------------
 * Intent seam — how screens talk to the shell (S16 slice c1)
 * ------------------------------------------------------------------- */

/**
 * ff_shell_intent — dispatch one semantic intent from the UI.
 *
 * The receiving end of the seam ff_intent.h defines. Dispatch targets
 * `ff_route_visible(&route, flare.takeover_active)` — the takeover flag
 * is read from `ff_flare_t` fresh, at this call, never cached (S16
 * routing rule 4). Note the residual honesty of that read: `ff_flare_tick`
 * clears an expired takeover on the *tick*, so between ticks this
 * dispatches against the same fact the last-rendered frame showed the
 * user — which is the frame their finger was aimed at.
 *
 * Handled in this slice:
 *  - SWIPE           -> `ff_route_swipe` (bounded, modal-suppressed —
 *                       the route's own rules). Rejected while a
 *                       takeover is visible.
 *  - BACK            -> `ff_route_pop_modal`. Rejected while a takeover
 *                       is visible (AC3b: both halves, routing AND the
 *                       draft, since the draft is shell-owned T9 state
 *                       as of slice c3 and BACK never touches it either
 *                       way — only SEND_TEXT does, on success).
 *  - OPEN_COMPOSE    -> `ff_route_push_modal(COMPOSE)`. The destination:
 *                       `u.node_id` names a paired roster member -> that
 *                       member; otherwise (0, or an id the trust policy
 *                       won't message) S08's Behavior rule — "TO =
 *                       selected crew member" — resolves it to the
 *                       current selection, else broadcast. An unknown
 *                       explicit id falls back to BROADCAST, never to a
 *                       different member: a message must not be silently
 *                       retargeted at somebody the caller did not name.
 *  - OPEN_SETTINGS   -> `ff_route_goto(SETTINGS)` — the nav long-press
 *                       jumps straight to the far-right Settings swipe
 *                       face (the horizontal-carousel rework made
 *                       Settings a base face, not a modal). Suppressed
 *                       under a takeover or a modal, exactly as swipe is,
 *                       and still gated on the S11b renderer constant.
 *                       There is no OPEN_MAP: Map is reached by swipe.
 *  - TAKEOVER_GO / TAKEOVER_DISMISS -> `ff_flare_go` /
 *                       `ff_flare_dismiss_takeover`, only while the
 *                       takeover is visible (they are decisions ABOUT
 *                       the takeover screen).
 *  - RELEASE_LOCK    -> `ff_flare_release_lock`, and deliberately NOT
 *                       gated on the takeover: S10 Ruling 3's race —
 *                       tapping "stop navigating" as a new takeover
 *                       arrives — must release the lock and leave the
 *                       takeover pending and shown. Folding it into the
 *                       dismiss branch, or dropping it, re-creates the
 *                       exact bug the ruling split two core functions to
 *                       kill. RELEASE_LOCK and TAKEOVER_DISMISS are
 *                       distinct and never folded.
 *  - T9_KEY / T9_SPACE / T9_BACKSPACE / T9_MODE / T9_INSERT (slice c3)
 *                    -> mutate `compose_draft` / `compose_mode` directly
 *                       (`ff_t9_key` et al. — the same calls
 *                       scr_compose.c used to make locally). All five
 *                       rejected while a takeover is visible.
 *  - SEND_TEXT (slice c3) -> if `compose_draft`'s text is non-empty,
 *                       sent via `ff_wiring_ctx_t.sender.send_text` to
 *                       `compose_to_node` (or `MC_ADDR_BROADCAST`), then
 *                       the draft resets and the modal pops. Rejected
 *                       while a takeover is visible (AC3b) — the draft is
 *                       left completely untouched, not partially sent.
 *                       An empty draft is a silent no-op either way: SEND
 *                       on an untouched composer must not broadcast "".
 *  - SETTING_SET (slice e) -> validated per `ff_setting_id_t`'s
 *                       documented range, applied to `ff_settings_t`, and
 *                       persisted via `cfg->store` ON CHANGE ONLY (never
 *                       every tick — S16 "Behavior"). An out-of-range
 *                       value is rejected outright, not clamped. Rejected
 *                       while a takeover is visible, same routing rule as
 *                       every other core-mutating intent (AC8).
 *
 * Every other kind is a documented no-op until its owning slice (c2:
 * remaining core-mutating intents) — see ff_shell.c.
 *
 * Pointer payloads (`u.text`, `u.setting.v.s`) are borrowed for this
 * call only; the shell copies what it keeps (ff_intent.h, "Payload
 * ownership"). State changes surface in the projection on the NEXT
 * `ff_shell_tick` — this function never rebuilds the view itself.
 *
 * NULL `sh` or `in`: safe no-op.
 */
void ff_shell_intent(ff_shell_t *sh, ff_intent_t const *in);

/**
 * ff_shell_intent_sink — `ff_intent_emit_fn`-shaped adapter: `user` must
 * be the `ff_shell_t *`. Exists so a target binds the seam without a
 * function-pointer cast (`ff_intent_emit_bind(ff_shell_intent_sink,
 * &shell)`) — casting `ff_shell_intent` itself to `ff_intent_emit_fn`
 * would call through an incompatible pointer type, which is undefined
 * behavior even where it happens to work. Same "callback-shaped wrapper"
 * convention as ff_wiring's mc_events_t-shaped handlers.
 */
void ff_shell_intent_sink(void *user, ff_intent_t const *in);

/* ---------------------------------------------------------------------
 * Sensor seam — what the shell cannot know by itself
 * ------------------------------------------------------------------- */

/**
 * ff_shell_set_my_pos — where this puck is.
 *
 * The shell has no GPS of its own: the comms brain owns position, and on
 * the sim there is none at all. Until this is called, `my_pos_ok` is
 * false and the radar face honestly reports NOFIX for any selection —
 * "no position of mine to compare against" is a true statement, not a
 * bug. Nothing else ever sets this; see `ff_shell_load_pack` for why a
 * pack's venue origin does not.
 */
void ff_shell_set_my_pos(ff_shell_t *sh, ff_latlon_t pos);

/** ff_shell_clear_my_pos — go back to "I do not know where I am". */
void ff_shell_clear_my_pos(ff_shell_t *sh);

/**
 * ff_shell_set_sender — override the shell's outbound `ff_wiring_sender_t`
 * (the vtable every send — canned reply, pulse, rally, composed text —
 * goes through). No-op if `sh` is NULL.
 *
 * `ff_shell_init` binds this to the real `mc_client_t` by default (the
 * mc_send_text/mc_send_private wrappers `ff_wiring_init` supplies), which
 * is what a field build keeps. It exists for two callers:
 *  - a target with no mesh transport that still wants sends to be VISIBLE
 *    — the CONFIG_FF_DEMO_MODE loopback sender (ff_demo.h), whose
 *    send_text/send_private accept (return 0) so the OUT feed item is
 *    pushed and the user's own message/pulse/rally shows in the thread.
 *    This echoes the user's REAL outbound as an OUT item; it fabricates no
 *    incoming content, and is wired only in clearly-labelled demo mode.
 *  - a test injecting a recording mock (the ff_wiring_init_with_sender
 *    seam, reached at the shell layer).
 *
 * The sender's `ctx` and function pointers must outlive the shell (they
 * are stored by value into the shell's wiring context).
 */
void ff_shell_set_sender(ff_shell_t *sh, ff_wiring_sender_t sender);

/**
 * ff_shell_set_heading — compass heading, degrees [0, 360), 0 = north.
 *
 * Pass a **negative** value for "unknown / unreliable" — the same
 * sentinel `ff_geo_heading_deg` already returns when the puck is tilted
 * past 60 degrees or the magnetometer reading is unusable, and what
 * `ff_radar_compute` already tests for. The shell starts at -1.
 */
void ff_shell_set_heading(ff_shell_t *sh, float heading_deg);

/* ---------------------------------------------------------------------
 * Read-only accessors (status bar, pairing UI, tests)
 * ------------------------------------------------------------------- */

/** ff_shell_link — current link state. FF_SHELL_LINK_NONE if `sh` is NULL. */
ff_shell_link_t ff_shell_link(ff_shell_t const *sh);

/** ff_shell_my_node_id — this node's id as reported by
 *  `mc_events_t.on_my_info`, or 0 if the handshake has not got that far.
 *  The shell uses it to avoid treating its own traffic as inbound. */
uint32_t ff_shell_my_node_id(ff_shell_t const *sh);

/** ff_shell_crew — the paired roster, read-only. NULL if `sh` is NULL. */
ff_crew_t const *ff_shell_crew(ff_shell_t const *sh);

/** ff_shell_heard — heard-but-unpaired nodes, read-only (the
 *  "add from heard nodes" list). NULL if `sh` is NULL. */
ff_heard_t const *ff_shell_heard(ff_shell_t const *sh);

/** ff_shell_feed — the Signals feed ring, read-only. NULL if `sh` is NULL. */
ff_feed_t const *ff_shell_feed(ff_shell_t const *sh);

/** ff_shell_flare — flare state, read-only. NULL if `sh` is NULL. The
 *  takeover is this struct's single fact; see AC13 in this header's top
 *  comment for why it is not also in `active_face`. */
ff_flare_t const *ff_shell_flare(ff_shell_t const *sh);

/** ff_shell_settings — current settings, read-only. NULL if `sh` is
 *  NULL. Write-through is `FF_INTENT_SETTING_SET` (slice e); persisted
 *  via the injected `ff_store_t` on change, never every tick. */
ff_settings_t const *ff_shell_settings(ff_shell_t const *sh);

/**
 * ff_shell_compose_to_node — the composer's current destination node
 * id; 0 = broadcast (and 0 for a NULL `sh`).
 *
 * This is the FACT behind the projected `compose.to_name`, exposed
 * because the name is a lossy proxy for the destination: "" is both
 * "broadcast" and "a member whose name has not arrived yet", and an
 * unknown node's name lookup returns "" too. PR #54's review found a
 * surviving mutant hiding in exactly that overlap — a
 * `shell_compose_dest` with its trust-policy guard deleted stored a
 * stranger's id while still projecting "", so every name-based
 * assertion passed. Tests (and any future status/pairing UI) assert
 * this fact directly; slice c3's SEND reads the same field internally.
 */
uint32_t ff_shell_compose_to_node(ff_shell_t const *sh);

/**
 * ff_shell_keep_awake — S26 slice (c)'s combined "keep awake" predicate
 * (docs/specs/S26-device-lifecycle.md, "(c) Inactivity -> dim -> screen
 * off", AC1: "no transition while an FSM-declared keep awake holds
 * (flare takeover pending, power menu open, calibration running)").
 * Feed the result straight into `ff_idle_tick`'s (core/include/ff_idle.h)
 * `keep_awake` parameter every tick.
 *
 * A pure function of already-public facts, deliberately NOT a method on
 * `ff_shell_t` — it takes the PROJECTED view (`ff_shell_view`'s return)
 * plus the one fact the view does not carry, so it is unit-testable with
 * a bare `ff_app_state_t` (no shell instance, no clock, no store) the
 * same way `ff_shell.c`'s render-key reduction is exercised. Two of the
 * three sources are already-public view fields:
 *
 *  - `view->flare.takeover_active` — a pending flare takeover (the
 *    existing S07 field; this slice adds no new one).
 *  - `view->active_face == FF_APP_FACE_POWER_MENU` — the power menu is a
 *    real, renderable modal face (S26 slice b), unlike FLARE's
 *    routing-only sentinel — see `ff_app_state_t`'s doc comment on
 *    `FF_APP_FACE_POWER_MENU` for why that distinction matters here:
 *    this is the same fact `face_dispatch.c` renders from, not a
 *    reach-into-private-route check.
 *
 * `touch_cal_running` is the one source the view cannot carry: the S21
 * §3 crosshair capture is a blocking device-runtime flow
 * (targets/esp32s3/main/app_main.c's `ff_display_run_calibration`) that
 * lives entirely outside `ff_shell_tick`'s projection, so the caller
 * (app_main) tracks it itself (set true immediately before the blocking
 * call, false immediately after) and passes it through here — this
 * function only combines it, per the house rule that the DECISION (the
 * OR, and what it feeds) lives in core/shell, never a scattered `if` in
 * app_main.
 *
 * Returns true if ANY of the three hold. `view == NULL` is treated as
 * "nothing to hold awake for" (false), same safe-default convention as
 * `ff_shell_link` etc.
 */
bool ff_shell_keep_awake(ff_app_state_t const *view, bool touch_cal_running);

/* ---------------------------------------------------------------------
 * Sim-only dev affordances (S16 AC6, slice b2) — COMPILED OUT on device
 * ---------------------------------------------------------------------
 * These exist only when FF_TARGET_SIM is defined (firmware/CMakeLists.txt
 * defines it iff FF_TARGET=sim). In a device build there is no
 * declaration, no field, and no branch — a device-target caller fails to
 * COMPILE, which is the spec's "compiled out, not defaulted off" demand:
 * a runtime flag would ship the auto-pair branch into device firmware,
 * one stray default change from being live. `targets/sim/main.c`
 * additionally carries an #error guard so a sim build that loses the
 * define fails loudly instead of silently parsing a flag that does
 * nothing.
 * ------------------------------------------------------------------- */
#if defined(FF_TARGET_SIM)

/**
 * ff_shell_dev_trust_all — `ffsim --dev-trust-all`: treat the dev bench
 * as trusted. Off by default; the sim target enables it only for that
 * flag, and logs a line naming it at startup.
 *
 * Two effects, both needed because the dockerized dev meshtasticd is a
 * SINGLE node that is also what `on_my_info` reports as our own id (see
 * firmware/tools/dev/crew_sim.py's verified-constraints note — the
 * harness's one node plays every role):
 *
 *  1. An inbound **NodeInfo** auto-pairs its sender into the roster —
 *     NodeInfo ONLY, never a bare Position (pairing on the most
 *     untrusted packet on the mesh is this spec's headline defect, and
 *     the dev affordance does not get to reintroduce it).
 *  2. The self filter is suspended: traffic from our own node id is
 *     processed as inbound, so the daemon's node can play a crew member.
 *
 * Recorded as an S16 AC6 amendment (effect 2 goes beyond the AC's
 * wording): without it the flag is useless against the only dev harness
 * this repo has, since every packet the harness can produce is "ours".
 */
void ff_shell_dev_trust_all(ff_shell_t *sh, bool enabled);

/**
 * ff_shell_dev_wall_observe — offer the HOST's clock to the wall latch,
 * exactly as a live packet's `rx_time` would be (unconditional, both
 * directions, still subject to ff_wall's plausibility window).
 *
 * Why this exists (and why it is honest): the desktop the sim runs on
 * genuinely knows what time it is — its clock is the same NTP-synced
 * clock the dockerized meshtasticd stamps `last_heard` from. Without
 * this, a single-node want_config replay can NEVER age the one position
 * it carries: the lone NodeInfo's `last_heard` is what bootstraps the
 * latch, and the D1 rule ("a timestamp may not age a fix if that same
 * timestamp defines the clock the age is measured against") then —
 * correctly — refuses to age its position, so the e2e radar scenario
 * reads NO FIX forever. Pre-latching from the host clock gives that
 * replay an independent clock to be measured against, and every age it
 * produces is a real measurement.
 *
 * This is NOT the FF_WALL_USER the spec cut: that was an unfalsifiable
 * number typed on a T9 keypad on the device. This is a machine clock,
 * sim-only, gated by the same plausibility window as any mesh
 * observation, and absent from device builds by construction.
 *
 * S18 slice a: offered to the latch as FF_WALL_TRUST_TRUSTED, not
 * BOOTSTRAP — consistent with the rationale above (an NTP-synced desktop
 * clock is exactly the TRUSTED case) and preserving this affordance's
 * pre-existing "unconditional both directions" behavior against the new
 * trust gate (a BOOTSTRAP-tier disagreement with an established latch
 * would otherwise be silently refused).
 */
/* Returns true iff the observation was ACCEPTED (latched, re-latched, or
 * agreed-within-tolerance) — false means the plausibility gate rejected
 * it and the latch is untouched. Callers reporting success must use this
 * return, not "is the wall resolvable afterwards": once ANY latch exists,
 * resolvability survives a rejected value, so that proxy reads ok for
 * exactly the inputs the gate refused (found by PR #64's own test). */
bool ff_shell_dev_wall_observe(ff_shell_t *sh, int64_t unix_now_s);

#endif /* FF_TARGET_SIM */

#ifdef __cplusplus
}
#endif

#endif /* FF_SHELL_H */
