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

#include "ff_inbox.h"   /* S24 — ff_inbox_t, the inbox half of the `signals` field */
#include "ff_notify.h"  /* S26(d) — ff_notify_kind_t, the real `banner` field's kind vocabulary */
#include "ff_radar.h"   /* S06 — ff_radar_view_t, the real `radar` field type */
#include "ff_sigview.h" /* S22/S24 — ff_target_kind_t (the send-scope vocabulary the S22(d) send machinery keeps) */

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
    /* 2026-08-24 amendment (S07-now-face.md ## Amendments, "starts-only
     * set grids"): mirrors ff_sched.h's ff_now_row_t.pct_valid exactly —
     * false iff this set's real end is unknowable (the last known-start
     * set on its stage this day, with a null end_min and no later
     * same-stage/same-day start to derive one from). The set IS live
     * (that's why it's a row at all); only its progress fraction isn't
     * knowable. Same "never let absence carry meaning" convention as
     * stage_color_valid above: scr_now.c must gate the progress-bar
     * render on this flag, not on pct_done being merely 0. */
    bool     pct_valid;
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

    /* [api] Issue #48 / S07-now-face.md Amendments ("PR #46 review, D3") —
     * added LAST per the renumbering caution both that Amendment and S16
     * slice a's ff_app_face_t precedent give: appending, not inserting,
     * keeps every already-committed fixture/golden's numeric encoding
     * stable, and targets/sim/fixture_view.c's own `default:` label means
     * `-Wswitch` under `-Werror` cannot flag an existing consumer over the
     * addition.
     *
     * A festpack IS loaded but the puck does not know what time it is yet
     * (ff_wall_t.src == FF_WALL_UNKNOWN) — the NORMAL boot path, since the
     * wall clock only latches once a plausible mesh timestamp arrives
     * during the want_config handshake, and a pack can load before that.
     * Before this member existed, `ff_shell.c`'s projection fell back to
     * NOW_NO_PACK, which `scr_now.c` rendered as "NO FESTIVAL LOADED /
     * Load a festpack..." — that MIS-claims: it names the wrong missing
     * fact (a pack the user already loaded) rather than the actual one
     * (the clock). NOW_TBD would be equally wrong the other way — it
     * asserts something about the DATA (the day's set times) when the gap
     * is entirely about the CLOCK. This member exists so the honest
     * unknown — "I have the schedule, I just don't know what time it is"
     * — is never forced to borrow a state that claims a different fact.
     * See `now_state_t`'s own "mutually exclusive by construction" doc
     * comment above: this is a distinct member, not an overload of
     * NOW_NO_PACK — never-let-absence-carry-meaning applies here exactly
     * as it does to `stage_color_valid`/`pct_valid` elsewhere in this
     * header. Render arm: scr_now.c's now_render_time_unknown. */
    NOW_TIME_UNKNOWN,
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
 * signals (S24, [api]) — the Signals face is now the S24 INBOX
 * (docs/specs/S24-signals-inbox.md, superseding the S22 unified-list
 * screen): `ff_app_signals_t` below embeds the genuine core `ff_inbox_t`
 * (core/include/ff_inbox.h, S24 slice a) by value — the same DRIFT-GUARD
 * resolution `radar` took (see that section above) and the S22(b)
 * precedent this replaces: the shell builds the conversation model
 * straight into the view each tick (`ff_inbox_build`), and the screen
 * (scr_signals.c) is a pure projection of it plus this struct's
 * navigation fields.
 *
 * What of S22 carries over ([api] note): the S22 target-line/action-
 * button RENDERING retires with its screen, but the S22(d) SEND
 * machinery stays for slice (d) — so `target_kind`/`target_node`/
 * `rally_confirm_armed` remain projected here (from the shell's own
 * persistent holders) even though slice (b) renders none of them; the
 * open thread IS the send scope, and opening one sets the target.
 * `ff_sigview_t` itself is no longer embedded anywhere in this struct
 * (the sigview module stays in core — ff_inbox reuses its presence
 * classifier and this header its `ff_target_kind_t` vocabulary).
 *
 * A fixture (tests/fixtures/signals_*.json) describes an
 * `ff_app_signals_t` directly — sub-view + conversation list — the same
 * "fixture is a view snapshot, not live wiring" convention the radar
 * fixtures use; see fx_parse_signals in targets/sim/fixture.c and
 * tests/fixtures/README.md.
 * ------------------------------------------------------------------- */

/**
 * ff_sig_subview_t — which Signals sub-screen is showing (S24's "more
 * screens that are easier to tap"; the shell owns the transitions, the
 * screen only renders whichever this names). INBOX is the zero value —
 * the least-claiming resting state, this header's standing convention.
 *
 * THREAD is the slice-(c) message screen (CREW + 1:1), rendered from
 * the embedded `thread` view below. POPUP and RALLY are DECLARED here so
 * the sub-view seam is complete for slice (d), but nothing routes to
 * them yet and the screen's `default:` arm falls back to the inbox
 * render — a clearly commented slice-d surface, not reachable dead
 * behavior.
 */
