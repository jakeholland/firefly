/**
 * test_sigview.c — S22 slice (a) acceptance criteria for core/sigview.
 *
 * Test names follow docs/specs/S22-signals-rework.md's numbered
 * acceptance criteria: S22_ACn_description. A few unnumbered tests cover
 * null-guard / accessor behavior the spec implies but doesn't number.
 *
 * Proxy-check discipline (AGENTS.md standing brief / docs/review/
 * code-review.md item 6) is applied throughout — notably:
 *  - the ordering test feeds quiet members in a SCRAMBLED roster order so
 *    a passing result cannot come from already-sorted input, and it
 *    exercises the LINKED and LOST branches (not just SEEN);
 *  - the join tests use node_ids that never equal their slot index, and
 *    assert the joined NAME, so a from_node==index coincidence can't pass;
 *  - presence tests exercise every branch including ASSERTED-is-silent and
 *    "the freshest sighting wins" (RSSI rescuing a LOST/NEVER position).
 */
#include <limits.h>
#include <string.h>

#include "unity.h"

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_sigview.h"

void setUp(void) {}
void tearDown(void) {}

/* Fixed reference "now" for age math. Member timestamps are ABSOLUTE
 * (ff_crew.h note), so a member with sighting age A sets its timestamp to
 * NOW - A. */
#define NOW ((uint32_t)1000000u)

/* ------------------------------------------------------------------- */
/* helpers                                                              */
/* ------------------------------------------------------------------- */

static ff_feed_item_t make_item(ff_feed_kind_t kind, uint32_t from_node, uint32_t at_ms, char const *text,
                                 bool unread)
{
    ff_feed_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind      = kind;
    it.from_node = from_node;
    it.at_ms     = at_ms;
    if (text != NULL) {
        strncpy(it.text, text, sizeof(it.text) - 1);
    }
    it.unread = unread;
    return it;
}

/* Add a crew member directly into a (zeroed) roster. Default: NO direct
 * packet ever (rssi_dbm == INT16_MIN) and NO position — the caller opts
 * into sightings explicitly, so a memset-zeroed rssi_dbm (== 0, which is
 * NOT the "never" sentinel) can never silently make a member look SEEN. */
static ff_crew_member_t *add_member(ff_crew_t *c, uint32_t node_id, char const *name, char initial,
                                    uint8_t color_idx, bool paired)
{
    ff_crew_member_t *m = &c->members[c->count++];
    memset(m, 0, sizeof(*m));
    m->node_id = node_id;
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->initial   = initial;
    m->color_idx = color_idx;
    m->paired    = paired;
    m->rssi_dbm  = INT16_MIN; /* no direct packet by default */
    return m;
}

static void set_pos_age(ff_crew_member_t *m, uint32_t age_ms, bool asserted)
{
    m->has_pos      = true;
    m->pos_age_ms   = NOW - age_ms;
    m->pos_asserted = asserted;
}

static void set_rssi_age(ff_crew_member_t *m, int16_t dbm, uint32_t age_ms)
{
    m->rssi_dbm    = dbm;
    m->rssi_age_ms = NOW - age_ms;
}

