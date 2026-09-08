/**
 * ff_find.h — core/find: the S29 FIND-mode session state machine.
 *
 * Spec: docs/specs/S29-radio-only.md, "PR 2 — FIND mode (active pings)".
 *
 * Pure C11, no I/O, no allocation, no clock-reading of its own — every
 * entry point that needs "now" takes it as an explicit `uint32_t now_ms`
 * parameter, same "explicit now_ms in, no hidden clock" shape
 * `ff_radar_compute`/`ff_flare_tick` already use for the identical reason
 * (the caller, app tick or a meshclient rx callback, always already has
 * that timestamp in hand). Safe on the stack or in a static.
 *
 * From the SIGNAL view with a friend selected, FIND actively pings that
 * node every 10 s and shows both directions of the link: our own
 * ordinary SIGNAL reading of them (unchanged — flows through the SAME
 * `ff_crew_on_rssi`/`ff_crew_on_heard` path any direct packet already
 * uses, PONG included, no new plumbing needed for that half), plus
 * `their_rssi_of_us`/`their_snr_of_us` — the one new fact FIND adds, from
 * the PONG's own payload ("they hear us at -xx dBm").
 *
 * ## Tick-returns-an-action shape
 * Mirrors `ff_flare_t`'s own convention (see ff_flare.h's top comment):
 * `ff_find_tick` returns an intent the CALLER acts on (encode + send),
 * rather than this module owning a sender callback. Keeps this module
 * transport-free and trivially testable with a fake clock.
 *
 * ## Single active session
 * `ff_find_start` on a new target cancels any prior session outright —
 * mirrors `ff_flare_t`'s own single-active-flare convention (this
 * codebase's precedent for "starting a new one of these always wins,
 * never queues"). Only one target is ever pinged at a time.
 *
 * ## Rate limiting (sender side) — a defensive floor, not just cadence
 * `ff_find_tick` refuses to return SEND_PING more than once per 10 s
 * regardless of how often the caller calls it — this is a hard floor
 * inside the module itself (`last_ping_sent_ms`), not merely "the caller
 * happens to call it every 10s", so a bug in the caller's own tick loop
 * can never turn this into a flood. Stops automatically at 30 pings (5
 * min at the 10s cadence) OR 5 minutes of wall-clock elapsed, whichever
 * comes first (both checked — a caller that ticks irregularly, e.g. the
 * sim's headless mode, must not be able to out-wait the cap by never
 * reaching 30 sends) — see `ff_find_tick`'s own doc comment.
 */
#ifndef FF_FIND_H
#define FF_FIND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ping cadence — one PING per session target at most this often. */
#define FF_FIND_PING_INTERVAL_MS ((uint32_t)10u * 1000u)

/** Session cap: whichever of these two limits is reached first ends the
 * session. 30 pings at the 10s cadence above IS 5 minutes under normal
 * conditions; both are enforced independently (not derived from one
 * another) so an irregular tick loop can't dodge the wall-clock cap by
 * simply never reaching 30 sends. */
#define FF_FIND_MAX_PINGS 30u
#define FF_FIND_SESSION_MAX_MS ((uint32_t)5u * 60u * 1000u)

/** Trend-haptic window (sample-count-based, NOT `ff_crew_rssi_trend`'s
 * 5s wall-clock window — PING/PONG's own cadence is a fixed 10s per
 * sample, so "last three samples" is already a 20-30s window; see
 * ff_find.c's own doc comment on this deliberate difference). */
#define FF_FIND_TREND_SAMPLES 3u
#define FF_FIND_TREND_THRESHOLD_DBM 3.0f

typedef enum {
    FF_FIND_INTENT_NONE = 0,
    FF_FIND_INTENT_SEND_PING, /* caller: ff_proto_encode_ping(buf, n, nonce), mc_send_private
                                * direct-addressed to target_node_id, want_ack = false */
} ff_find_intent_t;

typedef struct {
    ff_find_intent_t intent;
    uint32_t nonce; /* valid iff intent == FF_FIND_INTENT_SEND_PING */
} ff_find_result_t;