typedef enum {
    FF_SIG_SUB_INBOX = 0,
    FF_SIG_SUB_PICKER, /* recipient picker — the inbox FAB's scope step */
    FF_SIG_SUB_THREAD, /* the slice-(c) CREW / 1:1 message screen */
    FF_SIG_SUB_POPUP,  /* slice (d) — action popup over the (dimmed) thread */
    FF_SIG_SUB_RALLY,  /* slice (d) — the Rally WHERE/WHEN/Send screen */
} ff_sig_subview_t;

/**
 * ff_rally_when_t — the Rally screen's WHEN chip (S24 AC6): the rally time
 * relative to now. NOW is the zero value (the least-claiming default) and
 * appends nothing to the wire name; +15m / +30m ride in the rally NAME
 * suffix (ff_proto_rally_t has no time field — S24's WHEN-in-name, see the
 * shell's rally send). */
typedef enum {
    FF_RALLY_WHEN_NOW = 0,
    FF_RALLY_WHEN_15,
    FF_RALLY_WHEN_30,
} ff_rally_when_t;

/**
 * FF_APP_RALLY_MAX_PLACES — the Rally WHERE list's landmark-row cap. Held
 * as a literal here (this header stays free of fp_pack.h — the flattened-
 * struct convention), pinned to FP_MAX_LANDMARKS by a _Static_assert in
 * ff_shell.c so a festpack cap bump surfaces as a build failure, never a
 * silent truncation. */
#define FF_APP_RALLY_MAX_PLACES 12

/**
 * ff_app_rally_t — the Rally sub-view's projected state (S24 slice d,
 * AC6). Built by the shell each tick ONLY while `subview == FF_SIG_SUB_
 * RALLY` (like `thread` for THREAD); zeroed otherwise. The screen is a
 * pure renderer of this: it draws the WHERE radio list (On Me + the
 * landmark rows), the WHEN chip and the payload-echoing Send, and emits
 * the RALLY_SELECT_PLACE / RALLY_CYCLE_WHEN / RALLY_SEND intents by index
 * — it never touches the festpack or resolves a position itself.
 */
typedef struct {
    /* On Me (rally to my live location) — pinned first, index 0 of the
     * selection space. DISABLED with an honest reason when my position is
     * unknown (`on_me_ok` false); never a fabricated {0,0} (CLAUDE.md). */
    bool on_me_ok;

    /* The festpack landmark rows (real names, in pack order, only those
     * with a known position). Scales to the pack; 0 when no pack. */
    uint8_t place_count;
    char    place_names[FF_APP_RALLY_MAX_PLACES][FF_APP_NAME_LEN];

    /* The current selection: 0 = On Me, 1..place_count = landmark
     * (sel - 1). The shell keeps a selectable default (On Me if usable,
     * else the first landmark, else 0 with nothing sendable). */
    uint8_t sel;

    /* The WHEN chip's current value. */
    ff_rally_when_t when;

    /* The Send button's payload echo — "<place> · <when>" — composed by
     * the shell from the SAME resolution the send uses, so the echo can
     * never disagree with what goes on the wire. Empty when nothing is
     * sendable (On Me disabled and no places). */
    char echo_place[FF_APP_NAME_LEN];
    char echo_when[8]; /* "Now" / "+15m" / "+30m" */
    bool can_send;

    /* Crew-wide confirm (S22 AC4 precedent, reused): a rally to the whole
     * crew arms on the first Send tap and sends on the second. Mirrors the
     * shell's sig_rally_armed while the Rally screen is up. */
    bool confirm_armed;
} ff_app_rally_t;

