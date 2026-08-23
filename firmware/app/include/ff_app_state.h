/**
 * ff_app_state.h — ff_app_state_t: the single view-state snapshot screens
 * render (docs/ARCHITECTURE.md principle 3, "the UI is a projection").
 *
 * Spec: docs/specs/S13-sim-target.md slice b ("fixtures + headless"),
 * docs/specs/S14-testing-ci.md slice b ("goldens tooling").
 *
 * This header started as [api] scaffolding for Wave 3 (S06/S08/S10 own the
 * real screens): it existed so the dev-loop infra — JSON fixtures, the
 * headless per-fixture renderer, and golden-screenshot tooling — had a
 * concrete state shape to load into and render, before any real screen
 * existed. The `now`/`signals`/`flare`/`settings` sections are still that
 * kind of scaffolding (plain structs, no behavior, no core/ includes,
 * pending their own S07/S08/S10/S11 slices) — but `radar` is not, as of
 * S06 (see the section below).
 *
 * ## Shape: union of what each mockup face needs
 * One sub-struct per face (`radar`, `now`, `signals`, `flare`, `settings`),
 * flattened to plain, JSON-fixture-friendly data — no pointers into a live
 * `fp_pack_t`/`ff_crew_t`/etc. (fixtures are standalone JSON snapshots, not
 * live wiring; see tests/fixtures/README.md for the schema and why).
 *
 * ## `radar` — DRIFT GUARD resolution (PR #12 review finding #5)
 * Earlier waves carried a scaffolding `ff_app_radar_t` here that mirrored
 * docs/specs/S06-radar-face.md's `ff_radar_view_t` sketch field-for-field
 * under a different type name, specifically because `core/include/ff_radar.h`
 * didn't exist yet (two anonymous structs with identical member lists are
 * still distinct C types under the same typedef name, so reusing the name
 * before the real header existed would have been a redefinition hazard).
 * S06 has now landed `core/include/ff_radar.h` / `ff_radar_view_t`
 * (computed by `ff_radar_compute()`) for real, so per that spec comment's
 * option (a): `ff_app_radar_t` is gone, and `radar` below is the genuine
 * `ff_radar_view_t` — this header now takes a `core/` dependency
 * (`#include "ff_radar.h"`) specifically for this one field, which is why
 * the "no includes from core/" rule in this file's history no longer
 * applies file-wide (it still holds for every other section, which remain
 * plain scaffolding).
 */
#ifndef FF_APP_STATE_H
#define FF_APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_radar.h" /* S06 — ff_radar_view_t, the real `radar` field type */