/** Warmer/colder haptic verdict from a PONG update — fires exactly once
 * per CROSSING into a >= FF_FIND_TREND_THRESHOLD_DBM change, not once
 * per sample that happens to still be above threshold (see
 * `ff_find_on_pong`'s own doc comment). */
typedef enum {
    FF_FIND_HAPTIC_NONE = 0,
    FF_FIND_HAPTIC_WARMER,
    FF_FIND_HAPTIC_COLDER,
} ff_find_haptic_t;

/**
 * ff_find_t — one FIND session's state. Zero-initialize (or call
 * ff_find_stop, which is safe on a zeroed/never-started struct) before
 * use. Fully-defined (not opaque) — callers/renderers read the fields
 * directly, same convention `ff_crew_t`/`ff_radar_smooth_t` already use;
 * only the fields below marked "internal" are not meant for a renderer.
 */
typedef struct {
    bool     active;
    uint32_t target_node_id; /* valid iff active */
    uint32_t started_ms;     /* valid iff active */
    uint32_t ping_count;     /* pings sent this session so far */

    /* internal — rate-limit floor */
    bool     has_last_ping_sent_ms;
    uint32_t last_ping_sent_ms;
    uint32_t next_nonce; /* increments once per ping sent, wraps like any counter */

    /* internal — nonce correlation: the MOST RECENTLY SENT ping's nonce
     * (not a set of all outstanding nonces — at most one ping is ever in
     * flight at the 10s cadence). ff_find_on_pong only accepts a PONG
     * whose nonce matches this, rejecting a stale/duplicate reply to an
     * earlier ping in this same session (see that function's own doc
     * comment). */
    bool     has_last_sent_nonce;
    uint32_t last_sent_nonce;

    /* Their reading of US — the one new fact FIND adds. Absent (false)
     * until the first PONG of this session arrives; a NEW ff_find_start
     * always clears it (a prior target's reading must never be shown
     * against a new target). */
    bool     has_their_reading;
    int16_t  their_rssi_of_us;
    bool     their_has_snr;
    float    their_snr_of_us;      /* dB, valid iff their_has_snr */
    uint32_t their_reading_age_ms; /* absolute rx timestamp, ff_fmt_age convention */

    /* internal — trailing PONG-driven their_rssi_of_us samples for the
     * trend-haptic window (see ff_find_on_pong's doc comment). Ring
     * buffer, oldest overwritten first. */
    int16_t  sample_hist[2u * FF_FIND_TREND_SAMPLES];
    uint8_t  sample_count; /* saturates at 2*FF_FIND_TREND_SAMPLES */
    uint8_t  sample_head;
    int8_t   last_fired_trend; /* -1/0/+1: the trend direction last FIRED a haptic for
                                 * (0 = none fired / trend currently steady) — the
                                 * crossing-detection latch ff_find_on_pong reads */
} ff_find_t;

/** Reset to "no active session" — safe on a zeroed struct. Equivalent to
 * zero-initializing, provided as a named entry point (matches
 * `ff_radar_smooth_reset`'s convention) for a caller that wants an
 * explicit "cancel" without re-zeroing everything by hand at a call
 * site. */
void ff_find_stop(ff_find_t *f);

/**
 * ff_find_start — begin (or restart) a FIND session against
 * `target_node_id`. Cancels any prior session outright (single active
 * target, `ff_flare_t`'s own precedent) — starting on a NEW target while
 * one is already active silently replaces it, no queuing. Clears
 * `has_their_reading`/the sample history/the trend latch: a prior
 * target's PONG readings must never bleed into a new target's session.
 * Does NOT itself send a ping — the first `ff_find_tick` call after this
 * does (this call only arms the session; `has_last_ping_sent_ms` starts
 * false, so the very next tick fires immediately, not 10s later).
 */
void ff_find_start(ff_find_t *f, uint32_t target_node_id, uint32_t now_ms);

