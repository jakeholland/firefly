/**
 * test_proto.c — S04 firefly protocol acceptance criteria.
 *
 * Test names follow docs/specs/S04-firefly-protocol.md's numbered
 * acceptance criteria: S04_ACn_description.
 *
 *   AC1 — round-trip every type (table-driven), including max-length
 *         name/status and the empty-body types. Also covers the boundary
 *         cases that *should* succeed (24-char name, 20-char status).
 *   AC2 — bad ver / unknown type / truncated body all return 0, and never
 *         read past the buffer (the per-length truncation loops are what
 *         actually exercise "never OOB" — run this file under ASan, see
 *         PR notes). Also covers the whole-payload 200-byte cap.
 *   AC3 — RALLY lat/lon 1e-7 fixed-point matches Meshtastic's own
 *         convention, cross-checked against hand-computed literal bytes
 *         (not derived by calling the encoder circularly).
 *   AC4 — encode into a too-small buffer returns negative and writes
 *         nothing (verified by a sentinel-fill + memcmp, not just by the
 *         return value). Also covers name/status-too-long rejection,
 *         which shares the same "negative, no partial write" contract.
 *   AC5 — the committed golden fixture (tests/fixtures/ffproto_v1.bin)
 *         still decodes correctly. This test DECODES the committed file;
 *         it never regenerates it, so a future version must keep reading
 *         v1's byte layout (see tools note in the PR body for how the
 *         fixture was produced).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ff_proto.h"

#ifndef FF_PROTO_FIXTURES_DIR
#define FF_PROTO_FIXTURES_DIR "tests/fixtures"
#endif

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* Helpers                                                              */
/* ------------------------------------------------------------------- */

/* Fills buf with a recognizable sentinel so "writes nothing" (AC4) can be
 * asserted by memcmp against an untouched copy, not just inferred from the
 * return value. */
static void sentinel_fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = 0xAAu;
    }
}

static uint8_t *load_fixture(char const *name, size_t *out_len)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", FF_PROTO_FIXTURES_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    TEST_ASSERT_TRUE(sz >= 0);
    fseek(f, 0, SEEK_SET);
    static uint8_t buf[FF_PROTO_MAX_PAYLOAD + 16];
    TEST_ASSERT_TRUE((size_t)sz <= sizeof(buf));
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT(sz, got);
    *out_len = got;
    return buf;
}

/* ------------------------------------------------------------------- */
/* AC1 — round-trip every type                                         */
/* ------------------------------------------------------------------- */

static void S04_AC1_pulse_round_trips(void)
{
    uint8_t buf[8];
    int len = ff_proto_encode_pulse(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_UINT8(FF_PROTO_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_PROTO_TYPE_PULSE, buf[1]);

    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, (size_t)len, &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_PULSE, type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_PROTO_TYPE_PULSE, out.type);
}

static void S04_AC1_flare_end_round_trips(void)
{
    uint8_t buf[8];
    int len = ff_proto_encode_flare_end(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_PROTO_TYPE_FLARE_END, buf[1]);

    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, (size_t)len, &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE_END, type);
}

static void S04_AC1_rally_clear_round_trips(void)
{
    uint8_t buf[8];
    int len = ff_proto_encode_rally_clear(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FF_PROTO_TYPE_RALLY_CLEAR, buf[1]);

    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, (size_t)len, &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY_CLEAR, type);
}

static void S04_AC1_flare_round_trips_table(void)
{
    uint16_t const durs[] = {0u, 1u, 300u, 65535u};

    for (size_t i = 0; i < sizeof(durs) / sizeof(durs[0]); i++) {
        uint8_t buf[8];
        int len = ff_proto_encode_flare(buf, sizeof(buf), durs[i]);
        TEST_ASSERT_EQUAL_INT(4, len);

        ff_proto_msg_t out;
        int type = ff_proto_decode(buf, (size_t)len, &out);
        TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, type);
        TEST_ASSERT_EQUAL_UINT16(durs[i], out.body.flare.dur_s);
    }
}