/* Find the first row of a given kind, or NULL. */
static ff_sigrow_t const *first_of_kind(ff_sigview_t const *v, ff_sigrow_kind_t k)
{
    for (uint16_t i = 0; i < ff_sigview_row_count(v); ++i) {
        ff_sigrow_t const *r = ff_sigview_row_at(v, i);
        if (r->kind == k) {
            return r;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- */
/* AC1 — unified ordered list, identity join                           */
/* ------------------------------------------------------------------- */

static void S22_AC1_recent_rows_are_newest_first(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));

    ff_feed_item_t a = make_item(FEED_TEXT, 0, 100, "oldest", false);
    ff_feed_item_t b = make_item(FEED_TEXT, 0, 200, "middle", false);
    ff_feed_item_t d = make_item(FEED_TEXT, 0, 300, "newest", false);
    ff_feed_push(&f, &a);
    ff_feed_push(&f, &b);
    ff_feed_push(&f, &d);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    /* First three rows are RECENT, newest first -> smallest age first. */
    ff_sigrow_t const *r0 = ff_sigview_row_at(&v, 0);
    ff_sigrow_t const *r1 = ff_sigview_row_at(&v, 1);
    ff_sigrow_t const *r2 = ff_sigview_row_at(&v, 2);
    TEST_ASSERT_EQUAL(FF_SIGROW_RECENT, r0->kind);
    TEST_ASSERT_EQUAL(FF_SIGROW_RECENT, r1->kind);
    TEST_ASSERT_EQUAL(FF_SIGROW_RECENT, r2->kind);
    /* newest (at_ms 300) has the smallest age */
    TEST_ASSERT_EQUAL_UINT32(NOW - 300, r0->age_ms);
    TEST_ASSERT_EQUAL_UINT32(NOW - 200, r1->age_ms);
    TEST_ASSERT_EQUAL_UINT32(NOW - 100, r2->age_ms);
}

static void S22_AC1_recent_joins_from_node_to_correct_identity(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));

    /* Two members; node_ids deliberately != slot index. The feed item is
     * from the SECOND member — a join that keyed off slot index or the
     * first member would join wrong. */
    add_member(&c, 7001, "Alice", 'A', 2, true); /* slot 0 */
    add_member(&c, 7002, "Bob", 'B', 5, true);   /* slot 1 */

    ff_feed_item_t it = make_item(FEED_PULSE, 7002, 500, NULL, true);
    ff_feed_push(&f, &it);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    ff_sigrow_t const *r = ff_sigview_row_at(&v, 0);
    TEST_ASSERT_EQUAL(FF_SIGROW_RECENT, r->kind);
    TEST_ASSERT_TRUE(r->identity_known);
    TEST_ASSERT_EQUAL_UINT32(7002, r->node_id);
    TEST_ASSERT_EQUAL_STRING("Bob", r->name);
    TEST_ASSERT_EQUAL_CHAR('B', r->initial);
    TEST_ASSERT_EQUAL_UINT8(5, r->color_idx);
    TEST_ASSERT_EQUAL(FEED_PULSE, r->feed_kind);
    TEST_ASSERT_TRUE(r->unread);
}

static void S22_AC1_recent_unknown_from_node_is_honest_unknown(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 7001, "Alice", 'A', 2, true);

    ff_feed_item_t stranger = make_item(FEED_TEXT, 9999, 400, "who?", false); /* not in roster */
    ff_feed_item_t self     = make_item(FEED_TEXT, 0, 300, "me", false);      /* self-originated */
    ff_feed_push(&f, &stranger);
    ff_feed_push(&f, &self);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    /* self (newest) then stranger */
    ff_sigrow_t const *r_self     = ff_sigview_row_at(&v, 0);
    ff_sigrow_t const *r_stranger = ff_sigview_row_at(&v, 1);
    TEST_ASSERT_FALSE(r_self->identity_known);
    TEST_ASSERT_EQUAL_UINT32(0, r_self->node_id);
    TEST_ASSERT_EQUAL_STRING("", r_self->name);
    TEST_ASSERT_EQUAL_CHAR('\0', r_self->initial);
    TEST_ASSERT_FALSE(r_stranger->identity_known);
    TEST_ASSERT_EQUAL_UINT32(0, r_stranger->node_id);
    TEST_ASSERT_EQUAL_STRING("", r_stranger->name);
}

static void S22_AC1_single_divider_between_recent_and_quiet(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    ff_crew_member_t *q = add_member(&c, 7001, "Alice", 'A', 2, true);
    set_pos_age(q, 60000, false); /* quiet, SEEN */

    ff_feed_item_t it = make_item(FEED_TEXT, 0, 500, "hi", false);
    ff_feed_push(&f, &it);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    /* Exactly one divider, and every RECENT precedes it, every CREW_QUIET
     * follows it. */
    int      dividers   = 0;
    int      divider_at = -1;
    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        if (ff_sigview_row_at(&v, i)->kind == FF_SIGROW_DIVIDER) {
            dividers++;
            divider_at = (int)i;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, dividers);
    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        ff_sigrow_t const *r = ff_sigview_row_at(&v, i);
        if (r->kind == FF_SIGROW_RECENT) {
            TEST_ASSERT_TRUE((int)i < divider_at);
        } else if (r->kind == FF_SIGROW_CREW_QUIET) {
            TEST_ASSERT_TRUE((int)i > divider_at);
        }
    }
}

