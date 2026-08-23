/**
 * ff_flare.h — S10 (slice a): the flare come-find-me state machine.
 *
 * Spec: docs/specs/S10-flare.md (this slice: "state machine + tests";
 * takeover/sender UI is slice b, gated on the S06 app shell).
 *
 * Pure C11, no I/O, no allocation, no clock-reading of its own: every entry
 * point that needs "now" takes it as an explicit `uint32_t now_ms`
 * parameter rather than storing an `ff_clock_t` — the caller (app tick,
 * or a meshclient rx callback) always already has that timestamp in hand
 * at the point it calls in, and this keeps `ff_flare_t` a plain,
 * dependency-free struct (same "explicit now_ms in, no hidden clock"
 * shape `ff_radar_compute` uses in S06 for the same reason). Safe on the
 * stack or in a static.
 *
 * ## Dependency-light by design (documented per the task brief)
 *  - **No `ff_crew.h` include.** "Is this sender paired?" is passed in as
 *    a plain `bool paired` argument to `ff_flare_on_flare_rx` — the caller
 *    (app layer) is the one who already has an `ff_crew_t` and calls
 *    something like `ff_crew_upsert(...)->paired` before forwarding here.
 *    A callback/vtable seam was considered and rejected: pairing is a
 *    single cheap synchronous lookup with no I/O, so a callback would just
 *    be indirection without buying anything, and a plain `bool` is what
 *    keeps this module honestly zero-dependency (extraction-grade, like
 *    meshclient/ff_proto — see docs/ARCHITECTURE.md's reusable-libraries
 *    table) rather than merely *claiming* to be.
 *  - **No `ff_proto.h` include.** Decoding wire bytes into a `dur_s` (for
 *    FLARE) or recognizing FLARE_END is `ff_proto_decode`'s job, already
 *    done by the caller before it reaches this module; conversely, this
 *    module never emits wire bytes itself — `ff_flare_result_t.intent`
 *    tells the caller *what* to send (SEND_FLARE with `dur_s`, or
 *    SEND_FLARE_END), and the caller is the one who already owns
 *    `ff_proto_encode_flare`/`_flare_end` + `mc_send_private`. This is the
 *    "consume, don't duplicate" instruction from the task brief: the
 *    encode/decode logic lives in exactly one place (ff_proto.c), and this
 *    module only ever sees/produces already-parsed values.
 *  - **No `ff_crew.h`/meshclient dependency for the LOCKED selection
 *    either.** `ff_flare_locked_node()` returns the locked node id (or 0)
 *    for S06's radar/crew selection code to *consult* — this module never
 *    reaches into `ff_crew_t` to enforce the lock itself. Wiring
 *    `ff_crew_select_next` to no-op while locked (spec AC3) is S06's job
 *    once its shell exists; this slice only guarantees the accessor is
 *    correct (see test_flare.c's S10_AC3_* tests).
 *
 * ## Judgment calls (flagged per AGENTS.md; see the PR body for the same
 * notes)
 *  1. **Expiry boundary is INCLUSIVE.** `ff_flare_tick(f, now_ms)` treats
 *     `now_ms == expiry_ms` as already expired (fires the SENDING auto-end
 *     / RECEIVED-or-LOCKED unlock at that exact tick, not one tick later).
 *     Chosen to match `ff_crew_freshness`'s own "boundary rolls forward"
 *     convention (S02: age exactly 45000ms is already STALE, not LIVE) —
 *     "expiry" reads naturally as the instant the flare stops being valid,
 *     not the last instant it's still valid.
 *  2. **`ff_flare_send_begin` is unconditional.** It transitions to
 *     SENDING from *any* prior state (including overwriting an in-progress
 *     RECEIVED/LOCKED takeover), not just from IDLE. The spec doesn't
 *     restrict which face/state the send affordance is reachable from —
 *     that's a UI-layer call for S06/S10-slice-b to make (e.g. hiding the
 *     FLARE button during a takeover) — so the state machine itself stays
 *     permissive rather than guessing a restriction into existence.
 *  3. **A newly-received paired FLARE always lands in RECEIVED, never
 *     preserves LOCKED.** "Newest flare wins the takeover" (spec) is
 *     interpreted as: a genuinely new sender's takeover is a fresh
 *     decision the user hasn't made GO/DISMISS on yet, so it always starts
 *     unlocked — even if the *previous* takeover had been GO'd to LOCKED.
 *     Locking was a decision about the previous sender, not a sticky mode.
 *  4. **A newly-received paired FLARE also overrides an active SENDING
 *     state** (the state machine has exactly one "current flare" slot, and
 *     receive takeovers are specified as happening "regardless of current
 *     face"). This does NOT emit a SEND_FLARE_END for the caller's own
 *     in-flight send — the local UI stops showing "you are flaring", but
 *     the network-visible FLARE from this node isn't cancelled by this
 *     path (the user can still see and use `ff_flare_send_cancel` results
 *     from before the override if the app kept them; in practice this is a
 *     rare double-flare edge case, not a primary flow). Noted here rather
 *     than silently guessed at.
 */
