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

/* S07 slice b, round 2 (PR #21 code review finding #1): per-day list of
 * sets whose time is NOT known yet. Used two ways depending on
 * `ff_app_now_t.state`: under NOW_TBD it's every set on the day (all of
 * them lack a time, by definition of that state); under NOW_MIXED it's
 * just the still-unknown subset, alongside `rows`/`next` for the sets
 * that DO have a time. Never used to hide a known set the app knows
 * about — see NOW_MIXED's doc comment below for why this field exists at
 * all (the bug this fixes: a set with an unknown time used to disappear
 * entirely rather than showing up here). 32 is generous headroom over any
 * real single festival day we've actually seen (the vendored Lost Lands
 * 2026 pack's day 1 has 7 sets — see
 * firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json) while
 * staying well under fp_pack.h's whole-pack FP_MAX_SETS (256) cap; a
 * pathological single-day lineup beyond this fails loud at fixture-load
 * time (see fixture.c's fx_parse_now), same "cap enforced, never silently
 * truncated" contract as `rows`/signals.items. */
#define FF_APP_NOW_MAX_LINEUP 32

typedef struct {
    char    artist[FF_APP_ARTIST_LEN];
    char    stage_name[FF_APP_STAGE_LEN];
    uint32_t stage_color_rgb;  /* 0x00RRGGBB, from fp_stage_t.color_rgb — meaningless unless stage_color_valid */
    /* PR #21 code review finding #3: 0 used to double as BOTH "no color
     * given" and "the fixture's #rrggbb was malformed" (fx_color_rgb's old
     * failure return), which silently misrendered a genuinely BLACK stage
     * color as the same "unset -> muted grey" fallback as a missing/broken
     * one — and made a malformed color indistinguishable from a valid one
     * that just happens to parse to 0x000000. An explicit validity flag
     * fixes both: a real black stage renders black, and a malformed color
     * is now a distinctly observable/testable state (fixture.c sets this
     * false, not just stage_color_rgb=0, on a parse failure) rather than
     * silently masquerading as "not provided". See
     * targets/sim/tests/test_fixture.c's now_stage_color_rgb_* cases. */
    bool     stage_color_valid;
    uint8_t pct_done;
} ff_app_now_row_t;

typedef struct {
    bool    valid; /* false: no starred set upcoming (next_starred returned false) */
    char    artist[FF_APP_ARTIST_LEN];
    char    stage_name[FF_APP_STAGE_LEN];
    int16_t mins_until;
} ff_app_next_t;

/* One entry in the still-unknown-time list (`ff_app_now_t.lineup`): just
 * enough to render "ARTIST — STAGE" (or the explicit "STAGE UNKNOWN"
 * fallback scr_now.c renders when the stage itself is also unknown —
 * fp_set_t.stage_idx can be -1, e.g. several of the real Lost Lands
 * pack's early-entry sets). No times: every set that reaches this list
 * is, by construction, one whose start/end the app does not know. */
typedef struct {
    char artist[FF_APP_ARTIST_LEN];
    char stage_name[FF_APP_STAGE_LEN]; /* "" = stage unknown, render honestly (never invent one) */
} ff_app_lineup_item_t;

/**
 * now_state_t — the Now face's render state, mutually exclusive by
 * construction (PR #21 code review finding #2/ruling: this replaces an
 * earlier `pack_loaded`+`tbd` bool pair, which represented a 3-state
 * lifecycle with 4 representable combinations — the nonsense one wasn't
 * even the bug, the real cost was that "which state is this" depended on
 * check ORDER in the renderer rather than being a fact of the data. Same
 * fix `core/include/ff_radar.h`'s `radar_mode_t` already applied to the
 * Radar face.) A fixture/view-builder sets exactly one of these; nothing
 * downstream re-derives it from other fields.
 */
typedef enum {
    NOW_NO_PACK,         /* no festpack loaded at all (tests/fixtures/now_empty.json) */
    NOW_TBD,              /* pack loaded; every set on the day lacks a known time (now_tbd.json) */
    NOW_MIXED,            /* pack loaded; day.mixed, ROUND 2 fix — see NOW_MIXED's own note below */
    NOW_LIVE,             /* pack loaded; every set on the day has a known time; something's now/next */
    NOW_NOTHING_PLAYING,  /* pack loaded; every set on the day has a known time; nothing now/next right now */
} now_state_t;

