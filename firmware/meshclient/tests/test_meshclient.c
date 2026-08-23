/**
 * test_meshclient.c — S03 meshclient acceptance-criteria tests.
 *
 * Test names mirror docs/specs/S03-meshclient.md's AC numbering
 * (S03_ACn_...), per AGENTS.md.
 *
 * Fixtures under tests/fixtures/ (*.bin) are hand-crafted (see
 * tools/dev/record_fixture.py) from the meshtastic protobuf definitions —
 * we have no meshtasticd instance to record a real capture from in this
 * environment (no docker), so handshake.bin in particular is a synthetic
 * capture, not a real one. Called out explicitly in the PR body per the
 * task's instructions for AC2.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "mc_client.h"
#include "mc_framing.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include "meshtastic/mesh.pb.h"

#ifndef MC_FIXTURES_DIR
#define MC_FIXTURES_DIR "tests/fixtures"
#endif
#ifndef MC_MESHCLIENT_DIR
#define MC_MESHCLIENT_DIR "."
#endif

void setUp(void) {}
void tearDown(void) {}

/* -------------------------------------------------------------------- */
/* Fixture loading                                                      */
/* -------------------------------------------------------------------- */

static uint8_t *load_fixture(char const *name, size_t *out_len)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", MC_FIXTURES_DIR, name);

    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);

    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    long sz = ftell(f);
    TEST_ASSERT_TRUE(sz >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    TEST_ASSERT_NOT_NULL(buf);

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT((size_t)sz, rd);

    *out_len = (size_t)sz;
    return buf;
}

/* -------------------------------------------------------------------- */
/* Mock transport + clock                                               */
/* -------------------------------------------------------------------- */

typedef struct {
    uint8_t const *rx_data;
    size_t rx_len;
    size_t rx_pos;
    bool rx_error_once;

    uint8_t tx_buf[8192];
    size_t tx_len;
    bool tx_fail;
} mock_io_t;

static void mock_io_reset(mock_io_t *m)
{
    memset(m, 0, sizeof(*m));
}

static int mock_read(void *io, uint8_t *buf, size_t maxlen)
{
    mock_io_t *m = (mock_io_t *)io;
    if (m->rx_error_once) {
        m->rx_error_once = false;
        return -1;
    }
    size_t remaining = m->rx_len - m->rx_pos;
    if (remaining == 0) {
        return 0;
    }
    size_t n = (remaining < maxlen) ? remaining : maxlen;
    memcpy(buf, m->rx_data + m->rx_pos, n);
    m->rx_pos += n;
    return (int)n;
}

static int mock_write(void *io, uint8_t const *buf, size_t len)
{
    mock_io_t *m = (mock_io_t *)io;
    if (m->tx_fail) {
        return -1;
    }
    if (m->tx_len + len > sizeof(m->tx_buf)) {
        return -1;
    }
    memcpy(m->tx_buf + m->tx_len, buf, len);
    m->tx_len += len;
    return (int)len;
}

typedef struct {
    uint32_t t;
} mock_clock_t;

static uint32_t mock_now(void *u)
{
    return ((mock_clock_t *)u)->t;
}

/* -------------------------------------------------------------------- */
/* Event capture                                                        */
/* -------------------------------------------------------------------- */

typedef struct {
    mc_state_t states[16];
    int state_count;

    mc_nodeinfo_t nodes[8];
    int node_count;

    struct {
        uint32_t node;
        mc_position_t pos;
    } positions[8];
    int position_count;

    struct {
        uint32_t from, to;
        char text[256];
        size_t len;
    } texts[8];
    int text_count;

    struct {
        uint32_t from, portnum;
        uint8_t payload[300];
        size_t len;
    } privates[8];
    int private_count;

    bool got_my_info;
    uint32_t my_node_id;
} events_capture_t;

static void cap_on_state(void *u, mc_state_t s)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->state_count < (int)(sizeof(c->states) / sizeof(c->states[0]))) {
        c->states[c->state_count++] = s;
    }
}

static void cap_on_node(void *u, mc_nodeinfo_t const *n)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->node_count < (int)(sizeof(c->nodes) / sizeof(c->nodes[0]))) {
        c->nodes[c->node_count++] = *n;
    }
}