static void S22_AC1_paired_member_with_recent_item_is_not_in_quiet(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    ff_crew_member_t *chatty = add_member(&c, 7001, "Chatty", 'C', 1, true);
    ff_crew_member_t *quiet  = add_member(&c, 7002, "Quiet", 'Q', 2, true);
    set_pos_age(chatty, 30000, false);
    set_pos_age(quiet, 30000, false);

    ff_feed_item_t it = make_item(FEED_TEXT, 7001, 500, "yo", false); /* from Chatty */
    ff_feed_push(&f, &it);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    /* Chatty appears as RECENT, NOT as quiet; Quiet appears as quiet. */
    int chatty_quiet = 0, quiet_quiet = 0, chatty_recent = 0;
    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        ff_sigrow_t const *r = ff_sigview_row_at(&v, i);
        if (r->kind == FF_SIGROW_CREW_QUIET && r->node_id == 7001) chatty_quiet++;
        if (r->kind == FF_SIGROW_CREW_QUIET && r->node_id == 7002) quiet_quiet++;
        if (r->kind == FF_SIGROW_RECENT && r->node_id == 7001) chatty_recent++;
    }
    TEST_ASSERT_EQUAL_INT(1, chatty_recent);
    TEST_ASSERT_EQUAL_INT(0, chatty_quiet);
    TEST_ASSERT_EQUAL_INT(1, quiet_quiet);
}

static void S22_AC1_unpaired_member_never_in_quiet(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    ff_crew_member_t *unp = add_member(&c, 7003, "Heard", 'H', 3, false /* not paired */);
    set_pos_age(unp, 30000, false);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        TEST_ASSERT_NOT_EQUAL(FF_SIGROW_CREW_QUIET, ff_sigview_row_at(&v, i)->kind);
    }
}

static void S22_AC1_quiet_crew_ordered_freshest_first_scrambled_input(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));

    /* Add in a SCRAMBLED order (not freshest-first), and cover the LINKED
     * and LOST branches, so a pass cannot come from already-sorted input. */
    ff_crew_member_t *linked = add_member(&c, 5002, "Bravo", 'B', 1, true);   /* NEVER -> LINKED */
    ff_crew_member_t *lost   = add_member(&c, 5004, "Delta", 'D', 2, true);   /* LOST */
    ff_crew_member_t *seen5m = add_member(&c, 5001, "Alpha", 'A', 3, true);   /* SEEN 300s */
    ff_crew_member_t *seen1m = add_member(&c, 5003, "Charlie", 'E', 4, true); /* SEEN 60s */
    (void)linked;                                                             /* no sighting: leave NEVER */
    set_pos_age(lost, 700000, false);
    set_pos_age(seen5m, 300000, false);
    set_pos_age(seen1m, 60000, false);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    /* Collect quiet rows in order. */
    uint32_t              order[FF_CREW_MAX];
    ff_sigview_presence_t pres[FF_CREW_MAX];
    int                   n = 0;
    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        ff_sigrow_t const *r = ff_sigview_row_at(&v, i);
        if (r->kind == FF_SIGROW_CREW_QUIET) {
            order[n] = r->node_id;
            pres[n]  = r->presence;
            n++;
        }
    }
    TEST_ASSERT_EQUAL_INT(4, n);
    /* freshest first: SEEN 60s, SEEN 300s, LOST, LINKED */
    TEST_ASSERT_EQUAL_UINT32(5003, order[0]);
    TEST_ASSERT_EQUAL_UINT32(5001, order[1]);
    TEST_ASSERT_EQUAL_UINT32(5004, order[2]);
    TEST_ASSERT_EQUAL_UINT32(5002, order[3]);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, pres[0]);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, pres[1]);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, pres[2]);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, pres[3]);
}

static void S22_AC1_quiet_tie_break_is_node_id_ascending(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    /* Two quiet members with IDENTICAL sighting age -> deterministic order
     * by ascending node_id. Add higher node_id first to prove the sort
     * reorders it. */
    ff_crew_member_t *hi = add_member(&c, 6002, "Hi", 'H', 1, true);
    ff_crew_member_t *lo = add_member(&c, 6001, "Lo", 'L', 2, true);
    set_pos_age(hi, 120000, false);
    set_pos_age(lo, 120000, false);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    uint32_t order[FF_CREW_MAX];
    int      n = 0;
    for (uint16_t i = 0; i < ff_sigview_row_count(&v); ++i) {
        ff_sigrow_t const *r = ff_sigview_row_at(&v, i);
        if (r->kind == FF_SIGROW_CREW_QUIET) {
            order[n++] = r->node_id;
        }
    }
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT32(6001, order[0]);
    TEST_ASSERT_EQUAL_UINT32(6002, order[1]);
}

