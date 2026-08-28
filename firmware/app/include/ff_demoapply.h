/**
 * ff_demoapply.h — S23 (slice c): apply an ff_demofeed event through the
 * REAL mesh-inbound seam, plus the pure mapping helper the device apply
 * loop is built on.
 *
 * Spec: docs/specs/S23-demo-feed.md, slice (c) "app apply loop — drive
 * ff_demofeed_tick from the demo clock each tick and apply events through
 * ff_wiring; presence pokes".
 *
 * ## Where this sits
 * `ff_demofeed` (core, slice a) is a CONTENT-FREE event source: it emits
 * `{member_idx, kind, text_ref}`, knowing no real node ids and inventing
 * no strings (ff_demofeed.h). This module is the APP side that gives those
 * opaque indices meaning and drives them into the shell:
 *
 *   - `member_idx`  -> a demo crew `node_id` (the app owns the map, seeded
 *                      from the demo festpack — ff_demo.h's FF_DEMO_NODE_*).
 *   - `text_ref`    -> a demo string (the small, demo-gated table below).
 *   - `kind`        -> which mesh-inbound callback to drive:
 *                        TEXT   -> mc_events_t.on_text
 *                        PULSE/RALLY/STATUS/FLARE -> mc_events_t.on_private
 *                                                    (ff_proto payload)
 *   - PRESENCE_POKE -> mc_events_t.on_rx_meta (a DIRECT RSSI sample that
 *                      refreshes the member's rssi_age, so presence drifts
 *                      LIVE->STALE->LOST and recovers — S23 AC3).
 *
 * Events are applied through `ff_shell_events()` — the SAME seven-callback
 * seam a live radio drives (ff_shell.h) — so a synthetic pulse/rally/text
 * is indistinguishable in ff_feed_t / ff_crew_t from a mesh one (same
 * paired-sender filter, same unread=true push, same haptic; S23 AC2). This
 * module writes NO core state directly and fabricates no freshness: it only
 * hands honest-looking inbound events to the real inbound path.
 *
 * ## Honest-data gating (S23 AC4/AC5, [[firefly-touch-cal-default]])
 * This whole file is demo content and lives in the `ff-demo` library /
 * `ff_app` component alongside ff_demo.c. On device it is reachable ONLY
 * from app_main.c's `CONFIG_FF_DEMO_MODE` (+ `CONFIG_FF_DEMO_LIVE`) block;
 * with demo mode off nothing references it and `-Wl,--gc-sections` strips
 * the object — strings and all — exactly as it already does ff_demo.c
 * (verified by the field-build link check in the S23(b+c) PR). The demo
 * string table therefore never reaches a path a field build compiles in.
 *
 * ## Testability
 * The DECISION — idx->node_id, text_ref bounds, kind->dispatch — is the
 * pure `ff_demo_apply_plan`, unit-tested in the sim with no shell, clock or
 * radio (test_demoapply.c). The advancing demo clock + esp_timer glue that
 * calls `ff_demofeed_tick` is device-only (app_main.c) and not here.
 */
#ifndef FF_DEMOAPPLY_H
#define FF_DEMOAPPLY_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_demofeed.h" /* ff_demo_event_t */
#include "ff_feed.h"     /* ff_feed_kind_t */
#include "ff_proto.h"    /* ff_proto_type_t */

#include "mc_client.h" /* mc_events_t — the inbound seam events are applied through */

