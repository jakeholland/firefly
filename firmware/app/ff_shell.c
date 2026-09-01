/**
 * ff_shell.c — see ff_shell.h.
 *
 * This is the second file in the repo that includes core + meshclient +
 * app together, alongside app/ff_wiring.c. That is by construction (S16
 * "Layering — a correction to a prior claim"), not an accident: the glue
 * that turns decoded Meshtastic events into domain state has to see both
 * sides, and there is nowhere else for the object that owns them across
 * time to live.
 */
#include "ff_shell.h"

#include <string.h>

#include "ff_geo.h"
#include "ff_proto.h"
#include "ff_radar.h"
#include "ff_route.h"
#include "ff_sched.h"
#include "ff_t9.h" /* S16 slice c3 — the shell-owned compose draft */
#include "ff_t9pred.h" /* S08 addendum — the predictive-T9 session */
#include "fp_t9words.h" /* S08 addendum — festpack -> predictive supplementary words */
#include "ff_touchcal.h" /* S21 §3 — the touch-calibration transform the calibrate hook returns */
#include "ff_wall_window.h" /* S18 slice c — pack -> plausibility window */
#include "ff_wiring.h"

/* ---------------------------------------------------------------------
 * The real shell. Opaque to everyone else — see ff_shell.h's
 * ff_shell_t / FF_SHELL_BYTES comments.
 * ------------------------------------------------------------------- */

/* S08 predictive addendum — capacity of the shell's festpack word table
 * (`compose_extra`). ALL of a pack's names: every set artist, every stage
 * name and every landmark name, so `fp_t9words_collect` can never be capped
 * before the pack is (the maintainer chose all names). Pinned to the
 * festpack caps so a future FP_MAX_* bump surfaces here as a build failure
 * rather than a silent truncation. */
#define FF_COMPOSE_EXTRA_MAX (FP_MAX_SETS + FP_MAX_STAGES + FP_MAX_LANDMARKS)
_Static_assert(FF_COMPOSE_EXTRA_MAX == 280,
               "FF_COMPOSE_EXTRA_MAX drifted from FP_MAX_SETS+FP_MAX_STAGES+FP_MAX_LANDMARKS");

/* S22 slice d — how long a RALLY-to-WHOLE_CREW confirm stays armed after
 * the first tap (AC4). A second RALLY tap within this window sends; past
 * it the arm has lapsed and the next tap re-arms (a stale confirm never
 * silently fires the one loud broadcast). Four seconds: long enough to be
 * a deliberate double-tap, short enough that a forgotten arm cannot lurk. */
#define FF_SIG_RALLY_CONFIRM_MS 4000u

/* S22 slice d, SPEC GAP (see docs/specs/S22-signals-rework.md "Questions"):
 * a quick RALLY gathers the crew to the SENDER's own current location, and
 * names the place after the nearest festpack landmark within this radius
 * (else the honest constant "MY SPOT"). Naming a rally after a landmark the
 * sender is nowhere near would be dishonest, so the match is gated on real
 * proximity; 120 m ~ "you are effectively at it" at festival scale. */
#define FF_SIG_RALLY_LANDMARK_NEAR_M 120.0f
#define FF_SIG_RALLY_DEFAULT_NAME "MY SPOT"

/* S24 slice d — the Rally WHERE list projects at most this many festpack
 * landmark rows. Pinned to FP_MAX_LANDMARKS so a festpack cap bump fails
 * the build here rather than silently truncating the view's array. */
_Static_assert(FF_APP_RALLY_MAX_PLACES >= FP_MAX_LANDMARKS,
               "FF_APP_RALLY_MAX_PLACES must hold every festpack landmark");

typedef struct {
    /* --- injected config --------------------------------------------- */
    ff_clock_t const *clock;
    ff_store_t const *store;
    void (*haptic)(void *user);
    void *haptic_user;
    /* S21 §3 — device touch-calibration hook. NULL on targets with no touch
     * panel (the sim): FF_INTENT_CALIBRATE_TOUCH is then a no-op. On device
     * app_main supplies a fn that runs the crosshair capture and applies the
     * solved transform to the live touch path, returning it (and true) so the
     * shell persists it into ff_settings. */
    bool (*calibrate_touch)(void *user, ff_touchcal_t *out_cal);
    void *calibrate_touch_user;
    fp_pack_t *pack; /* caller-owned storage; NULL = this target has no pack */
    bool pack_loaded;

    /* --- mesh link --------------------------------------------------- */
    mc_client_t mc;
    bool attached; /* a transport was supplied and mc_connect has run */
    uint32_t my_node_id;
    bool has_my_node_id;
    ff_shell_link_t link;
    bool ever_connected; /* the link has been READY at least once */

    /* --- core domain state ------------------------------------------- */
    ff_crew_t crew;
    ff_heard_t heard;
    ff_feed_t feed;
    ff_flare_t flare;
    ff_settings_t settings;
    ff_wall_state_t wall;
    ff_radar_smooth_t smooth;
    ff_route_t route;

    /* The Signals send TARGET (S22 slice b) — the ONLY persistent Signals
     * state, held on the RADAR precedent: `ff_radar_compute` builds straight
     * into `sh->view.radar` and the shell keeps only a tiny `ff_radar_smooth_t`
     * beside it, never a second full view-model. So here the shell keeps just
     * the target (WHOLE_CREW or one member node, ~8 B) and `shell_project_signals`
     * builds the rows straight into `sh->view.signals` each tick, re-applying
     * this holder afterward (the whole view is memset each tick, so the target
     * cannot live in the view itself). Re-applying through
     * `ff_sigview_target_select` RE-VALIDATES the member against the current
     * roster every tick, so a member who unpaired/left honestly falls back to
     * WHOLE_CREW. SELECT/CLEAR intents mutate this holder. */
    ff_target_kind_t sig_target_kind;
    uint32_t         sig_target_node;

    /* S24 slice b — the Signals SUB-VIEW state (inbox / picker / thread;
     * popup + rally arrive in slices c/d): which sub-screen the Signals
     * face shows, plus the open thread's conversation key (0 = CREW —
     * the ff_inbox convention). Persistent for the same reason the
     * target holder above is (the view is memset per tick); mutated only
     * by the INBOX_* / BACK intent handlers and re-validated against the
     * roster each projection (a thread of a member who unpaired falls
     * back to the inbox honestly). Leaving the Signals face resets to
     * INBOX — a fresh entry always lands on the inbox. */
    ff_sig_subview_t sig_subview;
    uint32_t         sig_thread_node;

    /* S22 slice d — the RALLY-to-WHOLE_CREW confirm state machine (AC4:
     * "the one loud broadcast requires a confirm"). A first RALLY tap while
     * the target is WHOLE_CREW ARMS the confirm (`sig_rally_armed`, stamped
     * `sig_rally_armed_ms`) and renders the button armed instead of
     * sending; a second RALLY tap within FF_SIG_RALLY_CONFIRM_MS sends and
     * disarms; a timeout or ANY intervening Signals action (pulse, compose,
     * select, clear) disarms. Only WHOLE_CREW arms — a rally to one member,
     * and every pulse/compose, sends on the first tap. This lives in the
     * shell, not core, because it is time-based (needs the injected clock);
     * the shell reflects `sig_rally_armed` into the view's display flag
     * (`ff_sigview_set_rally_confirm_armed`) each tick. */
    bool     sig_rally_armed;
    uint32_t sig_rally_armed_ms;

    /* S24 slice d — the Rally sub-view's persistent WHERE/WHEN selection
     * (the view is memset each tick, so like sig_subview these live in the
     * shell). `sig_rally_sel`: 0 = On Me, 1..N = the (sel-1)-th displayable
     * landmark. Reset to a selectable default when the Rally screen opens
     * (FF_INTENT_INBOX_POPUP_RALLY). */
    uint8_t         sig_rally_sel;
    ff_rally_when_t sig_rally_when;

    /* The composer's destination, resolved when FF_INTENT_OPEN_COMPOSE
     * pushed the modal (S16 slice c1). 0 = broadcast ("TO: EVERYONE").
     * Cleared when BACK pops the composer. */
    uint32_t compose_to_node;

    /* The composer's DRAFT (S16 slice c3) — moved out of scr_compose.c's
     * `static ff_t9_t s_t9`, which reset on every build and made AC3/AC10
     * unimplementable (a draft could never survive anything, including a
     * takeover interruption, because it lived one layer below the object
     * that could observe one). T9_KEY/T9_SPACE/T9_BACKSPACE/T9_INSERT all
     * mutate this; T9_MODE cycles `compose_mode`. Reset (fresh session) on
     * a successful OPEN_COMPOSE, same "every build starts empty" behavior
     * the old static had — but unlike the static, this one keeps its
     * contents across a takeover, since nothing but the T9/SEND intents
     * (all gated on `takeover_up`, routing rule 4) ever touches it. */
    ff_t9_t compose_draft;
    ff_app_compose_mode_t compose_mode;

    /* S08 predictive addendum — the in-progress PREDICTED word (the digits
     * typed since the last accept, plus the selected candidate). Lives
     * beside `compose_draft`: committed text stays in the ff_t9 draft, the
     * word being typed lives here, exactly the split ff_t9pred.h's own
     * integration sketch describes. `compose_extra` is the festpack word
     * table the session ranks above its static dictionary — the pointers
     * ALIAS into `sh->pack` (fp_t9words_collect never copies), so the pack
     * must outlive them (it does: it is cfg->pack, caller-owned). A session
     * reset clears the extra binding (ff_t9pred.h), so `set_extra` follows
     * every reset — the THREE reset sites are init/open, SPACE-accept and
     * SEND. */
    ff_t9pred_session_t compose_pred;
    char const *compose_extra[FF_COMPOSE_EXTRA_MAX];
    int compose_extra_n;

    /* S21 removed `settings_page` (#105's pagination view state): the
     * Settings face is now one scrolling list, so there is no page for the
     * shell to own — scrolling is a live LVGL concern in scr_settings.c's
     * list container, not projected shell state. FF_INTENT_CALIBRATE_TOUCH
     * (new) is handled below via the injected calibrate hook. */

    /* Feed pushes + the crew-paired-sender filter are ff_wiring's, not
     * reimplemented here. Holds interior pointers into this struct — see
     * ff_shell.h's "NOT RELOCATABLE" note. */
    ff_wiring_ctx_t wiring;

    /* --- sensors the shell cannot know by itself --------------------- */
    ff_latlon_t my_pos;
    bool my_pos_ok;
    float heading_deg; /* negative = unknown/unreliable (ff_geo_heading_deg's sentinel) */

#if defined(FF_TARGET_SIM)
    /* --dev-trust-all (S16 AC6). SIM ONLY, and deliberately inside the
     * guard rather than an always-present field defaulted false: the
     * spec demands the affordance be COMPILED OUT of device builds, and
     * a field that exists is a field one stray write away from mattering.
     * The shell's footprint differs by one bool between targets; the
     * _Static_assert below bounds both. */
    bool dev_trust_all;
#endif

    /* --- S18 slice b: settle-then-age the cold-boot replay burst (#50) --
     * The want_config replay streams cached positions while the wall latch
     * is still settling. A reading that MOVES the latch cannot be aged from
     * (its own value defines the clock — the D1 lie), so it is buffered here
     * and re-aged against the SETTLED latch on the first tick after the link
     * reaches READY. FIXED, FF_CREW_MAX-bounded, zero-allocation.
     *
     * `burst_latch_base` is the greatest last_heard that has latched during
     * the current burst — the value the latch will settle to. At settle, a
     * buffered entry whose last_heard equals it defined the final latch and
     * stays NEVER (D1 preserved); older ones are honestly ageable against it.
     *
     * `was_ready` makes the re-age pass fire EXACTLY on the not-ready->ready
     * edge — once per handshake, not every ready tick (the spec's "first
     * tick after READY"). */
    struct {
        ff_latlon_t pos;
        uint32_t node_id;
        uint32_t last_heard;
        ff_crew_pos_meta_t meta;
    } replay_buf[FF_CREW_MAX];
    uint8_t replay_count;
    uint32_t replay_overflow;  /* buffer-overflow drops — bench-visible (AC4), never silent */
    uint32_t burst_latch_base; /* settling latch's base last_heard, unix seconds */
    bool was_ready;            /* READY-edge detection for the once-per-handshake settle pass */

    /* --- tick bookkeeping -------------------------------------------- */
    uint32_t now_ms;         /* the last ff_shell_tick's clock reading */
    ff_app_face_t prev_face; /* to detect "Signals became visible" (S08 AC3) */

    /* True for the duration of one inbound FLARE dispatch. Suppresses
     * the feed-push haptic so a single flare produces exactly ONE buzz —
     * the alert one, which overrides quiet hours — rather than the feed
     * one as well. See shell_haptic_feed. */
    bool in_flare_dispatch;

    /* --- projection --------------------------------------------------- */
    ff_app_state_t view;
    ff_app_state_t prev_key; /* the PREVIOUS frame's render key, see shell_render_key */
    bool has_prev_key;
} shell_t;

_Static_assert(sizeof(shell_t) <= FF_SHELL_BYTES,
               "ff_shell_t exceeds FF_SHELL_BYTES — raise the budget deliberately "
               "(and say why in the PR), or stop growing the shell");
_Static_assert(_Alignof(shell_t) <= FF_SHELL_ALIGN,
               "ff_shell_t needs stricter alignment than FF_SHELL_ALIGN provides");
_Static_assert(sizeof(ff_shell_t) >= sizeof(shell_t), "opaque storage smaller than the object it holds");

static shell_t *shell_of(ff_shell_t *sh)
{
    return (shell_t *)(void *)sh;
}

static shell_t const *shell_of_const(ff_shell_t const *sh)
{
    return (shell_t const *)(void const *)sh;
}

/* ---------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------- */

static uint32_t shell_now(shell_t const *sh)
{
    if (sh->clock != NULL && sh->clock->now_ms != NULL) {
        return sh->clock->now_ms(sh->clock->user);
    }
    return sh->now_ms;
}

