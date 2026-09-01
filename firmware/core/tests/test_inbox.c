/**
 * test_inbox.c — S24 slice (a) acceptance criteria for core/inbox (AC2).
 *
 * Test names follow docs/specs/S24-signals-inbox.md's numbered acceptance
 * criteria: S24_AC2_description (the feed-side direction/mark-read seam
 * is S24_AC1_, tested in test_feed.c and app/tests). A few unnumbered
 * tests cover null-guard / accessor behavior the spec implies but
 * doesn't number.
 *
 * Proxy-check discipline (AGENTS.md standing brief / docs/review/
 * code-review.md item 6) is applied throughout — notably:
 *  - the ordering test builds from a SCRAMBLED roster + interleaved feed
 *    so a passing order cannot come from insertion order;
 *  - direction tests include the UNKNOWN case (asserting it is
 *    preserved, not rewritten) and an UNPAIRED sender;
 *  - mark-read asserts the OTHER threads' unread flags item-by-item, not
 *    just counts;
 *  - a preview test where the newest item is OUTGOING;
 *  - identity-join assertions check the joined NAME, with node_ids that
 *    never equal their roster slot index.
 */
#include <limits.h>
#include <string.h>

#include "unity.h"

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_inbox.h"

void setUp(void) {}
void tearDown(void) {}

/* Fixed reference "now" for age math (member timestamps are ABSOLUTE —
 * ff_crew.h; a sighting of age A is stamped NOW - A). */
#define NOW ((uint32_t)1000000u)

/* ------------------------------------------------------------------- */
/* helpers                                                              */
/* ------------------------------------------------------------------- */

static ff_feed_item_t make_item(ff_feed_kind_t kind, ff_feed_dir_t dir, uint32_t from_node, uint32_t to_node,
                                 uint32_t at_ms, char const *text, bool unread)
{
    ff_feed_item_t it;
    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.dir = dir;
    it.from_node = from_node;
    it.to_node = to_node;
    it.at_ms = at_ms;
    if (text != NULL) {
        strncpy(it.text, text, sizeof(it.text) - 1);
    }
    it.unread = unread;
    return it;
}

static void push(ff_feed_t *f, ff_feed_kind_t kind, ff_feed_dir_t dir, uint32_t from_node, uint32_t to_node,
                 uint32_t at_ms, char const *text, bool unread)
{
    ff_feed_item_t it = make_item(kind, dir, from_node, to_node, at_ms, text, unread);
    ff_feed_push(f, &it);
}

/* Add a crew member directly into a (zeroed) roster — the test_sigview
 * convention: NO direct packet (rssi_dbm == INT16_MIN) and NO position
 * by default, so nothing is accidentally SEEN. */
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
    m->rssi_dbm  = INT16_MIN;
    return m;
}

static void set_rssi_age(ff_crew_member_t *m, int16_t dbm, uint32_t age_ms)
{
    m->rssi_dbm    = dbm;
    m->rssi_age_ms = NOW - age_ms;
}

/* Find a conversation by (kind, node_id) in the built list, or NULL. */
static ff_inbox_conv_t const *find_conv(ff_inbox_t const *ib, ff_conv_kind_t kind, uint32_t node_id)
{
    for (uint8_t i = 0; i < ff_inbox_conv_count(ib); ++i) {
        ff_inbox_conv_t const *cv = ff_inbox_conv_at(ib, i);
        if (cv->kind == kind && cv->node_id == node_id) {
            return cv;
        }
    }
    return NULL;
}

#define DANA 0x0000DA1Au
#define KEV  0x0000CEE0u
#define MAYA 0x00000A1Au

/* ------------------------------------------------------------------- */
/* AC2 — conversation membership                                        */
/* ------------------------------------------------------------------- */

/* The CREW conversation is always present — even over NULL sources — so
 * the inbox screen always has its honest "no signals yet" row. */
static void S24_AC2_crew_conversation_always_present(void)
{
    ff_inbox_t ib;
    ff_inbox_init(&ib);
    ff_inbox_build(&ib, NULL, NULL, NOW);

    TEST_ASSERT_EQUAL_UINT8(1, ff_inbox_conv_count(&ib));
    ff_inbox_conv_t const *cv = ff_inbox_conv_at(&ib, 0);
    TEST_ASSERT_EQUAL(FF_CONV_CREW, cv->kind);
    TEST_ASSERT_EQUAL_UINT8(0, cv->item_count);
    TEST_ASSERT_FALSE(cv->has_preview);
    TEST_ASSERT_FALSE(cv->presence_valid);
}