typedef struct {
    ff_sig_subview_t subview;

    /* The conversation model (rebuilt per tick by the shell; the picker
     * renders from the same list — CREW pinned + each paired member). */
    ff_inbox_t inbox;

    /* Open-thread scope — meaningful iff subview is THREAD (or, in
     * slices c/d, POPUP/RALLY over it). Node 0 = the CREW conversation
     * (the ff_inbox convention). `thread_name` is the scope's display
     * name, "CREW" for the communal thread, looked up at projection time
     * (so a NodeInfo rename mid-thread is reflected — the compose
     * to_name precedent); "" for a paired member whose name never
     * arrived, rendered honestly as such. */
    uint32_t thread_node;
    char     thread_name[FF_APP_NAME_LEN];
    uint8_t  thread_color_idx;

    /* [api] S24 slice (c) — the OPEN thread's messages: the genuine core
     * `ff_inbox_thread_t` (core/include/ff_inbox.h), embedded by value —
     * the same drift-guard resolution as `inbox` above. Built per tick by
     * the shell (`ff_inbox_thread_build`) iff `subview == FF_SIG_SUB_
     * THREAD`; left zeroed (msg_count 0) for every other sub-view, so a
     * fixture that names a thread sub-view but authors no `msgs` renders
     * an honest empty thread, never a stale one. Fixtures author it as
     * `signals.msgs` (targets/sim/fixture.c). */
    ff_inbox_thread_t thread;

    /* S22(d) send-machinery state, kept for slice (d) (see the section
     * comment): the current send scope + the rally-to-crew confirm
     * display flag, projected each tick from the shell's holders. */
    ff_target_kind_t target_kind;
    uint32_t         target_node;
    bool             rally_confirm_armed;

    /* [api] S24 slice (d) — the Rally sub-view's projected WHERE/WHEN/Send
     * state, built by the shell only while `subview == FF_SIG_SUB_RALLY`
     * (zeroed otherwise). See ff_app_rally_t. */
    ff_app_rally_t rally;
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
    /* [api] S08 predictive addendum — the PRIMARY predictive-T9 mode
     * (docs/specs/S08-signals-t9.md, "Predictive + pack dictionary =
     * v1.5"): press each key ONCE per letter and the engine
     * (core/ff_t9pred) ranks whole words. The composer defaults to this
     * on open (ff_shell.c). APPENDED last, not inserted, per this header's
     * own renumbering caution (see now_state_t's NOW_TIME_UNKNOWN and
     * ff_app_face_t's FLARE additions): ABC/123/SYM keep their numeric
     * encoding, so every already-committed compose fixture/golden (which
     * encodes `mode` as an int, default "abc") is byte-stable and the
     * predictive fields below stay zeroed there. */
    FF_APP_COMPOSE_PRED,
} ff_app_compose_mode_t;

/* Predictive projection budgets (S08 addendum). WORD_LEN mirrors the pack
 * artist cap (fp_set_t.artist[32] == FF_APP_ARTIST_LEN) so any word the
 * engine can return — a supplied festival name being the longest — always
 * fits; a dictionary word is far shorter. MAX_CAND is how many candidate
 * chips a snapshot carries for the "› cycle / tap-to-select" row; 6 is the
 * mockup's chip budget (the honest `total_cand` below still reports the real
 * engine count when it exceeds this). */
#define FF_APP_COMPOSE_WORD_LEN 32
#define FF_APP_COMPOSE_MAX_CAND 6

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

    /* -----------------------------------------------------------------
     * Predictive-T9 projection (S08 addendum). Meaningful ONLY when
     * `mode == FF_APP_COMPOSE_PRED`; the shell leaves every field below
     * zeroed in the other modes, so multitap/123/SYM project exactly as
     * they did before this addendum. HONEST-DATA (CLAUDE.md): none of
     * these is ever a fabricated value — `word` is verbatim from the
     * engine's current candidate (never a literal key string), `from_pack`
     * is set by POINTER IDENTITY against the shell's festpack word table
     * (not name-matching), and `total_cand` is the real engine count, not
     * the shown-chip count.
     * ----------------------------------------------------------------- */

    /* The in-progress predicted word: the engine's currently-selected
     * candidate for the digits typed so far. "" when nothing is predicted
     * yet (no digits) OR on an honest no-match (see `word_nomatch`). */
    char     word[FF_APP_COMPOSE_WORD_LEN];

    /* True iff the user has typed digits AND the engine honestly returned
     * NO word for them (dictionary and festpack both). Distinct from the
     * empty-word "nothing typed yet" state so the screen can say "no match"
     * without ever inventing one — the whole honest-no-match contract. */
    bool     word_nomatch;

    /* The top candidates for the current digits, best-first, up to
     * FF_APP_COMPOSE_MAX_CAND of them. `from_pack` is true iff this
     * candidate pointer is one of the shell's festpack-supplied names
     * (pointer identity), so the screen can badge festival vocabulary
     * distinctly from dictionary words. */
    struct {
        char text[FF_APP_COMPOSE_WORD_LEN];
        bool from_pack;
    } cand[FF_APP_COMPOSE_MAX_CAND];
    uint8_t  n_cand;      /* number of populated `cand[]` entries (0..MAX_CAND) */
    uint8_t  sel_cand;    /* the engine's selection index among ALL candidates
                           * (highlights cand[sel_cand] when < n_cand; `word`
                           * is authoritative when the selection has cycled
                           * past the shown window) */
    uint16_t total_cand;  /* the REAL total matches from the engine — may
                           * exceed n_cand; drives a "3 of 12" indicator */
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

    /* [api] S11 slice b addition: the Settings face's UTC-offset row needs
     * a current value to render and step from — omitted until now because
     * nothing rendered this section at all (S11b is the first real
     * consumer). Mirrors ff_settings_t's own utc_offset_min/utc_offset_set
     * pair field-for-field (core/include/ff_settings.h, S16 slice b0's
     * amendment to S11) — same "0 is legitimately UTC, so the flag is not
     * redundant" reasoning applies here: a bare int16_t cannot distinguish
     * "never configured" from "configured to UTC+0", and scr_settings.c
     * must render the former as an honest "UNSET", never a fabricated
     * "+0:00" (CLAUDE.md: "unknown = explicitly unknown"). */
    int16_t  utc_offset_min;
    bool     utc_offset_set;

    /* [api] S17 slice a addition — mirrors ff_settings_t.colorblind
     * field-for-field (core/include/ff_settings.h). The Settings face
     * renders this row's ON/OFF state; every screen that draws a crew
     * dot/wedge reads it (threaded as an explicit `bool colorblind`
     * parameter into ff_scr_radar_build/ff_scr_map_build, NOT read from
     * this struct directly by those two — see ff_theme.h's own doc
     * comment for why an explicit parameter over a hidden global). */
    bool     colorblind;

    /* [api] #100 — display brightness percent, mirrors ff_settings_t.
     * brightness_pct field-for-field (core/include/ff_settings.h). The
     * Settings face renders it as a slider; the device app forwards it to
     * the backlight HAL. Clamped to [FF_BRIGHTNESS_MIN_PCT,
     * FF_BRIGHTNESS_MAX_PCT] by the shell before it ever reaches here. */
    uint8_t  brightness_pct;
} ff_app_settings_t;
/* S21 removed ff_app_settings_t.page / FF_SETTINGS_PAGE_COUNT (#105's
 * pagination): the Settings face is now one scrolling list, so there is no
 * page state to project. Scrolling is a live LVGL concern owned by
 * scr_settings.c's list container, not projected shell state — see that
 * file and the scroll-aware sweep in test_face_hit_targets.c. */