static void shell_copy_str(char *dst, size_t n, char const *src)
{
    if (n == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/**
 * Translate `p`'s provenance/precision into core's own small vocabulary
 * (`ff_crew_pos_meta_t`) — issue #33's "core never sees Meshtastic enums"
 * boundary rule, applied at exactly one place shared by both position
 * ingest paths (shell_ev_node's want_config replay and shell_ev_position's
 * live packets) so the translation can't drift between them.
 *
 * `asserted` is true iff the wire value was exactly MC_LOC_MANUAL —
 * MC_LOC_UNKNOWN (the sender didn't say) is explicitly NOT asserted, same
 * as it is explicitly not a measurement either (mc_client.h's
 * MC_LOC_UNKNOWN doc comment): "didn't say" carries no information in
 * either direction.
 *
 * `has_precision_bits`/`precision_bits` are copied verbatim — this is a
 * direct field mirror, not a decision, so there is nothing to interpret
 * here; issue #47's asymmetry (replay-absent vs. live-absent) is already
 * fully resolved by the time `p` reaches this function (mc_client.c's
 * `mc_position_from_pb`, called identically on both paths — the replay
 * path is absent here because stock firmware's NodeInfo replay genuinely
 * never carries the field, not because this boundary drops it).
 */
static ff_crew_pos_meta_t shell_pos_meta(mc_position_t const *p)
{
    ff_crew_pos_meta_t meta;
    meta.asserted = (p->loc_source == MC_LOC_MANUAL);
    meta.has_precision_bits = p->has_precision_bits;
    meta.precision_bits = p->has_precision_bits ? (uint8_t)p->precision_bits : 0u;
    return meta;
}

/**
 * A MUTABLE handle to an EXISTING roster member, or NULL.
 *
 * Never creates. `ff_crew_upsert` is reached only after `ff_crew_find`
 * has already proven the slot exists, so its find-or-create contract
 * degenerates to pure find — the roster cannot grow through here. This
 * wrapper exists so that every inbound path in this file gets its
 * mutable member pointer through one audited place rather than each
 * reaching for `ff_crew_upsert` and hoping (S16's roster rule is about
 * EFFECT, not about which function is named — see ff_shell.h).
 */
static ff_crew_member_t *shell_member(shell_t *sh, uint32_t node_id)
{
    if (ff_crew_find(&sh->crew, node_id) == NULL) return NULL;
    return ff_crew_upsert(&sh->crew, node_id);
}

static bool shell_is_paired(shell_t const *sh, uint32_t node_id)
{
    ff_crew_member_t const *m = ff_crew_find(&sh->crew, node_id);
    return m != NULL && m->paired;
}

static bool shell_is_self(shell_t const *sh, uint32_t node_id)
{
    return sh->has_my_node_id && node_id == sh->my_node_id;
}

/**
 * shell_wall_trust_for — S18 slice a, AC2: classify a source's trust tier
 * for `ff_wall_observe`. A paired crew member (via `ff_crew_find` inside
 * `shell_is_paired` — never creates) or our own node id is TRUSTED;
 * everything else (unknown, merely-heard, or explicitly unpaired) is
 * BOOTSTRAP. Deliberately uses `shell_is_self`, NOT `shell_drop_as_self`:
 * whether a packet is treated as our own echo (and suspended under
 * `--dev-trust-all`) is an orthogonal question from whether this node's
 * clock is genuinely ours — the trust classification must not change
 * under the sim-only bench flag.
 */
static ff_wall_trust_t shell_wall_trust_for(shell_t const *sh, uint32_t node_id)
{
    if (shell_is_self(sh, node_id)) return FF_WALL_TRUST_TRUSTED;
    if (shell_is_paired(sh, node_id)) return FF_WALL_TRUST_TRUSTED;
    return FF_WALL_TRUST_BOOTSTRAP;
}

/**
 * Should this inbound event be dropped as our own echo?
 *
 * Normally identical to shell_is_self. Under `--dev-trust-all` (sim
 * only) the self filter is suspended: the dockerized dev meshtasticd is
 * a single node whose id is ALSO what on_my_info reports as ours, so
 * with the filter up the harness's every packet would be "our own
 * traffic" and the dev loop could exercise nothing. The harness's one
 * node plays every role — see ff_shell.h's dev-affordances section.
 */
static bool shell_drop_as_self(shell_t const *sh, uint32_t node_id)
{
#if defined(FF_TARGET_SIM)
    if (sh->dev_trust_all) return false;
#endif
    return shell_is_self(sh, node_id);
}

/**
 * The one internal path that may grow the roster — ff_shell_pair's body,
 * shared with the sim-only auto-pair branch in shell_ev_node so both go
 * through a single audited place.
 */
static bool shell_pair(shell_t *sh, uint32_t node_id, bool paired)
{
    bool const existed = ff_crew_find(&sh->crew, node_id) != NULL;
    ff_crew_member_t *m = ff_crew_upsert(&sh->crew, node_id);
    if (m == NULL) return false; /* roster full, no eviction in v1 */

    /* App-assigned identity color (ff_crew.h: `color_idx` is "app-assigned"
     * — core creates the slot memset to 0 and never fills it). Assigned ONCE,
     * when a member first enters the roster, to its roster slot index: stable
     * because v1 never evicts (crew_find_or_create), so every paired friend
     * keeps one distinct crew-palette color across radar / signals / map. The
     * shell is the ONLY roster-growth path (every inbound radio path is gated
     * behind a read-only ff_crew_find), so this one site colors every member.
     * ff_theme_crew_color wraps mod-8, so a 9th member reuses a color rather
     * than reading out of range. Without this, m->color_idx stayed 0 for all
     * members and every 1:1 (and every radar dot) rendered palette[0]. */
    if (!existed) {
        uint8_t const slot = (sh->crew.count > 0u) ? (uint8_t)(sh->crew.count - 1u) : 0u;
        m->color_idx = slot;
    }
    ff_crew_set_paired(&sh->crew, node_id, paired);
    return true;
}

/* ---------------------------------------------------------------------
 * Wall clock — a thin composition over core/ff_wall.c (slice b0)
 * ------------------------------------------------------------------- */

/**
 * The wall clock at monotonic time `now_ms`. `ff_shell_wall()` is this
 * and nothing else: the latch, the plausibility gate and the
 * festival-day mapping all live in core, and all this adds is the
 * offset-source resolution the shell is the only thing positioned to do
 * (it is the one object that can see both a pack and the settings).
 *
 * `now_ms` is a parameter rather than a read of `sh->now_ms` so the
 * projection and the quiet-hours gate can each ask for the instant they
 * mean. The projection passes its own tick time, so every field of one
 * frame describes one instant; the quiet-hours gate — which runs inside
 * an inbound callback, between ticks — passes the live clock, because
 * "is it quiet right now" must not be answered from a tick that happened
 * before the window opened.
 */
static ff_wall_t shell_wall(shell_t const *sh, uint32_t now_ms)
{
    ff_wall_offset_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* pack_loaded is carried explicitly rather than inferred from a
     * non-NULL pointer: fp_parse zeroes *out on failure, and a zeroed
     * fp_pack_t reads as a deliberately STATED offset of UTC, which would
     * outrank the user's configured one (ff_wall.h). */
    cfg.pack_loaded = sh->pack_loaded && sh->pack != NULL;
    if (cfg.pack_loaded) {
        cfg.pack_offset_min = sh->pack->utc_offset_min;
        cfg.pack_offset_assumed = sh->pack->utc_offset_assumed;
    }
    cfg.settings_offset_set = sh->settings.utc_offset_set;
    cfg.settings_offset_min = sh->settings.utc_offset_min;

    return ff_wall_now(&sh->wall, now_ms, &cfg);
}

/* ---------------------------------------------------------------------
 * Quiet hours + haptics
 * ------------------------------------------------------------------- */

/**
 * True only when the puck KNOWS it is inside quiet hours.
 *
 * `ff_wall.h` forbids evaluating `ff_quiet_now` while the wall clock is
 * FF_WALL_UNKNOWN — there is no honest `now_min` to hand it, and it would
 * silently accept a fabricated one. So an unknown clock does not suppress
 * anything. Under-suppressing is the safe direction: a surprise buzz is
 * recoverable, a swallowed alert is not.
 */
static bool shell_quiet_now(shell_t const *sh)
{
    ff_wall_t const w = shell_wall(sh, shell_now(sh));
    if (w.src == FF_WALL_UNKNOWN) return false;
    return ff_quiet_now(&sh->settings, w.now_min);
}

static void shell_haptic_fire(shell_t *sh)
{
    if (sh->haptic != NULL) sh->haptic(sh->haptic_user);
}

/**
 * The feed-push haptic — QUIET-HOURS GATED (S16 AC11, second half).
 *
 * Wired into `ff_wiring_ctx_t.haptic_cb`, so it fires once per item that
 * actually reaches the feed (a paired sender), which is exactly what
 * ff_wiring already guarantees.
 *
 * `in_flare_dispatch` suppression: an inbound FLARE produces both a feed
 * item and a `should_alert`. Letting both buzz would double-buzz one
 * event outside quiet hours, and — worse for reasoning about it — make
 * "did the flare alert fire?" untestable by buzz count. The alert is the
 * one that must survive quiet hours, so it is the one that keeps the
 * buzz.
 */
static void shell_haptic_feed(void *user)
{
    shell_t *sh = (shell_t *)user;
    if (sh == NULL || sh->in_flare_dispatch) return;
    if (!sh->settings.haptics) return;
    if (shell_quiet_now(sh)) return;
    shell_haptic_fire(sh);
}

/**
 * The flare alert haptic — NEVER quiet-hours gated (S16 AC11, first
 * half). `ff_flare_result_t.should_alert` explicitly overrides quiet
 * hours; re-running it through `ff_quiet_now` here silences the one
 * alert that must always land.
 *
 * `settings.haptics` — the user's master switch — still applies. That is
 * a different statement from quiet hours: "override the 4 a.m. window"
 * is not "override a user who turned buzzing off". Interpretation noted
 * in the PR body.
 */
static void shell_haptic_alert(shell_t *sh)
{
    if (!sh->settings.haptics) return;
    shell_haptic_fire(sh);
}

/* ---------------------------------------------------------------------
 * Wall-clock observation
 * ------------------------------------------------------------------- */

/**
 * Offer a NodeInfo's `last_heard` to the wall latch.
 *
 * **Returns true iff this reading is what the latch is now made of** —
 * it bootstrapped the latch or re-latched it. The caller must then not
 * age anything from the same reading; see shell_ev_node.
 *
 * Bootstrap source (ff_wall.h: populated unconditionally, including
 * during the want_config replay, so the offset latches during the
 * handshake) — but **forward-only once latched**, and that qualifier is
 * load-bearing rather than defensive.
 *
 * `last_heard` is "when the nodeDB last heard this node", which is by
 * construction <= now. On the want_config replay a cached node's
 * `last_heard` can be many minutes old. Offered unconditionally, that
 * reading disagrees with the latch by more than
 * FF_WALL_RELATCH_DELTA_S and RE-LATCHES the wall clock backwards by the
 * node's staleness. Every position age then computes from a clock that
 * has been dragged back to the moment of the replay — which reads as
 * "just now", which is precisely the reconnect defect S16 exists to
 * close, rebuilt one layer up (AC9 fails on exactly this).
 *
 * So a reading EARLIER than the latch predicts is evidence about the
 * node, not about our clock, and is ignored. A reading LATER than the
 * latch predicts cannot be explained by staleness at all — only by our
 * latch being wrong (the comms brain's clock stepping when GPS locks,
 * which is the case FF_WALL_RELATCH_DELTA_S exists for) — so that one is
 * offered and re-latches.
 *
 * The residual limit, stated: a clock that steps BACKWARDS is not
 * re-latched from NodeInfo. `on_position`'s `rx_time` is a genuine
 * per-packet local receive time rather than a cached summary, so it is
 * offered unconditionally and re-latches in both directions.
 *
 * **UNCONDITIONAL of trust (S18 slice a):** this rule is the #46
 * forward-only NodeInfo rule and it stays exactly as it was — a backward
 * NodeInfo reading is discarded here, before `ff_wall_observe` even runs,
 * regardless of who sent it. Trust does not change it. A paired member's
 * genuine backward correction flows through the live `on_position` path
 * (`shell_ev_position`), not this one. What IS new here is the tier
 * `ff_wall_observe` receives for the forward readings this function does
 * offer: `shell_wall_trust_for` classifies `node_id` (self or paired ->
 * TRUSTED, else BOOTSTRAP) exactly like every other call site.
 */
static bool shell_observe_wall_nodeinfo(shell_t *sh, uint32_t node_id, uint32_t last_heard, uint32_t now_ms)
{
    int64_t predicted = 0;
    if (ff_wall_unix_now(&sh->wall, now_ms, &predicted) && (int64_t)last_heard <= predicted) {
        return false; /* told us nothing new about the clock */
    }
    ff_wall_trust_t const tier = shell_wall_trust_for(sh, node_id);
    ff_wall_obs_t const obs = ff_wall_observe(&sh->wall, (int64_t)last_heard, now_ms, tier);
    return obs == FF_WALL_OBS_LATCHED || obs == FF_WALL_OBS_RELATCHED;
}

/**
 * Turn a unix receive time into the monotonic timestamp
 * `ff_crew_on_position` wants, or fail.
 *
 * Fails — and the caller then does NOT record the position at all — when
 * the age cannot be established honestly:
 *  - `unix_s == 0`, mc_client.h's "unknown" for `last_heard`;
 *  - outside the plausibility window (an uncorrected RTC below the floor,
 *    a corrupt or hostile clock above the ceiling);
 *  - nothing has latched, so unix seconds cannot be related to the
 *    monotonic clock at all;
 *  - the timestamp claims the future relative to our own derived now;
 *  - the fix is older than FF_WALL_LATCH_MAX_AGE_MS, past which the
 *    monotonic delta stops being unambiguous.
 *
 * The last case under-claims: a nine-day-old cached fix reads
 * FF_FRESH_NEVER rather than FF_FRESH_LOST. Both mean "do not trust this
 * position"; NEVER additionally declines to claim a fix we cannot place
 * in time, which is the direction this project errs in everywhere else
 * (FF_FRESH_NEVER, stage_color_valid, has_rssi).
 */
static bool shell_rx_ms_from_unix(shell_t const *sh, uint32_t unix_s, uint32_t now_ms, uint32_t *out_rx_ms)
{
    if (unix_s == 0u) return false;

    int64_t const u = (int64_t)unix_s;
    if (u < FF_WALL_EPOCH_FLOOR || u >= FF_WALL_EPOCH_CEILING) return false;

    int64_t now_unix = 0;
    if (!ff_wall_unix_now(&sh->wall, now_ms, &now_unix)) return false;

    int64_t const age_s = now_unix - u;
    if (age_s < 0) return false;
    if (age_s > (int64_t)(FF_WALL_LATCH_MAX_AGE_MS / 1000u)) return false;

    *out_rx_ms = now_ms - (uint32_t)(age_s * 1000);
    return true;
}

/* ---------------------------------------------------------------------
 * S18 slice b — the deferred cold-boot replay buffer (#50)
 * ------------------------------------------------------------------- */

/**
 * Stash one replayed position for a settle-time re-age.
 *
 * FIXED FF_CREW_MAX-bounded array, no allocation (AC4). On overflow the
 * OLDEST entry is dropped to make room and `replay_overflow` is bumped so
 * the loss is bench-visible (surfaced in the ctl `state` dump alongside
 * `rejected_relatches`) — a silently-dropped fix would be a freshness lie
 * by omission, which honest-data forbids. Only ever fed roster members
 * (the call site is inside shell_ev_node's roster-member block), so a
 * stranger flood cannot evict a crew entry; overflow is reachable only by
 * a single roster the burst re-sends more than FF_CREW_MAX times.
 */
static void shell_replay_buffer(shell_t *sh, uint32_t node_id, ff_latlon_t pos, uint32_t last_heard,
                                 ff_crew_pos_meta_t meta)
{
    if (sh->replay_count >= FF_CREW_MAX) {
        memmove(&sh->replay_buf[0], &sh->replay_buf[1], (size_t)(FF_CREW_MAX - 1) * sizeof(sh->replay_buf[0]));
        sh->replay_count = (uint8_t)(FF_CREW_MAX - 1);
        sh->replay_overflow++;
    }
    sh->replay_buf[sh->replay_count].pos = pos;
    sh->replay_buf[sh->replay_count].node_id = node_id;
    sh->replay_buf[sh->replay_count].last_heard = last_heard;
    sh->replay_buf[sh->replay_count].meta = meta;
    sh->replay_count++;
}

/**
 * Re-age every buffered replay position against the now-settled latch.
 *
 * Run EXACTLY once per handshake, on the not-ready->ready edge (see
 * ff_shell_tick). This recovers the precision the running-maximum aging
 * threw away: an ascending-`last_heard` burst buffered every reading (each
 * moved the latch in turn), and here each is finally aged against the
 * burst's settled clock — so ascending and descending replay of the same
 * node set land on the SAME freshness (#50, AC1).
 *
 * Honest-data is preserved in three places, none of which may over-claim:
 *  - An entry whose last_heard defined the settled latch (== burst_latch_base)
 *    is skipped and stays NEVER — we learned the time FROM it, so its own
 *    fix cannot be placed in time (the D1 rule, applied to the settled
 *    latch instead of the mid-burst one).
 *  - An entry the roster no longer holds (unpaired/evicted since it was
 *    buffered) is dropped.
 *  - An entry still older than the window (shell_rx_ms_from_unix fails —
 *    e.g. genuinely older than FF_WALL_LATCH_MAX_AGE_MS) stays NEVER (AC3).
 */
static void shell_settle_replay(shell_t *sh, uint32_t now_ms)
{
    for (uint8_t i = 0; i < sh->replay_count; i++) {
        uint32_t const node_id = sh->replay_buf[i].node_id;
        uint32_t const last_heard = sh->replay_buf[i].last_heard;

        if (last_heard >= sh->burst_latch_base) continue;        /* defined the settled latch (D1) -> NEVER */
        if (ff_crew_find(&sh->crew, node_id) == NULL) continue;  /* roster no longer holds it */

        uint32_t rx_ms = 0;
        if (shell_rx_ms_from_unix(sh, last_heard, now_ms, &rx_ms)) {
            ff_crew_on_position(&sh->crew, node_id, sh->replay_buf[i].pos, rx_ms, sh->replay_buf[i].meta);
        }
        /* else: older than the window -> stays NEVER (AC3). */
    }
    sh->replay_count = 0;
    sh->burst_latch_base = 0;
}

/* ---------------------------------------------------------------------
 * mc_events_t callbacks — all seven
 * ------------------------------------------------------------------- */

static void shell_ev_state(void *u, mc_state_t s)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL) return;

    switch (s) {
    case MC_STATE_READY:
        sh->ever_connected = true;
        sh->link = FF_SHELL_LINK_CONNECTED;
        break;
    case MC_STATE_HANDSHAKE:
        sh->link = FF_SHELL_LINK_RECONNECTING;
        break;
    case MC_STATE_DISCONNECTED:
    default:
        /* mc_client schedules a retry on every failure and never gives
         * up, so once the link has been up, "disconnected" IS
         * "reconnecting" — see ff_shell.h's ff_shell_link_t comment. */
        sh->link = sh->ever_connected ? FF_SHELL_LINK_RECONNECTING : FF_SHELL_LINK_NONE;
        break;
    }
}

static void shell_ev_my_info(void *u, uint32_t my_node_id)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL) return;
    sh->my_node_id = my_node_id;
    sh->has_my_node_id = true;

    /* S24 AC1 — the wiring needs to know who "me" is to classify an
     * inbound text addressed to a specific node as DIRECT vs UNKNOWN. */
    ff_wiring_set_self_node(&sh->wiring, my_node_id);

    /* PR #46 review caveat, closed here (slice b2): if our own NodeInfo
     * arrived BEFORE this callback named us — whichever order the radio
     * picked — shell_ev_node could not recognise it as ours and noted
     * our own id in ff_heard, where it would linger until LRU eviction
     * and be offered in S12's "add from heard nodes" list. The reviewer
     * suggested reading my_node_id from mc_client_t instead of this
     * callback, but that closes nothing: mc_client.c's `my_info_tag`
     * case sets its own copy and fires this callback in adjacent
     * statements, so the two records can never disagree — the residual window is
     * "own NodeInfo earlier in the STREAM than MyNodeInfo", which
     * defeats both reads equally. The honest closure is to purge the
     * entry the moment we learn who we are, which covers every ordering.
     * Pinned by S16_b2_my_info_purges_our_own_id_from_heard. */
    (void)ff_heard_remove(&sh->heard, my_node_id);
}

static void shell_ev_node(void *u, mc_nodeinfo_t const *n)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL || n == NULL) return;

    uint32_t const now = shell_now(sh);

    /* Before the self-check: our own NodeInfo carries the freshest
     * `last_heard` of any node in the dump, and it is the one the
     * bootstrap most wants. */
    bool const defined_the_latch = shell_observe_wall_nodeinfo(sh, n->node_num, n->last_heard, now);

    /* S18 slice b (#50): while the replay burst is still settling the latch
     * (link not yet READY), remember the greatest last_heard that has
     * latched — the value the latch will settle to. Tracked BEFORE the
     * self-drop so a positionless or self NodeInfo that pins the clock
     * higher than any buffered position still raises the base, keeping the
     * settle pass's D1 exclusion correct. `defined_the_latch` implies this
     * reading is the new forward maximum, so a straight assignment suffices;
     * the > guard is belt-and-braces. */
    if (defined_the_latch && sh->link != FF_SHELL_LINK_CONNECTED && n->last_heard > sh->burst_latch_base) {
        sh->burst_latch_base = n->last_heard;
    }

    if (shell_drop_as_self(sh, n->node_num)) return; /* never treat our own traffic as inbound */

#if defined(FF_TARGET_SIM)
    /* --dev-trust-all (S16 AC6): auto-pair on NodeInfo, and on NodeInfo
     * ONLY — a bare Position must still not grow the roster, even on the
     * dev bench (pairing on the most untrusted packet on the mesh is the
     * exact defect S16 exists to close; the dev affordance does not get
     * to reintroduce it). Routed through shell_pair, the same single
     * audited growth path ff_shell_pair uses. Compiled out of device
     * builds entirely; see ff_shell.h's dev-affordances section. */
    if (sh->dev_trust_all) {
        (void)shell_pair(sh, n->node_num, true); /* roster full -> falls through to heard, below */
    }
#endif

    /* ROSTER TRUST POLICY. Read-only first; a miss is noted in the
     * bounded heard list and dropped. Inbound radio traffic never grows
     * the roster — not even a NodeInfo, which is the friendliest-looking
     * packet on the mesh and still arrives unauthenticated. */
    ff_crew_member_t *m = shell_member(sh, n->node_num);
    if (m == NULL) {
        ff_heard_note(&sh->heard, n->node_num, now);
        return;
    }

    char const *name = n->has_short_name ? n->short_name : (n->has_long_name ? n->long_name : "");
    if (name[0] != '\0') {
        shell_copy_str(m->name, sizeof(m->name), name);
        m->initial = name[0];
    }
    if (n->has_battery_level) {
        m->battery_pct = (int8_t)(n->battery_level > 100u ? 100u : n->battery_level);
    }

    if (n->has_position) {
        /* THE RECONNECT RULE (AC9). This is the want_config replay's
         * shape: a cached position arriving now. `mc_client.c:222`
         * hardcodes has_rx_time = false on this path — rx_time is a
         * MeshPacket field and a NodeInfo is not a MeshPacket — so the
         * honest age source is `last_heard`, and there is no fallback to
         * the local clock. If the age cannot be established, the
         * position is not recorded at all and freshness stays
         * FF_FRESH_NEVER.
         *
         * AND: NOT FROM THE READING THAT JUST DEFINED THE CLOCK.
         * `defined_the_latch` is the whole of PR #46 review finding D1.
         * If this NodeInfo bootstrapped or moved the latch, then
         * ff_wall_unix_now() now returns exactly this node's
         * `last_heard`, so its position's derived age is zero BY
         * CONSTRUCTION rather than by measurement — a six-hour-old
         * cached fix reads LIVE on every cold boot, and, ordering aside,
         * whichever node carries the greatest `last_heard` in the burst
         * always does. That is defect 2 of S16 a third time, arriving
         * through the latch instead of through the local clock.
         *
         * We have just learned what time it is FROM this node; we cannot
         * also use it to say how old this node is. Not recorded ->
         * FF_FRESH_NEVER, which the radar renders as "NO FIX YET" rather
         * than a fabricated "LAST SEEN" (ff_radar.h's renderer
         * contract). A later node in the same burst with an OLDER
         * `last_heard` does not move the latch, so it IS aged — against
         * the running maximum, which is the best estimate of "now" the
         * puck has.
         *
         * That made the OUTCOME ordering-dependent: a descending burst
         * left only its freshest node unplaceable, an ascending one left
         * everything NEVER. Honest either way — never fresher than reality
         * — but the ascending case was needlessly pessimistic. S18 slice b
         * (#50) recovers that precision by DEFERRING the aging of any
         * reading that moved the still-settling latch: instead of dropping
         * it, buffer it and re-age it against the burst's SETTLED clock on
         * the first tick after READY (shell_settle_replay). A reading that
         * did NOT move the latch is still aged immediately, against the best
         * current estimate of "now" — unchanged, and for a purely ascending
         * or descending burst that estimate already equals the settled
         * clock, so this changes only the mid-burst-moving-latch entries.
         *
         * `n->position.has_rx_time` is deliberately not consulted: it is
         * hardcoded false on this path today, so a branch on it would be
         * dead code that reads as if it were handling a case. If
         * meshclient ever populates it here, it becomes the better
         * source and this is where to prefer it. */
        ff_latlon_t const p = {n->position.lat, n->position.lon};
        uint32_t rx_ms = 0;
        if (!defined_the_latch && shell_rx_ms_from_unix(sh, n->last_heard, now, &rx_ms)) {
            ff_crew_on_position(&sh->crew, n->node_num, p, rx_ms, shell_pos_meta(&n->position));
        } else if (defined_the_latch && sh->link != FF_SHELL_LINK_CONNECTED) {
            /* This reading moved a still-settling latch, so aging it now
             * would age it against a clock its own value defines (D1). Defer
             * to the settle pass (#50). In steady state (link CONNECTED) the
             * latch has already settled, so a forward-moving NodeInfo there
             * is a genuine GPS-step re-latch whose own fix still cannot be
             * aged (D1) — left NEVER exactly as before, not buffered. */
            shell_replay_buffer(sh, n->node_num, p, n->last_heard, shell_pos_meta(&n->position));
        }
    }

    /* n->rx_path is a nodeDB SUMMARY with no timestamp (mc_client.h), so
     * it is deliberately not used to attribute RSSI. That question is
     * per-packet and is answered in shell_ev_rx_meta. */
}