static void cap_on_position(void *u, uint32_t node, mc_position_t const *p)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->position_count < (int)(sizeof(c->positions) / sizeof(c->positions[0]))) {
        c->positions[c->position_count].node = node;
        c->positions[c->position_count].pos = *p;
        c->position_count++;
    }
}

static void cap_on_text(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->text_count < (int)(sizeof(c->texts) / sizeof(c->texts[0]))) {
        c->texts[c->text_count].from = from;
        c->texts[c->text_count].to = to;
        size_t n = len < sizeof(c->texts[0].text) - 1 ? len : sizeof(c->texts[0].text) - 1;
        memcpy(c->texts[c->text_count].text, utf8, n);
        c->texts[c->text_count].text[n] = '\0';
        c->texts[c->text_count].len = len;
        c->text_count++;
    }
}

static void cap_on_private(void *u, uint32_t from, uint32_t portnum, uint8_t const *payload, size_t len)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->private_count < (int)(sizeof(c->privates) / sizeof(c->privates[0]))) {
        c->privates[c->private_count].from = from;
        c->privates[c->private_count].portnum = portnum;
        size_t n = len < sizeof(c->privates[0].payload) ? len : sizeof(c->privates[0].payload);
        memcpy(c->privates[c->private_count].payload, payload, n);
        c->privates[c->private_count].len = len;
        c->private_count++;
    }
}

static void cap_on_my_info(void *u, uint32_t my_node_id)
{
    events_capture_t *c = (events_capture_t *)u;
    c->got_my_info = true;
    c->my_node_id = my_node_id;
}

static mc_events_t make_events(events_capture_t *cap)
{
    mc_events_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.on_state = cap_on_state;
    ev.on_node = cap_on_node;
    ev.on_position = cap_on_position;
    ev.on_text = cap_on_text;
    ev.on_private = cap_on_private;
    ev.on_my_info = cap_on_my_info;
    ev.user = cap;
    return ev;
}

/* -------------------------------------------------------------------- */
/* AC1 — framing: byte-dribble, garbage-prefix, oversize-len resync      */
/* -------------------------------------------------------------------- */

static void S03_AC1_byte_dribble_yields_frame_once(void)
{
    size_t len;
    uint8_t *fixture = load_fixture("text_packet.bin", &len);

    mc_framer_t f;
    mc_framer_init(&f);

    int complete_count = 0;
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (mc_framer_feed(&f, fixture[i], &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(len - 4), out_len);
            TEST_ASSERT_EQUAL_MEMORY(fixture + 4, out, out_len);
        }
    }

    TEST_ASSERT_EQUAL_INT(1, complete_count);
    free(fixture);
}

static void S03_AC1_garbage_prefix_yields_frame_once(void)
{
    size_t garbage_len, text_len;
    uint8_t *garbage = load_fixture("garbage_prefix.bin", &garbage_len);
    uint8_t *text = load_fixture("text_packet.bin", &text_len);

    mc_framer_t f;
    mc_framer_init(&f);

    int complete_count = 0;
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    for (size_t i = 0; i < garbage_len; i++) {
        if (mc_framer_feed(&f, garbage[i], &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(text_len - 4), out_len);
            TEST_ASSERT_EQUAL_MEMORY(text + 4, out, out_len);
        }
    }

    TEST_ASSERT_EQUAL_INT(1, complete_count);
    free(garbage);
    free(text);
}

static void S03_AC1_oversize_len_resyncs_without_overflow(void)
{
    size_t oversize_len, text_len;
    uint8_t *oversize = load_fixture("oversize_len.bin", &oversize_len);
    uint8_t *text = load_fixture("text_packet.bin", &text_len);

    mc_framer_t f;
    mc_framer_init(&f);

    int complete_count = 0;
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    for (size_t i = 0; i < oversize_len; i++) {
        if (mc_framer_feed(&f, oversize[i], &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(text_len - 4), out_len);
            TEST_ASSERT_EQUAL_MEMORY(text + 4, out, out_len);
        }
    }

    TEST_ASSERT_EQUAL_INT(1, complete_count);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, f.resync_count);
    free(oversize);
    free(text);
}

/* PR #7 review finding 2: oversize_len.bin declares 0xFFFF, nowhere near
 * MC_MAX_FRAME (512) — that doesn't exercise the actual boundary check
 * in mc_framing.c (`expected > MC_MAX_FRAME`). These two tests pin the
 * exact edge: 512 must be accepted, 513 must resync. */