/* -------------------------------------------------------------------
 * map (S09) — flattened festpack features + crew/rally/YOU, all already
 * expressed in the SAME east/north-meters frame `ff_map_xform_t`
 * (core/include/ff_map.h) fits. `fixture.c` intentionally has zero
 * festpack dependency (see its own header comment) — same as `now`
 * above, this is a flattened, JSON-fixture-friendly mirror of
 * `fp_feature_t`, NOT `radar`'s "the real core struct" drift-guard
 * exception, and NOT a live `fp_pack_t` pointer.
 * ------------------------------------------------------------------- */

/* Deliberately SMALLER than fp_pack.h's FP_MAX_FEATURES(24)/
 * FP_MAX_POLY_PTS(24) — same "view-level cap, independent of core's
 * storage cap" convention as ff_app_now_t.lineup (32) against
 * FP_MAX_SETS (256): fp_pack_t answers to a 48KB budget, this flattened
 * view answers to ff_app_state_t's much smaller budget (see that
 * struct's own _Static_assert comment) — and `ff_shell_t` carries TWO
 * full `ff_app_state_t` copies (the rendered view plus the previous
 * frame's render key, `ff_shell.c`'s dirty-bit machinery), so this
 * struct's real cost to the shell's own budget (`FF_SHELL_BYTES`,
 * `ff_shell.h`) is doubled again.
 *
 * PR #73 REVIEW FINDING #1 (BLOCKING, both tier-3 and Bailey,
 * independently): the ORIGINAL 8/12 here was NOT generous headroom over
 * real data — it was measured against this repo's own VENDORED test
 * fixture (`firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json`,
 * every feature `n_pts: 0` at the time), not against the real,
 * currently-merged `fest-almanac` pack, which has **13** `map.features`
 * entries (9-point "Venue extent" the largest polygon) the moment
 * anyone actually renders it — `shell_project_map` silently dropped
 * RV/tent camping, Village Marketplace and First aid with zero
 * indication anything was missing, on the exact face that's supposed to
 * tell you where medical is. Raised to comfortably clear BOTH the real
 * pack (13 features, 9 points) AND the fest-almanac schema's own stated
 * guidance (~15 features): 20 features (~54% headroom over real, ~33%
 * over the schema's own guidance) / 16 points (~78% headroom over the
 * real pack's largest polygon). This is no longer a "never observed a
 * pack this size" cap — it comfortably out-sizes every number anyone
 * has actually named. Silent-truncation-if-still-exceeded is
 * ALSO closed now: `shell_project_map` sets `ff_app_map_t.truncated`
 * (+ `features_omitted`) instead of dropping data with no signal, and
 * `scr_map.c` renders an honest "+N MORE" indicator when it's true —
 * see that struct's own doc comment. A pack that outgrows even this is
 * still a real, HONESTLY-SURFACED constraint of the flattened view, not
 * a silent one. */
#define FF_APP_MAP_MAX_FEATURES 20
#define FF_APP_MAP_MAX_POLY_PTS 16
#define FF_APP_MAP_LABEL_LEN    32 /* mirrors fp_pack.h's fp_feature_t.label */