/* Broadcast + outgoing-broadcast + UNKNOWN items belong to CREW; direct
 * items to/from a member and outgoing-directs to them belong to that
 * member's conversation. Items pushed interleaved so filtering, not
 * push order, must do the splitting. */
static void S24_AC2_membership_splits_crew_and_member_traffic(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);
    add_member(&c, KEV, "kev", 'K', 2, true);

    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 100, "hi all", false);   /* CREW */
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 200, "just you", true);     /* DANA */
    push(&f, FEED_PULSE, FEED_DIR_UNKNOWN, KEV, 0, 300, "", true);            /* CREW (placement) */
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, DANA, 400, "omw", false);            /* DANA (my send) */
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, 0, 500, "party at 9", false);        /* CREW (my broadcast) */

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    TEST_ASSERT_EQUAL_UINT8(3, ff_inbox_conv_count(&ib)); /* CREW + DANA + KEV */

    ff_inbox_conv_t const *crew = find_conv(&ib, FF_CONV_CREW, 0);
    ff_inbox_conv_t const *dana = find_conv(&ib, FF_CONV_MEMBER, DANA);
    ff_inbox_conv_t const *kev  = find_conv(&ib, FF_CONV_MEMBER, KEV);
    TEST_ASSERT_NOT_NULL(crew);
    TEST_ASSERT_NOT_NULL(dana);
    TEST_ASSERT_NOT_NULL(kev);

    TEST_ASSERT_EQUAL_UINT8(3, crew->item_count); /* broadcast + unknown pulse + my broadcast */
    TEST_ASSERT_EQUAL_UINT8(2, dana->item_count); /* her direct + my direct to her */
    TEST_ASSERT_EQUAL_UINT8(0, kev->item_count);  /* his pulse was UNKNOWN -> CREW, not 1:1 */

    /* Identity came from the roster join. */
    TEST_ASSERT_EQUAL_STRING("dana", dana->name);
    TEST_ASSERT_EQUAL_UINT8(1, dana->color_idx);
}

/* Direction is a fact, not a guess: an UNKNOWN item PLACED in the CREW
 * thread still reads FEED_DIR_UNKNOWN there (and in the preview), never
 * rewritten to BROADCAST. */
static void S24_AC2_unknown_direction_preserved_not_rewritten(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);

    push(&f, FEED_PULSE, FEED_DIR_UNKNOWN, DANA, 0, 500, "", true);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);
    ff_inbox_conv_t const *crew = find_conv(&ib, FF_CONV_CREW, 0);
    TEST_ASSERT_EQUAL_UINT8(1, crew->item_count);
    TEST_ASSERT_EQUAL(FEED_DIR_UNKNOWN, crew->preview_dir);

    ff_inbox_thread_t t;
    ff_inbox_thread_build(&t, &f, &c, FF_CONV_CREW, 0, NOW);
    TEST_ASSERT_EQUAL_UINT8(1, ff_inbox_thread_count(&t));
    TEST_ASSERT_EQUAL(FEED_DIR_UNKNOWN, ff_inbox_thread_at(&t, 0)->dir);
    /* The sender IS a paired member — identity joins even when direction
     * doesn't. */
    TEST_ASSERT_TRUE(ff_inbox_thread_at(&t, 0)->identity_known);
    TEST_ASSERT_EQUAL_STRING("dana", ff_inbox_thread_at(&t, 0)->name);
}

/* A DIRECT item from a sender who is NOT a paired member gets NO
 * conversation (identity is never fabricated), and does not leak into
 * CREW or another member's row — while the feed's own global unread
 * count still counts it honestly. */