static void S04_AC1_status_round_trips_table(void)
{
    /* "" (empty), a normal status, and the max-length (20 char) status. */
    char const *statuses[] = {
        "",
        "RAGING",
        "12345678901234567890", /* placeholder, overwritten below to 20 chars exactly */
    };
    statuses[2] = "AAAAAAAAAAAAAAAAAAAA"; /* exactly 20 'A's */
    TEST_ASSERT_EQUAL_UINT(FF_PROTO_STATUS_MAX, strlen(statuses[2]));

    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        uint8_t buf[64];
        size_t slen = strlen(statuses[i]);
        int len = ff_proto_encode_status(buf, sizeof(buf), statuses[i]);
        TEST_ASSERT_EQUAL_INT((int)(2 + 1 + slen), len);

        ff_proto_msg_t out;
        int type = ff_proto_decode(buf, (size_t)len, &out);
        TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_STATUS, type);
        TEST_ASSERT_EQUAL_STRING(statuses[i], out.body.status.text);
    }
}

static void S04_AC1_rally_round_trips_table(void)
{
    struct {
        ff_latlon_t p;
        char const *name;
    } cases[] = {
        {{0.0, 0.0}, ""},
        {{39.9012, -82.4562}, "LEGEND VALLEY"},
        {{-39.9012, 82.4562}, "AAAAAAAAAAAAAAAAAAAAAAAA"}, /* exactly 24 chars */
    };
    TEST_ASSERT_EQUAL_UINT(FF_PROTO_RALLY_NAME_MAX, strlen(cases[2].name));

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[64];
        size_t nlen = strlen(cases[i].name);
        int len = ff_proto_encode_rally(buf, sizeof(buf), cases[i].p, cases[i].name);
        TEST_ASSERT_EQUAL_INT((int)(2 + 4 + 4 + 1 + nlen), len);

        ff_proto_msg_t out;
        int type = ff_proto_decode(buf, (size_t)len, &out);
        TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, type);
        /* Test values all have <=4 decimal digits, so the 1e-7 fixed-point
         * round trip is exact (no quantization loss to tolerate). */
        TEST_ASSERT_EQUAL_DOUBLE(cases[i].p.lat, out.body.rally.pos.lat);
        TEST_ASSERT_EQUAL_DOUBLE(cases[i].p.lon, out.body.rally.pos.lon);
        TEST_ASSERT_EQUAL_STRING(cases[i].name, out.body.rally.name);
    }
}

/* ACK_PING (0x07) is reserved — spec provides no encoder for it yet (see
 * ff_proto.h's deviation note), so this hand-crafts the wire bytes instead
 * of round-tripping through an encoder, to prove decode already
 * understands the reserved type's shape for forward compat. */
static void S04_AC1_ack_ping_reserved_type_decodes_from_raw_bytes(void)
{
    uint8_t buf[] = {
        (uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_ACK_PING,
        0x78, 0x56, 0x34, 0x12, /* nonce = 0x12345678, LE */
    };

    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_ACK_PING, type);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, out.body.ack_ping.nonce);
}

/* ------------------------------------------------------------------- */
/* AC2 — bad ver / unknown type / truncated body / whole-payload cap    */
/* ------------------------------------------------------------------- */

static void S04_AC2_bad_version_zero_is_ignored(void)
{
    uint8_t buf[] = {0x00, (uint8_t)FF_PROTO_TYPE_PULSE};
    ff_proto_msg_t out;
    memset(&out, 0xAA, sizeof(out));
    int type = ff_proto_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(0, type);
    TEST_ASSERT_EQUAL_UINT8(0, out.type); /* decode zeroes *out even on reject */
}

static void S04_AC2_bad_version_two_is_ignored(void)
{
    uint8_t buf[] = {0x02, (uint8_t)FF_PROTO_TYPE_PULSE};
    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(0, type);
}

static void S04_AC2_unknown_type_0x7F_is_ignored(void)
{
    uint8_t buf[] = {(uint8_t)FF_PROTO_VERSION, 0x7F};
    ff_proto_msg_t out;
    int type = ff_proto_decode(buf, sizeof(buf), &out);
    TEST_ASSERT_EQUAL_INT(0, type);
}

static void S04_AC2_null_buf_and_null_out_are_ignored(void)
{
    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(NULL, 10, &out));

    uint8_t buf[] = {(uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_PULSE};
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, sizeof(buf), NULL));
}

static void S04_AC2_zero_and_one_byte_buffers_are_ignored(void)
{
    uint8_t buf[] = {(uint8_t)FF_PROTO_VERSION};
    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, 0, &out));
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, 1, &out));
}