#ifdef __cplusplus
extern "C" {
#endif

/* Shared small-string budgets. Match the byte counts in the specs this
 * header transcribes (S06/S07/S08/S10/S11) field-for-field; kept as named
 * constants here rather than repeating magic numbers at every field. */
#define FF_APP_NAME_LEN   16 /* crew short name / my_name (S02, S11) */
#define FF_APP_STR_SHORT  12 /* dist_str / age_str (S06) */
#define FF_APP_STAGE_LEN  28 /* stage/artist display names (S05 fp_stage_t/fp_set_t) */
#define FF_APP_ARTIST_LEN 32
#define FF_APP_TEXT_LEN   64 /* feed item text (S08 ff_feed_item_t) */

/* fixture_name budget: the debug placeholder face (S13 slice b) renders
 * this verbatim as an lv_label, so it doubles as the "which fixture is
 * this" caption in golden screenshots. */
#define FF_APP_FIXTURE_NAME_LEN 32

/* -------------------------------------------------------------------
 * radar (S06) — real ff_radar_view_t, defined in core/include/ff_radar.h
 * (included above). See this header's top comment for the DRIFT GUARD
 * resolution: there is no app-local radar struct anymore.
 * ------------------------------------------------------------------- */

/* -------------------------------------------------------------------
 * now (S07) — flattened: fp_set_t pointers in the real core struct
 * (ff_now_row_t/ff_next_t) don't survive a standalone JSON fixture, so
 * this face's rows carry plain display fields instead of a pack pointer.
 * ------------------------------------------------------------------- */

#define FF_APP_NOW_MAX_ROWS 3 /* S07: "three now-rows max" */

typedef struct {
    char    artist[FF_APP_ARTIST_LEN];
    char    stage_name[FF_APP_STAGE_LEN];
    uint32_t stage_color_rgb; /* 0x00RRGGBB, from fp_stage_t.color_rgb */
    int16_t mins_left;
    uint8_t pct_done;
} ff_app_now_row_t;

typedef struct {
    bool    valid; /* false: no starred set upcoming (next_starred returned false) */
    char    artist[FF_APP_ARTIST_LEN];
    char    stage_name[FF_APP_STAGE_LEN];
    int16_t mins_until;
} ff_app_next_t;

typedef struct {
    ff_app_now_row_t rows[FF_APP_NOW_MAX_ROWS];
    uint8_t n_rows;
    ff_app_next_t next;
    bool tbd; /* S07: all-null-times pack -> "SET TIMES TBD" banner */
} ff_app_now_t;

/* -------------------------------------------------------------------
 * signals (S08) — flattened feed slice. Core's ff_feed_t is a 32-deep
 * ring buffer with node ids and raw timestamps; the debug/fixture view
 * only needs a short, already-formatted slice to prove the pipeline —
 * full feed semantics (eviction, unread badge math) are S08's job.
 * ------------------------------------------------------------------- */

/* Mirrors S08's ff_feed_kind_t exactly (name, order, members). */
typedef enum {
    FF_APP_FEED_PULSE,
    FF_APP_FEED_TEXT,
    FF_APP_FEED_RALLY,
    FF_APP_FEED_STATUS,
    FF_APP_FEED_FLARE,
} ff_app_feed_kind_t;

#define FF_APP_SIGNALS_MAX_ITEMS 8 /* debug-view slice, not the full 32-cap ring */

typedef struct {
    ff_app_feed_kind_t kind;
    char from_name[FF_APP_NAME_LEN];
    char text[FF_APP_TEXT_LEN];
    char age_str[FF_APP_STR_SHORT];
    bool unread;
} ff_app_feed_item_t;

typedef struct {
    ff_app_feed_item_t items[FF_APP_SIGNALS_MAX_ITEMS]; /* newest first */
    uint8_t n_items;
    uint8_t unread_count;
} ff_app_signals_t;

/* -------------------------------------------------------------------
 * flare (S10 slice b) — [api] REPLACES the old single `state`
 * IDLE/SENDING/RECEIVED/LOCKED enum mirror (S10 slice a scaffolding,
 * predating the spec's Amendments) with a flattened mirror of
 * `ff_flare_t`'s THREE INDEPENDENT pieces — see core/include/ff_flare.h's
 * "Independent state" doc comment and docs/specs/S10-flare.md's
 * Amendments (2026-08-23, PR #15) for the full rationale: outbound send,
 * the pending takeover, and the navigation lock are legitimately
 * simultaneous, unrelated facts (I can be sending my own flare AND have
 * a different crew member's takeover pending AND still be locked onto a
 * third member's earlier flare, all at once), so one shared `state` enum
 * that only fits ONE of them at a time is exactly the bug PR #15's
 * review caught and ruled out in core. This mirror carries the same
 * three-way split, flattened to plain display fields (not a live
 * `ff_flare_t *`) for the same reason `now`/`signals` are flattened
 * above: fixtures are standalone JSON snapshots, not live wiring.
 *
 * This is a breaking change to every field this struct used to have
 * (`state`, unqualified `expires_in_ms`) — flagged in the PR title with
 * `[api]` per AGENTS.md. Safe to make now because, same as ff_flare.h's
 * own Ruling 3 note, nothing in this repo has a real (non-fixture) call
 * site for the old shape yet — S10 slice a never wired this struct to
 * anything, and slice b (this PR) is the first and only consumer.
 * ------------------------------------------------------------------- */

typedef struct {
    /* Outbound: am I currently sending my own flare? Independent of the
     * two groups below — mirrors ff_flare_t's `sending`/`send_expiry_ms`. */
    bool     sending;
    int32_t  send_expires_in_ms; /* -1: n/a (not sending) or unknown */

    /* The full-screen takeover currently awaiting a GO/DISMISS decision,
     * if any — mirrors ff_flare_t's `takeover_active`/`takeover_node_id`/
     * `takeover_expiry_ms`. `takeover_bearing_deg`/`takeover_dist_str`
     * are NOT part of core's `ff_flare_t` (that struct deliberately has
     * no ff_crew/ff_radar dependency, see ff_flare.h's "Dependency-light
     * by design") — they're the app-layer's own bearing/distance-to-sender
     * read for this screen, the same "already-computed display string,
     * not a raw fact for the renderer to re-derive" convention
     * `ff_radar_view_t.dist_str` uses. */
    bool     takeover_active;
    char     takeover_from_name[FF_APP_NAME_LEN];
    float    takeover_bearing_deg;            /* [0, 360), compass bearing to sender */
    char     takeover_dist_str[FF_APP_STR_SHORT];
    int32_t  takeover_expires_in_ms;          /* -1: n/a */

    /* The node navigation is actually locked to, if any — mirrors
     * ff_flare_t's `locked_node_id`/`locked_expiry_ms`. Set only by a
     * user GO decision, cleared only by ff_flare_release_lock()/its own
     * expiry/a matching FLARE_END — see ff_flare.h's "Independent state"
     * section for why a newly-arriving takeover_active above never
     * touches this. */
    bool     locked;
    char     locked_from_name[FF_APP_NAME_LEN];
    int32_t  locked_expires_in_ms; /* -1: n/a */
} ff_app_flare_t;

/* -------------------------------------------------------------------
 * settings (S11) — mirrors ff_settings_t's user-facing fields (omits
 * compass_cal/cal_valid: not renderable/fixturable display data).
 * ------------------------------------------------------------------- */

typedef struct {
    bool     imperial;
    uint8_t  share_mode;       /* FF_SHARE_LIVE / _ZONES / _GHOST, see ff_settings.h */
    bool     haptics;
    bool     night_glow;
    uint16_t water_min;
    uint16_t quiet_from_min;
    uint16_t quiet_to_min;
    char     my_name[FF_APP_NAME_LEN];
} ff_app_settings_t;

/* -------------------------------------------------------------------
 * ff_app_state_t — the whole snapshot.
 * ------------------------------------------------------------------- */

/* Which face is on screen; drives both the real shell (S06 slice b,
 * scr_nav.c) and — until real screens exist — which section of state the
 * S13 debug placeholder face highlights (see targets/sim/fixture_view.c). */
typedef enum {
    FF_APP_FACE_RADAR,
    FF_APP_FACE_NOW,
    FF_APP_FACE_SIGNALS,
    FF_APP_FACE_SETTINGS,
} ff_app_face_t;

typedef struct {
    /* Debug/provenance only: which fixture produced this state, e.g.
     * "radar_live". Not part of any mockup — the S13 placeholder face
     * renders it so goldens visibly identify themselves; a real S06+
     * screen ignores this field entirely. */
    char fixture_name[FF_APP_FIXTURE_NAME_LEN];

    ff_app_face_t active_face;

    ff_radar_view_t   radar;
    ff_app_now_t      now;
    ff_app_signals_t  signals;
    ff_app_flare_t    flare;
    ff_app_settings_t settings;
} ff_app_state_t;

#ifdef __cplusplus
}
#endif

#endif /* FF_APP_STATE_H */
