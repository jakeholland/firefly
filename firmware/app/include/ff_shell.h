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
#include "ff_wall.h"

#include "fp_pack.h"

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
 * 16 KB against 12,240 bytes actually used today, measured, not
 * estimated: two `ff_app_state_t` projections at 3,448 B each — the view
 * and the previous frame's render key — plus `ff_feed_t`, `ff_crew_t`,
 * `mc_client_t` and small change. Headroom is deliberate but modest:
 * c1-c3 add an `ff_t9_t` draft, and d adds render bookkeeping. Note that
 * `fp_pack_t` is NOT in here — see `ff_shell_cfg_t.pack`.
 */
#define FF_SHELL_BYTES 16384u

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
 * caller-provided `cfg->pack`.
 *
 * Bytes, not a path: the device has no filesystem the way the sim does,
 * and `fp_parse` is already bytes-based. The target reads the file under
 * its own documented budget; the shell parses.
 *
 * Returns 0 on success, negative on failure (no pack storage was
 * configured, NULL/empty input, or `fp_parse` rejected it). On failure
 * the shell's "a pack is loaded" flag is cleared — `fp_parse` zeroes
 * `*out` on any error, and a zeroed `fp_pack_t` reads as a deliberately
 * STATED UTC offset of 0, which would silently outrank the user's
 * configured offset (see `ff_wall_offset_cfg_t`).
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
 *  - OPEN_SETTINGS   -> deliberately rejected (a no-op) until the S11b
 *                       Settings renderer exists — see the judgment-call
 *                       note in ff_shell.c's OPEN_SETTINGS case. The
 *                       route/dispatch path is complete; S11b flips one
 *                       named constant, no seam change.
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
 */
void ff_shell_dev_wall_observe(ff_shell_t *sh, int64_t unix_now_s);

#endif /* FF_TARGET_SIM */

#ifdef __cplusplus
}
#endif

#endif /* FF_SHELL_H */
