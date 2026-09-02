/**
 * ff_demo.h — S20 demo mode: seed the shell into the fictional
 * "Firefly Fields" festival so every face shows real-looking data with no
 * mesh, no comms brain, no real festival.
 *
 * Spec: docs/specs/S20-demo-mode.md.
 *
 * ---------------------------------------------------------------------
 * HONESTY (the whole point — CLAUDE.md: "honest data over pretty data")
 * ---------------------------------------------------------------------
 * There is NO faked freshness, no canned splash, no shortcut around the
 * core state machines here. `ff_demo_seed` drives the SAME public shell
 * entry points a live radio would (`ff_shell_events()`'s
 * on_node/on_position/on_rx_meta/on_private/on_text, `ff_shell_pair`,
 * `ff_shell_load_pack`, `ff_shell_set_my_pos`/`_heading`), latches the wall
 * clock from a plausible mesh timestamp exactly as the want_config
 * handshake would, and lets `ff_crew_freshness` / `ff_crew_close_range` /
 * `ff_wall_now` compute what they compute. Inside the demo world the state
 * is genuinely true; the mode is meant to be clearly labelled DEMO by its
 * caller (the sim's --demo banner, the device's CONFIG_FF_DEMO_MODE) so it
 * never masquerades as live field data.
 *
 * ---------------------------------------------------------------------
 * LAYERING
 * ---------------------------------------------------------------------
 * This is a THIRD app-boundary file that legitimately sees core +
 * meshclient + app together, alongside `ff_wiring.c` and `ff_shell.c`
 * (ff_shell.h's "Layering" note). It has to: seeding crew positions/names/
 * RSSI honestly means constructing `mc_nodeinfo_t` / `mc_position_t` /
 * `mc_rx_meta_t` and handing them to the shell's real inbound callbacks,
 * the one seam that keeps the roster-trust and freshness rules intact. It
 * is deliberately NOT in core/ (core stays pure) — it only CALLS existing
 * APIs, adds no new core logic, and both targets link it.
 *
 * Content is FICTIONAL (see the festpack it parses,
 * firmware/assets/demo/firefly-fields.festpack.json) and lives in the
 * firefly repo, never in fest-almanac (real festivals only).
 */
#ifndef FF_DEMO_H
#define FF_DEMO_H

#include <stddef.h>
#include <stdint.h>

#include "ff_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The five seeded crew members' Meshtastic node ids (fictional, in a
 * private range picked so they cannot collide with a real node). Exposed
 * so a caller (the sim's radar screenshots, tests) can name which member
 * to make the default radar selection via `primary_node`. */
#define FF_DEMO_NODE_DANA  0x00DE3001u /* near The Beacon — LIVE, live arrow */
#define FF_DEMO_NODE_KEV   0x00DE3002u /* at Bass Hollow — LIVE, ~120 m */
#define FF_DEMO_NODE_RILEY 0x00DE3003u /* by the Ferris Wheel — LIVE + close-range */
#define FF_DEMO_NODE_MAYA  0x00DE3004u /* Camp Glow — stale, "LAST SEEN 25 MIN" */
#define FF_DEMO_NODE_SAM   0x00DE3005u /* paired but no position fix */

/**
 * The demo wall-clock instant: Saturday 2026-09-05 21:30 local
 * (America/Los_Angeles, UTC-7 / PDT) — festival peak, so The Beacon's
 * Saturday headliner (FIREFLY) reads mid-set with a live progress bar and
 * the starred-set countdown is ticking. Unix seconds.
 *
 * Comfortably inside both the fixed plausibility window
 * ([FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING)) and the tighter window
 * `ff_shell_load_pack` derives from the festival's own dates, so the
 * latch takes at BOOTSTRAP tier exactly as a real NodeInfo would.
 */
#define FF_DEMO_WALL_UNIX_S ((int64_t)1788669000)

/** The monotonic-clock value (ms) the demo world is pinned at. The caller
 *  must drive the shell at THIS `now_ms` (via the clock it injected) after
 *  seeding, so freshness/countdowns read the seeded instant. */
#define FF_DEMO_NOW_MS ((uint32_t)3600000u)

/**
 * ff_demo_seed — seed an already-initialised `sh` into Firefly Fields.
 *
 * Steps, all through the real core APIs:
 *  1. `*clock_ms = FF_DEMO_NOW_MS` — pin the injected monotonic clock at
 *     the demo instant (the caller's `ff_clock_t` must read `*clock_ms`).
 *  2. `ff_shell_load_pack` — parse `festpack_json[0..festpack_len)` into
 *     the shell's pack storage (which also tightens the wall-clock
 *     plausibility window to the festival's dates).
 *  3. Latch the wall clock to FF_DEMO_WALL_UNIX_S via a NodeInfo
 *     `last_heard` (the same bootstrap path the want_config handshake
 *     uses).
 *  4. Seed the five crew: DANA (LIVE), KEV (LIVE), RILEY (LIVE +
 *     close-range via a DIRECT RSSI sample), MAYA (a 25-minute-old fix —
 *     renders "LAST SEEN 25 MIN"), SAM (paired, no fix). Positions are
 *     recorded with honest receive-time ages; MAYA's older fix is offered
 *     while she is still un-paired so its disagreeing timestamp cannot
 *     drag the latch (BOOTSTRAP tier is refused a re-latch), then she is
 *     paired.
 *  5. Set my position (The Firefly Tower, the meetup landmark) and a north
 *     heading, so Radar can point and the Map can place YOU.
 *  6. Seed the Signals feed: two statuses from KEV, a rally point at
 *     The Firefly Tower, MAYA's "omw" canned reply, and a broadcast
 *     message — each via the real ff_wiring feed path (paired-sender
 *     gated).
 *
 * `primary_node` names the crew member to pair FIRST, which makes it the
 * default Radar selection (`ff_crew_selected` returns the first paired
 * slot). Pass one of FF_DEMO_NODE_* to spotlight that member's Radar mode
 * (live arrow / close-range / stale / no-fix), or 0 for the default
 * (DANA, live arrow).
 *
 * Returns 0 on success; negative if `sh`/`clock_ms`/`festpack_json` is
 * NULL, the festpack fails to parse, or the wall clock could not be
 * latched (which would leave Now unable to say what time it is).
 */
int ff_demo_seed(ff_shell_t *sh, char const *festpack_json, size_t festpack_len, uint32_t *clock_ms,
                 uint32_t primary_node);

/* NOTE (S24 slice d): the demo LOOPBACK sender — the accept-every-send
 * `ff_wiring_sender_t` that makes outbound VISIBLE in the transport-less
 * demo build — deliberately lives in the device target's app_main.c under
 * `#if CONFIG_FF_DEMO_MODE`, NOT here. ff_demo.c is compiled into the
 * device image unconditionally (ESP-IDF does not dead-strip it — verified),
 * so a loopback symbol placed here would survive into a FIELD build; keeping
 * it #if-gated in app_main is the only way to guarantee a field build
 * contains zero loopback symbols. It is installed via ff_shell_set_sender
 * (ff_shell.h) after ff_demo_seed. The generic seam ff_shell_set_sender is
 * unit-tested on host (test_shell.c) with an equivalent accepting mock. */

#ifdef __cplusplus
}
#endif

#endif /* FF_DEMO_H */