static void shell_ev_position(void *u, uint32_t node, mc_position_t const *p)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL || p == NULL) return;

    uint32_t const now = shell_now(sh);

    /* WALL OBSERVATION BEFORE THE SELF-DROP (S18 slice a, AC3/AC4 — the
     * spec's "wiring the TRUSTED sources" note). Self's own position is
     * still dropped for crew/feed purposes below — ff_crew_find,
     * ff_heard_note and ff_crew_on_position all stay gated behind
     * shell_drop_as_self, unchanged — but its GPS-disciplined receive
     * time is exactly the gold-anchor TRUSTED source the trust model
     * names ("the local comms-brain's own GPS-disciplined receive clock
     * — that node *is* you"), and before this reorder it could never
     * reach ff_wall_observe at all: the old `if (shell_drop_as_self...)
     * return;` sat above this block and returned first. shell_ev_node
     * already observed the wall before its own self-check (D1's own
     * comment) — this brings shell_ev_position into that same shape
     * rather than leaving the two inconsistent.
     *
     * A live per-packet local receive time is the authoritative re-latch
     * source, offered unconditionally in both directions (unlike a
     * NodeInfo's cached `last_heard` — see shell_observe_wall_nodeinfo).
     * The plausibility window in ff_wall_observe is the guard, and
     * shell_wall_trust_for classifies the tier: self or a paired member
     * is TRUSTED and can move a disagreeing fresh latch (#49's fix);
     * anyone else is BOOTSTRAP and cannot.
     *
     * Note this path deliberately does NOT carry shell_ev_node's
     * "don't age from the reading that defined the latch" guard (D1).
     * `rx_time` is THIS PACKET's local receive time, so "when did this
     * arrive" and "what time is it now" genuinely coincide and an age of
     * ~0 is a measurement, not a construction. See ff_shell.h for the
     * assumption that rests on. */
    if (p->has_rx_time) {
        ff_wall_trust_t const tier = shell_wall_trust_for(sh, node);
        (void)ff_wall_observe(&sh->wall, (int64_t)p->rx_time, now, tier);
    }

    if (shell_drop_as_self(sh, node)) return; /* never treat our own traffic as inbound crew/feed */

    /* AC5c. `ff_crew_on_position` calls crew_find_or_create internally,
     * so calling it here on an unknown sender would satisfy "we never
     * call ff_crew_upsert" while committing the exact roster-exhaustion
     * bug the rule exists to prevent — on the MORE untrusted trigger, a
     * bare Position with no name and no handshake behind it. Find first;
     * an unknown sender's position is dropped and the sender noted. */
    if (ff_crew_find(&sh->crew, node) == NULL) {
        ff_heard_note(&sh->heard, node, now);
        return;
    }

    /* No rx_time means no honest age — and "it arrived now" is not the
     * answer (S16 defect 2). Drop it; freshness stays whatever it
     * honestly was. */
    if (!p->has_rx_time) return;

    uint32_t rx_ms = 0;
    if (!shell_rx_ms_from_unix(sh, p->rx_time, now, &rx_ms)) return;

    ff_latlon_t const pos = {p->lat, p->lon};
    ff_crew_on_position(&sh->crew, node, pos, rx_ms, shell_pos_meta(p));
}

static void shell_ev_text(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL) return;
    if (shell_drop_as_self(sh, from)) return;
    /* ff_wiring owns the crew-paired filter, the heard-note on a miss,
     * the feed push and the (quiet-gated, via shell_haptic_feed) buzz. */
    ff_wiring_on_text(&sh->wiring, from, to, utf8, len);
}

static void shell_ev_private(void *u, uint32_t from, uint32_t to, uint32_t portnum,
                              uint8_t const *payload, size_t len)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL) return;
    if (portnum != FF_PORTNUM) return; /* not this app's protocol */
    if (shell_drop_as_self(sh, from)) return;

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int const type = ff_proto_decode(payload, len, &msg);

    /* Feed side first, so the Signals item exists before the takeover
     * that references it. For a FLARE the feed haptic is suppressed —
     * one inbound flare, one buzz, and it is the alert one. */
    sh->in_flare_dispatch = (type == FF_PROTO_TYPE_FLARE);
    ff_wiring_on_private(&sh->wiring, from, to, portnum, payload, len);
    sh->in_flare_dispatch = false;

    if (type == FF_PROTO_TYPE_FLARE) {
        /* `paired` is a plain bool by ff_flare.h's design — an unpaired
         * sender's flare is ignored entirely there, so this read-only
         * lookup is also the trust gate. */
        ff_flare_result_t const r = ff_flare_on_flare_rx(&sh->flare, from, shell_is_paired(sh, from),
                                                         msg.body.flare.dur_s, shell_now(sh));
        if (r.should_alert) {
            shell_haptic_alert(sh); /* UNCONDITIONAL w.r.t. quiet hours — AC11 */
        }
    } else if (type == FF_PROTO_TYPE_FLARE_END) {
        (void)ff_flare_on_flare_end_rx(&sh->flare, from);
    }
}

static void shell_ev_rx_meta(void *u, uint32_t from, mc_rx_meta_t const *m)
{
    shell_t *sh = (shell_t *)u;
    if (sh == NULL || m == NULL) return;

    /* mc_client.h warns explicitly: self-packets are NOT filtered by the
     * library, and a caller maintaining a per-peer roster wants to skip
     * its own id rather than create a slot for itself. */
    if (shell_drop_as_self(sh, from)) return;

    uint32_t const now = shell_now(sh);

    /* This fires for EVERY inbound MeshPacket naming a sender, including
     * encrypted ones and portnums out of decode scope — which makes it
     * the most complete "who has this puck heard" signal there is, and
     * exactly what ff_heard is for (bounded, LRU-evictable, expected to
     * churn under real festival RF volume). It still never grows the
     * roster. */
    ff_crew_member_t const *sender = ff_crew_find(&sh->crew, from);
    if (sender == NULL) {
        ff_heard_note(&sh->heard, from, now);
        return;
    }

    /* PAIRED is required, not a nicety — same trust rule ff_wiring.c's
     * wiring_push_if_paired applies to feed pushes (ff_wiring.c:42): a
     * roster slot can exist and be unpaired (known-but-not-trusted, or
     * paired-then-unpaired — ff_shell.h's ff_shell_pair). Feeding RSSI
     * from an unpaired slot would let a merely-heard node's signal
     * strength satisfy ff_crew_close_range's CLOSE predicate for
     * someone the wearer never chose to trust.
     *
     * `ff_crew_on_rssi` find-or-creates too — same effect-not-name rule
     * as ff_crew_on_position above; the find (and the paired check) has
     * already gated it.
     *
     * DIRECT is required, not a nicety either: RSSI is measured against
     * the signal that actually arrived, which for a relayed packet is
     * the RELAY's transmission. Attributing it to `from` reports a loud
     * neighbouring relay as if the distant friend it forwarded for were
     * standing next to you — and ff_crew_close_range turns exactly that
     * number into a CLOSE lock. */
    if (sender->paired && m->rx_path == MC_RX_PATH_DIRECT && m->has_rssi) {
        ff_crew_on_rssi(&sh->crew, from, m->rssi_dbm);
    }

    /* m->has_snr / m->snr_db: nothing in core consumes SNR yet. Left
     * unread rather than stashed somewhere it would go stale. */
}

/* ---------------------------------------------------------------------
 * Projection: core state -> ff_app_state_t
 * ------------------------------------------------------------------- */

static char const *shell_name_of(shell_t const *sh, uint32_t node_id)
{
    ff_crew_member_t const *m = ff_crew_find(&sh->crew, node_id);
    return (m != NULL) ? m->name : "";
}

/** Milliseconds until `expiry_ms`, or -1 when the fact is not live.
 *  Wraparound-safe: the subtraction is unsigned, and a deadline already
 *  passed (which ff_flare_tick clears in the same tick) reads as 0. */
static int32_t shell_remaining_ms(bool live, uint32_t expiry_ms, uint32_t now_ms)
{
    if (!live) return -1;
    uint32_t const d = expiry_ms - now_ms;
    if (d > (uint32_t)0x7FFFFFFF) return 0; /* already past */
    return (int32_t)d;
}

static void shell_project_flare(shell_t const *sh, uint32_t now_ms, ff_app_flare_t *out)
{
    ff_flare_t const *f = &sh->flare;

    out->sending = f->sending;
    out->send_expires_in_ms = shell_remaining_ms(f->sending, f->send_expiry_ms, now_ms);

    out->takeover_active = f->takeover_active;
    out->takeover_expires_in_ms = shell_remaining_ms(f->takeover_active, f->takeover_expiry_ms, now_ms);
    out->takeover_bearing_valid = false;
    out->takeover_bearing_deg = 0.0f;
    out->takeover_dist_str[0] = '\0';
    out->takeover_from_name[0] = '\0';

    if (f->takeover_active) {
        shell_copy_str(out->takeover_from_name, sizeof(out->takeover_from_name),
                        shell_name_of(sh, f->takeover_node_id));

        /* Bearing/distance to the sender are the app layer's own read
         * (ff_flare_t deliberately has no crew/geo dependency). Both stay
         * honestly absent unless BOTH ends have a position — a bare float
         * cannot express "unknown", which is why takeover_bearing_valid
         * exists at all. */
        ff_crew_member_t const *m = ff_crew_find(&sh->crew, f->takeover_node_id);
        if (m != NULL && m->has_pos && sh->my_pos_ok) {
            out->takeover_bearing_valid = true;
            out->takeover_bearing_deg = ff_geo_bearing_deg(sh->my_pos, m->pos);
            ff_fmt_distance(out->takeover_dist_str, sizeof(out->takeover_dist_str),
                             ff_geo_distance_m(sh->my_pos, m->pos), sh->settings.imperial);
        }
    }

    out->locked = (f->locked_node_id != 0u);
    out->locked_expires_in_ms = shell_remaining_ms(out->locked, f->locked_expiry_ms, now_ms);
    out->locked_from_name[0] = '\0';
    if (out->locked) {
        shell_copy_str(out->locked_from_name, sizeof(out->locked_from_name),
                        shell_name_of(sh, f->locked_node_id));
    }
}

/* shell_member_paired — the one roster predicate the Signals projection and
 * intent handlers share: does `node_id` name a currently-PAIRED member? */
static bool shell_member_paired(shell_t const *sh, uint32_t node_id)
{
    ff_crew_member_t const *m = ff_crew_find(&sh->crew, node_id);
    return node_id != 0u && m != NULL && m->paired;
}

/* S24 slice b: the Signals face is the INBOX now, so the projection builds
 * the core conversation model straight into the view (the RADAR / S22(b)
 * precedent — build into the view, tiny persistent state only) and then
 * projects the shell's persistent sub-view + thread scope + S22(d) target
 * holders. Everything is RE-VALIDATED against the current roster each tick:
 * a thread (or a send target) naming a member who unpaired/left falls back
 * honestly rather than lingering. Age/presence FORMATTING lives in the
 * screen (ff_fmt_age — one age-formatter in the tree, the S22-a honesty
 * note); this only carries the raw model. `sh` is non-const (it may
 * downgrade its own holders on a failed re-validation) unlike the other
 * projectors — see its caller. */
/* Defined alongside the Rally send helpers below (it uses them); forward-
 * declared here for shell_project_signals. */
static void shell_project_rally(shell_t *sh, ff_app_rally_t *out);

static void shell_project_signals(shell_t *sh, uint32_t now_ms, ff_app_signals_t *out)
{
    /* Re-validate an open MEMBER scope (node 0 = CREW, always valid). The
     * thread, and the popup / rally screens layered over it, all scope to
     * `sig_thread_node`; a member who unpaired drops the whole stack back
     * to the inbox honestly. */
    bool const scoped_subview = (sh->sig_subview == FF_SIG_SUB_THREAD ||
                                 sh->sig_subview == FF_SIG_SUB_POPUP ||
                                 sh->sig_subview == FF_SIG_SUB_RALLY);
    if (scoped_subview && sh->sig_thread_node != 0u && !shell_member_paired(sh, sh->sig_thread_node)) {
        sh->sig_subview     = FF_SIG_SUB_INBOX;
        sh->sig_thread_node = 0u;
    }

    /* S24 slice (c) — live arrivals into the OPEN, VISIBLE thread are on
     * glass the moment they are projected, so their badge debt clears
     * BEFORE the conversation list is built this tick (the AC4 open-
     * transition mark-read in the INBOX_OPEN_THREAD/PICK handlers covers
     * the backlog; this covers arrivals while the thread is up — the
     * same honesty rule's second half, and doing it first keeps this
     * tick's badges coherent with it). Gated on the thread actually
     * being VISIBLE: under a modal (compose/settings/map) or a flare
     * takeover nothing here is on glass and nothing is marked read. */
    /* THREAD and the POPUP over it both render the thread (the popup dims
     * it behind the action rows), so arrivals into either clear their badge
     * debt; the RALLY screen does not show the thread, so it does not. */
    bool const thread_visible = (sh->sig_subview == FF_SIG_SUB_THREAD || sh->sig_subview == FF_SIG_SUB_POPUP);
    if (thread_visible && sh->view.active_face == FF_APP_FACE_SIGNALS && !sh->flare.takeover_active) {
        (void)ff_inbox_mark_thread_read(&sh->feed,
                                        (sh->sig_thread_node == 0u) ? FF_CONV_CREW : FF_CONV_MEMBER,
                                        sh->sig_thread_node);
    }

    ff_inbox_build(&out->inbox, &sh->feed, &sh->crew, now_ms);

    out->subview     = sh->sig_subview;
    out->thread_node = sh->sig_thread_node;
    /* Scope display name — needed by THREAD (header), POPUP (the "SEND TO
     * <scope>" line) and kept uniform for RALLY (harmless). Looked up at
     * projection time (the compose to_name precedent) so a rename mid-scope
     * is reflected; a paired member whose name never arrived projects "". */
    if (scoped_subview) {
        if (sh->sig_thread_node == 0u) {
            shell_copy_str(out->thread_name, sizeof(out->thread_name), "CREW");
        } else {
            ff_crew_member_t const *m = ff_crew_find(&sh->crew, sh->sig_thread_node);
            shell_copy_str(out->thread_name, sizeof(out->thread_name), (m != NULL) ? m->name : "");
            out->thread_color_idx = (m != NULL) ? m->color_idx : 0u;
        }
    }
    if (thread_visible) {
        /* The open thread's messages (mark-read of live arrivals already
         * ran above, before the inbox build). */
        ff_inbox_thread_build(&out->thread, &sh->feed, &sh->crew,
                              (sh->sig_thread_node == 0u) ? FF_CONV_CREW : FF_CONV_MEMBER,
                              sh->sig_thread_node, now_ms);
    }

    /* S24 slice (d) — the Rally sub-view's projected WHERE/WHEN/Send state.
     * Built only while the Rally screen is up (like the thread above); the
     * `out->rally` struct is otherwise the memset-zeroed default. */
    if (sh->sig_subview == FF_SIG_SUB_RALLY) {
        shell_project_rally(sh, &out->rally);
    }

    /* S22(d) send machinery, kept for slice (d): re-validate the member
     * target against the roster (a gone member falls back to WHOLE_CREW),
     * expire a lapsed RALLY-to-WHOLE_CREW confirm (S22 AC4: a forgotten
     * first tap must not lurk armed forever), and project both. */
    if (sh->sig_target_kind == FF_TARGET_MEMBER && !shell_member_paired(sh, sh->sig_target_node)) {
        sh->sig_target_kind = FF_TARGET_WHOLE_CREW;
        sh->sig_target_node = 0u;
    }
    out->target_kind = sh->sig_target_kind;
    out->target_node = sh->sig_target_node;

    if (sh->sig_rally_armed && (now_ms - sh->sig_rally_armed_ms) > FF_SIG_RALLY_CONFIRM_MS) {
        sh->sig_rally_armed = false;
    }
    out->rally_confirm_armed = sh->sig_rally_armed;
}

/**
 * Stack budget for one day's set list inside shell_project_now.
 *
 * `ff_sched_day_sets` would happily fill FP_MAX_SETS (256) pointers —
 * 2 KB of stack, on a tick path that on device runs inside an ESP-IDF
 * task whose default stack is single-digit KB. 64 pointers is 512 bytes
 * and is generous headroom over any real day (the vendored Lost Lands
 * 2026 pack's busiest day has 7 sets, and FP_MAX_STAGES is 12).
 *
 * The truncation semantics, stated because they are not free: past this
 * many sets on one day, later sets are dropped in pack order and would
 * not reach the unknown-time lineup. That is `ff_sched_day_sets`'s own
 * documented "silently drop past max" contract, and `n_lineup` is capped
 * at FF_APP_NOW_MAX_LINEUP (32) below anyway.
 */
#define SHELL_DAY_SETS_MAX 64

static char const *shell_stage_name(fp_pack_t const *p, int8_t stage_idx)
{
    if (stage_idx < 0 || (uint8_t)stage_idx >= p->n_stages) return "";
    return p->stages[stage_idx].name;
}

static bool shell_stage_color(fp_pack_t const *p, int8_t stage_idx, uint32_t *out_rgb)
{
    if (stage_idx < 0 || (uint8_t)stage_idx >= p->n_stages) return false;
    *out_rgb = p->stages[stage_idx].color_rgb;
    return true;
}

/**
 * The Now face.
 *
 * Issue #48 (S07-now-face.md Amendments, "PR #46 review, D3"), resolved:
 * a pack loaded with the clock still unknown (FF_WALL_UNKNOWN — the
 * NORMAL boot path, since the wall clock only latches once a plausible
 * mesh timestamp arrives during the want_config handshake, and a pack
 * can load before that) now projects NOW_TIME_UNKNOWN, not NOW_NO_PACK.
 * The old fallback named the wrong missing fact — "no pack" when a pack
 * WAS loaded — and scr_now.c rendered it as "NO FESTIVAL LOADED / Load a
 * festpack...", which mis-claims rather than under-claims: it tells the
 * user to redo something they already did. NOW_TBD would have been
 * equally wrong the other way (a claim about the DATA, not the clock).
 * The honest unknown here is the TIME, so it gets its own member —
 * see now_state_t's own doc comment (ff_app_state.h) for the full
 * reasoning and the never-let-absence-carry-meaning framing.
 *
 * NOW_NO_PACK is reserved again for its original, narrower meaning: no
 * festpack loaded at all, regardless of clock state.
 */
static void shell_project_now(shell_t const *sh, ff_wall_t wall, ff_app_now_t *out)
{
    if (!sh->pack_loaded || sh->pack == NULL) {
        out->state = NOW_NO_PACK;
        return;
    }
    if (wall.src == FF_WALL_UNKNOWN) {
        out->state = NOW_TIME_UNKNOWN;
        return;
    }

    fp_pack_t const *p = sh->pack;
    uint16_t const day = wall.day_doy;
    int16_t const now_min = wall.now_min;

    ff_now_row_t rows[FF_APP_NOW_MAX_ROWS];
    uint8_t const n_rows = ff_sched_now_playing(p, day, now_min, rows, (uint8_t)FF_APP_NOW_MAX_ROWS);
    for (uint8_t i = 0; i < n_rows; i++) {
        ff_app_now_row_t *o = &out->rows[i];
        shell_copy_str(o->artist, sizeof(o->artist), rows[i].set->artist);
        shell_copy_str(o->stage_name, sizeof(o->stage_name), shell_stage_name(p, rows[i].set->stage_idx));
        o->stage_color_valid = shell_stage_color(p, rows[i].set->stage_idx, &o->stage_color_rgb);
        o->pct_done = rows[i].pct_done;
        o->pct_valid = rows[i].pct_valid;
    }
    out->n_rows = n_rows;

    ff_next_t next;
    if (ff_sched_next_starred(p, day, now_min, &next)) {
        out->next.valid = true;
        shell_copy_str(out->next.artist, sizeof(out->next.artist), next.set->artist);
        shell_copy_str(out->next.stage_name, sizeof(out->next.stage_name),
                        shell_stage_name(p, next.set->stage_idx));
        out->next.mins_until = next.mins_until;
    }

    /* The still-unknown-time sets. Under NOW_TBD that is every set on the
     * day (by definition of the state); under NOW_MIXED it is the subset
     * that still lacks a time, listed ALONGSIDE rows/next rather than
     * disappearing the moment one set on the day gets a real time — the
     * expected near-term Lost Lands state.
     *
     * 2026-08-24 amendment: "lacks a time" means "lacks a start_min",
     * full stop — end_min plays no part. A set with a known start_min
     * belongs in `rows`/`next` (via ff_sched_now_playing/next_starred,
     * which now derive a null end_min themselves — see ff_sched.h's
     * "Timed means a known start_min" section) or in neither if it has
     * already finished; either way it does NOT belong here just because
     * its end_min happens to be null. Before this amendment the check
     * below required BOTH fields, so a starts-only real festpack (Bass
     * Canyon 2026: 82 published start times, every end_min null) put
     * every one of those 82 sets in this unknown-time lineup — the exact
     * "SET TIMES TBD" lie this amendment fixes. */
    fp_set_t const *day_sets[SHELL_DAY_SETS_MAX];
    uint16_t const n_day = ff_sched_day_sets(p, day, day_sets, (uint16_t)SHELL_DAY_SETS_MAX);
    uint8_t n_lineup = 0;
    for (uint16_t i = 0; i < n_day && n_lineup < FF_APP_NOW_MAX_LINEUP; i++) {
        if (day_sets[i]->start_min >= 0) continue;
        ff_app_lineup_item_t *o = &out->lineup[n_lineup];
        shell_copy_str(o->artist, sizeof(o->artist), day_sets[i]->artist);
        shell_copy_str(o->stage_name, sizeof(o->stage_name), shell_stage_name(p, day_sets[i]->stage_idx));
        n_lineup++;
    }
    out->n_lineup = n_lineup;

    if (ff_sched_day_tbd(p, day)) {
        out->state = NOW_TBD;
    } else if (n_lineup > 0) {
        out->state = NOW_MIXED;
    } else if (n_rows > 0 || out->next.valid) {
        out->state = NOW_LIVE;
    } else {
        out->state = NOW_NOTHING_PLAYING;
    }
}