static void S03_AC1_frame_length_exactly_512_is_accepted(void)
{
    static uint8_t payload[MC_MAX_FRAME];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)((i * 37u) + 11u);
    }

    uint8_t framed[MC_MAX_FRAME + 4u];
    uint16_t framed_len = mc_frame_encode(framed, sizeof(framed), payload, (uint16_t)sizeof(payload));
    TEST_ASSERT_EQUAL_UINT16(sizeof(framed), framed_len);
    /* Confirm the fixture actually declares 512 before trusting the rest. */
    TEST_ASSERT_EQUAL_UINT16(MC_MAX_FRAME, (uint16_t)((framed[2] << 8) | framed[3]));

    mc_framer_t f;
    mc_framer_init(&f);
    int complete_count = 0;
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    for (uint16_t i = 0; i < framed_len; i++) {
        if (mc_framer_feed(&f, framed[i], &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16(MC_MAX_FRAME, out_len);
            TEST_ASSERT_EQUAL_MEMORY(payload, out, sizeof(payload));
        }
    }

    TEST_ASSERT_EQUAL_INT(1, complete_count);
    TEST_ASSERT_EQUAL_UINT32(0, f.resync_count);
}

static void S03_AC1_frame_length_513_resyncs(void)
{
    size_t text_len;
    uint8_t *text = load_fixture("text_packet.bin", &text_len);

    /* mc_frame_encode() itself refuses payload_len > MC_MAX_FRAME, so a
     * 513-byte declared length has to be hand-built to reach the receive
     * side at all. 513 = 0x0201. */
    uint8_t const oversize_513_hdr[4] = {MC_FRAME_MAGIC1, MC_FRAME_MAGIC2, 0x02, 0x01};
    TEST_ASSERT_EQUAL_UINT16(513u, (uint16_t)((oversize_513_hdr[2] << 8) | oversize_513_hdr[3]));

    mc_framer_t f;
    mc_framer_init(&f);
    uint8_t const *out = NULL;
    uint16_t out_len = 0;

    for (size_t i = 0; i < sizeof(oversize_513_hdr); i++) {
        TEST_ASSERT_FALSE(mc_framer_feed(&f, oversize_513_hdr[i], &out, &out_len));
    }
    /* Resynced immediately after reading the length, before ever touching
     * PAYLOAD — the whole point of the MC_MAX_FRAME check. */
    TEST_ASSERT_EQUAL(MC_FRAMER_START1, f.state);
    TEST_ASSERT_EQUAL_UINT32(1, f.resync_count);

    /* Recovery: a real frame immediately after must still complete
     * cleanly — none of the 513-declared header's bytes should have been
     * mistaken for payload of anything. */
    int complete_count = 0;
    for (size_t i = 0; i < text_len; i++) {
        if (mc_framer_feed(&f, text[i], &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(text_len - 4), out_len);
            TEST_ASSERT_EQUAL_MEMORY(text + 4, out, out_len);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, complete_count);

    free(text);
}

/* -------------------------------------------------------------------- */
/* AC2 — handshake                                                      */
/* -------------------------------------------------------------------- */

static uint16_t build_config_complete_frame(uint32_t nonce, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    fr.payload_variant.config_complete_id = nonce;

    uint8_t payload[16];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
}

static void S03_AC2_connect_sends_want_config_and_enters_handshake(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c);

    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    TEST_ASSERT_EQUAL_INT(1, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);

    TEST_ASSERT_TRUE(io.tx_len >= 4);
    TEST_ASSERT_EQUAL_UINT8(0x94, io.tx_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xC3, io.tx_buf[1]);

    uint16_t plen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, plen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL(meshtastic_ToRadio_want_config_id_tag, tr.which_payload_variant);
    TEST_ASSERT_EQUAL_UINT32(c.want_config_id, tr.payload_variant.want_config_id);
}

static void S03_AC2_handshake_dump_reaches_ready_with_node_and_myinfo(void)
{
    size_t len;
    uint8_t *handshake = load_fixture("handshake.bin", &len);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = handshake;
    io.rx_len = len;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    /* handshake.bin's config_complete_id is baked to a fixed nonce by
     * tools/dev/record_fixture.py (WANT_CONFIG_NONCE). We drive the
     * decode/completion path directly rather than through mc_connect()'s
     * random nonce — mc_connect()'s own outbound framing is covered by
     * S03_AC2_connect_sends_want_config_and_enters_handshake above. */
    c.state = MC_STATE_HANDSHAKE;
    c.want_config_id = 0x1234ABCDu;

    mc_tick(&c, 100);

    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_TRUE(cap.got_my_info);
    TEST_ASSERT_EQUAL_UINT32(0x42424242u, cap.my_node_id);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, cap.node_count);

    free(handshake);
}