static void S24_AC2_direct_from_unpaired_sender_has_no_conversation(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);
    add_member(&c, KEV, "kev", 'K', 2, false); /* in roster, NOT paired */

    push(&f, FEED_TEXT, FEED_DIR_DIRECT, KEV, 0, 100, "psst", true);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, 0x777777u, 0, 200, "stranger", true);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    /* CREW + DANA only — no KEV row, no stranger row. */
    TEST_ASSERT_EQUAL_UINT8(2, ff_inbox_conv_count(&ib));
    TEST_ASSERT_NULL(find_conv(&ib, FF_CONV_MEMBER, KEV));
    TEST_ASSERT_NULL(find_conv(&ib, FF_CONV_MEMBER, 0x777777u));

    /* Neither item leaked anywhere. */
    TEST_ASSERT_EQUAL_UINT8(0, find_conv(&ib, FF_CONV_CREW, 0)->item_count);
    TEST_ASSERT_EQUAL_UINT8(0, find_conv(&ib, FF_CONV_MEMBER, DANA)->item_count);
    TEST_ASSERT_EQUAL_UINT16(0, find_conv(&ib, FF_CONV_CREW, 0)->unread);
    TEST_ASSERT_EQUAL_UINT16(0, find_conv(&ib, FF_CONV_MEMBER, DANA)->unread);

    /* The feed itself still tells the truth about total unread. */
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(&f));
}

/* ------------------------------------------------------------------- */
/* AC2 — unread counts + preview                                        */
/* ------------------------------------------------------------------- */

static void S24_AC2_unread_counts_are_per_conversation(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);

    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 100, "a", true);
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 200, "b", false);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 300, "c", true);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 400, "d", true);
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, DANA, 500, "e", false);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    TEST_ASSERT_EQUAL_UINT16(1, find_conv(&ib, FF_CONV_CREW, 0)->unread);
    TEST_ASSERT_EQUAL_UINT16(2, find_conv(&ib, FF_CONV_MEMBER, DANA)->unread);
}

/* The preview is the conversation's NEWEST item — including when that
 * item is OUTGOING (the proxy the brief names: a preview scan that only
 * looked at inbound items would pass every all-inbound test). */
static void S24_AC2_preview_is_newest_item_even_when_outgoing(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);

    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 1000, "you close?", true);
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, DANA, 2000, "omw", false);
    /* Newer traffic in ANOTHER conversation must not become this
     * conversation's preview. */
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 3000, "hi all", false);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    ff_inbox_conv_t const *dana = find_conv(&ib, FF_CONV_MEMBER, DANA);
    TEST_ASSERT_TRUE(dana->has_preview);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, dana->preview_dir);
    TEST_ASSERT_EQUAL(FEED_TEXT, dana->preview_kind);
    TEST_ASSERT_EQUAL_STRING("omw", dana->preview_text);
    TEST_ASSERT_EQUAL_UINT32(NOW - 2000, dana->preview_age_ms);
}

/* [api] S24 slice (b) — the preview's SENDER join (preview_from_*): a
 * paired inbound sender's name is joined (the CREW row's "KEV: ..."
 * prefix); an OUTGOING newest item and an UNPAIRED sender both yield
 * preview_from_known == false — never a fabricated name. Node ids never
 * equal roster slot indices (the join must be by id, not position). */