static void shell_project_settings(shell_t const *sh, ff_app_settings_t *out)
{
    out->imperial = sh->settings.imperial;
    out->share_mode = sh->settings.share_mode;
    out->haptics = sh->settings.haptics;
    out->night_glow = sh->settings.night_glow;
    out->water_min = sh->settings.water_min;
    out->quiet_from_min = sh->settings.quiet_from_min;
    out->quiet_to_min = sh->settings.quiet_to_min;
    /* ff_settings_t.my_name is NOT guaranteed NUL-terminated by that
     * layer (it round-trips raw bytes); bound it here before it becomes a
     * C string anything renders. */
    memcpy(out->my_name, sh->settings.my_name, sizeof(out->my_name) - 1u);
    out->my_name[sizeof(out->my_name) - 1u] = '\0';
    /* S11 slice b: the Settings face's UTC-offset row. Projected verbatim
     * — `utc_offset_set` is the "prove you meant this" flag scr_settings.c
     * must gate the render on, same as every other *_valid field in this
     * projection. */
    out->utc_offset_min = sh->settings.utc_offset_min;
    out->utc_offset_set = sh->settings.utc_offset_set;
    /* S17 slice a: the colorblind toggle, projected verbatim. */
    out->colorblind = sh->settings.colorblind;
    /* #100: brightness percent, projected verbatim (already clamped on write
     * — see shell_setting_set). */
    out->brightness_pct = sh->settings.brightness_pct;
}

/**
 * The Map face (S09).
 *
 * Honest-empty unless a pack is loaded with a KNOWN origin: every
 * projected east_m/north_m below — features' own (already projected by
 * `fp_parse` at load time) and crew/rally/YOU's (projected here, against
 * the SAME origin) — is meaningless without one (`fp_pack_t.origin_known`'s
 * own doc comment: "Meaningless... unless origin_known is true"). A
 * missing pack, or a pack whose venue is unknown, therefore leaves
 * `sh->view.map` at its whole-view memset zero — the same honestly-empty
 * stub S09 AC3's untraced-pack fixture exercises, never an invented
 * (0,0)-origin guess.
 */
static void shell_project_map(shell_t const *sh, ff_app_map_t *out)
{
    if (!sh->pack_loaded || sh->pack == NULL || !sh->pack->origin_known) {
        return;
    }

    fp_pack_t const *p = sh->pack;

    /* PR #73 review finding #1 (BLOCKING): the OLD code silently kept
     * only the first FF_APP_MAP_MAX_FEATURES pack features in pack
     * order and dropped the rest with zero indication anything was
     * missing — verified against the real, currently-merged Lost Lands
     * pack (13 features), which lost RV/tent camping, Village
     * Marketplace and First aid this way. Both caps are now sized with
     * real headroom (ff_app_state.h's own doc comment), AND any overflow
     * that still happens is surfaced honestly rather than silently: */
    uint8_t const n_feat = (p->n_features < FF_APP_MAP_MAX_FEATURES) ? p->n_features : FF_APP_MAP_MAX_FEATURES;
    if (p->n_features > FF_APP_MAP_MAX_FEATURES) {
        out->truncated = true;
        out->features_omitted = (uint8_t)(p->n_features - FF_APP_MAP_MAX_FEATURES);
    }
    for (uint8_t i = 0; i < n_feat; i++) {
        fp_feature_t const *f = &p->features[i];
        ff_app_map_feature_t *o = &out->features[out->n_features];
        o->kind = (ff_app_map_kind_t)f->kind; /* mirrors fp_feature_kind_t exactly — ff_app_state.h's doc comment */
        shell_copy_str(o->label, sizeof(o->label), f->label);
        o->color_valid = shell_stage_color(p, f->stage_idx, &o->color_rgb);
        uint8_t const n_pts = (f->n_pts < FF_APP_MAP_MAX_POLY_PTS) ? f->n_pts : FF_APP_MAP_MAX_POLY_PTS;
        if (f->n_pts > FF_APP_MAP_MAX_POLY_PTS) {
            /* Same overflow signal, for a KEPT feature's own polygon —
             * the latent per-polygon half of finding #1 (not yet
             * triggered by real data: the real pack's largest polygon is
             * 9 points against this cap's 16, but the defect shape was
             * identical and silent either way). */
            out->truncated = true;
        }
        o->n_pts = n_pts;
        for (uint8_t k = 0; k < n_pts; k++) {
            o->pts_en[k][0] = f->pts_en[k][0];
            o->pts_en[k][1] = f->pts_en[k][1];
        }
        out->n_features++;
    }

    for (uint8_t i = 0; i < sh->crew.count && out->n_crew < FF_CREW_MAX; i++) {
        ff_crew_member_t const *m = &sh->crew.members[i];
        if (!m->paired || !m->has_pos) continue; /* same "only known positions get a ring dot" gate as ff_radar_compute's dots[] */
        ff_app_map_crew_t *o = &out->crew[out->n_crew];
        o->initial = m->initial;
        o->color_idx = m->color_idx;
        o->has_pos = true;
        ff_geo_project(p->origin, m->pos, &o->east_m, &o->north_m);
        ff_freshness_t const fr = ff_crew_freshness(m, shell_now(sh));
        o->stale = (fr == FF_FRESH_STALE || fr == FF_FRESH_LOST || fr == FF_FRESH_NEVER);
        o->place = (fr == FF_FRESH_ASSERTED); /* issue #33 */
        o->imprecise = m->has_precision_bits && m->precision_bits < FF_CREW_POS_PRECISION_MIN_BITS; /* issue #47 */
        out->n_crew++;
    }

    /* Rally: no core rally-selection state exists yet — ff_app_state.h's
     * has_rally doc comment (`ff_crew.h`'s own documented gap). Always
     * false until that lands; not invented here. */

    out->you_has_pos = sh->my_pos_ok;
    if (sh->my_pos_ok) {
        ff_geo_project(p->origin, sh->my_pos, &out->you_east_m, &out->you_north_m);
    }
    out->you_heading_valid = (sh->heading_deg >= 0.0f); /* ff_geo_heading_deg's "unreliable" sentinel */
    out->you_heading_deg = out->you_heading_valid ? sh->heading_deg : 0.0f;
}

/** "HH:MM", or "" when the puck does not know what time it is.
 *  scr_radar renders an empty clock_str as "--:--" — an explicit
 *  unknown, never an invented time. */
static void shell_project_clock_str(ff_wall_t w, char *buf, size_t n)
{
    if (n == 0) return;
    buf[0] = '\0';
    if (w.src == FF_WALL_UNKNOWN || n < 6u) return;

    int const minute_of_day = (int)(((w.now_min % 1440) + 1440) % 1440);
    int const hh = minute_of_day / 60;
    int const mm = minute_of_day % 60;
    buf[0] = (char)('0' + (hh / 10));
    buf[1] = (char)('0' + (hh % 10));
    buf[2] = ':';
    buf[3] = (char)('0' + (mm / 10));
    buf[4] = (char)('0' + (mm % 10));
    buf[5] = '\0';
}

static void shell_project(shell_t *sh, uint32_t now_ms)
{
    ff_wall_t const wall = shell_wall(sh, now_ms);

    /* Rebuild from zero every tick. Two reasons, both load-bearing: a
     * field a later slice stops writing cannot linger from an earlier
     * frame, and the render key below is compared with memcmp — so
     * padding bytes have to be deterministic, which memset guarantees
     * and field-by-field assignment does not. */
    memset(&sh->view, 0, sizeof(sh->view));

    /* AC13. `modal ? modal : base` — deliberately NOT ff_route_visible(),
     * whose one distinctive answer is FF_APP_FACE_FLARE. That is a
     * routing answer ("where does the next intent go?"), not a render
     * instruction: writing it here would put the takeover in two places
     * and re-create the desync that keeps `takeover` out of ff_route_t.
     * The takeover reaches the screen as flare.takeover_active, which
     * face_dispatch.c already reads. */
    sh->view.active_face = (sh->route.modal != FF_APP_FACE_NONE) ? sh->route.modal : sh->route.base;

    /* S24 supersedes S08 AC3's "unread clears on face view": merely
     * LOOKING at the inbox no longer marks anything read — that would
     * zero every conversation badge the inbox exists to show. Unread now
     * clears PER THREAD, on the open transition (`ff_inbox_mark_thread_
     * read` in the INBOX_OPEN_THREAD/PICK handlers — S24 AC4), which is
     * what makes the badges honest. What the face transition still does:
     * leaving Signals resets the sub-view, so a fresh entry always lands
     * on the inbox rather than a stale thread/picker. */
    if (sh->view.active_face != FF_APP_FACE_SIGNALS && sh->prev_face == FF_APP_FACE_SIGNALS) {
        sh->sig_subview     = FF_SIG_SUB_INBOX;
        sh->sig_thread_node = 0u;
    }
    sh->prev_face = sh->view.active_face;

    ff_radar_compute(&sh->view.radar, &sh->smooth, &sh->crew, sh->heading_deg, sh->my_pos, sh->my_pos_ok,
                      sh->settings.imperial, now_ms);
    /* ff_radar_compute deliberately does not write these three — they
     * come from the RTC, the battery ADC and the mesh link, none of which
     * are its inputs (see ff_radar.h's deviation note). */
    shell_project_clock_str(wall, sh->view.radar.clock_str, sizeof(sh->view.radar.clock_str));
    sh->view.radar.batt_pct = -1; /* no battery ADC on either target yet: honestly unknown */
    sh->view.radar.mesh_ok = (sh->link == FF_SHELL_LINK_CONNECTED);

    shell_project_now(sh, wall, &sh->view.now);
    shell_project_signals(sh, now_ms, &sh->view.signals);
    shell_project_flare(sh, now_ms, &sh->view.flare);
    shell_project_settings(sh, &sh->view.settings);
    shell_project_map(sh, &sh->view.map);

    /* compose: the draft (text/mode/pending) is shell-owned T9 state as
     * of slice c3 (`sh->compose_draft`) — projected verbatim from
     * `ff_t9_text()`/`has_pending`, the exact mirror `ff_app_compose_t`'s
     * own doc comment describes. The DESTINATION is c1's (the intent seam
     * resolved it when OPEN_COMPOSE pushed the modal): the name is
     * looked up at projection time, not captured at open time, so a
     * NodeInfo rename mid-compose is reflected — same "the view is a
     * projection" rule as everything else in this function. 0 (or a
     * member the roster no longer holds) projects "", which scr_compose
     * renders as "TO: EVERYONE".
     *
     * Known edge, stated rather than hidden: a paired member whose name
     * has never arrived projects "" too, which renders as EVERYONE while
     * the stored destination is that member — dishonest for the one
     * frame-sequence between pairing and the first named NodeInfo. */
    shell_copy_str(sh->view.compose.text, sizeof(sh->view.compose.text), ff_t9_text(&sh->compose_draft));
    sh->view.compose.has_pending = sh->compose_draft.has_pending;
    sh->view.compose.mode = sh->compose_mode;
    if (sh->compose_to_node != 0u) {
        shell_copy_str(sh->view.compose.to_name, sizeof(sh->view.compose.to_name),
                        shell_name_of(sh, sh->compose_to_node));
    }

    /* S08 addendum — predictive projection. ONLY in PRED mode; the
     * whole-view memset at the top of this function leaves every field
     * below zeroed in the other modes, so multitap/123/SYM project exactly
     * as they did before. HONEST-DATA throughout (see ff_app_compose_t's
     * doc comment): `word` is verbatim from the engine, never a fabricated
     * key string; `from_pack` is POINTER IDENTITY against the shell's
     * festpack table; `total_cand` is the real engine count. */
    if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
        ff_t9pred_session_t const *ps = &sh->compose_pred;
        char const *cur = ff_t9pred_session_current(ps);
        if (cur != NULL) {
            shell_copy_str(sh->view.compose.word, sizeof(sh->view.compose.word), cur);
        }
        /* Honest no-match: digits typed AND the engine returned nothing.
         * Distinct from "no digits yet" (word "" but not a no-match). */
        sh->view.compose.word_nomatch = (ps->n > 0u && cur == NULL);

        char const *cands[FF_APP_COMPOSE_MAX_CAND] = {0};
        size_t const nc = ff_t9pred_session_candidates(ps, cands, FF_APP_COMPOSE_MAX_CAND);
        for (size_t i = 0; i < nc; i++) {
            shell_copy_str(sh->view.compose.cand[i].text, sizeof(sh->view.compose.cand[i].text),
                            cands[i]);
            /* Pointer identity, not a name match: the engine hands back the
             * very pointer the shell supplied for a festpack word. */
            bool from_pack = false;
            for (int e = 0; e < sh->compose_extra_n; e++) {
                if (cands[i] == sh->compose_extra[e]) {
                    from_pack = true;
                    break;
                }
            }
            sh->view.compose.cand[i].from_pack = from_pack;
        }
        sh->view.compose.n_cand = (uint8_t)nc;
        sh->view.compose.sel_cand = (uint8_t)ps->sel;

        size_t const total = ff_t9pred_count_ex(ps->digits, ps->n, ps->extra, ps->n_extra);
        sh->view.compose.total_cand = (total > UINT16_MAX) ? UINT16_MAX : (uint16_t)total;
    }
}

/* ---------------------------------------------------------------------
 * The dirty bit, computed over the RENDERED projection
 * ------------------------------------------------------------------- */

/** Milliseconds -> whole seconds, preserving the -1 "not applicable"
 *  sentinel. Countdowns reach the screen through flare_fmt at 1 s
 *  granularity, so that is the granularity they are compared at. */
static int32_t shell_coarsen_ms(int32_t ms)
{
    return (ms < 0) ? -1 : (ms / 1000);
}

/* Signals rows carry a RAW `age_ms` that advances every tick; the screen
 * renders it through `ff_fmt_age`, which only changes at coarse boundaries
 * (whole SEC under a minute, whole MIN under an hour, whole HR beyond). Key
 * on the same buckets so a still Signals face does not rebuild — and lose
 * its scroll position — sub-second, exactly the arrow_deg problem one field
 * over. This mirrors ff_fmt_age's own thresholds so two ages that format
 * identically key identically (and no finer). */
static uint32_t shell_coarsen_age_ms(uint32_t age_ms)
{
    uint32_t s = age_ms / 1000u;
    if (s < 60u) return 0u;              /* "now" — ONE bucket for the whole sub-minute span (ff_fmt_age
                                          * renders every <60s age identically), so a ticking sub-minute
                                          * age never changes the render key and never rebuilds a screen
                                          * (the row/chip under a finger survives). This is the dominant
                                          * demo-LIVE churn source, killed at the source. */
    if (s < 3600u) return 60u + s / 60u; /* "N MIN" — per-minute (offset so buckets never alias the now/SEC bucket) */
    return 3660u + s / 3600u;            /* "N HR"  — per-hour  (offset past the MIN range) */
}

/**
 * Build the render key: the projection, with exactly the fields that are
 * pure functions of elapsed time coarsened to what actually reaches the
 * screen.
 *
 * Built by COPYING the whole projection and then coarsening named
 * fields, rather than by listing the fields that matter. That direction
 * is the point: a view field added by a later slice is in the key
 * automatically, so forgetting to update this function causes a
 * redundant repaint, never a stale screen. The reverse construction
 * fails silently and looks fine in review.
 */
