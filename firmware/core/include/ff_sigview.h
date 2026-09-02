/**
 * ff_sigview.h — core/sigview: the Signals-face view-model & send target.
 *
 * Spec: docs/specs/S22-signals-rework.md, slice (a). This is the pure
 * core the reworked Signals face renders and forwards intents to; the
 * screen (`scr_inbox.c`, slice b) only projects this model and never
 * decides ordering, identity, presence, or targeting itself.
 *
 * Pure C11, no I/O, no LVGL, zero heap allocation — `ff_sigview_t` is a
 * plain struct (fixed-size arrays only) safe to put on the stack or in a
 * static. Zero-initialize or call `ff_sigview_init` before use.
 *
 * ## What this module does
 * It merges the two EXISTING sources — the incoming event feed
 * (`ff_feed_t`, ff_feed.h) and the crew roster (`ff_crew_t`, ff_crew.h) —
 * into one ordered, renderable row list, and it owns the "who does a send
 * go to" target state. It reads those sources; it never mutates them.
 *
 *   1. RECENT rows   — every feed item, newest-first (the feed's own
 *                      order), each JOINED to its crew member by
 *                      `from_node` for name / initial / color_idx.
 *   2. one DIVIDER   — the single "· CREW ·" marker row.
 *   3. CREW_QUIET    — every PAIRED crew member that has NO recent feed
 *                      item, ordered by presence (freshest sighting
 *                      first).
 *
 * ## Honesty rules this module is bound by (CLAUDE.md, [[firefly-touch-cal-default]])
 * Two provenance rules are load-bearing here and enforced in review:
 *
 *  - **Identity is never fabricated.** A RECENT row whose `from_node`
 *    has no paired match in the roster (or is 0 — a self-originated feed
 *    item carries no node id, see ff_feed.h) is emitted with
 *    `identity_known == false`, empty name, `'\0'` initial. The screen
 *    renders it as an explicitly-unknown sender, never a guessed name.
 *
 *  - **Presence is a freshness value, never a guessed "online"/"now".**
 *    A CREW_QUIET row's `presence` is derived only from real evidence —
 *    a measured position age (`ff_crew_freshness`) and/or a direct-packet
 *    RSSI age. A member we have never heard from shows `FF_PRESENCE_LINKED`
 *    (paired but no sighting) — NOT a fabricated recent time. An ASSERTED
 *    position (Meshtastic LOC_MANUAL, issue #33) is silent on age and so
 *    contributes no sighting age here.
 *
 * ## Where formatting lives
 * The CATEGORY decision (SEEN / LOST / LINKED, and the age in ms) is made
 * in core; turning an age into "6 MIN" is the screen's job via the shared
 * `ff_fmt_age` helper (ff_crew.h) — this module deliberately emits no
 * strings for ages, so there is one age-formatting implementation in the
 * tree, not two.
 */
#ifndef FF_SIGVIEW_H
#define FF_SIGVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "ff_crew.h"
#include "ff_feed.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Presence category for a quiet crew member — the honest "how recently is
 * there evidence of this person" axis the screen renders dimmed next to a
 * quiet member. Derived by `ff_sigview_presence`; see the honesty note in
 * this header's top comment.
 *
 *  - FF_PRESENCE_SEEN   — recent evidence exists (a measured position
 *                         and/or a direct packet within FF_CREW_LOST_MS);
 *                         the row carries the sighting age in `age_ms`,
 *                         which the screen formats ("SEEN 6 MIN").
 *  - FF_PRESENCE_LOST   — evidence exists but the freshest sighting is
 *                         older than FF_CREW_LOST_MS; `age_ms` still
 *                         carries the (old) real age.
 *  - FF_PRESENCE_LINKED — paired, but NO sighting has ever arrived (no
 *                         measured position, no direct packet). There is
 *                         no honest age; `age_ms` is 0 and meaningless.
 */
typedef enum {
    FF_PRESENCE_SEEN,
    FF_PRESENCE_LOST,
    FF_PRESENCE_LINKED,
} ff_sigview_presence_t;

/** The three row kinds the screen renders top-to-bottom (see top comment). */
typedef enum {
    FF_SIGROW_RECENT,     /* a feed item joined to a crew identity */
    FF_SIGROW_DIVIDER,    /* the single "· CREW ·" marker */
    FF_SIGROW_CREW_QUIET, /* a paired member with no recent feed item */
} ff_sigrow_kind_t;

/**
 * One renderable row. A row is exactly one `kind`; fields not relevant to
 * that kind are zeroed. `age_ms` does double duty by kind (a row is only
 * ever one kind, so this never ambiguates a single row):
 *   - RECENT     : age of the feed item, `now_ms - item.at_ms`.
 *   - CREW_QUIET : the presence sighting age (meaningful iff
 *                  `presence == FF_PRESENCE_SEEN` or `FF_PRESENCE_LOST`).
 *   - DIVIDER    : 0.
 */