/* ------------------------------------------------------------------- */
/* AC2 — honest presence                                               */
/* ------------------------------------------------------------------- */

static void S22_AC2_live_position_is_seen_with_age(void)
{
    uint32_t              age = 0xDEADBEEF;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_LIVE, 10000, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, p);
    TEST_ASSERT_EQUAL_UINT32(10000, age);
}

static void S22_AC2_stale_position_is_seen(void)
{
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_STALE, 300000, false, 0, NULL));
}

static void S22_AC2_lost_position_is_lost_with_real_age(void)
{
    uint32_t              age = 0;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_LOST, 700000, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST, p);
    TEST_ASSERT_EQUAL_UINT32(700000, age);
}

static void S22_AC2_never_no_rssi_is_linked_and_leaves_age_untouched(void)
{
    uint32_t              age = 0x1234;
    ff_sigview_presence_t p   = ff_sigview_presence(FF_FRESH_NEVER, 999999, false, 0, &age);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, p);
    TEST_ASSERT_EQUAL_UINT32(0x1234, age); /* untouched — no honest age exists */
}

static void S22_AC2_asserted_is_silent_on_age_no_rssi_is_linked(void)
{
    /* An ASSERTED position (LOC_MANUAL) must NOT be read as a recent
     * sighting — with no direct packet, the member is LINKED, never SEEN,
     * regardless of the (meaningless) pos_age value. */
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, ff_sigview_presence(FF_FRESH_ASSERTED, 5000, false, 0, NULL));
}

static void S22_AC2_rssi_rescues_never_to_seen(void)
{
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_NEVER, 999999, true, 5000, &age));
    TEST_ASSERT_EQUAL_UINT32(5000, age);
}

static void S22_AC2_freshest_sighting_wins_rssi_over_lost_position(void)
{
    /* Old measured position (LOST) but a fresh direct packet -> SEEN with
     * the fresh RSSI age (we DID just hear them). */
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_LOST, 700000, true, 5000, &age));
    TEST_ASSERT_EQUAL_UINT32(5000, age);
}

static void S22_AC2_freshest_sighting_wins_position_over_old_rssi(void)
{
    uint32_t age = 0;
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_sigview_presence(FF_FRESH_LIVE, 10000, true, 700000, &age));
    TEST_ASSERT_EQUAL_UINT32(10000, age); /* min of the two */
}

static void S22_AC2_seen_lost_boundary_is_inclusive_toward_seen(void)
{
    /* age == FF_CREW_LOST_MS is still SEEN; one ms more is LOST. */
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN,
                      ff_sigview_presence(FF_FRESH_STALE, FF_CREW_LOST_MS, false, 0, NULL));
    TEST_ASSERT_EQUAL(FF_PRESENCE_LOST,
                      ff_sigview_presence(FF_FRESH_LOST, FF_CREW_LOST_MS + 1, false, 0, NULL));
}

static void S22_AC2_build_quiet_row_rssi_only_is_seen(void)
{
    /* End-to-end through build: a member with NO position ever but a fresh
     * direct packet must render SEEN (via rssi), not LINKED — proves build
     * feeds rssi state into presence, not just the position leg. */
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    ff_crew_member_t *m = add_member(&c, 8100, "Rssi", 'R', 1, true); /* has_pos stays false */
    set_rssi_age(m, -55, 4000);                                       /* heard 4s ago */

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    ff_sigrow_t const *q = first_of_kind(&v, FF_SIGROW_CREW_QUIET);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_UINT32(8100, q->node_id);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, q->presence);
    TEST_ASSERT_EQUAL_UINT32(4000, q->age_ms);
}

static void S22_AC2_build_quiet_row_never_heard_is_linked(void)
{
    /* The paranoid proxy guard: a memset-zeroed member has rssi_dbm == 0
     * (NOT the INT16_MIN "never" sentinel); add_member sets the sentinel,
     * so a never-heard member is honestly LINKED with age 0. */
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8101, "Cold", 'C', 1, true); /* no pos, no rssi */

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);

    ff_sigrow_t const *q = first_of_kind(&v, FF_SIGROW_CREW_QUIET);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, q->presence);
    TEST_ASSERT_EQUAL_UINT32(0, q->age_ms);
}