static void shell_render_key(ff_app_state_t const *v, ff_app_state_t *key)
{
    memcpy(key, v, sizeof(*key));

    key->flare.send_expires_in_ms = shell_coarsen_ms(v->flare.send_expires_in_ms);
    key->flare.takeover_expires_in_ms = shell_coarsen_ms(v->flare.takeover_expires_in_ms);
    key->flare.locked_expires_in_ms = shell_coarsen_ms(v->flare.locked_expires_in_ms);

    /* The arrow is exponentially smoothed, so with a completely static
     * scene it converges toward its target forever without ever quite
     * arriving — a raw float compare is true on every frame in the field,
     * which is exactly the whole-struct-memcmp failure S16 warns about.
     * 0.1 degrees is LVGL's own rotation unit: below it, no pixel moves. */
    key->radar.arrow_deg = (float)(int32_t)(v->radar.arrow_deg * 10.0f);

    /* #bug1 — brightness is kept OUT of the render key (coarsened to a
     * constant). A live brightness drag emits a value change every frame;
     * were it in the key, each would mark the view dirty and force a full
     * face teardown+rebuild, destroying the very slider being dragged. It
     * needs no rebuild anyway: brightness changes no rendered pixel except
     * the Settings slider, which scr_settings.c already tracks live in its
     * own deco, and the device backlight, which app_main applies every tick
     * from the projected brightness_pct independently of the dirty bit. So a
     * brightness change never repaints a face; only a genuine face/content
     * change does. (The committed value still reaches a fresh build via the
     * projection on the next real repaint / on re-entry to Settings.) */
    key->settings.brightness_pct = 0;

    /* S24 signals (the flare-octant lesson, applied at birth rather than
     * as a fix): the inbox's key covers exactly its RENDERED projection.
     * (1) Coarsen every conversation's raw ages to the ff_fmt_age BUCKET
     *     the screen actually renders (see shell_coarsen_age_ms) — the
     *     preview age ("6 MIN") and the presence age ("SEEN 2 HR") both
     *     reach the glass only through ff_fmt_age, so sub-bucket ticking
     *     must not churn the key (and destroy the row under a finger).
     *     Presence CATEGORY changes (SEEN->LOST) live in `presence`
     *     itself, already in the key uncoarsened.
     * (2) Zero the convs BEYOND conv_count — ff_inbox_build memsets the
     *     whole struct each build, so today the tail is already zero;
     *     normalizing anyway keeps the key a pure function of what is
     *     shown even if a future build stops clearing the tail (the
     *     ff_sigview stale-tail hazard, pre-empted). */
    for (uint8_t i = 0; i < key->signals.inbox.conv_count && i < FF_INBOX_MAX_CONVS; i++) {
        ff_inbox_conv_t *kc = &key->signals.inbox.convs[i];
        kc->preview_age_ms = shell_coarsen_age_ms(v->signals.inbox.convs[i].preview_age_ms);
        /* The presence AGE is only ever drawn for SEEN ("SEEN 2 HR");
         * LOST/LINKED render as bare words, so their age is not part of
         * the rendered projection at all — keyed as 0, not a bucket. */
        kc->presence_age_ms = (v->signals.inbox.convs[i].presence == FF_PRESENCE_SEEN)
                                  ? shell_coarsen_age_ms(v->signals.inbox.convs[i].presence_age_ms)
                                  : 0u;
    }
    for (uint8_t i = key->signals.inbox.conv_count; i < FF_INBOX_MAX_CONVS; i++) {
        memset(&key->signals.inbox.convs[i], 0, sizeof(key->signals.inbox.convs[i]));
    }

    /* S24 slice (c) — while a THREAD is open, the thread is the ONLY
     * Signals surface rendered (scr_signals.c's THREAD arm; the nav
     * page-dots/badge are suppressed there too, per the design
     * artboards), so the key reduces to exactly the thread's own
     * rendered projection: the messages (ages coarsened to their
     * ff_fmt_age bucket), the header scope facts (name, member
     * dot/color, the CREW roster count, the 1:1 presence line's
     * category + SEEN bucket), and nothing else. Everything the inbox
     * rows render — other conversations' previews, ages, presence and
     * unread badges — is masked to zero, so a live thread cannot be
     * rebuilt (and the chip under a finger destroyed) by churn in
     * conversations that change no pixel on this screen. The same
     * flare-takeover lesson one block down, applied at this sub-view's
     * birth. */
    if (v->signals.subview == FF_SIG_SUB_THREAD) {
        ff_inbox_conv_t open_cv;
        memset(&open_cv, 0, sizeof(open_cv));
        for (uint8_t i = 0; i < v->signals.inbox.conv_count && i < FF_INBOX_MAX_CONVS; i++) {
            ff_inbox_conv_t const *cv = &v->signals.inbox.convs[i];
            bool const is_crew = (cv->kind == FF_CONV_CREW);
            if ((v->signals.thread_node == 0u && is_crew) ||
                (v->signals.thread_node != 0u && !is_crew && cv->node_id == v->signals.thread_node)) {
                /* Header facts only — never the conversation's preview/
                 * unread, which this screen does not draw. */
                open_cv.kind     = cv->kind;
                open_cv.node_id  = cv->node_id;
                memcpy(open_cv.name, cv->name, sizeof(open_cv.name));
                open_cv.initial   = cv->initial;
                open_cv.color_idx = cv->color_idx;
                open_cv.presence_valid  = cv->presence_valid;
                open_cv.presence        = cv->presence;
                open_cv.presence_age_ms = (cv->presence == FF_PRESENCE_SEEN)
                                              ? shell_coarsen_age_ms(cv->presence_age_ms)
                                              : 0u;
                break;
            }
        }
        uint8_t const conv_count = v->signals.inbox.conv_count;
        memset(&key->signals.inbox, 0, sizeof(key->signals.inbox));
        key->signals.inbox.conv_count = conv_count; /* the CREW header's "N CREW" roster fact */
        key->signals.inbox.convs[0]   = open_cv;
        for (uint8_t i = 0; i < key->signals.thread.msg_count && i < FF_INBOX_MAX_MSGS; i++) {
            key->signals.thread.msgs[i].age_ms =
                shell_coarsen_age_ms(v->signals.thread.msgs[i].age_ms);
        }
        for (uint8_t i = key->signals.thread.msg_count; i < FF_INBOX_MAX_MSGS; i++) {
            memset(&key->signals.thread.msgs[i], 0, sizeof(key->signals.thread.msgs[i]));
        }
    }

    /* S24 slice (d) — the action POPUP and the Rally screen are OPAQUE
     * overlays in the key (AC8, the flare/thread precedent). While either
     * is up it IS the interactive surface, and the (dimmed) thread or
     * inbox beneath it changes no pixel the overlay's own controls sit on
     * — so a live feed item, an aging row or a presence flip beneath must
     * not dirty the key, rebuild the tree, and destroy a popup row / Send
     * button under the finger. Mask those churny sources; the overlay's
     * OWN rendered facts survive in the key verbatim (the memcpy above):
     * the popup's scope label reads signals.thread_name/thread_node/
     * thread_color_idx (top-level, untouched here), and the Rally screen
     * its projected `signals.rally` struct (untouched). The subview itself
     * is in the key too, so opening/closing the overlay still dirties. */
    if (v->signals.subview == FF_SIG_SUB_POPUP || v->signals.subview == FF_SIG_SUB_RALLY) {
        memset(&key->signals.inbox, 0, sizeof(key->signals.inbox));
        memset(&key->signals.thread, 0, sizeof(key->signals.thread));
    }

    /* #flare-repeat-dismiss — the takeover is OPAQUE in the render key.
     *
     * While a takeover is up it is the ONLY thing rendered: both
     * targets/esp32s3/main/ff_face.c and targets/sim/face_dispatch.c build
     * the full-screen takeover and return before touching any face beneath
     * it. So the rendered screen is a pure function of the takeover's own
     * fields — a moving radar arrow, an aging signals row, or a fresh feed
     * item from the busy live demo (ff_sigview_build reads the feed every
     * tick) changes no pixel.
     *
     * Keying on the whole projection there made every such underlying
     * change a dirty frame, and the device answers a dirty frame with
     * lv_obj_clean()+rebuild — which destroys and recreates the takeover's
     * DISMISS button. Under the live demo's steady feed/presence churn that
     * ran many times a second, and any DISMISS tap whose press and release
     * straddled a rebuild was dropped by LVGL (its pressed object had been
     * deleted): the "second flare won't dismiss" report. (It also restarted
     * the takeover's attention animation every frame, so it never played.)
     *
     * So reduce the key, while takeover_active, to exactly the fields
     * ff_scr_flare_build_takeover renders (headline sender name, the
     * bearing/distance read, and the GO-drops-lock disclosure) — NOT the
     * countdown, which that screen does not draw. The takeover tree, and
     * its live DISMISS button, is then rebuilt only when the takeover
     * itself changes, never from anything beneath it. Kept stack-cheap
     * (small locals, not a second ff_app_state_t — see ff_shell_tick's note
     * on why a full copy is unsafe on this stack) and in the same
     * normalize-the-key spirit as the coarsening above. This changes only
     * the dirty comparison; the alert haptic (shell_ev_private's
     * should_alert path) is independent of the key and is untouched. */
    if (v->flare.takeover_active) {
        char from[sizeof(key->flare.takeover_from_name)];
        char dist[sizeof(key->flare.takeover_dist_str)];
        char locked_from[sizeof(key->flare.locked_from_name)];
        bool const b_valid = v->flare.takeover_bearing_valid;
        /* Key bearing on the RENDERED octant, not the raw float. The
         * takeover draws bearing ONLY as ff_flare_fmt_compass8's 1-of-8
         * compass point (scr_flare.c) — a 45-degree bucket — while the
         * projected takeover_bearing_deg is recomputed every tick from live
         * positions (ff_geo_bearing_deg), so keying the raw float (even
         * quantized to 0.1 degree) repainted on sub-octant jitter that
         * moves no glyph: on device that repaint is an lv_obj_clean+rebuild
         * that destroys the DISMISS button mid-tap — the same clobber this
         * change exists to stop, from a second live source. So fold the
         * bearing to its octant index exactly the way ff_flare_fmt_compass8
         * does (that formatter is the rendered authority; this mirrors its
         * [0,360) normalization + half-bucket-shift floor divide), and use
         * a -1 sentinel when the bearing is not valid so a valid<->unknown
         * transition — which DOES change the drawn line — still dirties.
         * (takeover_dist_str is already the formatted string, so it keys
         * itself; this gives bearing the same rendered-projection footing.) */
        float b_octant;
        if (b_valid) {
            float const a = ff_geo_wrap_deg(v->flare.takeover_bearing_deg);
            b_octant = (float)((int)((a + 22.5f) / 45.0f) % 8); /* 0..7, mirrors ff_flare_fmt_compass8 */
        } else {
            b_octant = -1.0f; /* sentinel distinct from every 0..7 octant */
        }
        bool const locked = v->flare.locked;
        memcpy(from, v->flare.takeover_from_name, sizeof(from));
        memcpy(dist, v->flare.takeover_dist_str, sizeof(dist));
        memcpy(locked_from, v->flare.locked_from_name, sizeof(locked_from));

        memset(key, 0, sizeof(*key));
        key->flare.takeover_active = true;
        memcpy(key->flare.takeover_from_name, from, sizeof(from));
        key->flare.takeover_bearing_valid = b_valid;
        key->flare.takeover_bearing_deg = b_octant;
        memcpy(key->flare.takeover_dist_str, dist, sizeof(dist));
        key->flare.locked = locked;
        memcpy(key->flare.locked_from_name, locked_from, sizeof(locked_from));
    }
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

int ff_shell_init(ff_shell_t *sh_pub, ff_shell_cfg_t const *cfg)
{
    if (sh_pub == NULL || cfg == NULL || cfg->clock == NULL) return -1;

    shell_t *sh = shell_of(sh_pub);
    memset(sh, 0, sizeof(*sh));

    sh->clock = cfg->clock;
    sh->store = cfg->store;
    sh->haptic = cfg->haptic;
    sh->haptic_user = cfg->haptic_user;
    sh->calibrate_touch = cfg->calibrate_touch;
    sh->calibrate_touch_user = cfg->calibrate_touch_user;
    sh->pack = cfg->pack;
    sh->pack_loaded = false;

    ff_crew_init(&sh->crew, sh->clock);
    ff_heard_init(&sh->heard);
    ff_feed_init(&sh->feed);
    ff_flare_init(&sh->flare);
    ff_wall_init(&sh->wall);
    ff_radar_smooth_reset(&sh->smooth);
    ff_route_init(&sh->route);
    sh->sig_target_kind = FF_TARGET_WHOLE_CREW; /* S22 slice b — default target */
    sh->sig_target_node = 0u;
    sh->sig_rally_sel   = 0u; /* S24 slice d — On Me by default (projection re-validates) */
    sh->sig_rally_when  = FF_RALLY_WHEN_NOW;
    ff_t9_reset(&sh->compose_draft); /* S16 slice c3 */
    /* S08 addendum — reset site #1 (init). memset zeroed compose_extra_n,
     * so no pack words are bound yet; a later ff_shell_load_pack collects
     * them and OPEN_COMPOSE re-binds. Default to the predictive mode. */
    ff_t9pred_session_reset(&sh->compose_pred);
    ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
    sh->compose_mode = FF_APP_COMPOSE_PRED;

    /* Settings are loaded at init and never re-read per tick (S16
     * "Behavior"). A NULL store yields the exact defaults. */
    ff_settings_load(&sh->settings, sh->store);

    sh->my_pos_ok = false;
    sh->heading_deg = -1.0f; /* unknown, ff_geo_heading_deg's sentinel */
    sh->link = FF_SHELL_LINK_NONE;
    sh->prev_face = FF_APP_FACE_NONE;
    sh->has_prev_key = false;

    mc_events_t const ev = ff_shell_events(sh_pub);
    mc_init(&sh->mc, cfg->transport, ev, sh->clock);

    /* Feed pushes go through ff_wiring, which already owns the
     * crew-paired-sender filter, the heard-note on a miss and the feed
     * push itself — none of that is reimplemented here. The haptic it
     * fires is the quiet-gated one. Bound after mc_init so its
     * canned-reply sender points at a client that exists (the replies
     * themselves are slice c2). */
    ff_wiring_init(&sh->wiring, &sh->feed, &sh->crew, &sh->heard, &sh->mc, shell_haptic_feed, sh, sh->clock);

    /* A transport with neither read nor write is the documented "no
     * transport" case: events get injected through ff_shell_events()
     * instead (tests, and any target driving its own client). */
    if (cfg->transport.read != NULL || cfg->transport.write != NULL) {
        sh->attached = true;
        mc_connect(&sh->mc);
    }

    return 0;
}

int ff_shell_load_pack(ff_shell_t *sh_pub, char const *json, size_t len)
{
    if (sh_pub == NULL) return -1;
    shell_t *sh = shell_of(sh_pub);

    /* Cleared up front: fp_parse zeroes *out on any failure, and a zeroed
     * fp_pack_t reads as a deliberately STATED UTC offset of 0 — which
     * would silently outrank the user's configured offset (ff_wall.h's
     * ff_wall_offset_cfg_t comment). A failed load must leave "no pack",
     * not "a pack that says London". */
    sh->pack_loaded = false;
    /* And no festpack words: fp_parse zeroes *out on failure, so keeping a
     * previous pack's collected count would leave the table pointing at
     * now-empty names. Cleared here so a failed load honestly supplies no
     * supplement (the next successful load re-collects). */
    sh->compose_extra_n = 0;

    if (sh->pack == NULL || json == NULL || len == 0u) return -1;
    if (fp_parse(json, len, sh->pack) != FP_OK) return -1;

    sh->pack_loaded = true;

    /* S08 addendum — collect the pack's names (set artists, stage names,
     * landmark names) into the shell's festpack word table. The pointers
     * ALIAS into `sh->pack` (no copy), which outlives them. The bound
     * predictive session picks these up on the next OPEN_COMPOSE (which
     * re-binds against the fresh count); a pack rarely loads mid-compose,
     * and even then the next reset re-binds. */
    sh->compose_extra_n = fp_t9words_collect(sh->pack, sh->compose_extra, FF_COMPOSE_EXTRA_MAX);

    /* S18 slice c (#40): tighten the wall-clock plausibility window to
     * THIS pack's festival dates. The honest bound on "is this a plausible
     * time" is "is it near the festival we're at", and a pack carries its
     * own year, so the window moves with the data — a 2030 pack pins a
     * 2030 window and the fixed bootstrap window's slow decay stops
     * mattering here. A pack with no usable dates (null/absent/corrupt)
     * makes ff_wall_window_from_pack return false; we then explicitly
     * RESET to the fixed bootstrap window rather than leave whatever a
     * previously-loaded pack installed — the honest fallback is the wide
     * fixed window, never an invented tight one, and never a stale one
     * from a different pack. The latch itself is untouched either way;
     * only future observations are gated by the new window. */
    int64_t win_floor = 0;
    int64_t win_ceiling = 0;
    if (ff_wall_window_from_pack(sh->pack, &win_floor, &win_ceiling)) {
        (void)ff_wall_set_window(&sh->wall, win_floor, win_ceiling);
    } else {
        (void)ff_wall_set_window(&sh->wall, FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING);
    }
    return 0;
}

bool ff_shell_tick(ff_shell_t *sh_pub, uint32_t now_ms)
{
    if (sh_pub == NULL) return false;
    shell_t *sh = shell_of(sh_pub);

    sh->now_ms = now_ms;

    if (sh->attached) {
        mc_tick(&sh->mc, now_ms);
    }

    /* S18 slice b (#50): the cold-boot want_config replay buffered its
     * cached positions while the wall latch was still settling. Re-age the
     * whole buffer against the now-settled latch EXACTLY on the link's
     * not-ready->ready edge — the burst-end signal, needing no mc_client
     * change. `was_ready` makes it the edge, so the pass runs once per
     * handshake rather than every ready tick. Placed before the projection
     * below so this tick already renders the recovered freshness. */
    bool const ready = (sh->link == FF_SHELL_LINK_CONNECTED);
    if (ready && !sh->was_ready) {
        shell_settle_replay(sh, now_ms);
    }
    sh->was_ready = ready;

    /* All three flare deadlines in one call; takeover_active can clear
     * here with no route involved, which is why ff_route_visible takes
     * takeover as a parameter rather than caching it. */
    (void)ff_flare_tick(&sh->flare, now_ms);

    /* S16 slice c3: the injected clock, not lv_tick_get() (scr_compose.c
     * no longer touches the clock at all — see ff_t9.h's "Deviations"
     * note for why the multi-tap commit timeout needs an explicit poll).
     * Safe to call every tick unconditionally, pending char or not. */
    ff_t9_tick(&sh->compose_draft, now_ms);

    shell_project(sh, now_ms);

    /* Dirty-detection key: a masked copy of the view. Held as a function-static
     * (~7 KB) rather than a stack local — a full ff_app_state_t spilled onto the
     * stack every tick, and on the ESP32-S3 (8 KB main-task stack) a populated
     * projection (demo mode's seeded festpack + crew, or a busy field session)
     * tipped it into a stack overflow. .bss instead of stack; the shell is
     * single-threaded, and this is pure scratch (recomputed every call, no state
     * carried between calls), so a shared static is safe. The desktop sim never
     * hit this — its stack is megabytes. Cost: ~7 KB permanent .bss in EVERY
     * build (the fixed main-task stack doesn't shrink in return) — the
     * RAM-comfortable field build absorbs it; the demo build, which is the one
     * that was starved, is the beneficiary. */
    static ff_app_state_t key;
    shell_render_key(&sh->view, &key);

    bool const changed = (!sh->has_prev_key) || (memcmp(&key, &sh->prev_key, sizeof(key)) != 0);
    sh->prev_key = key;
    sh->has_prev_key = true;
    return changed;
}

ff_app_state_t const *ff_shell_view(ff_shell_t const *sh_pub)
{
    if (sh_pub == NULL) return NULL;
    return &shell_of_const(sh_pub)->view;
}

ff_wall_t ff_shell_wall(ff_shell_t const *sh_pub)
{
    if (sh_pub == NULL) {
        ff_wall_t unknown;
        memset(&unknown, 0, sizeof(unknown));
        return unknown;
    }
    shell_t const *sh = shell_of_const(sh_pub);
    return shell_wall(sh, shell_now(sh));
}

void ff_shell_close(ff_shell_t *sh_pub)
{
    if (sh_pub == NULL) return;
    shell_t *sh = shell_of(sh_pub);
    sh->attached = false;
    sh->link = FF_SHELL_LINK_NONE;
    /* The transport is the target's: it was handed in as a vtable, and
     * closing a socket the shell never opened is not the shell's call. */
}

mc_events_t ff_shell_events(ff_shell_t *sh_pub)
{
    mc_events_t ev;
    memset(&ev, 0, sizeof(ev));
    if (sh_pub == NULL) return ev;

    ev.on_state = shell_ev_state;
    ev.on_node = shell_ev_node;
    ev.on_position = shell_ev_position;
    ev.on_text = shell_ev_text;
    ev.on_private = shell_ev_private;
    ev.on_my_info = shell_ev_my_info;
    ev.on_rx_meta = shell_ev_rx_meta;
    ev.user = shell_of(sh_pub);
    return ev;
}

/* ---------------------------------------------------------------------
 * Intent dispatch (S16 slice c1) — see ff_shell.h's ff_shell_intent doc
 * ------------------------------------------------------------------- */

/**
 * THE SETTINGS JUDGMENT CALL (S16 slice c1, argued in the PR body) —
 * RESOLVED (S11 slice b): the renderer now exists (`ff_scr_settings_build`,
 * app/screens/scr_settings.c), so the shell stops rejecting the intent and
 * pushes the modal, same as OPEN_COMPOSE.
 *
 * The history, kept because it explains why this was ever a runtime `if`
 * instead of a straight push: FF_INTENT_OPEN_SETTINGS used to route to a
 * modal whose renderer did not exist. Until it did, the shell REJECTED
 * the intent — the long-press stayed the no-op it had been since S06
 * reserved the hook — rather than pushing a modal:
 *
 *  - Pushing without a renderer would have created a DEAD END, not a
 *    placeholder: any modal suppresses swipe (AC2), a placeholder has no
 *    BACK control, and nothing else on a placeholder emits — the user
 *    would be wedged on a not-a-screen until reboot. The repo's own UX
 *    checklist treats a dead-end screen as a blocking finding
 *    (scr_compose.c's back button exists for exactly that item), and
 *    building an escapable placeholder was renderer work c1's scope
 *    excluded.
 *  - Honest-data: a screen claiming to be Settings, or the S13 debug
 *    fixture view standing in for one, would have asserted a feature the
 *    device did not have. A gesture that does nothing under-claims; a
 *    fake screen mis-claims — this repo consistently prefers the former.
 *
 * The routing was complete and stayed compiled the whole time (`if`, not
 * `#if`, so -Werror and -Wswitch kept checking it): this slice flips the
 * one constant below to true, and the seam needed no other change. The
 * push path it enables is the same `ff_route_push_modal` machinery
 * OPEN_COMPOSE exercises below, and SETTINGS-as-modal was already covered
 * at the route layer by slice a's AC2 tests.
 */
static bool const k_settings_renderer_exists = true; /* S11 slice b — flipped */

/**
 * The composer destination rule (S08 Behavior: "Composer: reached from
 * Signals '+'; TO = selected crew member").
 *
 *  - An explicit `requested` naming a paired roster member is honored —
 *    a future per-item reply affordance passes the item's sender, who
 *    is paired by construction (only paired senders reach the feed).
 *  - An explicit id the trust policy won't message (unknown, or known
 *    but unpaired) degrades to BROADCAST, never to a different member:
 *    silently retargeting a message at someone the caller did not name
 *    is worse than over-sharing to everyone.
 *  - No explicit id (0 — the Signals '+', which is a pure renderer and
 *    cannot know the selection) resolves per S08: the currently
 *    selected paired member, else broadcast. NOT the newest feed item —
 *    that is the CANNED-REPLY context rule (`ff_wiring_send_canned_reply`,
 *    issue #23), and S08's Behavior section gives the composer its own,
 *    different rule.
 */
static uint32_t shell_compose_dest(shell_t *sh, uint32_t requested)
{
    if (requested != 0u) {
        ff_crew_member_t const *m = ff_crew_find(&sh->crew, requested);
        return (m != NULL && m->paired) ? requested : 0u;
    }
    ff_crew_member_t const *sel = ff_crew_selected(&sh->crew);
    return (sel != NULL) ? sel->node_id : 0u;
}

/* ---------------------------------------------------------------------
 * S22 slice d — Signals action SEND wiring.
 * ------------------------------------------------------------------- */

/**
 * shell_sig_dest — resolve the current Signals send target to the address
 * a send goes to, RE-VALIDATED against the live roster (S22 honest-data,
 * Tier 3): a member target is honoured only if that node is still a paired
 * roster member. Writes `*out_dest` and returns true on success:
 *   - WHOLE_CREW  -> MC_ADDR_BROADCAST (the broadcast the user saw on the
 *                    target line).
 *   - MEMBER      -> that member's node id.
 * Returns false (and writes nothing) if the target is a member who is no
 * longer paired/known — the caller then sends NOTHING rather than either
 * addressing a node the roster does not hold OR silently falling back to a
 * broadcast the user never asked for. (The per-tick projection already
 * downgrades such a stale target to WHOLE_CREW; this is the belt-and-braces
 * check at the exact instant of a send.) */
static bool shell_sig_dest(shell_t const *sh, uint32_t *out_dest)
{
    if (sh->sig_target_kind == FF_TARGET_MEMBER) {
        ff_crew_member_t const *m = ff_crew_find(&sh->crew, sh->sig_target_node);
        if (sh->sig_target_node == 0u || m == NULL || !m->paired) {
            return false;
        }
        *out_dest = sh->sig_target_node;
        return true;
    }
    *out_dest = MC_ADDR_BROADCAST;
    return true;
}

/**
 * shell_sig_reset_target — S22 AC3: after a successful PULSE/RALLY send the
 * target returns to WHOLE_CREW. Resets both the shell's persistent holder
 * (the source of truth, which survives the per-tick view rebuild) and the
 * live view's mirror of it (so a same-frame read sees the reset, the same
 * immediacy the old ff_sigview_target_reset_after_send call had), and drops
 * any armed rally confirm. */
static void shell_sig_reset_target(shell_t *sh)
{
    sh->sig_target_kind = FF_TARGET_WHOLE_CREW;
    sh->sig_target_node = 0u;
    sh->sig_rally_armed = false;
    sh->view.signals.target_kind = FF_TARGET_WHOLE_CREW;
    sh->view.signals.target_node = 0u;
}

/**
 * shell_rally_place — S22 slice d SPEC-GAP MVP (see the spec "Questions"):
 * derive the (position, name) a quick RALLY gathers the crew to. The point
 * is the SENDER's own current location (`my_pos`); the name is the nearest
 * festpack landmark within FF_SIG_RALLY_LANDMARK_NEAR_M, else the honest
 * constant "MY SPOT".
 *
 * Returns false — and the caller sends NOTHING — when `my_pos` is unknown:
 * a RALLY carries a lat/lon in its wire body, and encoding {0,0} for "I do
 * not know where I am" would be exactly the fabricated-position dishonesty
 * this repo forbids (CLAUDE.md, [[firefly-touch-cal-default]]). A rally to
 * one's own location is only meaningful once that location is known.
 *
 * `*out_name` aliases either the string literal above or a landmark name
 * INSIDE `sh->pack` (no copy); it is consumed immediately by the encode at
 * the call site, well within the pack's lifetime. A landmark whose name
 * would exceed the protocol's FF_PROTO_RALLY_NAME_MAX budget is skipped
 * (the encoder would reject it), leaving the honest default. */
static bool shell_rally_place(shell_t const *sh, ff_latlon_t *out_pos, char const **out_name)
{
    if (!sh->my_pos_ok) {
        return false;
    }
    *out_pos = sh->my_pos;
    *out_name = FF_SIG_RALLY_DEFAULT_NAME;

    if (sh->pack_loaded && sh->pack != NULL && sh->pack->origin_known) {
        float my_e = 0.0f, my_n = 0.0f;
        ff_geo_project(sh->pack->origin, sh->my_pos, &my_e, &my_n);
        /* Compare squared distances — no sqrt, no <math.h>. */
        float best2 = FF_SIG_RALLY_LANDMARK_NEAR_M * FF_SIG_RALLY_LANDMARK_NEAR_M;
        for (uint8_t i = 0; i < sh->pack->n_landmarks; ++i) {
            fp_landmark_t const *lm = &sh->pack->landmarks[i];
            if (!lm->has_pos || lm->name[0] == '\0') {
                continue;
            }
            if (strlen(lm->name) > FF_PROTO_RALLY_NAME_MAX) {
                continue;
            }
            float const de = lm->east_m - my_e;
            float const dn = lm->north_m - my_n;
            float const d2 = de * de + dn * dn;
            if (d2 < best2) {
                best2 = d2;
                *out_name = lm->name;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------------
 * S24 slice d — the Rally screen's WHERE list + WHEN-in-name send.
 * ------------------------------------------------------------------- */

/* A landmark is a Rally WHERE row iff it has a known position AND a name —
 * the same displayable/sendable filter used for BOTH the projected row
 * list and the send-time resolution, so a `sel` index can never diverge
 * between what the screen shows and what the shell sends. */
static bool shell_rally_landmark_displayable(fp_landmark_t const *lm)
{
    return lm != NULL && lm->has_pos && lm->name[0] != '\0';
}

/* Resolve the `idx`-th displayable landmark (0-based) to its wire position
 * (unprojected from the pack origin) and name. Returns false if there is
 * no pack, no known origin, or fewer than idx+1 displayable landmarks. */
static bool shell_rally_landmark_at(shell_t const *sh, uint8_t idx, ff_latlon_t *out_pos,
                                    char const **out_name)
{
    if (!sh->pack_loaded || sh->pack == NULL || !sh->pack->origin_known) {
        return false;
    }
    uint8_t seen = 0;
    for (uint8_t i = 0; i < sh->pack->n_landmarks; ++i) {
        fp_landmark_t const *lm = &sh->pack->landmarks[i];
        if (!shell_rally_landmark_displayable(lm)) {
            continue;
        }
        if (seen == idx) {
            ff_geo_unproject(sh->pack->origin, lm->east_m, lm->north_m, out_pos);
            *out_name = lm->name;
            return true;
        }
        seen++;
    }
    return false;
}

/* The WHEN suffix that rides in the rally NAME (S24 AC6): NOW appends
 * nothing; +15m/+30m append a fixed short tag. Kept well under
 * FF_PROTO_RALLY_NAME_MAX so a place name always has room. */
static char const *shell_rally_when_suffix(ff_rally_when_t when)
{
    switch (when) {
    case FF_RALLY_WHEN_15: return " +15m";
    case FF_RALLY_WHEN_30: return " +30m";
    case FF_RALLY_WHEN_NOW:
    default: return "";
    }
}

/* The WHEN chip's echo word ("Now" / "+15m" / "+30m"). */
static char const *shell_rally_when_echo(ff_rally_when_t when)
{
    switch (when) {
    case FF_RALLY_WHEN_15: return "+15m";
    case FF_RALLY_WHEN_30: return "+30m";
    case FF_RALLY_WHEN_NOW:
    default: return "Now";
    }
}

/* Compose the wire rally NAME from a base place name + the WHEN suffix,
 * into `out` (capacity FF_PROTO_RALLY_NAME_MAX + 1). The PLACE is truncated
 * to fit, the SUFFIX never (S24 AC6: "truncate the PLACE, never the time
 * suffix"). Returns false — the caller sends NOTHING — if the base place is
 * empty, or the budget cannot hold the suffix plus at least one place byte
 * (a misleading name is never sent). */
static bool shell_rally_compose_name(char const *place, ff_rally_when_t when, char *out, size_t out_cap)
{
    if (place == NULL || place[0] == '\0' || out_cap < (size_t)FF_PROTO_RALLY_NAME_MAX + 1u) {
        return false;
    }
    char const *suffix = shell_rally_when_suffix(when);
    size_t const suffix_len = strlen(suffix);
    /* The suffix must leave room for at least one place byte. */
    if (suffix_len + 1u > (size_t)FF_PROTO_RALLY_NAME_MAX) {
        return false;
    }
    size_t const place_budget = (size_t)FF_PROTO_RALLY_NAME_MAX - suffix_len;
    size_t place_len = strlen(place);
    if (place_len > place_budget) {
        place_len = place_budget; /* truncate the PLACE only */
    }
    memcpy(out, place, place_len);
    memcpy(out + place_len, suffix, suffix_len + 1u); /* includes the NUL */
    return true;
}

/* Resolve the CURRENT Rally selection to its (position, base name) — On Me
 * via shell_rally_place, else the selected landmark. Returns false (nothing
 * sendable) when On Me is picked with an unknown position, or a landmark
 * index no longer resolves. */
static bool shell_rally_resolve_sel(shell_t const *sh, ff_latlon_t *out_pos, char const **out_name)
{
    if (sh->sig_rally_sel == 0u) {
        return shell_rally_place(sh, out_pos, out_name); /* On Me */
    }
    return shell_rally_landmark_at(sh, (uint8_t)(sh->sig_rally_sel - 1u), out_pos, out_name);
}

/* Is a thread (or the popup / rally screen OVER it) the visible Signals
 * scope? The open thread IS the send scope (S24), and the popup and rally
 * sub-views sit over that same thread, so all three re-assert the scope
 * from `sig_thread_node`. */
static bool shell_scope_thread_open(shell_t const *sh)
{
    return sh->route.base == FF_APP_FACE_SIGNALS && sh->route.modal == FF_APP_FACE_NONE &&
           (sh->sig_subview == FF_SIG_SUB_THREAD || sh->sig_subview == FF_SIG_SUB_POPUP ||
            sh->sig_subview == FF_SIG_SUB_RALLY);
}

/* Encode + send a PULSE to the CURRENT resolved scope, pushing the OUT
 * feed item on an accepted send (S22(d) pulse, factored so the 1:1 chip and
 * the popup Pulse row share one send path). No-op on a stale member scope
 * (shell_sig_dest false) or a missing sender. Does NOT touch the target/
 * arm state — the caller owns that. */
static void shell_pulse_to_scope(shell_t *sh)
{
    uint32_t dest = MC_ADDR_BROADCAST;
    if (!shell_sig_dest(sh, &dest)) return; /* stale member scope: send nothing */
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int const n = ff_proto_encode_pulse(buf, sizeof buf);
    if (n > 0 && sh->wiring.sender.send_private != NULL) {
        int const rc = sh->wiring.sender.send_private(sh->wiring.sender.ctx, dest, buf, (size_t)n);
        if (rc == 0) {
            /* My own send lands in the feed (FEED_DIR_OUT) so the thread
             * shows both sides. Accepted sends only; a refused one
             * fabricates nothing. */
            ff_wiring_push_outgoing(&sh->wiring, FEED_PULSE, dest, NULL);
        }
    }
}

/* Encode + send a FLARE ("come find me") to the CURRENT resolved scope,
 * pushing the OUT feed item on an accepted send. The outbound quick signal
 * is a flare, not a pulse (the maintainer's "in send to crew we should have
 * flare not pulse") — but it rides the SAME S22(d) scope+send seam PULSE
 * used: shell_sig_dest resolves WHOLE_CREW -> broadcast and a member -> its
 * addressed id, and the wire body is `ff_proto_encode_flare` at
 * FF_FLARE_DEFAULT_DUR_S (the S10 default duration; the quick chip/row has
 * no duration picker). This does NOT touch the S10 self-flare "sending"
 * state (ff_flare_send_begin / the sender overlay) — that is the Radar
 * face's own "light my puck up" flare; this is an addressed come-find-me
 * signal into a conversation, exactly parallel to shell_pulse_to_scope.
 * No-op on a stale member scope (shell_sig_dest false) or a missing sender.
 * Does NOT touch the target/arm state — the caller owns that. */
static void shell_flare_to_scope(shell_t *sh)
{
    uint32_t dest = MC_ADDR_BROADCAST;
    if (!shell_sig_dest(sh, &dest)) return; /* stale member scope: send nothing */
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int const n = ff_proto_encode_flare(buf, sizeof buf, FF_FLARE_DEFAULT_DUR_S);
    if (n > 0 && sh->wiring.sender.send_private != NULL) {
        int const rc = sh->wiring.sender.send_private(sh->wiring.sender.ctx, dest, buf, (size_t)n);
        if (rc == 0) {
            /* My own send lands in the feed (FEED_DIR_OUT) so the thread
             * shows both sides. Accepted sends only; a refused one
             * fabricates nothing. */
            ff_wiring_push_outgoing(&sh->wiring, FEED_FLARE, dest, NULL);
        }
    }
}

/* Re-assert the S22(d) send holders from the open thread's scope (used
 * where "the open thread IS the send scope" applies — the thread chips and
 * the popup rows). Node 0 = whole crew, else that member. */
static void shell_scope_from_thread(shell_t *sh)
{
    sh->sig_target_kind = (sh->sig_thread_node == 0u) ? FF_TARGET_WHOLE_CREW : FF_TARGET_MEMBER;
    sh->sig_target_node = sh->sig_thread_node;
}

/* Project the Rally sub-view's WHERE/WHEN/Send state (S24 AC6). Re-derives
 * the SAME displayable-landmark list the send resolves against, so the rows
 * the screen shows and the place a `sel` sends to can never diverge. Also
 * re-validates `sig_rally_sel` (a place list that shrank, or On Me becoming
 * unusable, must not leave a stale/dangling selection) and composes the
 * Send echo from the same resolution the send uses. */
static void shell_project_rally(shell_t *sh, ff_app_rally_t *out)
{
    out->on_me_ok = sh->my_pos_ok;

    /* The displayable landmark rows, in pack order. */
    uint8_t n = 0;
    if (sh->pack_loaded && sh->pack != NULL && sh->pack->origin_known) {
        for (uint8_t i = 0; i < sh->pack->n_landmarks && n < FF_APP_RALLY_MAX_PLACES; ++i) {
            fp_landmark_t const *lm = &sh->pack->landmarks[i];
            if (!shell_rally_landmark_displayable(lm)) {
                continue;
            }
            shell_copy_str(out->place_names[n], sizeof(out->place_names[n]), lm->name);
            n++;
        }
    }
    out->place_count = n;

    /* Re-validate the selection against what is actually selectable now:
     * a landmark index past the list, or On Me while my position is
     * unknown, falls back to the first usable option (On Me if usable,
     * else the first landmark, else 0 with nothing sendable). */
    uint8_t sel = sh->sig_rally_sel;
    if (sel > n) {
        sel = 0u;
    }
    if (sel == 0u && !out->on_me_ok) {
        sel = (n > 0u) ? 1u : 0u;
    }
    sh->sig_rally_sel = sel;
    out->sel = sel;
    out->when = sh->sig_rally_when;

    /* The Send echo — composed from the SAME resolution the send uses, so
     * the payload preview cannot disagree with the wire. */
    ff_latlon_t pos;
    char const *base = NULL;
    out->can_send = shell_rally_resolve_sel(sh, &pos, &base);
    if (out->can_send && base != NULL) {
        shell_copy_str(out->echo_place, sizeof(out->echo_place), base);
    } else {
        out->echo_place[0] = '\0';
    }
    shell_copy_str(out->echo_when, sizeof(out->echo_when), shell_rally_when_echo(sh->sig_rally_when));

    /* Crew-wide confirm display: mirror the shell's armed flag while the
     * scope is the whole crew (a member rally sends on the first tap). */
    out->confirm_armed = sh->sig_rally_armed && (sh->sig_target_kind == FF_TARGET_WHOLE_CREW);
}

/**
 * FF_INTENT_SETTING_SET write-through (S16 slice e, AC8).
 *
 * Any nonzero `v.i` means true for the four bool-backed settings — the
 * struct fields are `bool`, and there is no documented narrower contract
 * than "nonzero is true" for an int payload crossing that seam.
 *
 * Every other field is validated against the range its own doc comment
 * states, and an OUT-OF-RANGE value is REJECTED — left exactly as it
 * was, never clamped to the nearest legal value. Clamping would silently
 * apply something the caller did not ask for; honest-data (CLAUDE.md)
 * says refuse it instead:
 *   - FF_SETTING_SHARE_MODE: [FF_SHARE_LIVE, FF_SHARE_GHOST] (ff_settings.h).
 *   - FF_SETTING_QUIET_FROM_MIN / _TO_MIN: [0, 1439], the local
 *     minutes-of-day domain ff_quiet_now's own doc states.
 *   - FF_SETTING_UTC_OFFSET_MIN: [FF_WALL_OFFSET_MIN_LO,
 *     FF_WALL_OFFSET_MIN_HI] (ff_wall.h) — the same real-world
 *     UTC-12:00..UTC+14:00 gate the wall clock itself validates offsets
 *     against, so a setting the shell would accept can never be one
 *     `ff_wall_resolve_offset` would then silently ignore.
 *   - FF_SETTING_WATER_MIN: the field's own type, uint16_t — "0 = off"
 *     is its only documented constraint, so anything a uint16_t can hold
 *     is valid.
 *   - FF_SETTING_MY_NAME: bounded/NUL-terminated into the field's
 *     documented [FF_SETTINGS_NAME_LEN] budget (truncated, not
 *     rejected — ff_settings.h already documents this layer's own
 *     bytes-as-given contract; slice e is the first writer through this
 *     seam and chooses to bound rather than refuse an overlong name).
 *   - FF_SETTING_COLORBLIND (S17 slice a): bool-backed, same "nonzero is
 *     true" convention as IMPERIAL/HAPTICS/NIGHT_GLOW — no range to reject.
 *
 * Persisted on CHANGE ONLY (S16 "Behavior": "saved on change, never
 * every tick") — every branch below compares the OLD value before
 * overwriting it, and the save call at the bottom is gated on that
 * comparison, not on "a SETTING_SET intent arrived". A NULL store is the
 * documented "no persistence" case (`ff_settings_save` itself no-ops on
 * one) and is not treated as a rejection: the in-memory setting still
 * applies.
 */
static void shell_setting_set(shell_t *sh, ff_intent_t const *in)
{
    ff_settings_t *s = &sh->settings;
    bool changed = false;

    switch (in->u.setting.id) {
    case FF_SETTING_IMPERIAL: {
        bool const v = (in->u.setting.v.i != 0);
        changed = (s->imperial != v);
        s->imperial = v;
        break;
    }
    case FF_SETTING_SHARE_MODE: {
        int32_t const v = in->u.setting.v.i;
        if (v < FF_SHARE_LIVE || v > FF_SHARE_GHOST) return; /* out of range: rejected, not clamped */
        changed = (s->share_mode != (uint8_t)v);
        s->share_mode = (uint8_t)v;
        break;
    }
    case FF_SETTING_HAPTICS: {
        bool const v = (in->u.setting.v.i != 0);
        changed = (s->haptics != v);
        s->haptics = v;
        break;
    }
    case FF_SETTING_NIGHT_GLOW: {
        bool const v = (in->u.setting.v.i != 0);
        changed = (s->night_glow != v);
        s->night_glow = v;
        break;
    }
    case FF_SETTING_COLORBLIND: {
        bool const v = (in->u.setting.v.i != 0);
        changed = (s->colorblind != v);
        s->colorblind = v;
        break;
    }
    case FF_SETTING_BRIGHTNESS: {
        /* #100 — CLAMPED, not rejected: a slider that drags past either end
         * should pin to the floor/ceiling, not silently drop the change (the
         * UTC stepper clamps for the same "no dead control" reason). The
         * floor is non-zero on purpose — never a black, unrecoverable
         * backlight (ff_settings.h). */
        int32_t v = in->u.setting.v.i;
        if (v < (int32_t)FF_BRIGHTNESS_MIN_PCT) v = (int32_t)FF_BRIGHTNESS_MIN_PCT;
        if (v > (int32_t)FF_BRIGHTNESS_MAX_PCT) v = (int32_t)FF_BRIGHTNESS_MAX_PCT;
        /* #bug1 — always apply the value to in-memory state so a projection
         * consumer (the device backlight, driven every tick from the
         * projected brightness_pct — see targets/esp32s3/main/app_main.c)
         * tracks a live drag. But COALESCE the NVS write: a TRANSIENT value
         * is a mid-drag preview and must NOT hit flash (a drag fires dozens
         * of them; persisting each would thrash the NVS wear-levelling for no
         * benefit — the intermediate values are never the settled setting).
         * Only the committed (non-transient) RELEASED value persists, and it
         * persists unconditionally: the live drag has already moved
         * brightness_pct, so the usual "changed?" guard would see no delta at
         * release and skip the one write that matters. */
        s->brightness_pct = (uint8_t)v;
        if (in->u.setting.transient) {
            return; /* live preview: applied, deliberately not persisted */
        }
        changed = true; /* committed: persist the settled value once */
        break;
    }
    case FF_SETTING_WATER_MIN: {
        int32_t const v = in->u.setting.v.i;
        if (v < 0 || v > (int32_t)UINT16_MAX) return;
        changed = (s->water_min != (uint16_t)v);
        s->water_min = (uint16_t)v;
        break;
    }
    case FF_SETTING_QUIET_FROM_MIN: {
        int32_t const v = in->u.setting.v.i;
        if (v < 0 || v > 1439) return;
        changed = (s->quiet_from_min != (uint16_t)v);
        s->quiet_from_min = (uint16_t)v;
        break;
    }
    case FF_SETTING_QUIET_TO_MIN: {
        int32_t const v = in->u.setting.v.i;
        if (v < 0 || v > 1439) return;
        changed = (s->quiet_to_min != (uint16_t)v);
        s->quiet_to_min = (uint16_t)v;
        break;
    }
    case FF_SETTING_UTC_OFFSET_MIN: {
        int32_t const v = in->u.setting.v.i;
        if (v < FF_WALL_OFFSET_MIN_LO || v > FF_WALL_OFFSET_MIN_HI) return;
        /* Unset -> set is itself a change worth persisting even when the
         * magnitude happens to coincide with the struct's zeroed default. */
        changed = (!s->utc_offset_set) || (s->utc_offset_min != (int16_t)v);
        s->utc_offset_min = (int16_t)v;
        s->utc_offset_set = true;
        break;
    }
    case FF_SETTING_MY_NAME: {
        char const *v = in->u.setting.v.s;
        if (v == NULL) return; /* nothing to copy: not owned, borrowed (ff_intent.h) */
        char tmp[FF_SETTINGS_NAME_LEN];
        memset(tmp, 0, sizeof(tmp)); /* every byte defined, so the memcmp below is honest */
        shell_copy_str(tmp, sizeof(tmp), v);
        changed = (memcmp(s->my_name, tmp, sizeof(tmp)) != 0);
        memcpy(s->my_name, tmp, sizeof(tmp));
        break;
    }
    }

    if (changed && sh->store != NULL) {
        ff_settings_save(s, sh->store);
    }
}

void ff_shell_intent(ff_shell_t *sh_pub, ff_intent_t const *in)
{
    if (sh_pub == NULL || in == NULL) return;
    shell_t *sh = shell_of(sh_pub);

    /* Routing rule 4: dispatch targets the VISIBLE face. The takeover
     * flag is ff_flare_t's single fact, read fresh at every dispatch —
     * ff_flare_tick clears it autonomously on expiry, so a cached copy
     * would outlive the fact it copied (ff_route_visible's doc). */
    bool const takeover_up = (ff_route_visible(&sh->route, sh->flare.takeover_active) == FF_APP_FACE_FLARE);

    switch (in->kind) {
    case FF_INTENT_SWIPE:
        /* While a takeover is up the swipe faces are not the visible
         * face, so they receive no intents (AC3b's routing half). The
         * modal-suppression and bounded-not-wrapping rules are the
         * route's own (AC1/AC2). */
        if (takeover_up) return;
        (void)ff_route_swipe(&sh->route, in->u.swipe_dir);
        return;

    case FF_INTENT_BACK:
        if (takeover_up) return; /* a tap where "<" was must not close the composer under a takeover */
        if (ff_route_pop_modal(&sh->route)) {
            /* Leaving the composer abandons its destination; the next
             * OPEN_COMPOSE re-resolves. (The T9 draft's lifecycle is
             * c3's; this only keeps the "who" from leaking across two
             * unrelated compose sessions.) */
            sh->compose_to_node = 0u;
            return;
        }
        /* S24 — no modal to pop: BACK steps the Signals sub-view stack by
         * exactly ONE level (the spec's "44px escapes"), following the
         * open chain so each close lands where it was reached from:
         *   RALLY  -> POPUP  (the popup it was opened from)
         *   POPUP  -> THREAD (the dimmed thread it sits over)
         *   THREAD -> INBOX
         *   PICKER -> INBOX
         * A BACK on the inbox itself — or on any other base face — stays a
         * no-op. `sig_thread_node` is only cleared on the return to INBOX
         * (POPUP/RALLY keep the scope so the thread underneath is intact).
         * Any pending rally-to-crew confirm disarms on a back step (an
         * intervening action, S22 AC4). */
        if (sh->route.base == FF_APP_FACE_SIGNALS && sh->sig_subview != FF_SIG_SUB_INBOX) {
            sh->sig_rally_armed = false;
            switch (sh->sig_subview) {
            case FF_SIG_SUB_RALLY:  sh->sig_subview = FF_SIG_SUB_POPUP; break;
            case FF_SIG_SUB_POPUP:  sh->sig_subview = FF_SIG_SUB_THREAD; break;
            case FF_SIG_SUB_THREAD:
            case FF_SIG_SUB_PICKER:
            default:
                sh->sig_subview     = FF_SIG_SUB_INBOX;
                sh->sig_thread_node = 0u;
                break;
            }
        }
        return;

    case FF_INTENT_OPEN_COMPOSE:
        if (takeover_up) return;
        /* push_modal enforces the rest: rejected over an existing modal
         * (one slot, never silently replaced — a half-typed draft is
         * exactly what that rule protects) and over an off-axis base. */
        if (ff_route_push_modal(&sh->route, FF_APP_FACE_COMPOSE)) {
            sh->compose_to_node = shell_compose_dest(sh, in->u.node_id);
            /* S16 slice c3: every OPEN_COMPOSE starts a FRESH session —
             * the same "resets every build" behavior scr_compose.c's old
             * `static ff_t9_t` had, now made explicit here since the
             * draft survives everything else (in particular a takeover
             * interruption, AC3b) rather than resetting on its own. */
            ff_t9_reset(&sh->compose_draft);
            /* S08 addendum — reset site #2 (open). A fresh predictive
             * session, re-bound to the current festpack words (reset clears
             * the binding, per ff_t9pred.h), and the predictive mode is the
             * default the composer opens in. */
            ff_t9pred_session_reset(&sh->compose_pred);
            ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
            sh->compose_mode = FF_APP_COMPOSE_PRED;
        }
        return;

    case FF_INTENT_OPEN_SETTINGS:
        if (takeover_up) return;
        if (!k_settings_renderer_exists) return; /* the judgment call — see the constant's comment */
        /* S21: the Settings face is one scrolling list with no page state to
         * reset — a fresh open always renders scrolled to the top (the list
         * container's own default scroll offset), no shell bookkeeping. */
        (void)ff_route_push_modal(&sh->route, FF_APP_FACE_SETTINGS);
        return;

    case FF_INTENT_OPEN_MAP:
        /* S09 [api]. Same push_modal machinery as OPEN_COMPOSE/
         * OPEN_SETTINGS above (rejected over an existing modal or an
         * off-axis base); no per-open state to reset (unlike Compose's
         * draft) since Map has none of its own — the view it renders
         * comes from `sh->view.map`'s projection, populated fresh every
         * tick like `now`/`radar`. */
        if (takeover_up) return;
        (void)ff_route_push_modal(&sh->route, FF_APP_FACE_MAP);
        return;

    case FF_INTENT_TAKEOVER_GO:
        /* A decision ABOUT the takeover screen: only meaningful while it
         * is the visible face. (ff_flare_go would no-op anyway with no
         * pending takeover; the gate keeps the routing statement true
         * rather than merely the outcome.) */
        if (!takeover_up) return;
        (void)ff_flare_go(&sh->flare);
        return;

    case FF_INTENT_TAKEOVER_DISMISS:
        if (!takeover_up) return;
        (void)ff_flare_dismiss_takeover(&sh->flare);
        return;

    case FF_INTENT_RELEASE_LOCK:
        /* Deliberately NOT gated on the takeover, and deliberately not
         * one branch shared with TAKEOVER_DISMISS: S10 Ruling 3. The
         * only real-world source of this intent while a takeover is
         * visible is the race the ruling exists for — "stop navigating"
         * tapped in the instant a new takeover arrives — and the correct
         * outcome is: lock released (the user's actual intent), takeover
         * left pending and shown (nothing swallowed unseen).
         * ff_flare_release_lock touches only the lock, by that ruling's
         * own construction. */
        (void)ff_flare_release_lock(&sh->flare);
        return;

    case FF_INTENT_FLARE_START:
        /* The Radar-face CLOSE-mode FLARE button (S16 slice c2). Gated on
         * the visible face like every other base-face control (routing
         * rule 4): the button only exists on the Radar tile, which is not
         * the visible face while a takeover is up. dur_s=0 -> core's own
         * FF_FLARE_DEFAULT_DUR_S; now_ms is the shell's own clock reading,
         * not lv_tick_get() — the screen no longer touches the clock at
         * all (ff_scr_radar_build dropped ff_flare_t* in this same
         * slice). */
        if (takeover_up) return;
        (void)ff_flare_send_begin(&sh->flare, 0, shell_now(sh));
        return;

    case FF_INTENT_FLARE_END:
        /* The sender overlay's CANCEL button (S16 slice c2). Deliberately
         * UNGATED on takeover_up — but NOT because the overlay renders
         * during a takeover; it doesn't (corrected here, PR #58 review).
         * `face_dispatch.c` dispatches the full-screen takeover INSTEAD
         * of `ff_scr_nav_build` whenever `takeover_active` is set, so the
         * sender overlay — built at the tail of `ff_scr_nav_build`, on
         * the puck, on top of whichever BASE face is showing
         * (scr_nav.c's "regardless of current face" note is about
         * Radar/Now/Signals, not the takeover) — is simply absent from
         * the screen for as long as a takeover owns it.
         *
         * The real reason to leave this ungated is the same race S10
         * Ruling 3 exists for (RELEASE_LOCK, just above): `sending` and
         * `takeover_active` are independent facts (ff_flare.h's
         * "Independent state" design) — I can be sending my own flare AND
         * have a different member's takeover arrive an instant later,
         * between the CANCEL tap landing and this intent dispatching.
         * Gate on takeover_up and that arrival silently swallows the
         * cancel; ungated, CANCEL still reaches ff_flare_send_cancel and
         * the new takeover is still shown — nothing lost either way. */
        (void)ff_flare_send_cancel(&sh->flare);
        return;

    case FF_INTENT_CANNED_REPLY:
        /* OMW / 5 MIN / PULSE (S16 slice c2, AC7). Reply context is the
         * newest feed item — `ff_feed_at(feed, 0)`, per
         * ff_wiring_send_canned_reply's documented contract — deliberately
         * NOT the composer's destination rule (shell_compose_dest above):
         * S16's Amendments (PR #54) draw this distinction explicitly.
         * NULL when the feed is empty, which ff_wiring_send_canned_reply
         * turns into MC_ADDR_BROADCAST itself. Gated on the visible face:
         * the reply chips live on the Signals tile. */
        if (takeover_up) return;
        /* S24 slice (c) — the thread quick chips (OMW / IN 5 MIN): with a
         * thread OPEN, the reply goes to the THREAD's scope ("the open
         * thread IS the send scope"), never to whoever happened to send
         * the newest feed item in some other conversation. The member
         * scope is re-validated paired at the send instant (the
         * shell_sig_dest belt-and-braces rule); a stale scope sends
         * NOTHING rather than silently broadcasting. */
        if (sh->route.base == FF_APP_FACE_SIGNALS && sh->route.modal == FF_APP_FACE_NONE &&
            sh->sig_subview == FF_SIG_SUB_THREAD) {
            uint32_t dest = MC_ADDR_BROADCAST; /* thread_node 0 = the CREW thread */
            if (sh->sig_thread_node != 0u) {
                if (!shell_member_paired(sh, sh->sig_thread_node)) return;
                dest = sh->sig_thread_node;
            }
            (void)ff_wiring_send_canned_reply_to(&sh->wiring, in->u.reply, dest);
            return;
        }
        {
            /* S24 amendment to the "newest feed item" rule: outgoing
             * items (FEED_DIR_OUT, from_node 0) now live in the feed too,
             * and a reply's context must stay the newest thing RECEIVED —
             * replying to my own just-sent "omw" would resolve its
             * from_node of 0 to a broadcast the user never chose. Skip
             * OUT items; the newest inbound item is the same item the
             * pre-S24 rule picked. */
            ff_feed_item_t const *ctx = NULL;
            uint8_t const fn = ff_feed_count(&sh->feed);
            for (uint8_t i = 0; i < fn; ++i) {
                ff_feed_item_t const *it = ff_feed_at(&sh->feed, i);
                if (it != NULL && it->dir != FEED_DIR_OUT) {
                    ctx = it;
                    break;
                }
            }
            (void)ff_wiring_send_canned_reply(&sh->wiring, in->u.reply, ctx);
        }
        return;

    case FF_INTENT_SEND_TEXT:
        /* The composer's SEND button (S16 slice c3 — the seam has emitted
         * this since c2, but there was nothing to send FROM until the
         * draft moved in here). Rejected while a takeover is up (AC3b,
         * routing rule 4: "a touch landing where SEND was does not
         * send") — the draft is left completely untouched in that case,
         * not partially consumed. */
        if (takeover_up) return;
        {
            /* S08 addendum — LOAD-BEARING EDGE: in predictive mode a live
             * candidate the user typed but has NOT accepted (no space, no
             * tap) must still be sent — hitting SEND on a predicted word
             * must not silently drop it. Fold the current candidate into
             * the draft first, so `ff_t9_text` below already contains it.
             * Honest-data: only a real engine candidate (`session_current`
             * != NULL) is inserted — never a fabricated key string, and
             * nothing at all on an honest no-match. */
            if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
                char const *cur = ff_t9pred_session_current(&sh->compose_pred);
                if (cur != NULL) {
                    (void)ff_t9_insert_text(&sh->compose_draft, cur);
                }
            }
            char const *text = ff_t9_text(&sh->compose_draft);
            if (text != NULL && text[0] != '\0') {
                /* The sender seam ff_wiring already owns (ff_wiring.h's
                 * `ff_wiring_sender_t`) — same vtable the canned replies
                 * send through, reused rather than a second `mc_send_text`
                 * call site. `compose_to_node == 0` is "no explicit
                 * destination", resolved to broadcast the same way every
                 * other send in this file resolves it (MC_ADDR_BROADCAST,
                 * not the literal 0 mc_send_text would misread). */
                uint32_t const dest = (sh->compose_to_node != 0u) ? sh->compose_to_node : MC_ADDR_BROADCAST;
                if (sh->wiring.sender.send_text != NULL) {
                    int const rc = sh->wiring.sender.send_text(sh->wiring.sender.ctx, dest, text);
                    if (rc == 0) {
                        /* S24 — the composer's sent text lands in the feed
                         * as FEED_DIR_OUT (S08's long-promised "sent item
                         * appears in feed", now with an honest direction so
                         * ff_inbox can side it). Accepted sends only. */
                        ff_wiring_push_outgoing(&sh->wiring, FEED_TEXT, dest, text);
                    }
                }
                /* A sent message ends the compose session — S08's "sent
                 * item appears in feed... " reads as returning to Signals
                 * to see it, not staying on an emptied composer. Draft
                 * and destination both reset, same as a fresh open. */
                ff_t9_reset(&sh->compose_draft);
                /* S08 addendum — reset site #3 (send): fresh predictive
                 * session, re-bound to the festpack words, back to the
                 * predictive default. (Moot for the popped modal, but keeps
                 * the three reset sites uniform.) */
                ff_t9pred_session_reset(&sh->compose_pred);
                ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
                sh->compose_mode = FF_APP_COMPOSE_PRED;
                (void)ff_route_pop_modal(&sh->route);
                sh->compose_to_node = 0u;
            }
            /* An empty draft (nothing typed) is a no-op, not a broadcast
             * of "" — SEND on an untouched composer must not fire. */
        }
        return;

    case FF_INTENT_T9_KEY:
        /* Mode-polymorphic (S08 addendum). PRED: one keypress = one letter,
         * fed to the predictive session (which rejects 0/1 itself). Every
         * other mode: multi-tap letter/punctuation key, 0-9 (S16 slice c3),
         * with the commit-window timing from the shell's own clock
         * (`shell_now`), never `lv_tick_get()`. Rejected during a takeover
         * like every other Compose control (AC3b). */
        if (takeover_up) return;
        if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
            (void)ff_t9pred_session_key(&sh->compose_pred, in->u.t9_key);
        } else {
            ff_t9_key(&sh->compose_draft, in->u.t9_key, shell_now(sh));
        }
        return;

    case FF_INTENT_T9_SPACE:
        /* PRED: SPACE ACCEPTS the current candidate — insert the predicted
         * word into the committed draft, then a space, then reset+rebind the
         * session for the next word (reset site #2's twin). On an honest
         * no-match (no current candidate) it falls to the existing behavior:
         * just commit a space. Other modes: the existing behavior. */
        if (takeover_up) return;
        if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
            char const *cur = ff_t9pred_session_current(&sh->compose_pred);
            if (cur != NULL) {
                (void)ff_t9_insert_text(&sh->compose_draft, cur);
                ff_t9_space(&sh->compose_draft);
                ff_t9pred_session_reset(&sh->compose_pred);
                ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
            } else {
                ff_t9_space(&sh->compose_draft);
            }
        } else {
            ff_t9_space(&sh->compose_draft);
        }
        return;

    case FF_INTENT_T9_BACKSPACE:
        /* PRED: remove from the in-progress predicted word FIRST; only when
         * that word is already empty (session has no digits) does backspace
         * fall through to the committed draft. `session_backspace` returns
         * false exactly when the session is empty, which is the clean seam
         * for that fallthrough. Other modes: backspace the draft directly. */
        if (takeover_up) return;
        if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
            if (!ff_t9pred_session_backspace(&sh->compose_pred)) {
                ff_t9_backspace(&sh->compose_draft);
            }
        } else {
            ff_t9_backspace(&sh->compose_draft);
        }
        return;

    case FF_INTENT_T9_CYCLE:
        /* S08 addendum — advance the predicted-word selection (the › chip).
         * PRED-only; a well-defined no-op with <2 candidates (the engine's
         * own contract). */
        if (takeover_up) return;
        if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
            ff_t9pred_session_cycle(&sh->compose_pred);
        }
        return;

    case FF_INTENT_T9_SELECT:
        /* S08 addendum — tap a specific candidate chip: jump the selection
         * to that index (carried in u.t9_key, per ff_intent.h). PRED-only;
         * the engine clamps a past-the-end index and no-ops on no-match. */
        if (takeover_up) return;
        if (sh->compose_mode == FF_APP_COMPOSE_PRED) {
            ff_t9pred_session_select(&sh->compose_pred, in->u.t9_key);
        }
        return;

    case FF_INTENT_T9_MODE:
        /* Cycles the keypad page PRED -> ABC -> 123 -> SYM -> PRED (S08
         * predictive addendum extends the old ABC/123/SYM cycle; PRED is
         * the default the composer opens in). No `default:` so a future
         * mode addition trips -Wswitch here, this file's standing rule. */
        if (takeover_up) return;
        switch (sh->compose_mode) {
        case FF_APP_COMPOSE_PRED: sh->compose_mode = FF_APP_COMPOSE_ABC; break;
        case FF_APP_COMPOSE_ABC: sh->compose_mode = FF_APP_COMPOSE_123; break;
        case FF_APP_COMPOSE_123: sh->compose_mode = FF_APP_COMPOSE_SYM; break;
        case FF_APP_COMPOSE_SYM: sh->compose_mode = FF_APP_COMPOSE_PRED; break;
        }
        return;

    case FF_INTENT_T9_INSERT:
        /* Atomic literal insert — the 123 page's digits and the SYM
         * page's ASCII shortcuts both go through this one path (both
         * used `ff_t9_insert_text` directly before this slice; see
         * scr_compose.c's compose_key_pressed). `in->u.text` is borrowed
         * for this call only (ff_intent.h, "Payload ownership") —
         * `ff_t9_insert_text` copies every byte it keeps before
         * returning, so nothing here retains the pointer past this
         * statement. */
        if (takeover_up) return;
        (void)ff_t9_insert_text(&sh->compose_draft, in->u.text);
        return;

    case FF_INTENT_SETTING_SET:
        /* Settings write-through + persistence (S16 slice e, AC8; the
         * emit site is S11 slice b's scr_settings.c). Gated on the
         * visible face like every other core-mutating intent (routing
         * rule 4) — the Settings modal suppresses swipe like any other
         * modal, so this can only legitimately fire while Settings is
         * the visible face, and a takeover (which can only arrive while
         * some OTHER face was visible, since opening the modal itself
         * requires no takeover be up) preempts it exactly like it does
         * every other control. */
        if (takeover_up) return;
        shell_setting_set(sh, in);
        return;

    case FF_INTENT_CALIBRATE_TOUCH:
        /* S21 §3 — the Settings "CALIBRATE TOUCH" row. Gated on the takeover
         * exactly like SETTING_SET above: the row only exists on the Settings
         * modal, so this can only fire while Settings is visible. Runs the
         * injected device calibrate hook (NULL on the sim / any target with
         * no touch panel -> a safe no-op, tests stay green). On device the
         * hook runs the crosshair capture and applies the solved transform to
         * the live touch path; here the shell writes it into ff_settings and
         * persists on change, so calibration survives reboot via NVS (S21 §4)
         * the same way every other setting does. */
        if (takeover_up) return;
        if (sh->calibrate_touch != NULL) {
            ff_touchcal_t cal;
            memset(&cal, 0, sizeof(cal));
            if (sh->calibrate_touch(sh->calibrate_touch_user, &cal) && cal.valid) {
                ff_settings_t *s = &sh->settings;
                bool const changed = (!s->touch_calibrated) || (s->touch_ax != cal.ax) || (s->touch_bx != cal.bx) ||
                                     (s->touch_ay != cal.ay) || (s->touch_by != cal.by);
                s->touch_ax = cal.ax;
                s->touch_bx = cal.bx;
                s->touch_ay = cal.ay;
                s->touch_by = cal.by;
                s->touch_calibrated = true;
                if (changed && sh->store != NULL) {
                    ff_settings_save(s, sh->store);
                }
            }
        }
        return;

    case FF_INTENT_SIG_SELECT_MEMBER:
        /* S22 slice b (S24 note: the S22 screen that emitted this is
         * retired — nothing emits it today — but the handler stays as
         * the S22(d) send machinery's programmatic "set the send scope"
         * seam, VALIDATED against the roster exactly as before: paired
         * members only, never a trusted tap). Mirrored into the live
         * view immediately so a same-frame read sees it, matching the
         * old ff_sigview_target_select behavior. Takeover-gated like
         * every face intent. */
        if (takeover_up) return;
        /* Changing the target disarms any pending rally-to-WHOLE_CREW
         * confirm (S22 AC4 "anything else disarms"). */
        sh->sig_rally_armed = false;
        if (shell_member_paired(sh, in->u.node_id)) {
            sh->sig_target_kind = FF_TARGET_MEMBER;
            sh->sig_target_node = in->u.node_id;
            sh->view.signals.target_kind = FF_TARGET_MEMBER;
            sh->view.signals.target_node = in->u.node_id;
        }
        return;

    case FF_INTENT_SIG_CLEAR_TARGET:
        /* S22 slice b — back to WHOLE_CREW (see the S24 note above: kept
         * as the programmatic clear for the S22(d) machinery). */
        if (takeover_up) return;
        sh->sig_rally_armed = false; /* AC4 — an intervening action disarms */
        sh->sig_target_kind = FF_TARGET_WHOLE_CREW;
        sh->sig_target_node = 0u;
        sh->view.signals.target_kind = FF_TARGET_WHOLE_CREW;
        sh->view.signals.target_node = 0u;
        return;

    case FF_INTENT_INBOX_OPEN_THREAD:
    case FF_INTENT_INBOX_PICK:
        /* S24 — a conversation row tap (OPEN_THREAD) or a recipient-picker
         * row tap (PICK). u.node_id is the conversation key: 0 = CREW,
         * nonzero = that member. Both establish the SAME open-thread scope
         * (set the S22(d) send holders, mark that conversation read, set
         * sig_thread_node), and differ ONLY in the sub-view they land on:
         *  - OPEN_THREAD -> the THREAD screen (read the conversation).
         *  - PICK        -> the action POPUP over that thread (the spec's
         *    "then the action popup scoped to the pick" — slice d). The
         *    thread scope is fully established either way, so closing the
         *    popup reveals the real thread underneath.
         * A tap naming an unknown/unpaired member is rejected whole — no
         * scope change, no mark-read, no navigation (the roster, not the
         * tap, is trusted — the SIG_SELECT precedent). */
        if (takeover_up) return;
        sh->sig_rally_armed = false; /* an intervening action disarms (S22 AC4) */
        if (in->u.node_id != 0u && !shell_member_paired(sh, in->u.node_id)) {
            return;
        }
        sh->sig_thread_node = in->u.node_id;
        shell_scope_from_thread(sh);
        (void)ff_inbox_mark_thread_read(&sh->feed,
                                        (in->u.node_id == 0u) ? FF_CONV_CREW : FF_CONV_MEMBER,
                                        in->u.node_id);
        sh->view.signals.target_kind = sh->sig_target_kind;
        sh->view.signals.target_node = sh->sig_target_node;
        sh->sig_subview = (in->kind == FF_INTENT_INBOX_PICK) ? FF_SIG_SUB_POPUP : FF_SIG_SUB_THREAD;
        return;

    case FF_INTENT_INBOX_NEW:
        /* S24 — the `+` FAB. From the INBOX it opens the recipient picker
         * (the "first opens a scope step" flow); from an OPEN THREAD it
         * skips straight to the action POPUP scoped to that thread (the
         * spec's "a thread's FAB skips straight to the popup"). Any other
         * sub-view is a stray tap (the FAB only renders on inbox/thread). */
        if (takeover_up) return;
        if (sh->sig_subview == FF_SIG_SUB_INBOX) {
            sh->sig_rally_armed = false;
            sh->sig_subview = FF_SIG_SUB_PICKER;
        } else if (sh->sig_subview == FF_SIG_SUB_THREAD) {
            sh->sig_rally_armed = false;
            shell_scope_from_thread(sh); /* the popup acts on the thread's scope */
            sh->view.signals.target_kind = sh->sig_target_kind;
            sh->view.signals.target_node = sh->sig_target_node;
            sh->sig_subview = FF_SIG_SUB_POPUP;
        }
        return;

    case FF_INTENT_INBOX_POPUP_COMPOSE:
        /* S24 AC5 — the popup's Compose row: open the S08 composer at the
         * scope, dropping the sub-view back to THREAD so the composer sits
         * over the real thread (SEND/BACK then land there). Mirrors
         * SIG_COMPOSE's fresh-session setup. Only from the popup. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_POPUP) return;
        sh->sig_rally_armed = false;
        if (ff_route_push_modal(&sh->route, FF_APP_FACE_COMPOSE)) {
            uint32_t to = 0u;
            if (sh->sig_thread_node != 0u && shell_member_paired(sh, sh->sig_thread_node)) {
                to = sh->sig_thread_node;
            }
            sh->compose_to_node = to;
            ff_t9_reset(&sh->compose_draft);
            ff_t9pred_session_reset(&sh->compose_pred);
            ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
            sh->compose_mode = FF_APP_COMPOSE_PRED;
            sh->sig_subview = FF_SIG_SUB_THREAD; /* under the modal */
        }
        return;

    case FF_INTENT_INBOX_POPUP_PULSE:
        /* S24 AC5 — the popup's (retired) Pulse row: pulse the scope
         * immediately, then close the popup back to the thread (the OUT
         * pulse shows there). Only from the popup. No screen emits this now
         * (the row sends a FLARE — see FF_INTENT_INBOX_POPUP_FLARE below);
         * kept as the outbound-pulse programmatic seam. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_POPUP) return;
        sh->sig_rally_armed = false;
        shell_scope_from_thread(sh);
        shell_pulse_to_scope(sh);
        sh->sig_subview = FF_SIG_SUB_THREAD;
        return;

    case FF_INTENT_INBOX_POPUP_FLARE:
        /* The popup's third row: FLARE ("come find me") to the scope
         * immediately, then close the popup back to the thread (the OUT
         * flare shows there). Same shape as POPUP_PULSE above, only the wire
         * body differs (a flare, not a pulse). Only from the popup. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_POPUP) return;
        sh->sig_rally_armed = false;
        shell_scope_from_thread(sh);
        shell_flare_to_scope(sh);
        sh->sig_subview = FF_SIG_SUB_THREAD;
        return;

    case FF_INTENT_INBOX_POPUP_RALLY:
        /* S24 AC5 — the popup's Rally row: open the Rally sub-view. Reset
         * the Rally selection to a selectable default (On Me if my position
         * is known, else the first landmark; WHEN back to Now). Only from
         * the popup. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_POPUP) return;
        sh->sig_rally_armed = false;
        shell_scope_from_thread(sh);
        sh->sig_rally_sel  = sh->my_pos_ok ? 0u : 1u; /* projection re-validates against the real place list */
        sh->sig_rally_when = FF_RALLY_WHEN_NOW;
        sh->sig_subview = FF_SIG_SUB_RALLY;
        return;

    case FF_INTENT_RALLY_SELECT_PLACE:
        /* S24 AC6 — a WHERE radio-row tap. u.rally_idx: 0 = On Me,
         * 1..place_count = landmark. An On Me pick with my position
         * unknown is rejected (the row is disabled — never a fabricated
         * position). The projection re-validates the index against the
         * live place list, so an out-of-range index self-corrects. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_RALLY) return;
        sh->sig_rally_armed = false; /* changing the place disarms a crew confirm (S22 AC4) */
        if (in->u.rally_idx == 0u && !sh->my_pos_ok) return; /* On Me disabled */
        sh->sig_rally_sel = in->u.rally_idx;
        return;

    case FF_INTENT_RALLY_CYCLE_WHEN:
        /* S24 AC6 — the WHEN chip: Now -> +15m -> +30m -> Now. */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_RALLY) return;
        sh->sig_rally_armed = false; /* changing WHEN disarms a crew confirm */
        switch (sh->sig_rally_when) {
        case FF_RALLY_WHEN_NOW: sh->sig_rally_when = FF_RALLY_WHEN_15; break;
        case FF_RALLY_WHEN_15:  sh->sig_rally_when = FF_RALLY_WHEN_30; break;
        case FF_RALLY_WHEN_30:
        default:                sh->sig_rally_when = FF_RALLY_WHEN_NOW; break;
        }
        return;

    case FF_INTENT_RALLY_SEND:
        /* S24 AC6 — the Send button. Encodes the rally to the scope at the
         * selected place + WHEN (the WHEN rides in the NAME), then pops to
         * the thread. A crew-wide rally arms on the first tap and sends on
         * the second (S22 AC4, the flare-armed precedent); a member rally
         * sends on the first tap. Sends NOTHING (and does not navigate) if
         * the place cannot be resolved (On Me with unknown position, or a
         * name that cannot fit the wire with its WHEN suffix). */
        if (takeover_up) return;
        if (sh->sig_subview != FF_SIG_SUB_RALLY) return;
        shell_scope_from_thread(sh);
        {
            bool const whole_crew = (sh->sig_target_kind == FF_TARGET_WHOLE_CREW);
            if (whole_crew) {
                bool const armed_live =
                    sh->sig_rally_armed && (shell_now(sh) - sh->sig_rally_armed_ms) <= FF_SIG_RALLY_CONFIRM_MS;
                if (!armed_live) {
                    sh->sig_rally_armed = true; /* first tap: arm, do not send */
                    sh->sig_rally_armed_ms = shell_now(sh);
                    return;
                }
            }
            uint32_t dest = MC_ADDR_BROADCAST;
            if (!shell_sig_dest(sh, &dest)) return; /* stale member scope: send nothing */
            ff_latlon_t pos;
            char const *base = NULL;
            if (!shell_rally_resolve_sel(sh, &pos, &base)) return; /* place unresolvable: send nothing */
            char name[FF_PROTO_RALLY_NAME_MAX + 1];
            if (!shell_rally_compose_name(base, sh->sig_rally_when, name, sizeof name)) return;
            uint8_t buf[FF_PROTO_MAX_PAYLOAD];
            int const n = ff_proto_encode_rally(buf, sizeof buf, pos, name);
            if (n > 0 && sh->wiring.sender.send_private != NULL) {
                int const rc = sh->wiring.sender.send_private(sh->wiring.sender.ctx, dest, buf, (size_t)n);
                if (rc == 0) {
                    /* Outgoing rally in the feed, carrying the SAME name that
                     * went on the wire (both sides of the thread; accepted
                     * sends only). */
                    ff_wiring_push_outgoing(&sh->wiring, FEED_RALLY, dest, name);
                }
            }
            sh->sig_rally_armed = false;
            sh->sig_subview = FF_SIG_SUB_THREAD; /* pop to the thread to see the sent rally */
        }
        return;

    case FF_INTENT_SIG_PULSE:
        /* S22 slice d — PULSE (empty-body ping) to the current target. Sends
         * on the first tap for both WHOLE_CREW and a member (only RALLY-to-
         * WHOLE_CREW confirms). Any action other than a second RALLY tap
         * disarms a pending rally confirm. No screen emits this now (the 1:1
         * quick chip sends a FLARE — see FF_INTENT_SIG_FLARE below); kept as
         * the outbound-pulse programmatic seam. */
        if (takeover_up) return;
        sh->sig_rally_armed = false;
        {
            /* S24 — with a thread (or the popup/rally over it) open, the
             * scope IS the thread: re-assert the S22(d) holders from the
             * thread key at the send instant, so a pulse can never chase a
             * target the user is not looking at. */
            bool const thread_open = shell_scope_thread_open(sh);
            if (thread_open) {
                shell_scope_from_thread(sh);
            }
            shell_pulse_to_scope(sh);
            /* S22 AC3's reset-to-WHOLE_CREW belonged to the retired target-
             * line screen. With a thread open the open thread IS the scope
             * (S24's no-desync rule) — resetting would silently re-aim the
             * chips still under the user's thumb, so the scope stays. */
            if (!thread_open) {
                shell_sig_reset_target(sh); /* AC3 (no thread open) */
            }
        }
        return;

    case FF_INTENT_SIG_FLARE:
        /* The 1:1 thread's quick chip: FLARE ("come find me") to the current
         * target. Same scope/target handling as SIG_PULSE above (send on the
         * first tap; the open thread IS the scope when one is open; reset to
         * WHOLE_CREW only with no thread open) — only the wire body differs
         * (a flare, not a pulse). */
        if (takeover_up) return;
        sh->sig_rally_armed = false;
        {
            bool const thread_open = shell_scope_thread_open(sh);
            if (thread_open) {
                shell_scope_from_thread(sh);
            }
            shell_flare_to_scope(sh);
            if (!thread_open) {
                shell_sig_reset_target(sh); /* AC3 (no thread open) */
            }
        }
        return;

    case FF_INTENT_SIG_RALLY:
        /* S22 slice d — RALLY (gather + place) to the current target.
         *
         * Confirm gate (AC4): a rally to WHOLE_CREW is the one loud
         * broadcast, so the FIRST tap ARMS a confirm (rendered on the
         * button) instead of sending; only a SECOND tap within
         * FF_SIG_RALLY_CONFIRM_MS sends. A rally to a single member sends on
         * the first tap (no confirm). The place is the sender's own location
         * (`shell_rally_place`, SPEC-GAP MVP — see the spec Questions). */
        if (takeover_up) return;
        {
            bool const whole_crew = (sh->sig_target_kind == FF_TARGET_WHOLE_CREW);
            if (whole_crew) {
                bool const armed_live =
                    sh->sig_rally_armed && (shell_now(sh) - sh->sig_rally_armed_ms) <= FF_SIG_RALLY_CONFIRM_MS;
                if (!armed_live) {
                    /* First tap (or a lapsed arm): ARM, do not send. */
                    sh->sig_rally_armed = true;
                    sh->sig_rally_armed_ms = shell_now(sh);
                    return;
                }
                /* Second tap within the window: fall through and send. */
            }
            sh->sig_rally_armed = false;

            uint32_t dest = MC_ADDR_BROADCAST;
            if (!shell_sig_dest(sh, &dest)) return; /* stale member target: send nothing */
            ff_latlon_t pos;
            char const *name = FF_SIG_RALLY_DEFAULT_NAME;
            if (!shell_rally_place(sh, &pos, &name)) return; /* location unknown: send nothing */
            uint8_t buf[FF_PROTO_MAX_PAYLOAD];
            int const n = ff_proto_encode_rally(buf, sizeof buf, pos, name);
            if (n > 0 && sh->wiring.sender.send_private != NULL) {
                int const rc = sh->wiring.sender.send_private(sh->wiring.sender.ctx, dest, buf, (size_t)n);
                if (rc == 0) {
                    /* S24 — outgoing rally in the feed, carrying the same
                     * place name that went on the wire (both sides of the
                     * thread; accepted sends only). */
                    ff_wiring_push_outgoing(&sh->wiring, FEED_RALLY, dest, name);
                }
                shell_sig_reset_target(sh); /* AC3 */
            }
        }
        return;

    case FF_INTENT_SIG_COMPOSE:
        /* S22 slice d — COMPOSE opens the composer with its TO set to the
         * current Signals target, then switches to the compose face. The
         * composer's OWN SEND (existing T9 SEND_TEXT path) does the text
         * send; this intent only navigates + sets the destination the
         * composer already reads (`compose_to_node`), so it deliberately
         * does NOT reset the Signals target (that is the AC3 reset for a
         * direct PULSE/RALLY send, not for opening a modal). WHOLE_CREW maps
         * to broadcast (0); a member target maps to that member, re-checked
         * paired. Mirrors FF_INTENT_OPEN_COMPOSE's fresh-session setup. */
        if (takeover_up) return;
        sh->sig_rally_armed = false;
        if (ff_route_push_modal(&sh->route, FF_APP_FACE_COMPOSE)) {
            uint32_t to = 0u;
            if (sh->sig_target_kind == FF_TARGET_MEMBER) {
                ff_crew_member_t const *m = ff_crew_find(&sh->crew, sh->sig_target_node);
                to = (m != NULL && m->paired) ? sh->sig_target_node : 0u;
            }
            sh->compose_to_node = to;
            ff_t9_reset(&sh->compose_draft);
            ff_t9pred_session_reset(&sh->compose_pred);
            ff_t9pred_session_set_extra(&sh->compose_pred, sh->compose_extra, sh->compose_extra_n);
            sh->compose_mode = FF_APP_COMPOSE_PRED;
        }
        return;

    /* --- deliberate no-ops until their owning slice lands ------------ */
    case FF_INTENT_MARK_FEED_READ: /* c2 relic — S24 replaced the face-view clear with per-thread
                                     * mark-read on open (INBOX_OPEN_THREAD/PICK above); nothing
                                     * emits this and a whole-feed clear would zero every badge. */
    case FF_INTENT_SELECT_CREW:    /* c2 — radar tap-cycle */
    case FF_INTENT_SELECT_RALLY:   /* still unbuilt: ff_crew_select_rally does not exist yet
                                     * (core/include/ff_crew.h's own documented deviation —
                                     * a rally point doesn't fit ff_crew_member_t, deferred to
                                     * S06/S08). signals_rally_tap_cb emits this intent as of
                                     * c2 (S08 spec: "Rally row tap -> sets rally as radar/map
                                     * target"), and the shell has nothing to call yet — wiring
                                     * the emit site now means the eventual handler is the
                                     * only piece still missing, not a second UI change too. */
        return;
    }
    /* No default: -Wswitch under -Werror flags any new ff_intent_kind_t
     * member left unhandled (the S16 fixture_view.c trap, avoided). A
     * corrupted out-of-enum value falls through to here — a no-op. */
}