static void S24_AC3_preview_sender_joined_paired_only(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, KEV, "kev", 'K', 2, true);
    add_member(&c, DANA, "dana", 'D', 1, true);

    /* CREW's newest is a broadcast from paired KEV -> joined name. */
    push(&f, FEED_RALLY, FEED_DIR_BROADCAST, KEV, 0, 3000, "main stage", true);
    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);
    ff_inbox_conv_t const *crew_conv = find_conv(&ib, FF_CONV_CREW, 0);
    TEST_ASSERT_TRUE(crew_conv->preview_from_known);
    TEST_ASSERT_EQUAL_STRING("kev", crew_conv->preview_from_name);

    /* Newest becomes MY OWN whole-crew send: no sender to join (the
     * screen's honest cue is preview_dir == OUT, not a name). */
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, 0, 4000, "omw all", false);
    ff_inbox_build(&ib, &f, &c, NOW);
    crew_conv = find_conv(&ib, FF_CONV_CREW, 0);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, crew_conv->preview_dir);
    TEST_ASSERT_FALSE(crew_conv->preview_from_known);
    TEST_ASSERT_EQUAL_STRING("", crew_conv->preview_from_name);

    /* Newest becomes a broadcast from a node NOT in the roster at all:
     * shown in CREW (it is broadcast traffic) but its identity is
     * honestly unknown. */
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, 777001u, 0, 5000, "who dis", true);
    ff_inbox_build(&ib, &f, &c, NOW);
    crew_conv = find_conv(&ib, FF_CONV_CREW, 0);
    TEST_ASSERT_TRUE(crew_conv->has_preview);
    TEST_ASSERT_FALSE(crew_conv->preview_from_known);
    TEST_ASSERT_EQUAL_STRING("", crew_conv->preview_from_name);

    /* Review Finding 2 (the cardinal-sin class): a sender who IS in the
     * roster but is NOT paired — ff_crew_find succeeds for this node, so
     * only the `paired` gate stands between it and a leaked name. The
     * join must refuse: known == false, and MAYA's name never appears. */
    add_member(&c, MAYA, "maya", 'M', 3, false /* NOT paired */);
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, MAYA, 0, 6000, "let me in", true);
    ff_inbox_build(&ib, &f, &c, NOW);
    crew_conv = find_conv(&ib, FF_CONV_CREW, 0);
    TEST_ASSERT_TRUE(crew_conv->has_preview);
    TEST_ASSERT_FALSE_MESSAGE(crew_conv->preview_from_known,
                              "an in-roster but UNPAIRED sender was joined - the paired identity gate leaked");
    TEST_ASSERT_EQUAL_STRING("", crew_conv->preview_from_name);
}

/* ------------------------------------------------------------------- */
/* AC2 — ordering                                                       */
/* ------------------------------------------------------------------- */

/* Unread first (newest traffic first within), then read-with-traffic by
 * newest traffic, then quiet by presence freshness with LINKED last and
 * ascending-node_id ties. Roster added SCRAMBLED and feed interleaved so
 * sorted output can only come from the sort. */
static void S24_AC2_ordering_unread_then_traffic_then_quiet(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));

    /* Scrambled roster order, node ids unrelated to slot order. */
    ff_crew_member_t *linked_hi = add_member(&c, 0x50u, "elle", 'E', 5, true); /* quiet LINKED */
    ff_crew_member_t *read_m    = add_member(&c, 0x40u, "carl", 'C', 4, true); /* read traffic  */
    ff_crew_member_t *unread_new = add_member(&c, 0x30u, "anna", 'A', 3, true); /* unread, newest */
    ff_crew_member_t *seen_q    = add_member(&c, 0x05u, "dot", 'T', 6, true);  /* quiet SEEN */
    ff_crew_member_t *unread_old = add_member(&c, 0x10u, "bob", 'B', 7, true);  /* unread, older */
    (void)linked_hi;
    (void)unread_new;
    (void)unread_old;
    set_rssi_age(seen_q, -70, 60u * 1000u);
    set_rssi_age(read_m, -70, 5u * 1000u); /* fresh presence must NOT beat traffic ordering */

    /* Interleaved traffic. Ages: smaller = newer (age = NOW - at_ms). */
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, 0x10u, 0, NOW - 500, "old unread", true);
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, 0x40u, 0, NOW - 50, "crew read", false); /* CREW, read */
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, 0x40u, 0, NOW - 200, "carl read", false);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, 0x30u, 0, NOW - 100, "new unread", true);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    TEST_ASSERT_EQUAL_UINT8(6, ff_inbox_conv_count(&ib));
    /* 1-2: unread convs, newest first. */
    TEST_ASSERT_EQUAL_UINT32(0x30u, ff_inbox_conv_at(&ib, 0)->node_id);
    TEST_ASSERT_EQUAL_UINT32(0x10u, ff_inbox_conv_at(&ib, 1)->node_id);
    /* 3-4: read-with-traffic, newest first — CREW's newest (age 50)
     * beats carl's (age 200). */
    TEST_ASSERT_EQUAL(FF_CONV_CREW, ff_inbox_conv_at(&ib, 2)->kind);
    TEST_ASSERT_EQUAL_UINT32(0x40u, ff_inbox_conv_at(&ib, 3)->node_id);
    /* 5-6: quiet — SEEN before LINKED. */
    TEST_ASSERT_EQUAL_UINT32(0x05u, ff_inbox_conv_at(&ib, 4)->node_id);
    TEST_ASSERT_EQUAL(FF_PRESENCE_SEEN, ff_inbox_conv_at(&ib, 4)->presence);
    TEST_ASSERT_EQUAL_UINT32(0x50u, ff_inbox_conv_at(&ib, 5)->node_id);
    TEST_ASSERT_EQUAL(FF_PRESENCE_LINKED, ff_inbox_conv_at(&ib, 5)->presence);
}