/* ------------------------------------------------------------------- */
/* AC3 — target state                                                  */
/* ------------------------------------------------------------------- */

static void S22_AC3_default_target_is_whole_crew(void)
{
    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(&v));
    TEST_ASSERT_EQUAL_UINT32(0, ff_sigview_target_node(&v));
}

static void S22_AC3_select_paired_member_sets_target(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_TRUE(ff_sigview_target_select(&v, &c, 8001));
    TEST_ASSERT_EQUAL(FF_TARGET_MEMBER, ff_sigview_target_kind(&v));
    TEST_ASSERT_EQUAL_UINT32(8001, ff_sigview_target_node(&v));
}

static void S22_AC3_select_unpaired_is_rejected_target_unchanged(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8002, "Unpaired", 'U', 1, false);

    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_FALSE(ff_sigview_target_select(&v, &c, 8002));
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(&v));
}

static void S22_AC3_select_unknown_or_zero_is_rejected(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_FALSE(ff_sigview_target_select(&v, &c, 12345)); /* unknown id */
    TEST_ASSERT_FALSE(ff_sigview_target_select(&v, &c, 0));     /* the sentinel */
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(&v));
}

static void S22_AC3_clear_returns_to_whole_crew(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_target_select(&v, &c, 8001);
    ff_sigview_target_clear(&v);
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(&v));
    TEST_ASSERT_EQUAL_UINT32(0, ff_sigview_target_node(&v));
}

static void S22_AC3_reset_after_send_returns_to_whole_crew(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_target_select(&v, &c, 8001);
    ff_sigview_target_reset_after_send(&v);
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(&v));
}

static void S22_AC3_build_preserves_target(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_TRUE(ff_sigview_target_select(&v, &c, 8001));

    /* A rebuild is a pure re-projection; it must not clear the target. */
    ff_sigview_build(&v, &f, &c, NOW);
    TEST_ASSERT_EQUAL(FF_TARGET_MEMBER, ff_sigview_target_kind(&v));
    TEST_ASSERT_EQUAL_UINT32(8001, ff_sigview_target_node(&v));
}

/* ------------------------------------------------------------------- */
/* AC4 — rally-to-whole-crew confirm predicate                         */
/* ------------------------------------------------------------------- */

static void S22_AC4_rally_needs_confirm_only_for_whole_crew(void)
{
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, 8001, "Paired", 'P', 1, true);

    ff_sigview_t v;
    ff_sigview_init(&v);
    TEST_ASSERT_TRUE(ff_sigview_rally_needs_confirm(&v)); /* default whole-crew */

    ff_sigview_target_select(&v, &c, 8001);
    TEST_ASSERT_FALSE(ff_sigview_rally_needs_confirm(&v)); /* single member */

    ff_sigview_target_clear(&v);
    TEST_ASSERT_TRUE(ff_sigview_rally_needs_confirm(&v));
}

/* ------------------------------------------------------------------- */
/* Unnumbered — null guards & accessors (AC7: pure, safe surface)      */
/* ------------------------------------------------------------------- */

static void S22_null_guards_are_no_ops_not_crashes(void)
{
    ff_sigview_build(NULL, NULL, NULL, NOW);
    TEST_ASSERT_EQUAL_UINT16(0, ff_sigview_row_count(NULL));
    TEST_ASSERT_NULL(ff_sigview_row_at(NULL, 0));
    TEST_ASSERT_EQUAL(FF_TARGET_WHOLE_CREW, ff_sigview_target_kind(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, ff_sigview_target_node(NULL));
    TEST_ASSERT_TRUE(ff_sigview_rally_needs_confirm(NULL)); /* safe default: loud case */
    TEST_ASSERT_FALSE(ff_sigview_target_select(NULL, NULL, 1));
    ff_sigview_target_clear(NULL);
    ff_sigview_target_reset_after_send(NULL);
}

static void S22_build_with_null_sources_yields_only_divider(void)
{
    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, NULL, NULL, NOW);
    TEST_ASSERT_EQUAL_UINT16(1, ff_sigview_row_count(&v));
    TEST_ASSERT_EQUAL(FF_SIGROW_DIVIDER, ff_sigview_row_at(&v, 0)->kind);
    TEST_ASSERT_NULL(ff_sigview_row_at(&v, 1));
}