#ifdef __cplusplus
extern "C" {
#endif

/** Number of demo crew members the live generator references (DANA, KEV,
 *  RILEY, MAYA, SAM — ff_demo.h's FF_DEMO_NODE_*). The idx->node_id map
 *  `ff_demo_live_node_ids` returns has exactly this many entries. */
#define FF_DEMO_LIVE_MEMBER_COUNT ((uint8_t)5u)

/** Fixed PRNG seed the device apply loop inits ff_demofeed with. A named
 *  constant so the live stream is reproducible and the same value the
 *  integration test can reuse (determinism is ff_demofeed's contract). */
#define FF_DEMO_LIVE_SEED ((uint32_t)0x1EAF1E5Au)

/** RSSI (dBm) a PRESENCE_POKE reports — a plausible mid-strength DIRECT
 *  sample. Its only job is to refresh the member's rssi_age so the freshest
 *  sighting reads recent again (SEEN); the exact value is demo data. */
#define FF_DEMO_LIVE_POKE_RSSI_DBM ((int16_t)-70)

/** Duration (s) a synthetic FLARE signal carries. Demo data. */
#define FF_DEMO_LIVE_FLARE_DUR_S ((uint16_t)300u)

/**
 * ff_demo_live_node_ids — the canonical idx->node_id map the live demo
 * uses (member_idx 0..FF_DEMO_LIVE_MEMBER_COUNT-1 -> a demo crew node id,
 * in ff_demo.c's canonical crew order). Writes the count to `*out_count`
 * if non-NULL. The returned pointer is to static storage (valid for the
 * program lifetime); never NULL.
 */
uint32_t const *ff_demo_live_node_ids(uint8_t *out_count);

/**
 * ff_demo_text_ref — resolve a generator `text_ref` (0-based) to a demo
 * string. Returns NULL if `text_ref >= FF_DEMOFEED_TEXT_REF_COUNT` (the
 * generator never emits such a value, but the bound is enforced here so a
 * bad ref yields no text rather than reading past the table). The returned
 * pointer is to static string storage; never freed.
 */
char const *ff_demo_text_ref(uint8_t text_ref);

/** How an event is dispatched into the mesh-inbound seam. */
typedef enum {
    FF_DEMO_DISPATCH_NONE = 0, /* invalid event (out-of-range idx) — do nothing */
    FF_DEMO_DISPATCH_TEXT,     /* -> mc_events_t.on_text (FEED_TEXT) */
    FF_DEMO_DISPATCH_PRIVATE,  /* -> mc_events_t.on_private (ff_proto payload) */
    FF_DEMO_DISPATCH_POKE,     /* -> mc_events_t.on_rx_meta (presence refresh) */
} ff_demo_dispatch_t;

/**
 * ff_demo_apply_plan_t — the pure decision for one event: whom it is from,
 * which inbound callback to drive, and (for a signal) with what payload.
 * Filled by `ff_demo_apply_plan`.
 */
typedef struct {
    bool               valid;      /* false => out-of-range member_idx; ignore the rest */
    ff_demo_dispatch_t dispatch;   /* which seam to drive */
    uint32_t           node_id;    /* resolved from member_idx */
    ff_feed_kind_t     kind;       /* SIGNAL: the feed kind (echoed); POKE: unused */
    ff_proto_type_t    proto_type; /* PRIVATE dispatch only: the ff_proto type to encode */
    char const        *text;       /* TEXT/STATUS body or RALLY name; NULL when the kind carries none */
} ff_demo_apply_plan_t;

/**
 * ff_demo_apply_plan — PURE mapping (no shell, no clock, no I/O): decide
 * how `ev` should be applied, given the idx->node_id map `node_ids`
 * (`member_count` entries).
 *
 *  - `ev->member_idx >= member_count` (or NULL args) => `out->valid=false`,
 *    dispatch NONE. The generator never emits an out-of-range idx, but the
 *    bound is the honest gate against a mismatched map.
 *  - PRESENCE_POKE => dispatch POKE, `node_id` resolved, no payload.
 *  - SIGNAL => dispatch by kind:
 *      FEED_TEXT   -> TEXT,    text = ff_demo_text_ref(text_ref)
 *      FEED_STATUS -> PRIVATE, proto STATUS, text = ff_demo_text_ref(...)
 *      FEED_RALLY  -> PRIVATE, proto RALLY,  text = ff_demo_text_ref(...)
 *      FEED_PULSE  -> PRIVATE, proto PULSE,  text = NULL
 *      FEED_FLARE  -> PRIVATE, proto FLARE,  text = NULL
 *
 * Returns `out->valid`. Safe no-op returning false if `ev`/`node_ids`/
 * `out` is NULL or `member_count` is 0.
 */
bool ff_demo_apply_plan(ff_demo_event_t const *ev, uint32_t const *node_ids, uint8_t member_count,
                        ff_demo_apply_plan_t *out);

/**
 * ff_demo_apply_event — the glue: build the plan for `event` and drive it
 * into `ev` (the shell's `ff_shell_events()`), encoding an ff_proto payload
 * for a PRIVATE signal and a DIRECT mc_rx_meta_t for a poke. A no-op if any
 * argument is NULL, the plan is invalid, or the needed callback on `ev` is
 * NULL. Best-effort (void): a signal whose text won't encode is simply not
 * pushed, never faked.
 *
 * This is the one call the device apply loop makes per emitted event; it is
 * also driven directly from the sim integration test against a real seeded
 * shell (so "a synthetic signal lands in the feed / a poke refreshes
 * presence" is asserted through the real projection, S23 AC2/AC3).
 */
void ff_demo_apply_event(mc_events_t const *ev, ff_demo_event_t const *event, uint32_t const *node_ids,
                         uint8_t member_count);

#ifdef __cplusplus
}
#endif

#endif /* FF_DEMOAPPLY_H */