/* Quiet-group determinism: LINKED members (no honest age) sort last and
 * tie-break by ascending node_id; a quiet CREW row anchors the quiet
 * group's top (the spec's always-present communal row). */
static void S24_AC2_quiet_ties_break_by_ascending_node_id_linked_last(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));

    /* Descending-id insertion — ascending output must come from the sort. */
    add_member(&c, 0x99u, "zed", 'Z', 1, true);   /* LINKED */
    add_member(&c, 0x22u, "amy", 'A', 2, true);   /* LINKED */
    ff_crew_member_t *s2 = add_member(&c, 0x88u, "sue", 'S', 3, true);
    ff_crew_member_t *s1 = add_member(&c, 0x11u, "moe", 'M', 4, true);
    set_rssi_age(s1, -70, 30u * 1000u); /* SEEN, same age */
    set_rssi_age(s2, -70, 30u * 1000u); /* SEEN, same age */

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);

    TEST_ASSERT_EQUAL_UINT8(5, ff_inbox_conv_count(&ib));
    TEST_ASSERT_EQUAL(FF_CONV_CREW, ff_inbox_conv_at(&ib, 0)->kind); /* quiet anchor */
    TEST_ASSERT_EQUAL_UINT32(0x11u, ff_inbox_conv_at(&ib, 1)->node_id); /* SEEN tie: ascending id */
    TEST_ASSERT_EQUAL_UINT32(0x88u, ff_inbox_conv_at(&ib, 2)->node_id);
    TEST_ASSERT_EQUAL_UINT32(0x22u, ff_inbox_conv_at(&ib, 3)->node_id); /* LINKED last, ascending id */
    TEST_ASSERT_EQUAL_UINT32(0x99u, ff_inbox_conv_at(&ib, 4)->node_id);
}

/* ------------------------------------------------------------------- */
/* AC2 — thread extraction                                              */
/* ------------------------------------------------------------------- */

/* One conversation's items, oldest -> newest, in/out preserved so the
 * screen can side bubbles; other conversations' items (including
 * newer ones) filtered out. */
static void S24_AC2_thread_is_oldest_first_with_direction_sided(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);
    add_member(&c, KEV, "kev", 'K', 2, true);

    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 1000, "you close?", true);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, KEV, 0, 1500, "not yours", true); /* another thread */
    push(&f, FEED_TEXT, FEED_DIR_OUT, 0, DANA, 2000, "omw", false);
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 2500, "crew stuff", true); /* CREW thread */
    push(&f, FEED_PULSE, FEED_DIR_OUT, 0, DANA, 3000, "", false);

    ff_inbox_thread_t t;
    ff_inbox_thread_build(&t, &f, &c, FF_CONV_MEMBER, DANA, NOW);

    TEST_ASSERT_EQUAL_UINT8(3, ff_inbox_thread_count(&t));

    ff_inbox_msg_t const *m0 = ff_inbox_thread_at(&t, 0);
    ff_inbox_msg_t const *m1 = ff_inbox_thread_at(&t, 1);
    ff_inbox_msg_t const *m2 = ff_inbox_thread_at(&t, 2);

    /* Oldest first. */
    TEST_ASSERT_EQUAL_STRING("you close?", m0->text);
    TEST_ASSERT_EQUAL_UINT32(NOW - 1000, m0->age_ms);
    TEST_ASSERT_EQUAL(FEED_DIR_DIRECT, m0->dir);
    TEST_ASSERT_TRUE(m0->identity_known);
    TEST_ASSERT_EQUAL_STRING("dana", m0->name);
    TEST_ASSERT_TRUE(m0->unread);

    /* My side. */
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, m1->dir);
    TEST_ASSERT_EQUAL_STRING("omw", m1->text);
    TEST_ASSERT_FALSE(m1->identity_known); /* no fabricated sender for my own item */

    TEST_ASSERT_EQUAL(FEED_DIR_OUT, m2->dir);
    TEST_ASSERT_EQUAL(FEED_PULSE, m2->kind);
}