/**
 * ff_find_tick — periodic pump, called with the current clock reading.
 * No-op (FF_FIND_INTENT_NONE) if `!active`.
 *
 * Auto-stops the session (sets `active = false`, returns
 * FF_FIND_INTENT_NONE for this AND every future call until a new
 * ff_find_start) once EITHER `ping_count >= FF_FIND_MAX_PINGS` OR
 * `now_ms - started_ms >= FF_FIND_SESSION_MAX_MS` — checked before the
 * rate-limit floor below, so a session that has already hit its cap
 * never sends one more ping no matter how the caller ticks.
 *
 * Otherwise, returns FF_FIND_INTENT_SEND_PING (with a fresh nonce) at
 * most once per FF_FIND_PING_INTERVAL_MS (10s) — a hard floor inside
 * this module (see this header's top comment), regardless of how often
 * the caller invokes this function. The very first tick after
 * `ff_find_start` always sends immediately (no 10s wait for the first
 * ping of a session).
 */
ff_find_result_t ff_find_tick(ff_find_t *f, uint32_t now_ms);

/**
 * ff_find_leave_face — cancel-on-face-leave (S29 spec: FIND stops "on
 * leaving the Radar face"). Identical to `ff_find_stop`; a distinctly
 * named entry point so a call site reads as "the user navigated away",
 * not "the user explicitly cancelled" — same fact, different caller
 * intent, matching this codebase's convention of naming call sites for
 * what triggered them (e.g. `ff_flare_send_cancel` vs. tick-driven
 * expiry, both of which clear the identical fields).
 */
void ff_find_leave_face(ff_find_t *f);

/**
 * ff_find_on_pong — record a PONG's payload as our fresh "how do they
 * hear us" reading, and evaluate the trend-haptic crossing.
 *
 * A no-op (returns FF_FIND_HAPTIC_NONE, no state change) unless
 * `active && from_node_id == f->target_node_id` — a PONG from anyone
 * else, or arriving after the session already ended, is not this
 * session's business (mirrors `ff_flare_t`'s own "independent state,
 * nothing inbound touches a session that isn't the one it names"
 * discipline). ALSO a no-op unless `nonce` matches the most recently
 * sent ping's nonce (`f->last_sent_nonce`) — a stale or duplicate reply
 * to an EARLIER ping in this same session must not overwrite a fresher
 * (or simply update the trend history with an out-of-order) reading.
 * Since at most one ping is ever in flight at the 10s cadence, tracking
 * only the single most recent sent nonce (not a set of outstanding
 * ones) is sufficient.
 *
 * Trend-haptic crossing (docs/specs/S29-radio-only.md's own
 * interpretation-call note): this module keeps the trailing
 * `2*FF_FIND_TREND_SAMPLES` (6) PONG-driven `rssi_dbm` samples in a ring
 * buffer. A trend is computable only once a FULL 6 samples have arrived
 * — both 3-sample halves must be populated, the SAME "not enough spread
 * in both halves" gate `ff_crew_rssi_trend` already uses (see that
 * function's own doc comment) — not merely "skip the first two", which
 * has no well-defined 3-vs-3 comparison below 4 samples anyway. Once
 * computable: `delta = avg(newest 3) - avg(oldest 3 of the 6)`,
 * thresholded at +/- FF_FIND_TREND_THRESHOLD_DBM (3 dB) into
 * -1/0/+1, mirroring `ff_crew_rssi_trend`'s own threshold-and-sign
 * shape at a smaller, session-local window.
 *
 * Fires FF_FIND_HAPTIC_WARMER/_COLDER exactly once per CROSSING into a
 * non-zero trend — i.e. only when the newly-computed trend differs from
 * the trend the LAST haptic fired for (`f->last_fired_trend`) — never
 * once per sample that happens to still read above threshold. A trend
 * that returns to 0 (steady) resets the latch, so a LATER re-crossing
 * into the SAME direction (e.g. warmer, steady, warmer again) fires
 * again; a trend that flips straight from warmer to colder (or vice
 * versa) without passing through steady also fires again immediately
 * (the two are different `last_fired_trend` values). Returns
 * FF_FIND_HAPTIC_NONE on every call that isn't itself a fresh crossing,
 * including every call before 6 samples exist.
 */
ff_find_haptic_t ff_find_on_pong(ff_find_t *f, uint32_t from_node_id, uint32_t nonce, int16_t rssi_dbm,
                                  bool has_snr, float snr_db, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* FF_FIND_H */