/* PR #7 review finding 4: every test that reaches READY does so with a
 * hand-matched nonce — nothing pinned that a *mismatched*
 * config_complete_id must NOT complete the handshake. This is the
 * mutation that would slip through: deleting the `== c->want_config_id`
 * comparison in mc_client.c's config_complete_id case. */
static void S03_AC2_handshake_wrong_nonce_stays_in_handshake(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c); /* -> HANDSHAKE with a real (random) want_config_id */

    uint8_t wrong_frame[32];
    /* Bitwise complement: guaranteed different from want_config_id for any
     * value, no modular-arithmetic edge case to worry about. */
    uint32_t wrong_nonce = c.want_config_id ^ 0xFFFFFFFFu;
    uint16_t wrong_len = build_config_complete_frame(wrong_nonce, wrong_frame, sizeof(wrong_frame));
    TEST_ASSERT_TRUE(wrong_len > 0);
    io.rx_data = wrong_frame;
    io.rx_len = wrong_len;
    io.rx_pos = 0;

    mc_tick(&c, 50);

    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    /* Only the initial HANDSHAKE transition fired — no spurious READY. */
    TEST_ASSERT_EQUAL_INT(1, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.decode_skipped);
}

/* -------------------------------------------------------------------- */
/* AC3 — position decode                                                */
/* -------------------------------------------------------------------- */

static void S03_AC3_position_packet_decodes_with_1e7_conversion_and_rx_time(void)
{
    size_t len;
    uint8_t *fixture = load_fixture("position_packet.bin", &len);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = fixture;
    io.rx_len = len;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.position_count);
    TEST_ASSERT_EQUAL_UINT32(0x0A0A0A0Au, cap.positions[0].node);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 40.7128, cap.positions[0].pos.lat);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -74.0060, cap.positions[0].pos.lon);
    TEST_ASSERT_TRUE(cap.positions[0].pos.has_altitude);
    TEST_ASSERT_EQUAL_INT32(42, cap.positions[0].pos.altitude_m);
    TEST_ASSERT_TRUE(cap.positions[0].pos.has_rx_time);
    TEST_ASSERT_EQUAL_UINT32(1700000101u, cap.positions[0].pos.rx_time);

    free(fixture);
}

/* -------------------------------------------------------------------- */
/* AC4 — send_text byte-golden                                          */
/* -------------------------------------------------------------------- */

static void S03_AC4_send_text_matches_byte_golden(void)
{
    size_t golden_len;
    uint8_t *golden = load_fixture("send_text_golden.bin", &golden_len);

    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    /* READY with no my_node_id learned yet (fresh client) — matches the
     * golden fixture's assumption that `from` is omitted. */
    c.state = MC_STATE_READY;

    int rc = mc_send_text(&c, 0x0A0A0A0Au, "hi");

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT(golden_len, io.tx_len);
    TEST_ASSERT_EQUAL_MEMORY(golden, io.tx_buf, golden_len);

    free(golden);
}

/* -------------------------------------------------------------------- */
/* AC5 — private portnum passthrough                                    */
/* -------------------------------------------------------------------- */

static void S03_AC5_on_private_fires_for_portnum_256_511_untouched(void)
{
    size_t len;
    uint8_t *fixture = load_fixture("private_packet.bin", &len);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = fixture;
    io.rx_len = len;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.private_count);
    TEST_ASSERT_EQUAL_UINT32(0x0B0B0B0Bu, cap.privates[0].from);
    TEST_ASSERT_EQUAL_UINT32(257u, cap.privates[0].portnum);
    uint8_t const expected[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    TEST_ASSERT_EQUAL_UINT(sizeof(expected), cap.privates[0].len);
    TEST_ASSERT_EQUAL_MEMORY(expected, cap.privates[0].payload, sizeof(expected));

    free(fixture);
}