typedef struct {
    ff_sigrow_kind_t kind;

    /* Identity — RECENT and CREW_QUIET. For CREW_QUIET `identity_known` is
     * always true (it is a paired roster member). For RECENT it is false
     * when `from_node` had no paired match / was self-originated. */
    uint32_t node_id;        /* sender/member node id; 0 when identity unknown */
    bool     identity_known; /* false: render as an explicitly-unknown sender */
    char     name[16];       /* copied from the crew member; "" if unknown */
    char     initial;        /* display letter; '\0' if unknown */
    uint8_t  color_idx;      /* crew-palette index; meaningful iff identity_known */

    /* RECENT-only feed facts. */
    ff_feed_kind_t feed_kind;
    bool           unread;

    /* CREW_QUIET-only presence category. */
    ff_sigview_presence_t presence;

    uint32_t age_ms; /* see the doc comment above for its meaning per kind */
} ff_sigrow_t;

/** Which send target a send acts on. WHOLE_CREW is the default and the
 * zero value, so a zero-initialized view targets the whole crew. */
typedef enum {
    FF_TARGET_WHOLE_CREW = 0,
    FF_TARGET_MEMBER,
} ff_target_kind_t;

/** Max rows: every feed item + the one divider + every crew member. */
#define FF_SIGVIEW_MAX_ROWS (FF_FEED_CAP + 1 + FF_CREW_MAX)

/**
 * ff_sigview_t — the whole view-model. Fully-defined (not opaque), same
 * "callers/tests can put it on the stack and inspect via the accessors"
 * convention as ff_feed_t / ff_crew_t. Holds two kinds of state:
 *   - PERSISTENT target state (survives rebuilds — `ff_sigview_build`
 *     never touches it).
 *   - the DERIVED row list, recomputed from scratch by each build.
 *
 * Use the accessors below rather than these fields directly.
 */
typedef struct {
    /* Persistent target state. */
    ff_target_kind_t target_kind;
    uint32_t         target_node; /* meaningful iff target_kind == FF_TARGET_MEMBER */

    /* Persistent RALLY-to-WHOLE_CREW confirm DISPLAY state (S22 slice d,
     * AC4). True when a first RALLY tap on a WHOLE_CREW target has ARMED
     * the one-loud-broadcast confirm and the screen should render the
     * RALLY button in its armed "tap again to send" state. This is purely
     * a display flag: the arming / timeout / disarm STATE MACHINE lives in
     * the shell (it needs a clock, which core has no business owning),
     * which reflects its live value into this field each tick the same way
     * it re-applies the target. Survives `ff_sigview_build` (a rebuild is a
     * pure re-projection of the sources and must not clear a pending
     * confirm). */
    bool rally_confirm_armed;

    /* Derived rows (rebuilt by ff_sigview_build). */
    ff_sigrow_t rows[FF_SIGVIEW_MAX_ROWS];
    uint16_t    row_count;
} ff_sigview_t;

/* Zero-heap guard, same idea as ff_crew_t's: pin the struct to a sane
 * upper bound so an accidental heap-shaped or oversized field fails the
 * build loudly. */
_Static_assert(sizeof(ff_sigview_t) < 8192,
               "ff_sigview_t grew unexpectedly large - check for accidental "
               "heap-shaped fields; zero-heap-allocation is an S22 AC");

/**
 * ff_sigview_init — clear a view to empty rows and the default target
 * (FF_TARGET_WHOLE_CREW). Equivalent to zero-initialization; provided so
 * call sites read as intentional.
 */
void ff_sigview_init(ff_sigview_t *v);

/**
 * ff_sigview_build — (re)compute the ordered row list from `feed` and
 * `crew`, as of `now_ms`. PRESERVES the target state (a rebuild is a
 * pure re-projection of the sources; it must not clear who you were about
 * to send to). No-op if `v` is NULL. A NULL `feed` or `crew` is treated
 * as the empty source (that half contributes no rows).
 *
 * Ordering (S22 AC1):
 *   1. RECENT — feed items newest-first (the feed's own order), each
 *      joined by `from_node` to a paired crew member for identity.
 *   2. one DIVIDER.
 *   3. CREW_QUIET — every paired member NOT already shown as a RECENT
 *      row, ordered freshest sighting first (LINKED members, having no
 *      sighting, sort last; ties broken by ascending node_id for a
 *      deterministic order).
 */
void ff_sigview_build(ff_sigview_t *v, ff_feed_t const *feed, ff_crew_t const *crew, uint32_t now_ms);

/** ff_sigview_row_count — number of rows the last build produced. */
uint16_t ff_sigview_row_count(ff_sigview_t const *v);

/**
 * ff_sigview_row_at — the `idx`-th row (0-based, top-to-bottom). NULL if
 * `v` is NULL or `idx >= ff_sigview_row_count(v)`. Valid until the next
 * `ff_sigview_build` on `v`.
 */
ff_sigrow_t const *ff_sigview_row_at(ff_sigview_t const *v, uint16_t idx);