/* Mirrors fp_pack.h's fp_feature_kind_t exactly (name, order, members) —
 * same "mirrors S08's ff_feed_kind_t exactly" convention as
 * ff_app_feed_kind_t above. */
typedef enum {
    FF_APP_MAP_KIND_UNKNOWN,
    FF_APP_MAP_KIND_STAGE,
    FF_APP_MAP_KIND_CAMPING,
    FF_APP_MAP_KIND_WATER,
    FF_APP_MAP_KIND_PATH,
    FF_APP_MAP_KIND_ENTRANCE,
    FF_APP_MAP_KIND_VENDOR,
    FF_APP_MAP_KIND_MEDICAL,
    FF_APP_MAP_KIND_POI,
} ff_app_map_kind_t;

typedef struct {
    ff_app_map_kind_t kind;
    char    label[FF_APP_MAP_LABEL_LEN];
    uint32_t color_rgb;   /* meaningful only if color_valid — same "prove you meant this" convention as ff_app_now_row_t.stage_color_valid */
    bool    color_valid;
    /* n_pts == 0: UNTRACED — S09's exact honesty policy: rendered as a
     * label only (never an invented shape), except a stage, which gets a
     * labeled stub circle IF it carries at least one point (n_pts >= 1
     * with a single entry) — see scr_map.c. A feature with no polygon
     * and no policy-covered stub is never drawn as a shape, full stop. */
    uint8_t n_pts;
    float   pts_en[FF_APP_MAP_MAX_POLY_PTS][2]; /* [east_m, north_m] */
} ff_app_map_feature_t;

/**
 * ff_app_map_crew_t — one crew member's dot on the map. Reuses the
 * freshness/asserted/precision vocabulary PR #69 (issue #33/#47) gave
 * Radar's ring dots, where it maps onto a map dot the same way it maps
 * onto a radar dot — see `stale`/`place`/`imprecise` below for exactly
 * how far that reuse goes and where it stops, per this slice's PR body
 * (the task brief's own instruction: "where it doesn't map naturally,
 * say so... rather than inventing policy").
 */
typedef struct {
    char    initial;
    uint8_t color_idx;   /* theme crew palette index, same convention as ff_radar_dot_t.color_idx */
    bool    has_pos;
    float   east_m, north_m;
    /* Mirrors ff_radar_dot_t.stale exactly: this member's OWN freshness
     * is STALE/LOST/NEVER. Renders as a dashed ring per S09's own spec
     * text ("stale ⇒ dashed"). */
    bool    stale;
    /* Mirrors ff_radar_dot_t.place: this member's latest position is an
     * ASSERTION (issue #33), not a measurement. Carried through so the
     * fact is available, but S09's spec text does not itself define a
     * distinct map render for it the way it defines "stale ⇒ dashed" —
     * scr_map.c renders a `place` dot as an ordinary solid ring (a real
     * coordinate exists; only the AGE claim `stale` would carry is what
     * `place` withholds, same as Radar's PLACE mode keeping a solid
     * arrow). A visually distinct map treatment for `place` (Radar's
     * "FIXED POSITION" chip has no map equivalent yet) is a real gap,
     * left unfixed here rather than invented — see the PR body. */
    bool    place;
    /* Mirrors issue #47's dist_imprecise gate: this member's latest fix
     * has a known-degraded precision (`has_precision_bits &&
     * precision_bits < FF_CREW_POS_PRECISION_MIN_BITS`). This is the one
     * flag with a MAP-SPECIFIC render, not a straight copy of Radar's
     * treatment of the same fact — and PR #73 review finding #3
     * corrected a wrong claim this comment used to make about WHY, so
     * stated precisely now: Radar's crew RING DOTS (`ff_radar_dot_t`,
     * `ff_radar_compute`'s `dots[]`) do NOT handle imprecision at all
     * today — an imprecise member gets an ordinary, undegraded, crisp
     * ring dot there, at a bearing computed from the raw (possibly
     * kilometers-off) coordinate. (`ff_radar_view_t.dist_imprecise` is a
     * different, narrower fact — it only degrades the *selected*
     * member's distance TEXT, not any ring dot's geometry.) So this
     * field's map-specific fuzzy-ring render (see below) is not
     * "matching what Radar already does" — it is a deliberately MORE
     * honest treatment than Radar's current status quo, motivated by
     * the same principle Radar's own dist_imprecise gate uses elsewhere:
     * a map dot IS a pin-point claim by construction (unlike a bare
     * distance number, which can honestly degrade to an area string),
     * so scr_map.c does NOT draw the normal 18px initial ring for an
     * imprecise member — see that file's doc comment for the
     * fuzzy-circle treatment it draws instead. Giving Radar's own ring
     * dots the same treatment is a real, separate gap, tracked as
     * issue #74 rather than folded into this PR. */
    bool    imprecise;
} ff_app_map_crew_t;