static void S04_AC2_flare_truncated_body_is_ignored(void)
{
    /* Needs 2 body bytes (dur_s), give it 0 and 1. */
    uint8_t buf0[] = {(uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_FLARE};
    uint8_t buf1[] = {(uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_FLARE, 0x2C};
    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf0, sizeof(buf0), &out));
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf1, sizeof(buf1), &out));
}

static void S04_AC2_ack_ping_truncated_body_is_ignored(void)
{
    uint8_t buf[] = {(uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_ACK_PING, 0x01, 0x02, 0x03};
    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, sizeof(buf), &out));
}

/* Rally's name_len byte can itself lie (claim more bytes than are actually
 * present) — this is the OOB-read hazard the spec calls out explicitly.
 * Encode a real 24-char-name RALLY, then feed decode every prefix length
 * from the bare envelope up to (full_len - 1): every one of them must be
 * rejected (0), and under ASan none may read past the prefix. Only the
 * full length may succeed. */
static void S04_AC2_rally_truncated_at_every_length_returns_zero(void)
{
    uint8_t buf[64];
    ff_latlon_t p = {12.3456, -65.4321};
    int len = ff_proto_encode_rally(buf, sizeof(buf), p, "AAAAAAAAAAAAAAAAAAAAAAAA");
    TEST_ASSERT_TRUE(len > 0);

    for (size_t n = 0; n < (size_t)len; n++) {
        ff_proto_msg_t out;
        int type = ff_proto_decode(buf, n, &out);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, type, "should reject every truncated prefix");
    }

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, ff_proto_decode(buf, (size_t)len, &out));
}

static void S04_AC2_status_truncated_at_every_length_returns_zero(void)
{
    uint8_t buf[64];
    int len = ff_proto_encode_status(buf, sizeof(buf), "AAAAAAAAAAAAAAAAAAAA"); /* 20 chars */
    TEST_ASSERT_TRUE(len > 0);

    for (size_t n = 0; n < (size_t)len; n++) {
        ff_proto_msg_t out;
        int type = ff_proto_decode(buf, n, &out);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, type, "should reject every truncated prefix");
    }

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_STATUS, ff_proto_decode(buf, (size_t)len, &out));
}

/* A name_len/status_len byte that claims more than the type's own max is
 * rejected outright, even if enough trailing bytes happen to be present
 * (it can never have come from this version's encoder). */
static void S04_AC2_rally_name_len_over_max_is_ignored(void)
{
    uint8_t buf[64];
    memset(buf, 'A', sizeof(buf));
    buf[0] = (uint8_t)FF_PROTO_VERSION;
    buf[1] = (uint8_t)FF_PROTO_TYPE_RALLY;
    buf[10] = FF_PROTO_RALLY_NAME_MAX + 1; /* 25: one over the max */

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, sizeof(buf), &out));
}

static void S04_AC2_status_len_over_max_is_ignored(void)
{
    uint8_t buf[64];
    memset(buf, 'A', sizeof(buf));
    buf[0] = (uint8_t)FF_PROTO_VERSION;
    buf[1] = (uint8_t)FF_PROTO_TYPE_STATUS;
    buf[2] = FF_PROTO_STATUS_MAX + 1; /* 21: one over the max */

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, sizeof(buf), &out));
}

/* Whole-payload cap (spec: "max 200 B"): exactly 200 is accepted (extra
 * trailing bytes past what PULSE needs are forward-compat padding, not an
 * error — see ff_proto.h); 201 is rejected outright, regardless of
 * content, before any type-specific parsing happens. */
static void S04_AC2_payload_of_200_bytes_is_accepted(void)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)FF_PROTO_VERSION;
    buf[1] = (uint8_t)FF_PROTO_TYPE_PULSE;

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_PULSE, ff_proto_decode(buf, sizeof(buf), &out));
}

static void S04_AC2_payload_of_201_bytes_is_rejected(void)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)FF_PROTO_VERSION;
    buf[1] = (uint8_t)FF_PROTO_TYPE_PULSE;

    ff_proto_msg_t out;
    TEST_ASSERT_EQUAL_INT(0, ff_proto_decode(buf, sizeof(buf), &out));
}

/* ------------------------------------------------------------------- */
/* AC3 — 1e-7 fixed point matches Meshtastic's convention               */
/* ------------------------------------------------------------------- */