/* PR #7 review finding 3: the fixture above only exercises portnum 257, an
 * interior value of the [256, 511] private range — nothing pinned the
 * boundaries themselves or the excluded neighbors. Built programmatically
 * (rather than more fixture files) so the four cases stay obviously
 * matched to MC_PORTNUM_PRIVATE_MIN/MAX in mc_client.h. */
static uint16_t build_data_packet_frame(uint32_t from, uint32_t to, uint32_t portnum,
                                         uint8_t const *payload, size_t len, uint8_t *out,
                                         size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.from = from;
    fr.payload_variant.packet.to = to;
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.payload_variant.packet.payload_variant.decoded.portnum = (meshtastic_PortNum)portnum;
    fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)len;
    if (len > 0) {
        memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, payload, len);
    }

    uint8_t buf[300];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

static void run_private_portnum_boundary_case(uint32_t portnum, bool expect_private)
{
    uint8_t const payload[3] = {0xAA, 0xBB, 0xCC};
    uint8_t frame[64];
    uint16_t flen =
        build_data_packet_frame(0x0A0A0A0Au, MC_ADDR_BROADCAST, portnum, payload, sizeof(payload), frame,
                                 sizeof(frame));
    TEST_ASSERT_TRUE(flen > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = flen;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    if (expect_private) {
        TEST_ASSERT_EQUAL_INT(1, cap.private_count);
        TEST_ASSERT_EQUAL_UINT32(portnum, cap.privates[0].portnum);
        TEST_ASSERT_EQUAL_UINT(sizeof(payload), cap.privates[0].len);
        TEST_ASSERT_EQUAL_MEMORY(payload, cap.privates[0].payload, sizeof(payload));
    } else {
        TEST_ASSERT_EQUAL_INT(0, cap.private_count);
        mc_stats_t stats = mc_get_stats(&c);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.decode_skipped);
    }
}

static void S03_AC5_private_portnum_boundary_255_is_not_private(void)
{
    run_private_portnum_boundary_case(255u, false);
}

static void S03_AC5_private_portnum_boundary_256_is_private(void)
{
    run_private_portnum_boundary_case(256u, true);
}

static void S03_AC5_private_portnum_boundary_511_is_private(void)
{
    run_private_portnum_boundary_case(511u, true);
}

static void S03_AC5_private_portnum_boundary_512_is_not_private(void)
{
    run_private_portnum_boundary_case(512u, false);
}

/* -------------------------------------------------------------------- */
/* AC6 — silence / transport-error reconnect                            */
/* -------------------------------------------------------------------- */

static void S03_AC6_silence_30s_reconnects_ready_disconnected_handshake(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c); /* -> HANDSHAKE */

    uint8_t cc_frame[32];
    uint16_t cc_len = build_config_complete_frame(c.want_config_id, cc_frame, sizeof(cc_frame));
    TEST_ASSERT_TRUE(cc_len > 0);
    io.rx_data = cc_frame;
    io.rx_len = cc_len;
    io.rx_pos = 0;

    mc_tick(&c, 100); /* -> READY */
    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));

    /* Go silent. */
    io.rx_data = NULL;
    io.rx_len = 0;
    io.rx_pos = 0;

    mc_tick(&c, 30100); /* 30100 - 100 >= 30000 -> DISCONNECTED, reconnect in 2s */
    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));

    mc_tick(&c, 31100); /* backoff not elapsed yet */
    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));

    mc_tick(&c, 32100); /* 2s backoff elapsed -> auto-reconnect */
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));

    TEST_ASSERT_EQUAL_INT(4, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);
    TEST_ASSERT_EQUAL(MC_STATE_READY, cap.states[1]);
    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, cap.states[2]);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[3]);

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.reconnects);
}

static void S03_AC6_transport_error_triggers_reconnect(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY;
    c.last_rx_ms = 0;

    io.rx_error_once = true;
    mc_tick(&c, 10);

    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));
    TEST_ASSERT_TRUE(c.reconnect_pending);
}

/* -------------------------------------------------------------------- */
/* AC7 — zero includes from core/ or app/ (except ff_clock_t)            */
/* -------------------------------------------------------------------- */

static bool line_has_forbidden_include(char const *line)
{
    char const *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "#include", 8) != 0) {
        return false;
    }
    p += 8;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"' && *p != '<') {
        return false;
    }
    p++;
    return (strncmp(p, "core/", 5) == 0) || (strncmp(p, "app/", 4) == 0);
}