typedef struct {
    ff_app_map_feature_t features[FF_APP_MAP_MAX_FEATURES];
    uint8_t n_features;

    /* PR #73 review finding #1: honest overflow surfacing, matching
     * fixture.c's fail-loud convention on the same cap. `truncated` is
     * true iff the source pack had MORE features than
     * FF_APP_MAP_MAX_FEATURES could hold, OR any KEPT feature's own
     * polygon had more points than FF_APP_MAP_MAX_POLY_PTS could hold —
     * i.e. this view is KNOWN incomplete, not merely small.
     * `features_omitted` is how many whole features were dropped (0 if
     * every feature was kept, even when some kept feature's polygon was
     * itself point-truncated). `scr_map.c` renders an honest "+N MORE"
     * indicator whenever `truncated` is true, rather than silently
     * presenting a truncated view as if it were the whole map. */
    bool    truncated;
    uint8_t features_omitted;

    ff_app_map_crew_t crew[FF_CREW_MAX];
    uint8_t n_crew;

    /* Rally: core has no rally-selection state yet (`ff_crew.h`'s own
     * documented gap — "ff_crew_select_rally() is not implemented here
     * ... deferred to S06/S08") — so `has_rally` is honestly `false` on
     * every LIVE projection today; this section exists so scr_map.c and
     * its fixtures/goldens can render the pin once that gap closes,
     * without another `[api]` change to this struct. */
    bool    has_rally;
    char    rally_label[FF_APP_MAP_LABEL_LEN];
    float   rally_east_m, rally_north_m;

    /* YOU. */
    bool    you_has_pos;
    float   you_east_m, you_north_m;
    /* false -> arrow hidden + "NO FIX" chip (S09 AC5) — same "prove you
     * meant this" convention as radar.arrow_valid / flare.takeover_bearing_valid. */
    bool    you_heading_valid;
    float   you_heading_deg;
} ff_app_map_t;

/* -------------------------------------------------------------------
 * ff_app_state_t — the whole snapshot.
 * ------------------------------------------------------------------- */

/* Which face is on screen; drives both the real shell (S06 slice b,
 * scr_nav.c) and — until real screens exist — which section of state the
 * S13 debug placeholder face highlights (see targets/sim/fixture_view.c). */