typedef struct {
    /* Mutually exclusive by construction — see now_state_t's own doc
     * comment for the [api] history (was `pack_loaded`+`tbd` bools).
     * Defaults to NOW_NO_PACK (the zero value: the enum's first member is
     * deliberately the least-claiming state, same "unknown/absent
     * defaults to the least-claiming state" convention as radar.mode
     * defaulting to RADAR_NOSEL — see fixture.c's ff_fixture_load_json).
     * A fixture that omits `now` entirely, or omits `state` within it,
     * honestly renders as "nothing loaded" rather than silently claiming
     * live schedule data exists. */
    now_state_t state;

    /* NOW_LIVE / NOW_NOTHING_PLAYING / NOW_MIXED: the day's known-time
     * sets that are currently playing (up to 3) and the next starred one.
     * Unused (left zeroed) under NOW_NO_PACK/NOW_TBD. */
    ff_app_now_row_t rows[FF_APP_NOW_MAX_ROWS];
    uint8_t n_rows;
    ff_app_next_t next;

    /* NOW_TBD (every entry) / NOW_MIXED (round 2 fix — code review
     * finding #1: the day's REMAINING unknown-time sets, rendered
     * alongside `rows`/`next` rather than disappearing the moment even
     * one set on the day gets a real time; the real-world trigger the
     * reviewer flagged is Lost Lands' pack times landing stage-by-stage
     * before the Sep 18-20 field test — a "some known, most still not"
     * day is the expected near-term state, not a hypothetical edge case).
     * Unused under NOW_NO_PACK/NOW_LIVE/NOW_NOTHING_PLAYING. */
    ff_app_lineup_item_t lineup[FF_APP_NOW_MAX_LINEUP];
    uint8_t n_lineup;
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
 * compose (S08 slice d) — the T9 composer screen. Flattened, same
 * "JSON-fixture-friendly, no live core struct" convention as now/signals
 * above (NOT the radar drift-guard exception): a fixture is a standalone
 * snapshot, and the real Compose screen owns its own live `ff_t9_t`
 * internally (app/screens/scr_compose.c), mutated by keypresses — it
 * isn't reconstructed from this struct on every render. This struct only
 * needs to describe what a snapshot LOOKS like: the recipient, the
 * display text `ff_t9_text()` would currently show (committed + any live
 * pending char), and which keypad page is active.
 * ------------------------------------------------------------------- */

/* Deliberately NOT `#include "ff_t9.h"` for this one budget constant —
 * see this section's header note above (a plain flattened struct, unlike
 * `radar`). Value mirrors FF_T9_MAX_LEN(160)+2 (ff_t9.h's own committed +
 * pending-char + NUL display budget) so a fixture's `compose.text` can
 * always hold anything the real engine could ever show; kept as an
 * independent literal rather than pulling in a second core include for
 * one number.  */
#define FF_APP_COMPOSE_TEXT_LEN 162

/* Keypad page — S08 Amendments (2026-08-23, owner ruling on the open
 * digits/symbols question): ABC (multi-tap letters, the original v1
 * scope) -> 123 (literal digits) -> SYM (curated ASCII-emoticon
 * shortcuts, the tier-2 fallback — see docs/specs/S08-signals-t9.md's
 * Amendments and app/screens/scr_compose.c's header comment for why
 * real-emoji-font tier 1 wasn't shipped this PR), cycled by a dedicated
 * mode key. */
typedef enum {
    FF_APP_COMPOSE_ABC,
    FF_APP_COMPOSE_123,
    FF_APP_COMPOSE_SYM,
} ff_app_compose_mode_t;

typedef struct {
    char text[FF_APP_COMPOSE_TEXT_LEN]; /* mirrors ff_t9_text(): committed + live pending char */
    char to_name[FF_APP_NAME_LEN];      /* recipient display name; "" = broadcast */
    /* True when the LAST character in `text` is a live pending (not yet
     * committed) char — drives the pending-char visual treatment
     * (S08: "message bubble with live pending character"). Not derivable
     * from `text` alone (a fixture can't tell "the last char is
     * committed" from "the last char is pending" just by reading the
     * string — both look identical), so it's carried as its own field,
     * same reasoning as `ff_t9_t.has_pending` in core/include/ff_t9.h. */
    bool                    has_pending;
    ff_app_compose_mode_t   mode;
} ff_app_compose_t;

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
     * `ff_radar_view_t.dist_str` uses.
     *
     * `takeover_bearing_valid` (PR #20 code review, LOW finding): a
     * bearing genuinely can be unknown (no position fix on either end —
     * the exact case `ff_radar_view_t.arrow_valid` exists to represent
     * on the sibling Radar face), and unlike `takeover_dist_str` (which
     * has an honest empty-string "unknown" of its own, see
     * `ff_scr_flare_build_takeover`'s "-- m" fallback), a bare `float`
     * has no such value — `0.0` is indistinguishable from "genuinely due
     * north." Mirrors `arrow_valid`'s name and meaning exactly: false
     * means `takeover_bearing_deg` must NOT be rendered as a real
     * reading (CLAUDE.md: "never fake... positions"). Defaults to false
     * (unknown) both via zero-init and the fixture loader's explicit
     * default, matching every other "prove you meant this" field in this
     * struct family. */
    bool     takeover_active;
    char     takeover_from_name[FF_APP_NAME_LEN];
    bool     takeover_bearing_valid;          /* false: bearing unknown, do not render takeover_bearing_deg */
    float    takeover_bearing_deg;            /* [0, 360), compass bearing to sender; meaningful iff *_valid */
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
    /* S08 slice d — reached from Signals' "+", not a swipe tile of its
     * own (scr_nav.c's tileview only ever has 3 tiles: Radar/Now/
     * Signals); rendered as its own full screen, see scr_compose.h. */
    FF_APP_FACE_COMPOSE,
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
    ff_app_compose_t  compose;
    ff_app_flare_t    flare;
    ff_app_settings_t settings;
} ff_app_state_t;

/* PR #21 code review finding #4: ff_app_state_t grew ~2.5x in this PR
 * (~1300B -> ~3.2KB) with nothing tracking the total. Mirrors
 * fp_pack_t's own documented budget (fp_pack.h: "must live comfortably
 * in ESP32-S3 PSRAM"). Unlike that 48KB figure, no spec states a hard
 * number for THIS struct — this is a judgment call (flagged per
 * AGENTS.md's "note the interpretation" rule), picked as generous
 * headroom (roughly 2.5x today's actual size) over a hard hardware
 * limit, specifically so it catches runaway growth (a face adding an
 * unbounded or needlessly large array) rather than nuisance-tripping on
 * ordinary per-slice growth as S08/S10/S11 add their own sections to
 * this same struct. Revisit the number if a future slice has a real
 * reason to grow past it. */
_Static_assert(sizeof(ff_app_state_t) <= 8 * 1024,
               "ff_app_state_t exceeds its 8KB view-state budget (see this assert's comment)");

#ifdef __cplusplus
}
#endif

#endif /* FF_APP_STATE_H */