void ff_shell_intent_sink(void *user, ff_intent_t const *in)
{
    ff_shell_intent((ff_shell_t *)user, in);
}

bool ff_shell_pair(ff_shell_t *sh_pub, uint32_t node_id, bool paired)
{
    if (sh_pub == NULL) return false;

    /* THE one place a roster slot may be created (shell_pair, shared —
     * on sim only — with the opt-in --dev-trust-all NodeInfo branch).
     * Reachable only from a user action on device; nothing in the seven
     * inbound callbacks calls it there. */
    return shell_pair(shell_of(sh_pub), node_id, paired);
}

void ff_shell_set_my_pos(ff_shell_t *sh_pub, ff_latlon_t pos)
{
    if (sh_pub == NULL) return;
    shell_t *sh = shell_of(sh_pub);
    sh->my_pos = pos;
    sh->my_pos_ok = true;
}

void ff_shell_clear_my_pos(ff_shell_t *sh_pub)
{
    if (sh_pub == NULL) return;
    shell_of(sh_pub)->my_pos_ok = false;
}

void ff_shell_set_sender(ff_shell_t *sh_pub, ff_wiring_sender_t sender)
{
    if (sh_pub == NULL) return;
    shell_of(sh_pub)->wiring.sender = sender;
}

void ff_shell_set_heading(ff_shell_t *sh_pub, float heading_deg)
{
    if (sh_pub == NULL) return;
    shell_of(sh_pub)->heading_deg = heading_deg;
}

