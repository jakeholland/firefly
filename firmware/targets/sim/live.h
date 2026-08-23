/**
 * live.h — S13c: minimal mc_client -> ff_app_state_t live wiring for
 * `ffsim --connect HOST:PORT` (and `--pack FILE`).
 *
 * `--connect`/`--pack` are S13 slice a/b deliverables (docs/specs/
 * S13-sim-target.md: "`--connect HOST:PORT`: live mode via mc TCP
 * transport; `--pack FILE` preloads festpack") that hadn't landed yet
 * when this slice (c: ctl socket, d: dev harness + e2e) started — the ctl
 * socket's `{"cmd":"state"}` dump and the S14 slice d e2e scenarios are
 * both meaningless without *something* live to dump/assert on, so this
 * slice implements the minimum live wiring needed to make them real
 * rather than stubbing them out. Flagged as a deviation/judgment call in
 * this slice's PR body per AGENTS.md ("if blocked by a spec gap... note
 * the interpretation").
 *
 * See ff_live_t's doc comment below for exactly what is and isn't wired.
 */
#ifndef FF_LIVE_H
#define FF_LIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_app_state.h"
#include "ff_clock.h"
#include "ff_crew.h"
#include "ff_radar.h"
#include "mc_client.h"
#include "mc_transport_tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ff_live_t — wires mc_client_t's events into a live-updated
 * ff_app_state_t.
 *
 * ## Deliberately out of scope (see this slice's PR body for the full
 * writeup)
 * This is NOT the S06+ app orchestration layer — there is no pairing
 * protocol (S09) yet, no now/settings live wiring, and FLARE/RALLY
 * messages aren't fed into the signals feed (STATUS is, trivially, since
 * it needed no extra plumbing beyond PULSE). It exists to prove the
 * transport -> core -> view-state pipeline end to end for exactly what
 * the S14 slice d e2e scenarios need:
 *   - position fixes -> the crew roster -> `ff_radar_compute()` ->
 *     `ff_app_state_t.radar` (distance/bearing against a fixed "my
 *     position" — see `ff_live_set_my_pos`/`ff_live_load_pack`);
 *   - PULSE (S04 private-portnum message) and plain text ->
 *     `ff_app_state_t.signals` (the feed).
 * Every heard node is auto-paired (`ff_crew_set_paired(..., true)` on
 * first NodeInfo) — there being no pairing protocol yet, "every node the
 * dev harness spawns is my crew" is the only honest behavior available.
 */
typedef struct {
    ff_app_state_t *state; /* caller-owned, must outlive this struct */
    ff_clock_t const *clock;

    mc_tcp_t    tcp;
    mc_client_t mc;
    bool        attached; /* true once ff_live_attach/ff_live_connect has run —
                              gates ff_live_tick's mc_tick() call. NOT the same
                              as tcp.fd >= 0: ff_live_attach (the test seam
                              test_live.c and any non-TCP transport uses)
                              never touches tcp at all. */

    ff_crew_t         crew;
    ff_radar_smooth_t smooth;

    ff_latlon_t my_pos;
    bool        my_pos_ok;
    float       heading_deg; /* fixed placeholder — sim target has no
                                 compass; 0deg (north), see header note */

    /* Receive timestamps for state->signals.items[], parallel-indexed
     * (newest-first, same as that array) — ff_app_feed_item_t only
     * stores an already-formatted age_str, so this is where the raw
     * timestamp ff_live_tick recomputes it from actually lives. */
    uint32_t feed_rx_ms[FF_APP_SIGNALS_MAX_ITEMS];
} ff_live_t;

/** Zeroes `*lv` and binds it to `state` (must outlive `lv`) and `clock`
 * (used for both crew-roster freshness math and, once ff_live_connect is
 * called, the underlying mc_client_t's own timing). Does not open any
 * connection — call ff_live_connect for that. */
void ff_live_init(ff_live_t *lv, ff_app_state_t *state, ff_clock_t const *clock);

/** Sets the fixed "my position" ff_radar_compute anchors distance/bearing
 * to (see this header's top comment — no real GPS in the sim target).
 * Until this is called (or ff_live_load_pack finds a known venue origin),
 * my_pos_ok is false and the radar face honestly reports NOFIX for any
 * selected member — "no position of mine to compare against" is a true
 * statement, not a bug. */
void ff_live_set_my_pos(ff_live_t *lv, ff_latlon_t pos);

/**
 * ff_live_load_pack — reads and parses `path` as a festpack.json
 * (fp_pack.h); if its venue origin is known, calls ff_live_set_my_pos
 * with it. Returns 0 if the file was read and parsed (even if the venue
 * origin turned out to be unknown/absent — that's not a failure, see
 * fp_pack.h's `origin_known`), negative on I/O failure, a parse error, or
 * a file over this function's 256 KiB read budget.
 */
int ff_live_load_pack(ff_live_t *lv, char const *path);

/**
 * ff_live_attach — starts the mc_client want_config handshake over an
 * arbitrary transport `t` (test seam, and the shared implementation
 * ff_live_connect below is built on). Wires up every mc_events_t callback
 * this module understands; `t` must outlive `lv` (or at least until
 * ff_live_close/another ff_live_attach call).
 */
void ff_live_attach(ff_live_t *lv, mc_transport_t t);

/**
 * ff_live_connect — opens a TCP connection to host:port and starts the
 * mc_client want_config handshake (via ff_live_attach). Returns 0 on
 * success, negative on failure (mc_tcp_open failed — bad host, connection
 * refused, etc).
 */
int ff_live_connect(ff_live_t *lv, char const *host, uint16_t port);

/**
 * ff_live_tick — pumps mc_tick() (if connected) and recomputes
 * state->radar from the current crew roster/selection, and refreshes
 * every signals-feed item's age_str from feed_rx_ms. Call at ~50 Hz
 * (mc_client.h's documented rate) from ffsim's ctl-serving loop. `now_ms`
 * should come from the same clock passed to ff_live_init.
 */
void ff_live_tick(ff_live_t *lv, uint32_t now_ms);

/** Closes the TCP connection, if open. Safe to call on a never-connected
 * or already-closed `*lv`. */
void ff_live_close(ff_live_t *lv);

#ifdef __cplusplus
}
#endif

#endif /* FF_LIVE_H */