typedef enum {
    /* S16 slice a [api] — "no face". The zero value, per this header's
     * "the enum's first member is deliberately the least-claiming
     * state" convention (see now_state_t's comment above, and
     * radar.mode's RADAR_NOSEL default).
     *
     * Its one real use is `ff_route_t.modal` (app/include/ff_route.h),
     * where `modal == FF_APP_FACE_NONE` IS the entire "no modal is up"
     * predicate — there is deliberately no separate has_modal flag.
     *
     * NOT a renderable `ff_app_state_t.active_face`: a projection names
     * a real face (the sole exception is a failed fixture load, which
     * zeroes the whole struct — see that field's own comment). Note
     * that adding this member renumbered the
     * enum, so `memset(0)` on an ff_app_state_t now leaves active_face
     * as NONE where it previously landed on RADAR — every producer that
     * relied on that coincidence sets RADAR explicitly now (see
     * targets/sim/fixture.c and targets/sim/main.c). */
    FF_APP_FACE_NONE = 0,
    FF_APP_FACE_RADAR,
    FF_APP_FACE_NOW,
    FF_APP_FACE_SIGNALS,
    /* HORIZONTAL-CAROUSEL REWORK [api]: Settings is the far-right SWIPE
     * FACE now (fifth of five: Radar · Now · Signals · Map · Settings),
     * not a modal. It used to be reached by a nav long-press pushing a
     * modal; the long-press survives as a JUMP shortcut
     * (`ff_route_goto`), but Settings is on `ff_route`'s swipe axis and
     * `ff_route_push_modal` no longer accepts it. You swipe left to leave
     * (the old back-button dismiss is gone with the modal). */
    FF_APP_FACE_SETTINGS,
    /* S08 slice d — reached from Signals' "+", NOT a swipe tile of its
     * own; rendered as its own full screen, see scr_compose.h. Compose
     * is the sole remaining MODAL face after the horizontal-carousel
     * rework (Map and Settings became swipe tiles) — the one value
     * `ff_route_push_modal` still accepts. */
    FF_APP_FACE_COMPOSE,
    /* S09 [api] — Radar's alternate view (docs/specs/S09-map-face.md:
     * "Purpose: Radar's alternate view").
     *
     * HORIZONTAL-CAROUSEL REWORK [api]: Map is now an ordinary SWIPE
     * FACE, the fourth of five in the single left-to-right carousel
     * `scr_nav.c`'s tileview builds — Radar · Now · Signals · Map ·
     * Settings — sitting between Signals and Settings. It used to be a
     * modal reached by a VERTICAL top-swipe on the tileview
     * (`LV_DIR_TOP` -> `FF_INTENT_OPEN_MAP`), which is precisely what let
     * a vertical list-scroll bubble up as a GESTURE and jump here; making
     * every face a horizontal neighbour is the fix. So `ff_route`'s swipe
     * axis now includes Map, `ff_route_push_modal` no longer accepts it,
     * and there is no `FF_INTENT_OPEN_MAP` — you swipe to Map like any
     * other tile, and swipe away to leave (the old "tap anywhere -> back"
     * modal-dismiss is gone with the modal). Recorded per
     * CLAUDE.md/AGENTS.md's "note the interpretation" rule — see the PR
     * body for the full reasoning. */
    FF_APP_FACE_MAP,
    /* S16 slice a [api] — ROUTING ONLY. This is `ff_route_visible()`'s
     * answer for "a takeover is up, so the next intent goes to the
     * takeover, not to whatever is underneath it" — an input-dispatch
     * answer, not a render instruction.
     *
     * `ff_app_state_t.active_face` is NEVER FF_APP_FACE_FLARE, in any
     * projection, including while a takeover is active (S16 AC13): the
     * takeover stays `ff_flare_t.takeover_active`'s single fact, and
     * targets/sim/face_dispatch.c keeps dispatching on that field
     * directly. Writing FLARE in here as well would put one fact in two
     * places and re-create, one layer down, exactly the desync that
     * keeps `takeover` out of ff_route_t. */
    FF_APP_FACE_FLARE,
    /* S26 slice b [api] (docs/specs/S26-device-lifecycle.md "(b) Power
     * button -> power menu -> soft power-off"). Appended after FLARE, so
     * no existing face's numeric value moves (this repo's standing
     * append-only convention for enums with call sites already keyed on
     * a specific member — e.g. ff_intent_kind_t's own "Appended, so no
     * existing intent's numeric value moves" notes). The PWR-button
     * LONG_PRESS modal: three big round-glass buttons (Power off /
     * Reboot / Cancel), see app/screens/scr_power_menu.h. The SECOND (and,
     * for now, last) value `ff_route_push_modal` accepts — Compose was
     * "the sole modal face" before this slice; see that function's
     * updated doc comment. Fully static content (no dynamic view-state),
     * so — unlike `compose`/`flare`/`map` above — this face has no
     * `ff_app_state_t` section of its own: `active_face ==
     * FF_APP_FACE_POWER_MENU` is the entire "render the power menu" fact
     * (scr_power_menu.c's build function takes no argument), and
     * `ff_shell.c`'s render key masks every OTHER section while this is
     * the active face (same "opaque overlay" discipline the flare
     * takeover / Signals popup already use — see shell_render_key's
     * comment there) so churn underneath cannot rebuild this modal
     * mid-press. Unlike FLARE, this IS a renderable `active_face` value
     * (it is a real modal, not a routing-only sentinel). */
    FF_APP_FACE_POWER_MENU,
    /* [api] S26 slice e (docs/specs/S26-device-lifecycle.md "(e) Home
     * button + launcher"). Appended after POWER_MENU, so no existing
     * face's numeric value moves — this repo's standing convention for
     * this enum. The BOOT-button home screen: a ring of app circles
     * (Now / Signals / Map / Settings — Radar is not listed, it IS
     * home), reached only from Radar (`ff_route_home`) and left either
     * by BOOT again (back to Radar) or by tapping a circle (straight
     * into that app, `ff_route_launcher_select`).
     *
     * The THIRD value `ff_route_push_modal` accepts (`app/include/
     * ff_route.h`) — like POWER_MENU, it is a modal over `base`, which
     * stays RADAR the whole time the launcher is up. Unlike POWER_MENU
     * it is not fully static: the Signals circle carries the unread
     * badge (`ff_scr_signals_unread_count`, moved off the old page-dot
     * row — see scr_launcher.h), so `ff_shell.c`'s render-key mask for
     * this face keeps that one scalar and zeroes everything else, the
     * same "reduce to exactly what's drawn" discipline the power-menu
     * mask uses (see shell_render_key's comment there).
     *
     * This slice RETIRES the five-face horizontal swipe carousel as a
     * navigation mechanism (`scr_nav.c`'s gesture handler and page-dot
     * row are gone; `ff_route_swipe` stays a tested pure primitive with
     * no live caller — see that function's doc comment) and the
     * long-press-anywhere shortcut to Settings (Settings is a launcher
     * circle now). `ff_route`'s five-member swipe axis itself
     * (RADAR/NOW/SIGNALS/MAP/SETTINGS) is UNCHANGED — it is still what
     * `base` ranges over, and still how `ff_scr_nav_build` picks what to
     * render; only how a user REACHES a given base face changed, from a
     * horizontal drag to a launcher tap. */
    FF_APP_FACE_LAUNCHER,
} ff_app_face_t;