/* The identity join gates on `paired` (the sigview B1 precedent): a
 * broadcast from a member who has since been UNPAIRED stays in the CREW
 * thread but renders as an explicitly-unknown sender — and that member
 * gets no conversation row. */
static void S24_AC2_thread_identity_join_gates_on_paired(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);
    add_member(&c, KEV, "kev", 'K', 2, false); /* paired=false: unpaired since */

    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, KEV, 0, 1000, "ghost", false);
    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 2000, "real", false);

    ff_inbox_thread_t t;
    ff_inbox_thread_build(&t, &f, &c, FF_CONV_CREW, 0, NOW);

    TEST_ASSERT_EQUAL_UINT8(2, ff_inbox_thread_count(&t));
    ff_inbox_msg_t const *ghost = ff_inbox_thread_at(&t, 0);
    TEST_ASSERT_FALSE(ghost->identity_known);
    TEST_ASSERT_EQUAL_STRING("", ghost->name); /* never a guessed name */
    TEST_ASSERT_EQUAL_UINT32(0, ghost->node_id);
    TEST_ASSERT_TRUE(ff_inbox_thread_at(&t, 1)->identity_known);
    TEST_ASSERT_EQUAL_STRING("dana", ff_inbox_thread_at(&t, 1)->name);

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);
    TEST_ASSERT_NULL(find_conv(&ib, FF_CONV_MEMBER, KEV));
}

/* ------------------------------------------------------------------- */
/* AC2 — per-thread mark-read                                           */
/* ------------------------------------------------------------------- */

/* Marking ONE thread read clears exactly its items and decrements the
 * feed's running unread count by exactly that many — the OTHER threads'
 * unread items keep their flags (asserted item-by-item, not via a count
 * proxy). */
static void S24_AC2_mark_thread_read_spares_other_threads(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    add_member(&c, DANA, "dana", 'D', 1, true);
    add_member(&c, KEV, "kev", 'K', 2, true);

    push(&f, FEED_TEXT, FEED_DIR_BROADCAST, DANA, 0, 100, "crew1", true);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 200, "dana1", true);
    push(&f, FEED_PULSE, FEED_DIR_UNKNOWN, KEV, 0, 300, "", true); /* CREW (placement) */
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, KEV, 0, 400, "kev1", true);
    push(&f, FEED_TEXT, FEED_DIR_DIRECT, DANA, 0, 500, "dana2", true);
    TEST_ASSERT_EQUAL_UINT16(5, ff_feed_unread_count(&f));

    uint16_t marked = ff_inbox_mark_thread_read(&f, FF_CONV_MEMBER, DANA);
    TEST_ASSERT_EQUAL_UINT16(2, marked);
    TEST_ASSERT_EQUAL_UINT16(3, ff_feed_unread_count(&f));

    /* Item-by-item: newest-first order is dana2, kev1, pulse, dana1, crew1. */
    TEST_ASSERT_FALSE(ff_feed_at(&f, 0)->unread); /* dana2 — marked */
    TEST_ASSERT_TRUE(ff_feed_at(&f, 1)->unread);  /* kev1 — spared */
    TEST_ASSERT_TRUE(ff_feed_at(&f, 2)->unread);  /* unknown pulse (CREW) — spared */
    TEST_ASSERT_FALSE(ff_feed_at(&f, 3)->unread); /* dana1 — marked */
    TEST_ASSERT_TRUE(ff_feed_at(&f, 4)->unread);  /* crew1 — spared */

    /* The rebuilt inbox agrees. */
    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);
    TEST_ASSERT_EQUAL_UINT16(0, find_conv(&ib, FF_CONV_MEMBER, DANA)->unread);
    TEST_ASSERT_EQUAL_UINT16(1, find_conv(&ib, FF_CONV_MEMBER, KEV)->unread);
    TEST_ASSERT_EQUAL_UINT16(2, find_conv(&ib, FF_CONV_CREW, 0)->unread);

    /* Marking CREW clears the broadcast AND the unknown-placed pulse. */
    marked = ff_inbox_mark_thread_read(&f, FF_CONV_CREW, 0);
    TEST_ASSERT_EQUAL_UINT16(2, marked);
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(&f));
    TEST_ASSERT_TRUE(ff_feed_at(&f, 1)->unread); /* kev1 still unread */

    /* Marking an already-read thread again marks nothing. */
    TEST_ASSERT_EQUAL_UINT16(0, ff_inbox_mark_thread_read(&f, FF_CONV_MEMBER, DANA));
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(&f));
}

