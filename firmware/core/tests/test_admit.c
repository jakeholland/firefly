/**
 * test_admit.c — the crew-admission rule, clause by clause
 * (docs/specs/S02-core-crew.md's 2026-09-13 amendment §B, acceptance
 * criteria **S02_AC11** and **S02_AC12**).
 *
 * ## How this file is built, and why
 *
 * There is exactly one "everything passes" fixture (`admit_ok()`), and
 * every negative test is that fixture with ONE field changed. So a test
 * that goes green can only do so because the clause under test rejected
 * the packet — not because some unrelated field in a hand-built input
 * happened to be wrong too. That is the anti-proxy discipline
 * (docs/review/code-review.md item 6) applied to a six-clause rule: the
 * failure mode it exists to catch is a clause that "works" in a test
 * because a *different* clause was doing the rejecting.
 *
 * The reason enum is what makes that checkable. Asserting `!= YES` would
 * pass for a rule that rejected the MQTT case because it also happened
 * to have the wrong portnum; asserting `== FF_ADMIT_NO_VIA_MQTT` cannot.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "ff_admit.h"
#include "ff_proto.h" /* FF_PORTNUM — pinned against ff_admit.h's own copy */

void setUp(void) {}
void tearDown(void) {}

#define ME   0x11112222u
#define THEM 0x33334444u

/* The one positive fixture: a decrypted NodeInfo from a stranger, on the
 * crew channel's index, over the air, with auto-crew on. Every negative
 * test below mutates exactly one field of this. */
static ff_admit_in_t admit_ok(void)
{
    ff_admit_in_t in;
    memset(&in, 0, sizeof(in));
    in.auto_crew_enabled = true;
    in.decrypted = true;
    in.crew_index_known = true;
    in.crew_index = 0u;
    in.has_channel_index = true;
    in.channel_index = 0u;
    in.from = THEM;
    in.has_my_node_id = true;
    in.my_node_id = ME;
    in.hidden = false;
    in.via_mqtt = false;
    in.has_portnum = true;
    in.portnum = FF_ADMIT_PORTNUM_NODEINFO;
    return in;
}

/* ------------------------------------------------------------------- */
/* S02_AC11 — the positive case, on each of the four portnums            */
/* ------------------------------------------------------------------- */

static void S02_AC11_four_portnums_admit(void)
{
    uint32_t const ok[] = {
        FF_ADMIT_PORTNUM_NODEINFO, FF_ADMIT_PORTNUM_POSITION,
        FF_ADMIT_PORTNUM_TEXT,     FF_ADMIT_PORTNUM_FIREFLY,
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        ff_admit_in_t in = admit_ok();
        in.portnum = ok[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(FF_ADMIT_YES, ff_admit(&in), ff_admit_reason(ff_admit(&in)));
        TEST_ASSERT_TRUE(ff_admit_portnum_ok(ok[i]));
    }
}

static void S02_AC11_firefly_portnum_is_269_by_raw_value(void)
{
    /* 269 is NOT PRIVATE_APP (256) and NOT ATAK_FORWARDER (257) — it is
     * a raw value inside the private range, and an implementation that
     * matched the named PRIVATE_APP enumerator would silently admit
     * nobody on Firefly's own traffic. Pinned against ff_proto.h so the
     * two constants can never drift. */
    TEST_ASSERT_EQUAL_UINT32(269u, (uint32_t)FF_PORTNUM);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FF_PORTNUM, (uint32_t)FF_ADMIT_PORTNUM_FIREFLY);
    TEST_ASSERT_TRUE(ff_admit_portnum_ok(269u));
    TEST_ASSERT_FALSE(ff_admit_portnum_ok(256u));
    TEST_ASSERT_FALSE(ff_admit_portnum_ok(257u));
}

static void S02_AC11_crew_index_is_not_assumed_to_be_zero(void)
{
    /* A radio provisioned by CLI or the stock app can hold the crew
     * channel anywhere; MeshPacket.channel is "inherently a local
     * concept". Index 3 admits when 3 is what we resolved. */
    ff_admit_in_t in = admit_ok();
    in.crew_index = 3u;
    in.channel_index = 3u;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_YES, ff_admit(&in));
}

/* ------------------------------------------------------------------- */
/* S02_AC11 — each clause rejects, on its own, for its own reason        */
/* ------------------------------------------------------------------- */

static void S02_AC11_auto_crew_off_admits_nobody(void)
{
    ff_admit_in_t in = admit_ok();
    in.auto_crew_enabled = false;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_DISABLED, ff_admit(&in));

    /* "Any packet at all" — every otherwise-admitting portnum, with the
     * flag off, still admits nobody. */
    uint32_t const ok[] = {
        FF_ADMIT_PORTNUM_NODEINFO, FF_ADMIT_PORTNUM_POSITION,
        FF_ADMIT_PORTNUM_TEXT,     FF_ADMIT_PORTNUM_FIREFLY,
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        in.portnum = ok[i];
        TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_DISABLED, ff_admit(&in));
    }
}

static void S02_AC11_undecrypted_admits_nobody(void)
{
    ff_admit_in_t in = admit_ok();
    in.decrypted = false;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_ENCRYPTED, ff_admit(&in));
}

static void S02_AC11_another_channel_index_admits_nobody(void)
{
    ff_admit_in_t in = admit_ok();
    in.channel_index = 1u; /* a friend chatting on LongFast is not crew */
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_OTHER_CHANNEL, ff_admit(&in));
}