ff_shell_link_t ff_shell_link(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? FF_SHELL_LINK_NONE : shell_of_const(sh_pub)->link;
}

uint32_t ff_shell_my_node_id(ff_shell_t const *sh_pub)
{
    if (sh_pub == NULL) return 0u;
    shell_t const *sh = shell_of_const(sh_pub);
    return sh->has_my_node_id ? sh->my_node_id : 0u;
}

ff_crew_t const *ff_shell_crew(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? NULL : &shell_of_const(sh_pub)->crew;
}

ff_heard_t const *ff_shell_heard(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? NULL : &shell_of_const(sh_pub)->heard;
}

ff_feed_t const *ff_shell_feed(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? NULL : &shell_of_const(sh_pub)->feed;
}

ff_flare_t const *ff_shell_flare(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? NULL : &shell_of_const(sh_pub)->flare;
}

ff_settings_t const *ff_shell_settings(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? NULL : &shell_of_const(sh_pub)->settings;
}

uint32_t ff_shell_compose_to_node(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? 0u : shell_of_const(sh_pub)->compose_to_node;
}

uint32_t ff_shell_wall_rejected_relatches(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? 0u : ff_wall_trust_rejected_count(&shell_of_const(sh_pub)->wall);
}

uint32_t ff_shell_replay_overflow_count(ff_shell_t const *sh_pub)
{
    return (sh_pub == NULL) ? 0u : shell_of_const(sh_pub)->replay_overflow;
}

