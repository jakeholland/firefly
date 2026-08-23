/**
 * test_wiring.c — S08 app/ff_wiring.c acceptance criteria, slice (b) +
 * AC6 (canned reply destination).
 *
 * Test names follow docs/specs/S08-signals-t9.md's numbered acceptance
 * criteria: S08_ACn_description. No live mc_client_t/transport/radio
 * anywhere in this file — `ff_wiring_on_private`/`ff_wiring_on_text` are
 * called directly with synthetic (from, payload, len) values (the "mock
 * event injector" AC4 asks for), and canned-reply sends go through a
 * trivial mock `ff_wiring_sender_t` (the "mock mc" AC6 asks for) that just
 * records what was sent — see ff_wiring.h's header comment for why this
 * seam exists instead of standing up a real handshaken mc_client_t.
 */
#include <string.h>

#include "unity.h"

#include "ff_wiring.h"

#include "ff_heard.h"
#include "ff_proto.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* fake clock                                                           */
/* ------------------------------------------------------------------- */

typedef struct {
    uint32_t t;
} fake_clock_t;

static uint32_t fake_now(void *user)
{
    return ((fake_clock_t *)user)->t;
}

static ff_clock_t make_clock(fake_clock_t *fc)
{
    ff_clock_t clk;
    clk.now_ms = fake_now;
    clk.user = fc;
    return clk;
}

/* ------------------------------------------------------------------- */
/* mock sender ("mock mc") + haptic spy                                 */
/* ------------------------------------------------------------------- */

typedef struct {
    int      calls;
    uint32_t last_dest;

    bool used_send_text;
    char last_text[64];

    bool    used_send_private;
    uint8_t last_payload[32];
    size_t  last_payload_len;
} mock_sender_state_t;

static int mock_send_text(void *ctx, uint32_t dest, char const *utf8)
{
    mock_sender_state_t *s = (mock_sender_state_t *)ctx;
    s->calls++;
    s->last_dest = dest;
    s->used_send_text = true;
    strncpy(s->last_text, utf8, sizeof(s->last_text) - 1);
    s->last_text[sizeof(s->last_text) - 1] = '\0';
    return 0;
}

static int mock_send_private(void *ctx, uint32_t dest, uint8_t const *payload, size_t len)
{
    mock_sender_state_t *s = (mock_sender_state_t *)ctx;
    s->calls++;
    s->last_dest = dest;
    s->used_send_private = true;
    size_t n = len;
    if (n > sizeof(s->last_payload)) n = sizeof(s->last_payload);
    memcpy(s->last_payload, payload, n);
    s->last_payload_len = len;
    return 0;
}

static ff_wiring_sender_t make_mock_sender(mock_sender_state_t *s)
{
    ff_wiring_sender_t sender;
    memset(s, 0, sizeof(*s));
    sender.send_text = mock_send_text;
    sender.send_private = mock_send_private;
    sender.ctx = s;
    return sender;
}

typedef struct {
    int count;
} haptic_state_t;

static void haptic_cb(void *user)
{
    ((haptic_state_t *)user)->count++;
}

/* ------------------------------------------------------------------- */
/* Fixture: wiring + crew + feed, wired together.                       */
/* ------------------------------------------------------------------- */

typedef struct {
    ff_crew_t            crew;
    ff_feed_t             feed;
    ff_heard_t            heard;
    fake_clock_t           fc;
    ff_clock_t              clk;
    mock_sender_state_t sender_state;
    haptic_state_t         haptic;
    ff_wiring_ctx_t           w;
} test_rig_t;

static void rig_init(test_rig_t *r)
{
    memset(r, 0, sizeof(*r));
    r->clk = make_clock(&r->fc);
    ff_crew_init(&r->crew, &r->clk);
    ff_feed_init(&r->feed);
    ff_heard_init(&r->heard);
    ff_wiring_sender_t sender = make_mock_sender(&r->sender_state);
    ff_wiring_init_with_sender(&r->w, &r->feed, &r->crew, &r->heard, sender, haptic_cb, &r->haptic, &r->clk);
}

static void rig_pair(test_rig_t *r, uint32_t node_id)
{
    ff_crew_set_paired(&r->crew, node_id, true);
}

#define PAIRED_NODE   0x1001u
#define UNPAIRED_NODE 0x2002u

/* ------------------------------------------------------------------- */
/* AC4 — PULSE from paired node -> feed item + haptic; unpaired -> drop */
/* ------------------------------------------------------------------- */

