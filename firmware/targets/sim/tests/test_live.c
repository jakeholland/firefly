/**
 * test_live.c — S13c: ff_live_t (targets/sim/live.h) tests.
 *
 * Two groups:
 *  - ff_live_load_pack against real festpack fixtures (no mesh traffic
 *    involved).
 *  - A full mc_client -> ff_live_t -> ff_app_state_t pipeline test, using
 *    the same in-memory mock transport pattern as
 *    firmware/meshclient/tests/test_meshclient.c (mc_transport_t is a
 *    plain vtable, so no real socket is needed) — this is what actually
 *    proves live.c's event wiring (NodeInfo -> crew roster -> radar
 *    compute; PULSE/text -> signals feed), independent of whether a real
 *    meshtasticd/docker is available anywhere in this environment.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "live.h"

#include "ff_proto.h"
#include "mc_framing.h"

#include "pb_encode.h"

#include "meshtastic/mesh.pb.h"

#ifndef FF_LIVE_FESTPACK_FIXTURES_DIR
#define FF_LIVE_FESTPACK_FIXTURES_DIR "../../festpack/tests/fixtures"
#endif

void setUp(void) {}
void tearDown(void) {}

/* ----------------------------------------------------------------------
 * ff_live_load_pack
 * -------------------------------------------------------------------- */

static char const *fixture_path(char const *name)
{
    static char buf[512];
    (void)snprintf(buf, sizeof(buf), "%s/%s", FF_LIVE_FESTPACK_FIXTURES_DIR, name);
    return buf;
}

static void S13c_live_load_pack_sets_my_pos_from_known_venue(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    struct { uint32_t t; } clk = {0};
    ff_clock_t clock = {.now_ms = NULL, .user = &clk}; /* not exercised in this test */

    ff_live_t lv;
    ff_live_init(&lv, &state, &clock);

    TEST_ASSERT_FALSE(lv.my_pos_ok);

    int rc = ff_live_load_pack(&lv, fixture_path("lost-lands-2026.festpack.json"));
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_TRUE(lv.my_pos_ok);
    /* Legend Valley — the task brief's "venue origin" (see
     * firmware/tools/dev/crew_sim.py's own use of the same fixture). */
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 39.936, lv.my_pos.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -82.414, lv.my_pos.lon);
}

static void S13c_live_load_pack_unknown_venue_leaves_my_pos_untouched(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    struct { uint32_t t; } clk = {0};
    ff_clock_t clock = {.now_ms = NULL, .user = &clk};

    ff_live_t lv;
    ff_live_init(&lv, &state, &clock);

    int rc = ff_live_load_pack(&lv, fixture_path("null_venue.festpack.json"));
    TEST_ASSERT_EQUAL_INT(0, rc); /* parses fine — an unknown venue is not a parse failure */
    TEST_ASSERT_FALSE(lv.my_pos_ok);
}

static void S13c_live_load_pack_missing_file_fails(void)
{
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));
    struct { uint32_t t; } clk = {0};
    ff_clock_t clock = {.now_ms = NULL, .user = &clk};

    ff_live_t lv;
    ff_live_init(&lv, &state, &clock);

    int rc = ff_live_load_pack(&lv, "/nonexistent/path/does-not-exist.festpack.json");
    TEST_ASSERT_LESS_THAN_INT(0, rc);
    TEST_ASSERT_FALSE(lv.my_pos_ok);
}

static void S13c_live_init_defaults(void)
{
    ff_app_state_t state;
    memset(&state, 0xAB, sizeof(state)); /* poison, to prove init actually zeroes what it owns */
    struct { uint32_t t; } clk = {0};
    ff_clock_t clock = {.now_ms = NULL, .user = &clk};

    ff_live_t lv;
    ff_live_init(&lv, &state, &clock);

    TEST_ASSERT_FALSE(lv.my_pos_ok);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lv.heading_deg);
    TEST_ASSERT_EQUAL_INT(-1, lv.tcp.fd);
    TEST_ASSERT_EQUAL_UINT8(0, lv.crew.count);
}

/* ----------------------------------------------------------------------
 * Full pipeline: mc_client events -> ff_live_t -> ff_app_state_t.
 * -------------------------------------------------------------------- */