#ifndef FF_FLARE_H
#define FF_FLARE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default flare duration when the caller doesn't specify one (`dur_s ==
 * 0` passed to `ff_flare_send_begin`), per spec ("expiry = now + dur,
 * default 300 s"). Receive-side `dur_s` (from a decoded wire FLARE) is
 * always used as-is, never defaulted — see this header's top comment. */
#define FF_FLARE_DEFAULT_DUR_S ((uint16_t)300u)

typedef enum {
    FF_FLARE_IDLE = 0,
    FF_FLARE_SENDING,
    FF_FLARE_RECEIVED,
    FF_FLARE_LOCKED,
} ff_flare_state_t;

/** Outbound network intent a transition wants the caller to act on. This
 * module never touches wire bytes or a transport — the caller already
 * owns `ff_proto_encode_flare`/`_flare_end` + `mc_send_private`(see this
 * header's top comment). */
typedef enum {
    FF_FLARE_INTENT_NONE = 0,
    FF_FLARE_INTENT_SEND_FLARE,     /* caller: ff_proto_encode_flare(..., dur_s), broadcast want_ack */
    FF_FLARE_INTENT_SEND_FLARE_END, /* caller: ff_proto_encode_flare_end(...), broadcast */
} ff_flare_intent_t;

/** Result of any transition entry point below. */
typedef struct {
    ff_flare_intent_t intent;
    uint16_t dur_s;    /* valid iff intent == FF_FLARE_INTENT_SEND_FLARE */
    bool should_alert; /* true iff the app should fire the flare haptic
                         * (3-long pattern) right now. This OVERRIDES
                         * quiet hours per spec — `ff_flare` doesn't know
                         * what quiet hours even are (no ff_settings
                         * dependency), so there is no quiet-hours branch
                         * here to skip: the caller must honor this flag
                         * unconditionally rather than running it through
                         * its own `ff_quiet_now` gate. The caller (app)
                         * owns the actual buzzing. */
} ff_flare_result_t;

/**
 * ff_flare_t — the whole flare state machine. Fully-defined (not opaque)
 * so callers can put it on the stack or in a static with zero heap
 * allocation; zero-initialize or call `ff_flare_init` before use.
 */
typedef struct {
    ff_flare_state_t state;
    uint32_t node_id;   /* active sender's Meshtastic node num; meaningful
                          * in RECEIVED/LOCKED only, 0 otherwise. */
    uint32_t expiry_ms; /* absolute deadline in the injected clock's
                          * now_ms domain; meaningful in
                          * SENDING/RECEIVED/LOCKED only, 0 in IDLE. */
} ff_flare_t;

/** ff_flare_init — zero a flare state machine to IDLE. */
void ff_flare_init(ff_flare_t *f);

/**
 * ff_flare_send_begin — start (or restart) sending a flare: records
 * `expiry_ms = now_ms + dur_s*1000` (or `+ FF_FLARE_DEFAULT_DUR_S*1000` if
 * `dur_s == 0`) and transitions to SENDING. Always returns
 * FF_FLARE_INTENT_SEND_FLARE with the actual duration used (post-default)
 * in `dur_s`. See this header's judgment call (2) for why this is
 * unconditional regardless of prior state.
 */
ff_flare_result_t ff_flare_send_begin(ff_flare_t *f, uint16_t dur_s, uint32_t now_ms);

/**
 * ff_flare_send_cancel — cancel an in-progress send: SENDING -> IDLE,
 * returning FF_FLARE_INTENT_SEND_FLARE_END exactly once. A no-op
 * (FF_FLARE_INTENT_NONE, no state change) when not currently SENDING —
 * including a second call right after the first, which is what guarantees
 * "emits FLARE_END once, not twice" (spec AC1).
 */
ff_flare_result_t ff_flare_send_cancel(ff_flare_t *f);

/**
 * ff_flare_on_flare_rx — handle an already-decoded incoming FLARE
 * (`ff_proto_decode`'s FF_PROTO_TYPE_FLARE case) from `node_id`, sent for
 * `dur_s` seconds, received at `now_ms`.
 *
 * `paired` is the caller's answer to "is this sender in my crew?" (see
 * this header's top comment) — if false, the message is ignored entirely:
 * no state change, FF_FLARE_INTENT_NONE, should_alert=false. This is the
 * unpaired-sender guard from the spec's Receive section.
 *
 * If `paired`, this always transitions to RECEIVED with `node_id`/expiry
 * set from this message — "newest flare wins the takeover" (spec),
 * unconditionally, from any prior state (see judgment calls 3 and 4).
 * should_alert is always true on this path (haptic override, spec AC2).
 */
ff_flare_result_t ff_flare_on_flare_rx(ff_flare_t *f, uint32_t node_id, bool paired, uint16_t dur_s, uint32_t now_ms);

/**
 * ff_flare_on_flare_end_rx — handle an already-decoded incoming
 * FLARE_END from `node_id`. If currently RECEIVED or LOCKED to exactly
 * this `node_id`, transitions to IDLE (unlocking if it was LOCKED).
 * Otherwise a no-op — including FLARE_END from a node that isn't the
 * active sender (ignored, spec AC1), and FLARE_END received while
 * IDLE/SENDING (nothing to end on the receive side).
 */
ff_flare_result_t ff_flare_on_flare_end_rx(ff_flare_t *f, uint32_t node_id);

/**
 * ff_flare_go — user pressed GO on the takeover: RECEIVED -> LOCKED,
 * keeping the same node_id/expiry. No-op outside RECEIVED (including when
 * already LOCKED, or IDLE/SENDING).
 */
ff_flare_result_t ff_flare_go(ff_flare_t *f);

/**
 * ff_flare_dismiss — user dismissed the takeover: RECEIVED or LOCKED ->
 * IDLE (unlocking if it was LOCKED). Per spec, the feed item itself is
 * NOT cleared by this — feed ownership is out of scope for this module
 * entirely (S08), so there is nothing here that could clear it anyway.
 * No-op outside RECEIVED/LOCKED.
 */
ff_flare_result_t ff_flare_dismiss(ff_flare_t *f);

/**
 * ff_flare_tick — periodic expiry check, called with the current clock
 * reading. SENDING past its deadline auto-ends (-> IDLE,
 * FF_FLARE_INTENT_SEND_FLARE_END). RECEIVED or LOCKED past its deadline
 * expires (-> IDLE, unlocking if it was LOCKED; no outbound intent — a
 * receiver has nothing to announce when someone else's flare times out).
 * A no-op in IDLE, or before the deadline. See this header's judgment
 * call (1) for the inclusive boundary: `now_ms == expiry_ms` already
 * fires. Wraparound-safe against `now_ms`/`expiry_ms` uint32_t rollover,
 * matching `ff_clock_t`'s documented convention.
 */
ff_flare_result_t ff_flare_tick(ff_flare_t *f, uint32_t now_ms);

/**
 * ff_flare_locked_node — the node this puck's selection is currently
 * locked to, or 0 if not LOCKED. For S06's radar/crew selection code to
 * consult (e.g. `ff_crew_select_next` should no-op while this is nonzero)
 * — this module never reaches into `ff_crew` itself (see top comment).
 */
uint32_t ff_flare_locked_node(ff_flare_t const *f);

#ifdef __cplusplus
}
#endif

#endif /* FF_FLARE_H */