/* lat=1.0 -> 10,000,000 -> 0x00989680 LE = 80 96 98 00
 * lon=-1.0 -> -10,000,000 -> two's complement 0xFF676980 LE = 80 69 67 FF
 * (hand-computed: 10,000,000 = 0x00989680; NOT(0x00989680)+1 = 0xFF676980;
 * both cross-checked against mc_client.c's identical
 * `(int32_t)(deg*1e7 + sign*0.5)` formula, per spec AC3's "shared fixture
 * with S03" — this module doesn't call into meshclient, so the formula is
 * independently reproduced in ff_proto.c, and this test pins its output
 * against literal bytes rather than against that reproduction.) */
static void S04_AC3_rally_latlon_1e7_matches_hand_computed_bytes(void)
{
    uint8_t expected[] = {
        (uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_RALLY,
        0x80, 0x96, 0x98, 0x00, /* lat_i = 10,000,000 (1.0 deg) */
        0x80, 0x69, 0x67, 0xFF, /* lon_i = -10,000,000 (-1.0 deg) */
        0x00,                   /* name_len = 0 */
    };

    uint8_t buf[32];
    ff_latlon_t p = {1.0, -1.0};
    int len = ff_proto_encode_rally(buf, sizeof(buf), p, "");
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));

    /* And decoding those same literal bytes (not the encoder's output —
     * `expected` was typed by hand above) recovers 1.0 / -1.0. */
    ff_proto_msg_t out;
    int type = ff_proto_decode(expected, sizeof(expected), &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, type);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, out.body.rally.pos.lat);
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, out.body.rally.pos.lon);
}

/* ------------------------------------------------------------------- */
/* AC4 — too-small buffer / too-long string: negative, writes nothing   */
/* ------------------------------------------------------------------- */