typedef struct {
    uint8_t const *rx_data;
    size_t rx_len;
    size_t rx_pos;
} mock_io_t;

static int mock_read(void *io, uint8_t *buf, size_t maxlen)
{
    mock_io_t *m = (mock_io_t *)io;
    size_t remaining = m->rx_len - m->rx_pos;
    if (remaining == 0) return 0;
    size_t n = (remaining < maxlen) ? remaining : maxlen;
    memcpy(buf, m->rx_data + m->rx_pos, n);
    m->rx_pos += n;
    return (int)n;
}

static int mock_write(void *io, uint8_t const *buf, size_t len)
{
    (void)io;
    (void)buf;
    return (int)len; /* live.c never inspects what mc_client writes (want_config etc) */
}

typedef struct {
    uint32_t t;
} mock_clock_t;

static uint32_t mock_now(void *user)
{
    return ((mock_clock_t *)user)->t;
}

static uint16_t frame_from_radio(meshtastic_FromRadio const *fr, uint8_t *out, size_t out_cap)
{
    uint8_t payload[512];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, fr)) return 0;
    return mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
}

#define DANA_NODE_NUM 0x0A0A0A0Au
#define MY_NODE_NUM 0x42424242u

static void S13c_live_pipeline_position_pulse_and_text(void)
{
    /* --- Wire up ff_live_t first (over a mock transport whose rx side is
     * still empty) so mc_connect() generates its real want_config nonce —
     * mc_client.c's want_config_id is randomly generated per connect
     * (see mc_rand_next in meshclient/src/mc_client.c), NOT something a
     * caller can predict, so the synthetic config_complete_id frame below
     * must echo lv.mc.want_config_id back, exactly like
     * meshclient/tests/test_meshclient.c's own handshake tests do
     * (`build_config_complete_frame(c.want_config_id, ...)`) — a fixed
     * guessed nonce here would leave the client stuck in HANDSHAKE
     * forever. --- */
    ff_app_state_t state;
    memset(&state, 0, sizeof(state));

    mock_clock_t clk = {.t = 1000};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};

    ff_live_t lv;
    ff_live_init(&lv, &state, &clock);
    ff_live_set_my_pos(&lv, (ff_latlon_t){.lat = 39.936, .lon = -82.414}); /* Legend Valley origin */

    mock_io_t io = {0};
    mc_transport_t transport = {.write = mock_write, .read = mock_read, .io = &io};
    ff_live_attach(&lv, transport); /* -> mc_connect(), generates lv.mc.want_config_id */

    /* --- Now build the synthetic mesh session: my_info, one NodeInfo
     * (Dana, with a position fix ~50m from "my" position above),
     * config_complete (echoing the real nonce), then two post-handshake
     * packets (a PULSE on the firefly portnum and a plain text message),
     * both from Dana. --- */
    uint8_t stream[4096];
    size_t stream_len = 0;

    {
        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_my_info_tag;
        fr.payload_variant.my_info.my_node_num = MY_NODE_NUM;
        stream_len += frame_from_radio(&fr, stream + stream_len, sizeof(stream) - stream_len);
    }
    {
        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        fr.payload_variant.node_info.num = DANA_NODE_NUM;
        fr.payload_variant.node_info.has_user = true;
        (void)snprintf(fr.payload_variant.node_info.user.long_name,
                        sizeof(fr.payload_variant.node_info.user.long_name), "Dana Test");
        (void)snprintf(fr.payload_variant.node_info.user.short_name,
                        sizeof(fr.payload_variant.node_info.user.short_name), "DANA");
        fr.payload_variant.node_info.has_position = true;
        fr.payload_variant.node_info.position.has_latitude_i = true;
        /* 39.9360000 -> 39.9364500 is ~50m north (0.00045 deg * 111320
         * m/deg =~ 50m) — deliberately outside FF_CREW_CLOSE_RANGE_M
         * (30m) so this exercises RADAR_LIVE, not RADAR_CLOSE. */
        fr.payload_variant.node_info.position.latitude_i = 399364500;
        fr.payload_variant.node_info.position.has_longitude_i = true;
        fr.payload_variant.node_info.position.longitude_i = -824140000; /* -82.414 */
        fr.payload_variant.node_info.last_heard = 1700000000u;
        stream_len += frame_from_radio(&fr, stream + stream_len, sizeof(stream) - stream_len);
    }
    {
        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
        fr.payload_variant.config_complete_id = lv.mc.want_config_id;
        stream_len += frame_from_radio(&fr, stream + stream_len, sizeof(stream) - stream_len);
    }
    {
        uint8_t pulse[FF_PROTO_ENVELOPE_LEN];
        int n = ff_proto_encode_pulse(pulse, sizeof(pulse));
        TEST_ASSERT_GREATER_THAN_INT(0, n);

        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
        fr.payload_variant.packet.from = DANA_NODE_NUM;
        fr.payload_variant.packet.to = 0xFFFFFFFFu;
        fr.payload_variant.packet.id = 111;
        fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        fr.payload_variant.packet.payload_variant.decoded.portnum = (meshtastic_PortNum)FF_PORTNUM;
        fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)n;
        memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, pulse, (size_t)n);
        stream_len += frame_from_radio(&fr, stream + stream_len, sizeof(stream) - stream_len);
    }
    {
        char const *text = "omw!";
        meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
        fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
        fr.payload_variant.packet.from = DANA_NODE_NUM;
        fr.payload_variant.packet.to = MY_NODE_NUM;
        fr.payload_variant.packet.id = 112;
        fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        fr.payload_variant.packet.payload_variant.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)strlen(text);
        memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, text, strlen(text));
        stream_len += frame_from_radio(&fr, stream + stream_len, sizeof(stream) - stream_len);
    }

    TEST_ASSERT_GREATER_THAN_UINT32(0, stream_len);

    /* --- Now make the bytes available and pump. --- */
    io.rx_data = stream;
    io.rx_len = stream_len;
    io.rx_pos = 0;

    for (int i = 0; i < 32; i++) {
        ff_live_tick(&lv, clk.t);
        clk.t += 20;
    }

    /* mc_state(&lv.mc) is MC_STATE_READY and mc_get_stats(&lv.mc).frames_ok
     * == 5 at this point (all five synthetic frames parsed clean, zero
     * resyncs/decode_errors/decode_skipped) — spot-checked manually while
     * building this test; not asserted here to avoid over-pinning
     * mc_client.c internals this file doesn't own, but see this comment
     * for what "the handshake actually completed" looks like if this
     * test ever needs re-debugging. */

    /* Crew: Dana auto-paired, named, positioned. */
    TEST_ASSERT_EQUAL_UINT8(1, lv.crew.count);
    TEST_ASSERT_EQUAL_STRING("DANA", lv.crew.members[0].name);
    TEST_ASSERT_TRUE(lv.crew.members[0].paired);
    TEST_ASSERT_TRUE(lv.crew.members[0].has_pos);

    /* Radar: with my_pos_ok + a valid heading + Dana selected (only paired
     * member) and a fresh fix, mode must be RADAR_LIVE, not NOFIX/NOSEL. */
    TEST_ASSERT_EQUAL(RADAR_LIVE, state.radar.mode);
    TEST_ASSERT_EQUAL_STRING("DANA", state.radar.name);
    TEST_ASSERT_TRUE(state.radar.arrow_valid);

    /* Signals: PULSE pushed first, text second -> text is newest (index 0). */
    TEST_ASSERT_EQUAL_UINT8(2, state.signals.n_items);
    TEST_ASSERT_EQUAL(FF_APP_FEED_TEXT, state.signals.items[0].kind);
    TEST_ASSERT_EQUAL_STRING("DANA", state.signals.items[0].from_name);
    TEST_ASSERT_EQUAL_STRING("omw!", state.signals.items[0].text);
    TEST_ASSERT_EQUAL(FF_APP_FEED_PULSE, state.signals.items[1].kind);
    TEST_ASSERT_EQUAL_STRING("DANA", state.signals.items[1].from_name);

    ff_live_close(&lv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(S13c_live_load_pack_sets_my_pos_from_known_venue);
    RUN_TEST(S13c_live_load_pack_unknown_venue_leaves_my_pos_untouched);
    RUN_TEST(S13c_live_load_pack_missing_file_fails);
    RUN_TEST(S13c_live_init_defaults);
    RUN_TEST(S13c_live_pipeline_position_pulse_and_text);
    return UNITY_END();
}