/* ---------------------------------------------------------------------
 * Sim-only dev affordances (S16 AC6, slice b2) — see ff_shell.h
 * ------------------------------------------------------------------- */
#if defined(FF_TARGET_SIM)

void ff_shell_dev_trust_all(ff_shell_t *sh_pub, bool enabled)
{
    if (sh_pub == NULL) return;
    shell_of(sh_pub)->dev_trust_all = enabled;
}

bool ff_shell_dev_wall_observe(ff_shell_t *sh_pub, int64_t unix_now_s)
{
    if (sh_pub == NULL) return false;
    shell_t *sh = shell_of(sh_pub);
    /* Same shape as a live rx_time observation: unconditional in both
     * directions, still gated by ff_wall_observe's plausibility window
     * — a wildly wrong host clock is rejected, not latched. S18 slice a:
     * offered as TRUSTED, not BOOTSTRAP — the header comment's own
     * rationale ("the desktop the sim runs on genuinely knows what time
     * it is") is exactly the TRUSTED case, and this preserves the
     * pre-existing "unconditional both directions" bench behavior; at
     * BOOTSTRAP tier a second `wall` ctl call disagreeing with the first
     * would now be silently refused by the trust gate, breaking the bench
     * time-travel affordance this exists for. */
    return ff_wall_observe(&sh->wall, unix_now_s, shell_now(sh), FF_WALL_TRUST_TRUSTED) != FF_WALL_OBS_REJECTED;
}

#endif /* FF_TARGET_SIM */