static void S22_row_at_out_of_range_is_null(void)
{
    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, NULL, NULL, NOW);
    TEST_ASSERT_NOT_NULL(ff_sigview_row_at(&v, 0));
    TEST_ASSERT_NULL(ff_sigview_row_at(&v, ff_sigview_row_count(&v)));
}

static void S22_full_feed_and_crew_stays_within_capacity(void)
{
    /* Worst case: cap feed items + full crew. Must not overrun rows. */
    ff_feed_t f;
    ff_feed_init(&f);
    for (uint32_t i = 0; i < FF_FEED_CAP + 5u; ++i) {
        ff_feed_item_t it = make_item(FEED_TEXT, 0, i + 1, "x", false);
        ff_feed_push(&f, &it);
    }
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    for (uint32_t i = 0; i < FF_CREW_MAX; ++i) {
        ff_crew_member_t *m = add_member(&c, 9000 + i, "M", 'M', (uint8_t)i, true);
        set_pos_age(m, 1000 * (i + 1), false);
    }

    ff_sigview_t v;
    ff_sigview_init(&v);
    ff_sigview_build(&v, &f, &c, NOW);
    TEST_ASSERT_TRUE(ff_sigview_row_count(&v) <= FF_SIGVIEW_MAX_ROWS);
    /* FF_FEED_CAP recent + 1 divider + FF_CREW_MAX quiet == the max. */
    TEST_ASSERT_EQUAL_UINT16(FF_FEED_CAP + 1 + FF_CREW_MAX, ff_sigview_row_count(&v));
}

/* ------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S22_AC1_recent_rows_are_newest_first);
    RUN_TEST(S22_AC1_recent_joins_from_node_to_correct_identity);
    RUN_TEST(S22_AC1_recent_unknown_from_node_is_honest_unknown);
    RUN_TEST(S22_AC1_single_divider_between_recent_and_quiet);
    RUN_TEST(S22_AC1_paired_member_with_recent_item_is_not_in_quiet);
    RUN_TEST(S22_AC1_unpaired_member_never_in_quiet);
    RUN_TEST(S22_AC1_quiet_crew_ordered_freshest_first_scrambled_input);
    RUN_TEST(S22_AC1_quiet_tie_break_is_node_id_ascending);

    RUN_TEST(S22_AC2_live_position_is_seen_with_age);
    RUN_TEST(S22_AC2_stale_position_is_seen);
    RUN_TEST(S22_AC2_lost_position_is_lost_with_real_age);
    RUN_TEST(S22_AC2_never_no_rssi_is_linked_and_leaves_age_untouched);
    RUN_TEST(S22_AC2_asserted_is_silent_on_age_no_rssi_is_linked);
    RUN_TEST(S22_AC2_rssi_rescues_never_to_seen);
    RUN_TEST(S22_AC2_freshest_sighting_wins_rssi_over_lost_position);
    RUN_TEST(S22_AC2_freshest_sighting_wins_position_over_old_rssi);
    RUN_TEST(S22_AC2_seen_lost_boundary_is_inclusive_toward_seen);
    RUN_TEST(S22_AC2_build_quiet_row_rssi_only_is_seen);
    RUN_TEST(S22_AC2_build_quiet_row_never_heard_is_linked);

    RUN_TEST(S22_AC3_default_target_is_whole_crew);
    RUN_TEST(S22_AC3_select_paired_member_sets_target);
    RUN_TEST(S22_AC3_select_unpaired_is_rejected_target_unchanged);
    RUN_TEST(S22_AC3_select_unknown_or_zero_is_rejected);
    RUN_TEST(S22_AC3_clear_returns_to_whole_crew);
    RUN_TEST(S22_AC3_reset_after_send_returns_to_whole_crew);
    RUN_TEST(S22_AC3_build_preserves_target);

    RUN_TEST(S22_AC4_rally_needs_confirm_only_for_whole_crew);

    RUN_TEST(S22_null_guards_are_no_ops_not_crashes);
    RUN_TEST(S22_build_with_null_sources_yields_only_divider);
    RUN_TEST(S22_row_at_out_of_range_is_null);
    RUN_TEST(S22_full_feed_and_crew_stays_within_capacity);

    return UNITY_END();
}