static void S08_AC4_pulse_from_paired_node_pushes_feed_item_and_fires_haptic(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);
    r.fc.t = 5000;

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(&r.feed));
    ff_feed_item_t const *it = ff_feed_at(&r.feed, 0);
    TEST_ASSERT_EQUAL(FEED_PULSE, it->kind);
    TEST_ASSERT_EQUAL_UINT32(PAIRED_NODE, it->from_node);
    TEST_ASSERT_EQUAL_UINT32(5000, it->at_ms);
    TEST_ASSERT_TRUE(it->unread);
    TEST_ASSERT_EQUAL_INT(1, r.haptic.count);
}

static void S08_AC4_pulse_from_unpaired_node_is_dropped(void)
{
    test_rig_t r;
    rig_init(&r);
    /* UNPAIRED_NODE deliberately never paired — a genuine crew miss (this
     * node has no crew slot at all), not trusted for feed purposes. */
    r.fc.t = 7000;

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));

    ff_wiring_on_private(&r.w, UNPAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
    TEST_ASSERT_EQUAL_INT(0, r.haptic.count); /* mutation-check: no buzz on a dropped event either */
    TEST_ASSERT_EQUAL_UINT8(0, r.crew.count); /* the roster-exhaustion fix: a crew MISS never claims a slot */
    /* ...but the sender IS noted in the heard list, per the PR #25 review
     * ruling — unpaired/unknown senders still populate the future
     * "add from heard nodes" pairing UI, just never at roster-slot cost. */
    TEST_ASSERT_TRUE(ff_heard_contains(&r.heard, UNPAIRED_NODE));
    ff_heard_entry_t const *he = ff_heard_at(&r.heard, 0);
    TEST_ASSERT_NOT_NULL(he);
    TEST_ASSERT_EQUAL_UINT32(7000, he->last_heard_ms);
}

static void S08_AC4_pulse_from_node_with_no_roster_room_is_dropped(void)
{
    /* Fill the crew roster to FF_CREW_MAX with OTHER node ids (directly,
     * simulating "the roster is already full for unrelated reasons"),
     * then hear from a brand-new node id: ff_crew_find (read-only) simply
     * misses — it never attempts to claim a slot, so "no room" and
     * "plenty of room" behave identically from wiring's point of view
     * (this is precisely the roster-exhaustion fix: a full roster no
     * longer changes what happens to an incoming unknown sender). */
    test_rig_t r;
    rig_init(&r);
    for (uint32_t i = 0; i < FF_CREW_MAX; i++) {
        ff_crew_upsert(&r.crew, 0x9000u + i);
    }
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, r.crew.count);

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));

    ff_wiring_on_private(&r.w, 0xABCDu, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
    TEST_ASSERT_EQUAL_INT(0, r.haptic.count);
    TEST_ASSERT_EQUAL_UINT8(FF_CREW_MAX, r.crew.count); /* unchanged — no upsert attempted */
    TEST_ASSERT_TRUE(ff_heard_contains(&r.heard, 0xABCDu)); /* still surfaced via the heard list */
}

/* ------------------------------------------------------------------- */
/* PR #25 code review, MEDIUM finding — roster exhaustion from a flood  */
/* of unpaired/unknown senders. This is the regression test for the     */
/* actual attack scenario, not just the unit-level guard above.        */
/* ------------------------------------------------------------------- */

