/**
 * ff_app_state.h — ff_app_state_t: the single view-state snapshot screens
 * render (docs/ARCHITECTURE.md principle 3, "the UI is a projection").
 *
 * Spec: docs/specs/S13-sim-target.md slice b ("fixtures + headless"),
 * docs/specs/S14-testing-ci.md slice b ("goldens tooling").
 *
 * This header is [api] scaffolding for Wave 3 (S06/S08/S10 own the real
 * screens): it exists now so the dev-loop infra — JSON fixtures, the
 * headless per-fixture renderer, and golden-screenshot tooling — has a
 * concrete state shape to load into and render, before any real screen
 * exists. Per CLAUDE.md ("core… no logic; screens only render") and the
 * task brief ("keep it dumb data, no logic"): this file is nothing but
 * plain structs and named constants. No function bodies, no behavior, no
 * includes from core/ or meshclient/ — deliberately self-contained so
 * targets/sim/fixture.c (and any screen, later) can depend on it without
 * pulling in domain logic it doesn't need.
 *
 * ## Shape: union of what each mockup face needs
 * One sub-struct per face (`radar`, `now`, `signals`, `flare`, `settings`),
 * flattened to plain, JSON-fixture-friendly data — no pointers into a live
 * `fp_pack_t`/`ff_crew_t`/etc. (fixtures are standalone JSON snapshots, not
 * live wiring; see tests/fixtures/README.md for the schema and why).
 *
 * ## `radar` and `ff_radar_view_t` — a deliberate naming split, not a typo
 * docs/specs/S06-radar-face.md specs a real `ff_radar_view_t` that will
 * live in `core/include/ff_radar.h` once S06 lands, computed by
 * `ff_radar_compute()`. This header's `ff_app_radar_t` mirrors that
 * struct's fields 1:1 (same names, same types, same semantics) so a
 * future adapter is a pure field-copy — but it is deliberately NOT named
 * `ff_radar_view_t` and does NOT live in core/. Two anonymous structs with
 * identical member lists defined in two different headers are still
 * distinct, incompatible C types even when the typedef name matches
 * exactly (each `struct { ... }` is its own type); reusing the name here
 * would set up a hard redefinition error (or, on some compilers, a silent
 * ODR violation) the moment both headers are included in the same
 * translation unit — which every future screen file will do. Deviation
 * flagged here and in the PR body per AGENTS.md; S06's implementer should
 * either (a) have `ff_app_state_t.radar` become a real `ff_radar_view_t`
 * once core/include/ff_radar.h exists (delete `ff_app_radar_t`, s/.//),
 * or (b) keep both and add a one-line copy adapter — (a) is preferred.
 */
#ifndef FF_APP_STATE_H
#define FF_APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

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
 * radar (S06) — mirrors ff_radar_view_t field-for-field. See the
 * header-comment deviation note above for why the type name differs.
 * ------------------------------------------------------------------- */

/* Mirrors S06's radar_mode_t exactly (name, order, and members). */
typedef enum {
    FF_APP_RADAR_LIVE,
    FF_APP_RADAR_STALE,
    FF_APP_RADAR_LOST,
    FF_APP_RADAR_CLOSE,
    FF_APP_RADAR_NOFIX,
    FF_APP_RADAR_NOSEL,
} ff_app_radar_mode_t;

/* FF_CREW_MAX (firmware/core/include/ff_crew.h) is 8 as of S02; mirrored
 * here as a literal rather than an include (see header-comment: this file
 * takes no core/ dependency). If FF_CREW_MAX ever changes, this constant
 * must be updated to match — there is no automatic link between them. */
#define FF_APP_RADAR_MAX_DOTS 8

typedef struct {
    float   ring_deg;  /* heading-relative bearing around the crew ring */
    char    initial;   /* display letter, '\0' if unknown */
    uint8_t color_idx;  /* index into the theme crew palette */
    bool    stale;      /* dashed-dot rendering */
} ff_app_radar_dot_t;

/* DRIFT GUARD (PR #12 review finding #5): this struct's field list must
 * stay byte-for-byte in sync with docs/specs/S06-radar-face.md's
 * ff_radar_view_t sketch — see that spec file's matching comment right
 * above its own struct. Nothing enforces this at compile time (can't,
 * until core/include/ff_radar.h actually exists — see the header
 * comment above for the naming-collision reason this isn't just a
 * shared typedef). Until S06 lands and this whole struct is deleted in
 * favor of the real ff_radar_view_t: if you add/remove/retype a field
 * in ONE of these two places, update the other in the same change. */
typedef struct {
    ff_app_radar_mode_t mode;
    float arrow_deg;             /* smoothed screen rotation */
    bool  arrow_valid;           /* false in CLOSE/NOFIX/NOSEL */
    char  name[FF_APP_NAME_LEN];
    char  dist_str[FF_APP_STR_SHORT];
    char  age_str[FF_APP_STR_SHORT];
    int8_t trend;                /* -1/0/+1 (CLOSE mode hot/cold) */
    ff_app_radar_dot_t dots[FF_APP_RADAR_MAX_DOTS];
    uint8_t n_dots;
    char  clock_str[6];
    int8_t batt_pct;
    bool  mesh_ok;
} ff_app_radar_t;

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
 * flare (S10) — mirrors the IDLE/SENDING/RECEIVED/LOCKED state machine.
 * ------------------------------------------------------------------- */

typedef enum {
    FF_APP_FLARE_IDLE,
    FF_APP_FLARE_SENDING,
    FF_APP_FLARE_RECEIVED,
    FF_APP_FLARE_LOCKED,
} ff_app_flare_state_t;

typedef struct {
    ff_app_flare_state_t state;
    char    from_name[FF_APP_NAME_LEN]; /* sender, meaningful in RECEIVED/LOCKED */
    int32_t expires_in_ms;              /* -1: n/a (IDLE) or unknown */
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

    ff_app_radar_t    radar;
    ff_app_now_t      now;
    ff_app_signals_t  signals;
    ff_app_flare_t    flare;
    ff_app_settings_t settings;
} ff_app_state_t;

#ifdef __cplusplus
}
#endif

#endif /* FF_APP_STATE_H */