static void S02_AC11_absent_channel_index_never_reads_as_zero(void)
{
    /* The trap this presence flag exists for: the crew channel IS
     * normally index 0, so "we don't know which channel" defaulting to 0
     * would admit every stranger whose packet carried no usable index
     * (an encrypted-variant packet, where the field holds the channel
     * HASH; or a PKI-encrypted DM, which proves possession of a key PAIR,
     * not of the crew key). */
    ff_admit_in_t in = admit_ok();
    in.has_channel_index = false;
    in.channel_index = 0u;
    in.crew_index = 0u;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_OTHER_CHANNEL, ff_admit(&in));
}

static void S02_AC12_no_resolved_crew_channel_admits_nobody(void)
{
    /* S02_AC12: with no channel matching our code by name AND PSK, the
     * puck admits nobody and says so — it never falls back to index 0. */
    ff_admit_in_t in = admit_ok();
    in.crew_index_known = false;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_NO_CREW_CHANNEL, ff_admit(&in));

    /* And specifically: a packet that really is on index 0 is still not
     * admitted, because index 0 being the usual crew slot is not
     * evidence that it is THIS radio's crew slot. */
    in.channel_index = 0u;
    in.crew_index = 0u;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_NO_CREW_CHANNEL, ff_admit(&in));
}

static void S02_AC11_our_own_id_admits_nobody(void)
{
    ff_admit_in_t in = admit_ok();
    in.from = ME;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_SELF, ff_admit(&in));

    in = admit_ok();
    in.from = 0u; /* the wire's "unset" sender */
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_SELF, ff_admit(&in));

    /* Before my_info arrives we cannot prove a packet is not our own
     * echo, so we refuse rather than guess. */
    in = admit_ok();
    in.has_my_node_id = false;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_SELF, ff_admit(&in));
}

static void S02_AC11_hidden_admits_nobody(void)
{
    ff_admit_in_t in = admit_ok();
    in.hidden = true;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_HIDDEN, ff_admit(&in));
}

static void S02_AC11_via_mqtt_admits_nobody(void)
{
    /* An MQTT-sourced packet may well carry our PSK if someone bridged
     * the crew — but a crew is people who are HERE, and an MQTT path can
     * replay. The exclusion is a test, by the amendment's own wording. */
    ff_admit_in_t in = admit_ok();
    in.via_mqtt = true;
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_VIA_MQTT, ff_admit(&in));
}

static void S02_AC11_telemetry_and_friends_admit_nobody(void)
{
    /* TELEMETRY_APP (67) is the one that matters: it refreshes an
     * existing member's presence through the shell's unconditional
     * ff_crew_on_heard call, but it must never ADMIT — admission rides
     * on a packet type that carries identity or intent. */
    uint32_t const no[] = {
        67u,  /* TELEMETRY_APP */
        5u,   /* ROUTING_APP */
        6u,   /* ADMIN_APP */
        0u,   /* UNKNOWN_APP */
        256u, /* PRIVATE_APP — near-miss for 269 */
        257u, /* ATAK_FORWARDER */
        270u, /* one past ours */
    };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        ff_admit_in_t in = admit_ok();
        in.portnum = no[i];
        TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_PORTNUM, ff_admit(&in));
        TEST_ASSERT_FALSE(ff_admit_portnum_ok(no[i]));
    }

    /* No portnum at all (a packet we never decoded a payload for). */
    ff_admit_in_t in = admit_ok();
    in.has_portnum = false;
    in.portnum = FF_ADMIT_PORTNUM_NODEINFO; /* value present but not usable */
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_PORTNUM, ff_admit(&in));
}

static void S02_AC11_null_admits_nobody(void)
{
    TEST_ASSERT_EQUAL_INT(FF_ADMIT_NO_DISABLED, ff_admit(NULL));
}

static void S02_AC11_every_reason_has_a_distinct_tag(void)
{
    /* Cheap guard against a copy-paste in ff_admit_reason that would
     * make two different rejections indistinguishable in a ctl dump or a
     * test failure message (AGENTS.md: honesty rules bind debug
     * surfaces too). */
    ff_admit_result_t const all[] = {
        FF_ADMIT_YES, FF_ADMIT_NO_DISABLED, FF_ADMIT_NO_ENCRYPTED,
        FF_ADMIT_NO_NO_CREW_CHANNEL, FF_ADMIT_NO_OTHER_CHANNEL,
        FF_ADMIT_NO_SELF, FF_ADMIT_NO_HIDDEN, FF_ADMIT_NO_VIA_MQTT,
        FF_ADMIT_NO_PORTNUM,
    };
    size_t const n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(ff_admit_reason(all[i]));
        TEST_ASSERT_TRUE(strlen(ff_admit_reason(all[i])) > 0u);
        for (size_t j = i + 1u; j < n; j++) {
            TEST_ASSERT_TRUE(strcmp(ff_admit_reason(all[i]), ff_admit_reason(all[j])) != 0);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S02_AC11_four_portnums_admit);
    RUN_TEST(S02_AC11_firefly_portnum_is_269_by_raw_value);
    RUN_TEST(S02_AC11_crew_index_is_not_assumed_to_be_zero);
    RUN_TEST(S02_AC11_auto_crew_off_admits_nobody);
    RUN_TEST(S02_AC11_undecrypted_admits_nobody);
    RUN_TEST(S02_AC11_another_channel_index_admits_nobody);
    RUN_TEST(S02_AC11_absent_channel_index_never_reads_as_zero);
    RUN_TEST(S02_AC12_no_resolved_crew_channel_admits_nobody);
    RUN_TEST(S02_AC11_our_own_id_admits_nobody);
    RUN_TEST(S02_AC11_hidden_admits_nobody);
    RUN_TEST(S02_AC11_via_mqtt_admits_nobody);
    RUN_TEST(S02_AC11_telemetry_and_friends_admit_nobody);
    RUN_TEST(S02_AC11_null_admits_nobody);
    RUN_TEST(S02_AC11_every_reason_has_a_distinct_tag);
    return UNITY_END();
}