static void S08_AC4_flood_of_unknown_senders_does_not_exhaust_roster_or_block_pairing(void)
{
    test_rig_t r;
    rig_init(&r);

    /* Flood: well past BOTH FF_CREW_MAX (8) and FF_HEARD_MAX (16) distinct
     * never-before-heard node ids, each sending one PULSE — large enough
     * to prove two separate things at once: the (non-evicting) crew
     * roster never grows at all, AND the heard list, which SHOULD grow,
     * stays genuinely bounded via real eviction rather than "happened to
     * be big enough to hold this test's flood". */
    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    enum { FLOOD_N = FF_HEARD_MAX + 5 };
    for (int i = 0; i < FLOOD_N; i++) {
        r.fc.t = (uint32_t)(1000 + i);
        ff_wiring_on_private(&r.w, 0xF0000u + (uint32_t)i, FF_PORTNUM, buf, (size_t)n);
    }

    /* Nothing got fed (all unpaired) and — the actual fix — the roster
     * itself never grew at all. */
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
    TEST_ASSERT_EQUAL_UINT8(0, r.crew.count);

    /* The real-world consequence this test exists to rule out: a real
     * crew member must still be pairable after the flood. Before the
     * fix, ff_crew_set_paired here would have silently no-op'd (roster
     * full, id not already present). */
    uint32_t const real_friend = 0xDEADBEEFu;
    ff_crew_set_paired(&r.crew, real_friend, true);
    ff_crew_member_t const *m = ff_crew_find(&r.crew, real_friend);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->paired);

    /* And the heard list did its job: bounded at FF_HEARD_MAX (not
     * FLOOD_N — proving it's genuinely capped, not just "big enough for
     * this test"), holding the MOST RECENT senders (LRU eviction, not
     * whichever arrived first). */
    TEST_ASSERT_EQUAL_UINT8(FF_HEARD_MAX, ff_heard_count(&r.heard));
    TEST_ASSERT_FALSE(ff_heard_contains(&r.heard, 0xF0000u)); /* earliest — evicted */
    TEST_ASSERT_TRUE(ff_heard_contains(&r.heard, 0xF0000u + (FLOOD_N - 1))); /* most recent — kept */
}

/* ------------------------------------------------------------------- */
/* AC4 — wrong portnum / malformed payload are ignored, not crashes     */
/* ------------------------------------------------------------------- */

static void S08_AC4_wrong_portnum_is_ignored(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM + 1, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
}

static void S08_AC4_malformed_payload_is_ignored(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    uint8_t garbage[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, garbage, sizeof(garbage));

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
}

/* ------------------------------------------------------------------- */
/* AC4 — RALLY/STATUS decode into feed text; FLARE_END/RALLY_CLEAR/     */
/* ACK_PING are state signals, never fed.                               */
/* ------------------------------------------------------------------- */

static void S08_AC4_rally_from_paired_node_pushes_feed_item_with_name(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    ff_latlon_t pos = {40.0, -105.0};
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_rally(buf, sizeof(buf), pos, "MAIN STAGE");
    TEST_ASSERT_TRUE(n > 0);

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(&r.feed));
    ff_feed_item_t const *it = ff_feed_at(&r.feed, 0);
    TEST_ASSERT_EQUAL(FEED_RALLY, it->kind);
    TEST_ASSERT_EQUAL_STRING("MAIN STAGE", it->text);
}

static void S08_AC4_status_from_paired_node_pushes_feed_item_with_text(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_status(buf, sizeof(buf), "RAGING");
    TEST_ASSERT_TRUE(n > 0);

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(&r.feed));
    ff_feed_item_t const *it = ff_feed_at(&r.feed, 0);
    TEST_ASSERT_EQUAL(FEED_STATUS, it->kind);
    TEST_ASSERT_EQUAL_STRING("RAGING", it->text);
}

static void S08_AC4_flare_end_is_not_fed(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_flare_end(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
}

static void S08_AC4_rally_clear_is_not_fed(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_rally_clear(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    ff_wiring_on_private(&r.w, PAIRED_NODE, FF_PORTNUM, buf, (size_t)n);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
}

/* ------------------------------------------------------------------- */
/* AC4 — on_text applies the same paired-sender filter.                 */
/* ------------------------------------------------------------------- */

static void S08_AC4_text_from_paired_node_pushes_feed_text_item(void)
{
    test_rig_t r;
    rig_init(&r);
    rig_pair(&r, PAIRED_NODE);

    char const *msg = "on my way!";
    ff_wiring_on_text(&r.w, PAIRED_NODE, MC_ADDR_BROADCAST, msg, strlen(msg));

    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(&r.feed));
    ff_feed_item_t const *it = ff_feed_at(&r.feed, 0);
    TEST_ASSERT_EQUAL(FEED_TEXT, it->kind);
    TEST_ASSERT_EQUAL_STRING("on my way!", it->text);
    TEST_ASSERT_EQUAL_INT(1, r.haptic.count);
}

static void S08_AC4_text_from_unpaired_node_is_dropped(void)
{
    test_rig_t r;
    rig_init(&r);

    char const *msg = "hello";
    ff_wiring_on_text(&r.w, UNPAIRED_NODE, MC_ADDR_BROADCAST, msg, strlen(msg));

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(&r.feed));
    TEST_ASSERT_EQUAL_INT(0, r.haptic.count);
}