static void scan_dir_for_forbidden_includes(char const *dir_path, int *violations)
{
    DIR *d = opendir(dir_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, dir_path);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        bool is_source = (nlen > 2 && strcmp(entry->d_name + nlen - 2, ".c") == 0) ||
                          (nlen > 2 && strcmp(entry->d_name + nlen - 2, ".h") == 0);
        if (!is_source) {
            continue;
        }

        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        FILE *f = fopen(path, "r");
        TEST_ASSERT_NOT_NULL_MESSAGE(f, path);

        char line[1024];
        while (fgets(line, sizeof(line), f) != NULL) {
            if (line_has_forbidden_include(line)) {
                (*violations)++;
            }
        }
        fclose(f);
    }
    closedir(d);
}

static void S03_AC7_zero_core_or_app_includes(void)
{
    int violations = 0;
    char path[1024];

    (void)snprintf(path, sizeof(path), "%s/include", MC_MESHCLIENT_DIR);
    scan_dir_for_forbidden_includes(path, &violations);

    (void)snprintf(path, sizeof(path), "%s/src", MC_MESHCLIENT_DIR);
    scan_dir_for_forbidden_includes(path, &violations);

    TEST_ASSERT_EQUAL_INT(0, violations);
}

/* -------------------------------------------------------------------- */
/* AC8 — fuzz smoke                                                     */
/* -------------------------------------------------------------------- */

/* PR #7 review finding 1: the original version of this test fed a
 * uniform-random 10,000-byte stream (fixed seed 0xC0FFEE) straight to
 * mc_tick() and called it a fuzz test. An independent replay of that exact
 * PRNG found *zero* 0x94 0xC3 occurrences anywhere in the stream — the
 * framer never left START1/START2, mc_framer_feed() never reached
 * LEN_HI/LEN_LO/PAYLOAD, and pb_decode() was never called. The test
 * "passed" without ever touching the code it claimed to fuzz.
 *
 * Fix: bias generation toward valid framing. fuzz_rng_next() drives three
 * kinds of segments: pure noise (still worth keeping — exercises the
 * resync path on real garbage), well-formed template frames encoded via
 * nanopb (exercises the full decode dispatch — my_info/node_info/
 * position/text/private/config_complete), and those same template frames
 * with a handful of payload bytes randomly flipped (exercises malformed-
 * but-framed input — decode_errors, truncated submessages, etc). Vacuity
 * is no longer just avoided by construction: the test structurally
 * ASSERTs (via mc_get_stats()) that frames_ok is nonzero, so a future
 * regression back to an all-garbage stream fails loudly instead of
 * quietly passing. */

static uint32_t fuzz_rng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Builds one of a handful of realistic FromRadio messages (selected by
 * `variant`), framed and ready to inject into the fuzz stream. Returns the
 * framed length, or 0 on failure (shouldn't happen — fixed, small inputs). */
static uint16_t build_fuzz_template_frame(uint32_t variant, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;

    switch (variant % 5u) {
    case 0:
        fr.which_payload_variant = meshtastic_FromRadio_my_info_tag;
        fr.payload_variant.my_info.my_node_num = 0x11223344u;
        break;

    case 1:
        fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
        fr.payload_variant.node_info.num = 0x0A0A0A0Au;
        fr.payload_variant.node_info.has_user = true;
        (void)snprintf(fr.payload_variant.node_info.user.long_name, MC_NAME_MAX, "Fuzz Node");
        fr.payload_variant.node_info.has_position = true;
        fr.payload_variant.node_info.position.has_latitude_i = true;
        fr.payload_variant.node_info.position.latitude_i = 407128000;
        fr.payload_variant.node_info.position.has_longitude_i = true;
        fr.payload_variant.node_info.position.longitude_i = -740060000;
        break;

    case 2: {
        fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
        fr.payload_variant.packet.from = 0x0A0A0A0Au;
        fr.payload_variant.packet.to = MC_ADDR_BROADCAST;
        fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        fr.payload_variant.packet.payload_variant.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        char const *txt = "fuzz";
        size_t tlen = strlen(txt);
        fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)tlen;
        memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, txt, tlen);
        break;
    }

    case 3: {
        fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
        fr.payload_variant.packet.from = 0x0B0B0B0Bu;
        fr.payload_variant.packet.to = MC_ADDR_BROADCAST;
        fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        fr.payload_variant.packet.payload_variant.decoded.portnum = (meshtastic_PortNum)300;
        uint8_t const priv[4] = {1, 2, 3, 4};
        fr.payload_variant.packet.payload_variant.decoded.payload.size = sizeof(priv);
        memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, priv, sizeof(priv));
        break;
    }

    default:
        fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
        fr.payload_variant.config_complete_id = 0xDEADBEEFu;
        break;
    }

    uint8_t payload[300];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
}