/* -------------------------------------------------------------------
 * banner (S26 slice d, docs/specs/S26-device-lifecycle.md "(d) ff_notify
 * + message banner") — the shell's `ff_notify` queue's HEAD, projected
 * for `scr_banner.c` to render as an overlay strip. `active == false`:
 * nothing to show (the queue is empty), the least-claiming default —
 * this header's standing zero-value convention.
 *
 * Reuses the real core `ff_notify_kind_t` directly for `kind` — the same
 * DRIFT GUARD resolution `radar`/`signals.inbox` take above (a real core
 * enum, not a hand-mirrored second one) — but is otherwise FLATTENED like
 * `flare`: `ff_notify_entry_t` deliberately has no crew dependency (core
 * stays dependency-light, the `ff_flare_t` precedent — see
 * `core/include/ff_notify.h`), so the sender's display NAME and crew
 * palette COLOR are this struct's own app-layer additions, looked up at
 * projection time (a NodeInfo rename mid-banner is reflected, same as
 * every other name lookup in this header).
 */
typedef struct {
    bool active; /* a BANNER is currently showing (ff_notify_head() != NULL) */

    ff_notify_kind_t kind;
    uint32_t         node_id; /* the sender's node id, verbatim from the queue entry */
    char             name[FF_APP_NAME_LEN]; /* sender display name at projection time; "" honestly unknown */
    uint8_t          color_idx;             /* the sender's crew palette index */
    char             text[FF_NOTIFY_TEXT_MAX]; /* the preview text, verbatim from the queue entry */

    /* now_ms - the entry's at_ms, REAL (CLAUDE.md: never a fabricated
     * "now" — S26 spec: "a banner shows the real at_ms age via
     * ff_fmt_age"). Rendered through that one shared formatter, same as
     * every other age in this app. */
    uint32_t age_ms;
} ff_app_banner_t;

typedef struct {
    /* Debug/provenance only: which fixture produced this state, e.g.
     * "radar_live". Not part of any mockup — the S13 placeholder face
     * renders it so goldens visibly identify themselves; a real S06+
     * screen ignores this field entirely. */
    char fixture_name[FF_APP_FIXTURE_NAME_LEN];

    /* Sim/golden render hint only (#bug5a), same category as fixture_name
     * above: a fixture may request the Settings list be scrolled to this
     * vertical offset (device points) before the screenshot, so a golden can
     * capture a NON-zero scroll position (goldens otherwise only ever see
     * scroll 0). The sim dispatcher (targets/sim/face_dispatch.c) applies it
     * via ff_scr_settings_apply_scroll_hint after building Settings; LVGL
     * clamps it to the scrollable range, and 0 (the default, and the only
     * value the live shell ever carries) is a no-op. No real screen or shell
     * logic reads this — it is authoring scaffolding for the scrolled
     * goldens, not projected state. Parsed by targets/sim/fixture.c. */
    int32_t ui_settings_scroll_y;

    /* In any RENDERABLE state: a real face — RADAR/NOW/SIGNALS/
     * SETTINGS/COMPOSE/MAP — and never FF_APP_FACE_FLARE (S16 AC13, see
     * that member's comment).
     *
     * The one exception, stated because the invariant is otherwise
     * false rather than because it is a good idea (PR #36 review, D2):
     * a FAILED ff_fixture_load_json leaves *out fully zeroed by its own
     * documented contract, so active_face reads NONE. That is a state
     * no caller may render — every call site today bails on a non-OK
     * return before building anything, and one that logs-and-continues
     * instead would get the S13 debug placeholder. Check the result
     * code; do not check this field for NONE. */
    ff_app_face_t active_face;

    ff_radar_view_t   radar;
    ff_app_now_t      now;
    ff_app_signals_t  signals; /* S24 — sub-view + the core ff_inbox_t, rendered directly */
    ff_app_compose_t  compose;
    ff_app_flare_t    flare;
    ff_app_settings_t settings;
    ff_app_map_t      map;
    ff_app_banner_t   banner; /* S26(d) — the ff_notify queue's head; scr_banner.c's whole input */
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
 * reason to grow past it. S22 is such a reason: embedding the real
 * `ff_sigview_t` view-model (its FF_SIGVIEW_MAX_ROWS=41 fixed row array +
 * target) in place of the small flattened `ff_app_signals_t` added ~1.2KB,
 * pushing past the old 8KB — bumped to 12KB, still generous headroom over
 * the ~8.2KB actual, and still a runaway-growth tripwire rather than a hard
 * hardware limit. */
_Static_assert(sizeof(ff_app_state_t) <= 12 * 1024,
               "ff_app_state_t exceeds its 12KB view-state budget (see this assert's comment)");

#ifdef __cplusplus
}
#endif

#endif /* FF_APP_STATE_H */