/* ------------------------------------------------------------------- */
/* AC6 — canned reply destination + payload.                            */
/* ------------------------------------------------------------------- */

static void S08_AC6_canned_omw_from_pulse_context_sends_to_that_sender(void)
{
    test_rig_t r;
    rig_init(&r);

    ff_feed_item_t reply_ctx;
    memset(&reply_ctx, 0, sizeof(reply_ctx));
    reply_ctx.kind = FEED_PULSE;
    reply_ctx.from_node = 0x777u;

    int rc = ff_wiring_send_canned_reply(&r.w, FF_WIRING_REPLY_OMW, &reply_ctx);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, r.sender_state.calls);
    TEST_ASSERT_TRUE(r.sender_state.used_send_text);
    TEST_ASSERT_FALSE(r.sender_state.used_send_private);
    TEST_ASSERT_EQUAL_UINT32(0x777u, r.sender_state.last_dest);
    TEST_ASSERT_EQUAL_STRING("omw", r.sender_state.last_text);
}

static void S08_AC6_canned_5min_sends_correct_text_to_that_sender(void)
{
    test_rig_t r;
    rig_init(&r);

    ff_feed_item_t reply_ctx;
    memset(&reply_ctx, 0, sizeof(reply_ctx));
    reply_ctx.from_node = 0x42u;

    int rc = ff_wiring_send_canned_reply(&r.w, FF_WIRING_REPLY_5MIN, &reply_ctx);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0x42u, r.sender_state.last_dest);
    TEST_ASSERT_EQUAL_STRING("5 min", r.sender_state.last_text);
}

static void S08_AC6_canned_pulse_sends_encoded_pulse_packet(void)
{
    test_rig_t r;
    rig_init(&r);

    ff_feed_item_t reply_ctx;
    memset(&reply_ctx, 0, sizeof(reply_ctx));
    reply_ctx.from_node = 0x99u;

    int rc = ff_wiring_send_canned_reply(&r.w, FF_WIRING_REPLY_PULSE, &reply_ctx);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(r.sender_state.used_send_private);
    TEST_ASSERT_EQUAL_UINT32(0x99u, r.sender_state.last_dest);

    /* Verify by independent decode, not by comparing raw bytes against
     * ff_proto_encode_pulse's own output (that would only prove "wiring
     * called the encoder", not "the encoder produced a real PULSE
     * packet" — an encoder bug and this test could both be wrong the
     * same way). */
    ff_proto_msg_t decoded;
    int type = ff_proto_decode(r.sender_state.last_payload, r.sender_state.last_payload_len, &decoded);
    TEST_ASSERT_EQUAL(FF_PROTO_TYPE_PULSE, type);
}

static void S08_AC6_canned_reply_without_context_broadcasts(void)
{
    test_rig_t r;
    rig_init(&r);

    int rc = ff_wiring_send_canned_reply(&r.w, FF_WIRING_REPLY_OMW, NULL);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, r.sender_state.last_dest);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S08_AC4_pulse_from_paired_node_pushes_feed_item_and_fires_haptic);
    RUN_TEST(S08_AC4_pulse_from_unpaired_node_is_dropped);
    RUN_TEST(S08_AC4_pulse_from_node_with_no_roster_room_is_dropped);
    RUN_TEST(S08_AC4_flood_of_unknown_senders_does_not_exhaust_roster_or_block_pairing);

    RUN_TEST(S08_AC4_wrong_portnum_is_ignored);
    RUN_TEST(S08_AC4_malformed_payload_is_ignored);

    RUN_TEST(S08_AC4_rally_from_paired_node_pushes_feed_item_with_name);
    RUN_TEST(S08_AC4_status_from_paired_node_pushes_feed_item_with_text);
    RUN_TEST(S08_AC4_flare_end_is_not_fed);
    RUN_TEST(S08_AC4_rally_clear_is_not_fed);

    RUN_TEST(S08_AC4_text_from_paired_node_pushes_feed_text_item);
    RUN_TEST(S08_AC4_text_from_unpaired_node_is_dropped);

    RUN_TEST(S08_AC6_canned_omw_from_pulse_context_sends_to_that_sender);
    RUN_TEST(S08_AC6_canned_5min_sends_correct_text_to_that_sender);
    RUN_TEST(S08_AC6_canned_pulse_sends_encoded_pulse_packet);
    RUN_TEST(S08_AC6_canned_reply_without_context_broadcasts);

    return UNITY_END();
}