/**
 * ff_sigview_presence — classify a member's presence as of `now_ms` from
 * its real evidence, and (for SEEN/LOST) report the freshest sighting age
 * via `out_age_ms`.
 *
 * Inputs are the honest, already-computed signals so the function is pure
 * and each branch is directly testable without constructing a whole
 * member:
 *   - `pos_fresh`     : the member's position freshness
 *                       (`ff_crew_freshness`).
 *   - `pos_age_ms`    : age of that position fix; a MEANINGFUL sighting
 *                       age ONLY when `pos_fresh` is LIVE / STALE / LOST.
 *                       For NEVER (no fix) and ASSERTED (silent on age,
 *                       issue #33) the position leg offers no age and is
 *                       ignored regardless of this value.
 *   - `have_rssi`     : true iff a direct packet has ever been heard
 *                       (`m->rssi_dbm != INT16_MIN`).
 *   - `rssi_age_ms`   : age of that last direct packet; used only when
 *                       `have_rssi`.
 *   - `out_age_ms`    : optional; on SEEN/LOST set to the freshest (min)
 *                       of the available sighting ages; left untouched on
 *                       LINKED. May be NULL.
 *
 * Result:
 *   - no evidence at all               -> FF_PRESENCE_LINKED.
 *   - freshest sighting <= FF_CREW_LOST_MS -> FF_PRESENCE_SEEN.
 *   - freshest sighting  > FF_CREW_LOST_MS -> FF_PRESENCE_LOST.
 * (The <=/> split matches ff_crew's own inclusive-toward-STALE boundary
 * convention: an age of exactly FF_CREW_LOST_MS is still SEEN.)
 */
ff_sigview_presence_t ff_sigview_presence(ff_freshness_t pos_fresh, uint32_t pos_age_ms, bool have_rssi,
                                          uint32_t rssi_age_ms, uint32_t *out_age_ms);

/* ---- Target (S22 AC3/AC4) ------------------------------------------- */

/**
 * ff_sigview_target_select — set the send target to a single paired
 * member. `crew` is REQUIRED (and is why this signature carries it,
 * unlike the spec's shorthand `(v, node_id)`): selection is validated
 * against the authoritative roster, not trusted blindly. Selecting an
 * unknown or NON-paired node — or node_id 0 — is REJECTED: the target is
 * left unchanged and the function returns false. On success the target
 * becomes FF_TARGET_MEMBER/`node_id` and it returns true. No-op returning
 * false if `v` or `crew` is NULL.
 */
bool ff_sigview_target_select(ff_sigview_t *v, ff_crew_t const *crew, uint32_t node_id);

/** ff_sigview_target_clear — return the target to WHOLE_CREW (an explicit
 * user "no single recipient"). No-op if `v` is NULL. */
void ff_sigview_target_clear(ff_sigview_t *v);

/** ff_sigview_target_reset_after_send — return the target to WHOLE_CREW
 * after a send (S22 AC3: "target resets after any send"). Same effect as
 * clear; named for intent at the call site. No-op if `v` is NULL. */
void ff_sigview_target_reset_after_send(ff_sigview_t *v);

/** ff_sigview_target_kind — the current target kind (WHOLE_CREW when `v`
 * is NULL). */
ff_target_kind_t ff_sigview_target_kind(ff_sigview_t const *v);

/** ff_sigview_target_node — the targeted member's node id, or 0 when the
 * target is WHOLE_CREW (or `v` is NULL). */
uint32_t ff_sigview_target_node(ff_sigview_t const *v);

/**
 * ff_sigview_rally_needs_confirm — true iff a RALLY would broadcast to the
 * WHOLE crew (S22 AC4: the one loud broadcast that requires a confirm).
 * Equivalent to `target_kind == FF_TARGET_WHOLE_CREW`. Pure; true when `v`
 * is NULL (the safe default is "treat it as the loud case").
 */
bool ff_sigview_rally_needs_confirm(ff_sigview_t const *v);

/**
 * ff_sigview_rally_confirm_armed — read the RALLY-to-WHOLE_CREW confirm
 * DISPLAY flag (S22 slice d): true iff the screen should render the RALLY
 * action button in its armed "tap again to send" state. False when `v` is
 * NULL. The flag is owned and driven by the shell's confirm state machine
 * (arm on the first WHOLE_CREW rally tap, disarm on send / timeout / any
 * intervening action); this accessor is the screen's read-only view of it.
 */
bool ff_sigview_rally_confirm_armed(ff_sigview_t const *v);

/**
 * ff_sigview_set_rally_confirm_armed — set the confirm display flag. Used
 * by the shell to reflect its live confirm-state-machine value into the
 * (per-tick rebuilt) view; no-op if `v` is NULL. Not part of the arming
 * logic itself — that lives in the shell.
 */
void ff_sigview_set_rally_confirm_armed(ff_sigview_t *v, bool armed);

#ifdef __cplusplus
}
#endif

#endif /* FF_SIGVIEW_H */