/* ------------------------------------------------------------------- */
/* capacity + guards                                                    */
/* ------------------------------------------------------------------- */

/* A full roster fits: CREW + FF_CREW_MAX member conversations. */
static void S24_AC2_full_roster_fits_conversation_cap(void)
{
    ff_feed_t f;
    ff_feed_init(&f);
    ff_crew_t c;
    memset(&c, 0, sizeof(c));
    for (uint8_t i = 0; i < FF_CREW_MAX; ++i) {
        add_member(&c, 0x1000u + i, "m", 'M', i, true);
    }

    ff_inbox_t ib;
    ff_inbox_build(&ib, &f, &c, NOW);
    TEST_ASSERT_EQUAL_UINT8(1 + FF_CREW_MAX, ff_inbox_conv_count(&ib));
    TEST_ASSERT_EQUAL_UINT8(FF_INBOX_MAX_CONVS, ff_inbox_conv_count(&ib));
}

static void S24_AC2_null_guards_are_no_ops_not_crashes(void)
{
    ff_inbox_init(NULL);
    ff_inbox_build(NULL, NULL, NULL, NOW);
    TEST_ASSERT_EQUAL_UINT8(0, ff_inbox_conv_count(NULL));
    TEST_ASSERT_NULL(ff_inbox_conv_at(NULL, 0));
    TEST_ASSERT_FALSE(ff_inbox_item_in_conv(NULL, FF_CONV_CREW, 0));

    ff_inbox_thread_build(NULL, NULL, NULL, FF_CONV_CREW, 0, NOW);
    TEST_ASSERT_EQUAL_UINT8(0, ff_inbox_thread_count(NULL));
    TEST_ASSERT_NULL(ff_inbox_thread_at(NULL, 0));
    TEST_ASSERT_EQUAL_UINT16(0, ff_inbox_mark_thread_read(NULL, FF_CONV_CREW, 0));

    /* A MEMBER key with node_id 0 (the no-node-id sentinel) matches
     * nothing — my own OUT-to-crew items (to_node 0) must not leak into
     * a "member 0" thread. */
    ff_feed_item_t out = make_item(FEED_TEXT, FEED_DIR_OUT, 0, 0, 100, "x", false);
    TEST_ASSERT_FALSE(ff_inbox_item_in_conv(&out, FF_CONV_MEMBER, 0));

    /* Out-of-range accessors after a real build. */
    ff_inbox_t ib;
    ff_inbox_build(&ib, NULL, NULL, NOW);
    TEST_ASSERT_NULL(ff_inbox_conv_at(&ib, ff_inbox_conv_count(&ib)));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S24_AC2_crew_conversation_always_present);
    RUN_TEST(S24_AC2_membership_splits_crew_and_member_traffic);
    RUN_TEST(S24_AC2_unknown_direction_preserved_not_rewritten);
    RUN_TEST(S24_AC2_direct_from_unpaired_sender_has_no_conversation);

    RUN_TEST(S24_AC2_unread_counts_are_per_conversation);
    RUN_TEST(S24_AC2_preview_is_newest_item_even_when_outgoing);
    RUN_TEST(S24_AC3_preview_sender_joined_paired_only);

    RUN_TEST(S24_AC2_ordering_unread_then_traffic_then_quiet);
    RUN_TEST(S24_AC2_quiet_ties_break_by_ascending_node_id_linked_last);

    RUN_TEST(S24_AC2_thread_is_oldest_first_with_direction_sided);
    RUN_TEST(S24_AC2_thread_identity_join_gates_on_paired);

    RUN_TEST(S24_AC2_mark_thread_read_spares_other_threads);

    RUN_TEST(S24_AC2_full_roster_fits_conversation_cap);
    RUN_TEST(S24_AC2_null_guards_are_no_ops_not_crashes);

    return UNITY_END();
}