static void S03_AC8_fuzz_smoke_10k_random_frames_no_crash(void)
{
    static uint8_t buf[10000];
    uint32_t rng = 0xC0FFEEu; /* fixed seed: deterministic */
    uint32_t variant = 0;
    size_t pos = 0;

    while (pos < sizeof(buf)) {
        uint32_t choice = fuzz_rng_next(&rng) % 3u;

        if (choice == 0) {
            /* Pure noise segment — still worth fuzzing: exercises the
             * resync path on real garbage between valid frames. */
            size_t n = 1u + (fuzz_rng_next(&rng) % 40u);
            for (size_t i = 0; i < n && pos < sizeof(buf); i++, pos++) {
                buf[pos] = (uint8_t)(fuzz_rng_next(&rng) & 0xFFu);
            }
            continue;
        }

        uint8_t frame[300];
        uint16_t flen = build_fuzz_template_frame(variant++, frame, sizeof(frame));
        TEST_ASSERT_TRUE(flen > 4);

        if (choice == 2) {
            /* Mutate a few payload bytes (never the 4-byte header, so it
             * still frames) — well-formed-but-malformed input, reaching
             * pb_decode()/mc_process_from_radio() with garbage inside a
             * structurally valid frame. */
            uint32_t nmut = 1u + (fuzz_rng_next(&rng) % 3u);
            for (uint32_t m = 0; m < nmut; m++) {
                uint16_t idx = (uint16_t)(4u + (fuzz_rng_next(&rng) % (uint32_t)(flen - 4u)));
                frame[idx] = (uint8_t)(fuzz_rng_next(&rng) & 0xFFu);
            }
        }

        size_t n = (size_t)flen;
        if (n > sizeof(buf) - pos) {
            n = sizeof(buf) - pos;
        }
        memcpy(buf + pos, frame, n);
        pos += n;
    }

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = buf;
    io.rx_len = sizeof(buf);

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY; /* exercise the packet-decode paths too */

    mc_tick(&c, 1); /* mc_tick drains until the mock transport returns 0 */

    /* Reaching here without crashing/asserting under ASan/UBSan (run
     * manually — see the S03 PR body) is half the point. The other half,
     * per review finding 1: prove the fuzzed bytes actually reached the
     * frame+decode path, not just the framer's garbage-scanning loop. */
    TEST_ASSERT_EQUAL_UINT(io.rx_len, io.rx_pos);
    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.frames_ok);
}

/* -------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S03_AC1_byte_dribble_yields_frame_once);
    RUN_TEST(S03_AC1_garbage_prefix_yields_frame_once);
    RUN_TEST(S03_AC1_oversize_len_resyncs_without_overflow);
    RUN_TEST(S03_AC1_frame_length_exactly_512_is_accepted);
    RUN_TEST(S03_AC1_frame_length_513_resyncs);

    RUN_TEST(S03_AC2_connect_sends_want_config_and_enters_handshake);
    RUN_TEST(S03_AC2_handshake_dump_reaches_ready_with_node_and_myinfo);
    RUN_TEST(S03_AC2_handshake_wrong_nonce_stays_in_handshake);

    RUN_TEST(S03_AC3_position_packet_decodes_with_1e7_conversion_and_rx_time);

    RUN_TEST(S03_AC4_send_text_matches_byte_golden);

    RUN_TEST(S03_AC5_on_private_fires_for_portnum_256_511_untouched);
    RUN_TEST(S03_AC5_private_portnum_boundary_255_is_not_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_256_is_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_511_is_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_512_is_not_private);

    RUN_TEST(S03_AC6_silence_30s_reconnects_ready_disconnected_handshake);
    RUN_TEST(S03_AC6_transport_error_triggers_reconnect);

    RUN_TEST(S03_AC7_zero_core_or_app_includes);

    RUN_TEST(S03_AC8_fuzz_smoke_10k_random_frames_no_crash);

    return UNITY_END();
}