static void S04_AC4_pulse_buffer_too_small_writes_nothing(void)
{
    uint8_t buf[2];
    uint8_t before[2];
    sentinel_fill(buf, sizeof(buf));
    memcpy(before, buf, sizeof(buf));

    int len = ff_proto_encode_pulse(buf, 1); /* needs 2, given 1 */
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

static void S04_AC4_flare_buffer_too_small_writes_nothing(void)
{
    uint8_t buf[4];
    uint8_t before[4];
    sentinel_fill(buf, sizeof(buf));
    memcpy(before, buf, sizeof(buf));

    int len = ff_proto_encode_flare(buf, 3, 300u); /* needs 4, given 3 */
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

static void S04_AC4_rally_buffer_too_small_writes_nothing(void)
{
    uint8_t buf[16];
    uint8_t before[16];

    ff_latlon_t p = {1.0, 2.0};
    int need = ff_proto_encode_rally(buf, sizeof(buf), p, "HI"); /* discover exact size */
    TEST_ASSERT_TRUE(need > 0);
    sentinel_fill(buf, sizeof(buf)); /* wipe the real bytes the probe call wrote */
    memcpy(before, buf, sizeof(buf));

    int len = ff_proto_encode_rally(buf, (size_t)need - 1, p, "HI");
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

static void S04_AC4_status_buffer_too_small_writes_nothing(void)
{
    uint8_t buf[16];
    uint8_t before[16];

    int need = ff_proto_encode_status(buf, sizeof(buf), "RAGING");
    TEST_ASSERT_TRUE(need > 0);
    sentinel_fill(buf, sizeof(buf));
    memcpy(before, buf, sizeof(buf));

    int len = ff_proto_encode_status(buf, (size_t)need - 1, "RAGING");
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

static void S04_AC4_null_buffer_pointer_is_rejected(void)
{
    TEST_ASSERT_TRUE(ff_proto_encode_pulse(NULL, 8) < 0);
    TEST_ASSERT_TRUE(ff_proto_encode_flare(NULL, 8, 300u) < 0);
    TEST_ASSERT_TRUE(ff_proto_encode_flare_end(NULL, 8) < 0);
    ff_latlon_t p = {0.0, 0.0};
    TEST_ASSERT_TRUE(ff_proto_encode_rally(NULL, 64, p, "x") < 0);
    TEST_ASSERT_TRUE(ff_proto_encode_rally_clear(NULL, 8) < 0);
    TEST_ASSERT_TRUE(ff_proto_encode_status(NULL, 64, "x") < 0);
}

/* Name/status longer than the wire max can never be represented — same
 * "negative, no partial write" contract as a too-small buffer, just
 * triggered by the string instead of the buffer capacity. */
static void S04_AC4_rally_name_25_chars_is_rejected(void)
{
    uint8_t buf[64];
    uint8_t before[64];
    sentinel_fill(buf, sizeof(buf));
    memcpy(before, buf, sizeof(buf));

    ff_latlon_t p = {0.0, 0.0};
    char const *name25 = "AAAAAAAAAAAAAAAAAAAAAAAAA"; /* 25 chars: one over the max */
    TEST_ASSERT_EQUAL_UINT(FF_PROTO_RALLY_NAME_MAX + 1, strlen(name25));

    int len = ff_proto_encode_rally(buf, sizeof(buf), p, name25);
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

static void S04_AC4_status_21_chars_is_rejected(void)
{
    uint8_t buf[64];
    uint8_t before[64];
    sentinel_fill(buf, sizeof(buf));
    memcpy(before, buf, sizeof(buf));

    char const *status21 = "AAAAAAAAAAAAAAAAAAAAA"; /* 21 chars: one over the max */
    TEST_ASSERT_EQUAL_UINT(FF_PROTO_STATUS_MAX + 1, strlen(status21));

    int len = ff_proto_encode_status(buf, sizeof(buf), status21);
    TEST_ASSERT_TRUE(len < 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before, buf, sizeof(buf));
}

/* ------------------------------------------------------------------- */
/* AC5 — golden bytes fixture (decode only, never regenerated here)     */
/* ------------------------------------------------------------------- */

static void S04_AC5_golden_v1_fixture_decodes(void)
{
    size_t len = 0;
    uint8_t *fixture = load_fixture("ffproto_v1.bin", &len);

    ff_proto_msg_t out;
    int type = ff_proto_decode(fixture, len, &out);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, type);
    TEST_ASSERT_EQUAL_DOUBLE(39.9012, out.body.rally.pos.lat);
    TEST_ASSERT_EQUAL_DOUBLE(-82.4562, out.body.rally.pos.lon);
    TEST_ASSERT_EQUAL_STRING("LEGEND VALLEY", out.body.rally.name);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S04_AC1_pulse_round_trips);
    RUN_TEST(S04_AC1_flare_end_round_trips);
    RUN_TEST(S04_AC1_rally_clear_round_trips);
    RUN_TEST(S04_AC1_flare_round_trips_table);
    RUN_TEST(S04_AC1_status_round_trips_table);
    RUN_TEST(S04_AC1_rally_round_trips_table);
    RUN_TEST(S04_AC1_ack_ping_reserved_type_decodes_from_raw_bytes);

    RUN_TEST(S04_AC2_bad_version_zero_is_ignored);
    RUN_TEST(S04_AC2_bad_version_two_is_ignored);
    RUN_TEST(S04_AC2_unknown_type_0x7F_is_ignored);
    RUN_TEST(S04_AC2_null_buf_and_null_out_are_ignored);
    RUN_TEST(S04_AC2_zero_and_one_byte_buffers_are_ignored);
    RUN_TEST(S04_AC2_flare_truncated_body_is_ignored);
    RUN_TEST(S04_AC2_ack_ping_truncated_body_is_ignored);
    RUN_TEST(S04_AC2_rally_truncated_at_every_length_returns_zero);
    RUN_TEST(S04_AC2_status_truncated_at_every_length_returns_zero);
    RUN_TEST(S04_AC2_rally_name_len_over_max_is_ignored);
    RUN_TEST(S04_AC2_status_len_over_max_is_ignored);
    RUN_TEST(S04_AC2_payload_of_200_bytes_is_accepted);
    RUN_TEST(S04_AC2_payload_of_201_bytes_is_rejected);

    RUN_TEST(S04_AC3_rally_latlon_1e7_matches_hand_computed_bytes);

    RUN_TEST(S04_AC4_pulse_buffer_too_small_writes_nothing);
    RUN_TEST(S04_AC4_flare_buffer_too_small_writes_nothing);
    RUN_TEST(S04_AC4_rally_buffer_too_small_writes_nothing);
    RUN_TEST(S04_AC4_status_buffer_too_small_writes_nothing);
    RUN_TEST(S04_AC4_null_buffer_pointer_is_rejected);
    RUN_TEST(S04_AC4_rally_name_25_chars_is_rejected);
    RUN_TEST(S04_AC4_status_21_chars_is_rejected);

    RUN_TEST(S04_AC5_golden_v1_fixture_decodes);

    return UNITY_END();
}
