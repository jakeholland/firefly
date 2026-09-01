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
        uint32_t from, to, portnum;
        uint8_t payload[300];
        size_t len;
    } privates[8];
    int private_count;

    bool got_my_info;
    uint32_t my_node_id;

    struct {
        uint32_t from;
        mc_rx_meta_t meta;
        int seq; /* dispatch order, shared with positions/texts below */
    } rx_metas[8];
    int rx_meta_count;

    /* Monotonic counter stamped by every callback that participates in the
     * on_rx_meta ordering guarantee, so a test can assert "meta first". */
    int seq_next;
    int first_position_seq;
    int first_text_seq;
    int first_private_seq;
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
    if (c->position_count == 0) {
        c->first_position_seq = c->seq_next;
    }
    c->seq_next++;
    if (c->position_count < (int)(sizeof(c->positions) / sizeof(c->positions[0]))) {
        c->positions[c->position_count].node = node;
        c->positions[c->position_count].pos = *p;
        c->position_count++;
    }
}

static void cap_on_rx_meta(void *u, uint32_t from, mc_rx_meta_t const *m)
{
    events_capture_t *c = (events_capture_t *)u;
    int seq = c->seq_next++;
    if (c->rx_meta_count < (int)(sizeof(c->rx_metas) / sizeof(c->rx_metas[0]))) {
        c->rx_metas[c->rx_meta_count].from = from;
        c->rx_metas[c->rx_meta_count].meta = *m;
        c->rx_metas[c->rx_meta_count].seq = seq;
        c->rx_meta_count++;
    }
}

static void cap_on_text(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->text_count == 0) {
        c->first_text_seq = c->seq_next;
    }
    c->seq_next++;
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

static void cap_on_private(void *u, uint32_t from, uint32_t to, uint32_t portnum, uint8_t const *payload,
                            size_t len)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->private_count == 0) {
        c->first_private_seq = c->seq_next;
    }
    c->seq_next++;
    if (c->private_count < (int)(sizeof(c->privates) / sizeof(c->privates[0]))) {
        c->privates[c->private_count].from = from;
        c->privates[c->private_count].to = to;
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
    ev.on_rx_meta = cap_on_rx_meta;
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
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, cap.privates[0].to); /* fixture is a broadcast (#123) */
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

/* Issue #123 — on_private delivers the MeshPacket's `to` address verbatim,
 * exactly as on_text already does. Two packets with DIFFERENT destinations
 * (a directed one, then a broadcast) must arrive with different captured
 * `to` values matching the wire — a dispatch that hardcodes either address
 * (or drops `to` again) fails on one of the two. */
static void I123_on_private_carries_to_address_verbatim(void)
{
    uint8_t const payload[2] = {0x01, 0x02};
    uint8_t frames[128];
    uint16_t f1 = build_data_packet_frame(0x0A0A0A0Au, 0x42424242u /* directed */, 300u, payload,
                                           sizeof(payload), frames, sizeof(frames));
    TEST_ASSERT_TRUE(f1 > 0);
    uint16_t f2 = build_data_packet_frame(0x0B0B0B0Bu, MC_ADDR_BROADCAST, 300u, payload, sizeof(payload),
                                           frames + f1, sizeof(frames) - f1);
    TEST_ASSERT_TRUE(f2 > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frames;
    io.rx_len = (size_t)f1 + (size_t)f2;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(2, cap.private_count);
    TEST_ASSERT_EQUAL_UINT32(0x42424242u, cap.privates[0].to);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, cap.privates[1].to);
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
        /* Check for truncation rather than (void)-ing it: a truncated path
         * would fopen the wrong file and the test would silently scan the
         * wrong source. Also what gcc-14's -Wformat-truncation flags (#81) —
         * dir_path is a runtime arg it can't bound. A real repo path is far
         * short of 1024; assert that rather than suppress. */
        int pn = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        TEST_ASSERT_TRUE_MESSAGE(pn > 0 && (size_t)pn < sizeof(path), entry->d_name);
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
/* AC9/AC10 shared builder — a fully parameterized MeshPacket frame      */
/* -------------------------------------------------------------------- */

/* One knob per wire field the two new features read, so each test names
 * exactly the packet shape it is pinning and nothing else. Zero-init gives
 * "a bare packet that states nothing", which is itself the input for the
 * absent-field cases. */
typedef struct {
    uint32_t from;
    uint32_t portnum;
    bool encrypted; /* which_payload_variant = encrypted rather than decoded */

    bool has_rx_rssi;
    int32_t rx_rssi;
    float rx_snr;

    uint32_t hop_start;
    uint32_t hop_limit;
    bool via_mqtt;
    bool set_bitfield; /* Data.bitfield present (sender >= 2.5.0) */

    /* POSITION_APP payload knobs (ignored for other portnums). */
    bool set_loc_source;
    uint32_t loc_source; /* raw wire value, so tests can inject unknown ones */
    uint32_t precision_bits; /* raw wire value; 0 encodes to nothing (proto3
                              * drops zeros), which IS the absent case — no
                              * separate set-flag could change those bytes */
    uint32_t rx_time;
} pkt_spec_t;

static bool encode_encrypted_blob(pb_ostream_t *stream, pb_field_t const *field, void *const *arg)
{
    static uint8_t const blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    (void)arg;
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }
    return pb_encode_string(stream, blob, sizeof(blob));
}

static uint16_t build_spec_frame(pkt_spec_t const *s, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;

    meshtastic_MeshPacket *pkt = &fr.payload_variant.packet;
    pkt->from = s->from;
    pkt->to = MC_ADDR_BROADCAST;
    pkt->id = 4242u;
    pkt->has_rx_rssi = s->has_rx_rssi;
    pkt->rx_rssi = s->rx_rssi;
    pkt->rx_snr = s->rx_snr;
    pkt->hop_start = s->hop_start;
    pkt->hop_limit = s->hop_limit;
    pkt->via_mqtt = s->via_mqtt;
    if (s->rx_time != 0u) {
        pkt->has_rx_time = true;
        pkt->rx_time = s->rx_time;
    }

    if (s->encrypted) {
        /* MeshPacket.encrypted is deliberately left as an uninstalled
         * pb_callback_t by mc_nanopb.options (encrypted payloads are out
         * of decode scope v1), so the test has to supply an encode
         * callback rather than filling a static byte array. */
        pkt->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
        pkt->payload_variant.encrypted.funcs.encode = encode_encrypted_blob;
    } else {
        pkt->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
        meshtastic_Data *d = &pkt->payload_variant.decoded;
        d->portnum = (meshtastic_PortNum)s->portnum;
        d->has_bitfield = s->set_bitfield;
        d->bitfield = s->set_bitfield ? 1u : 0u;

        if (s->portnum == (uint32_t)meshtastic_PortNum_POSITION_APP) {
            meshtastic_Position pos = meshtastic_Position_init_zero;
            pos.has_latitude_i = true;
            pos.latitude_i = 407128000;
            pos.has_longitude_i = true;
            pos.longitude_i = -740060000;
            if (s->set_loc_source) {
                pos.location_source = (meshtastic_Position_LocSource)s->loc_source;
            }
            pos.precision_bits = s->precision_bits;
            uint8_t pbuf[64];
            pb_ostream_t pos_os = pb_ostream_from_buffer(pbuf, sizeof(pbuf));
            if (!pb_encode(&pos_os, meshtastic_Position_fields, &pos)) {
                return 0;
            }
            d->payload.size = (pb_size_t)pos_os.bytes_written;
            memcpy(d->payload.bytes, pbuf, pos_os.bytes_written);
        } else {
            char const *txt = "hi";
            d->payload.size = (pb_size_t)strlen(txt);
            memcpy(d->payload.bytes, txt, strlen(txt));
        }
    }

    uint8_t buf[400];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

/* Runs one spec'd packet through a READY client and hands back the capture. */
static void run_spec(pkt_spec_t const *s, events_capture_t *cap)
{
    uint8_t frame[512];
    uint16_t flen = build_spec_frame(s, frame, sizeof(frame));
    TEST_ASSERT_TRUE(flen > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = flen;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    memset(cap, 0, sizeof(*cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(cap),
             &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);
}

/* -------------------------------------------------------------------- */
/* AC9 — position provenance (issue #33)                                 */
/* -------------------------------------------------------------------- */

static void run_loc_source_case(bool set_field, uint32_t wire_value, mc_loc_source_t expect)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_POSITION_APP,
        .set_loc_source = set_field,
        .loc_source = wire_value,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.position_count);
    TEST_ASSERT_EQUAL_INT((int)expect, (int)cap.positions[0].pos.loc_source);
}

/* The landmark case from issue #33: a fixed-position beacon asserts its
 * location rather than measuring it, and that must survive to the caller. */
static void S03_AC9_position_loc_source_manual_is_carried_through(void)
{
    run_loc_source_case(true, (uint32_t)meshtastic_Position_LocSource_LOC_MANUAL, MC_LOC_MANUAL);
}

static void S03_AC9_position_loc_source_internal_is_carried_through(void)
{
    run_loc_source_case(true, (uint32_t)meshtastic_Position_LocSource_LOC_INTERNAL, MC_LOC_INTERNAL);
}

static void S03_AC9_position_loc_source_external_is_carried_through(void)
{
    run_loc_source_case(true, (uint32_t)meshtastic_Position_LocSource_LOC_EXTERNAL, MC_LOC_EXTERNAL);
}

/* The absent-field case, and the one that matters most for honesty: a
 * sender that says nothing must NOT be reported as a GPS measurement. */
static void S03_AC9_position_absent_loc_source_is_unknown_not_internal(void)
{
    run_loc_source_case(false, 0u, MC_LOC_UNKNOWN);
}

/* LOC_UNSET explicitly on the wire is indistinguishable from absent (proto3
 * implicit presence) and must land on the same value — pinned so nobody
 * "helpfully" adds a has_loc_source flag that claims to tell them apart. */
static void S03_AC9_position_explicit_loc_unset_is_unknown(void)
{
    run_loc_source_case(true, (uint32_t)meshtastic_Position_LocSource_LOC_UNSET, MC_LOC_UNKNOWN);
}

/* Forward compatibility: a LocSource member added by future firmware must
 * degrade to UNKNOWN, never leak through as a raw number that core would
 * then compare against its own enum. */
static void S03_AC9_unknown_wire_loc_source_folds_to_unknown(void)
{
    run_loc_source_case(true, 99u, MC_LOC_UNKNOWN);
}

/* Provenance must also survive the nodeDB replay path, not just live
 * packets — a landmark beacon is typically first seen in the want_config
 * dump, which is exactly where mc_client.c already drops rx_time. */
static void S03_AC9_nodeinfo_position_carries_loc_source(void)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = 0x0C0C0C0Cu;
    fr.payload_variant.node_info.has_position = true;
    fr.payload_variant.node_info.position.has_latitude_i = true;
    fr.payload_variant.node_info.position.latitude_i = 407128000;
    fr.payload_variant.node_info.position.has_longitude_i = true;
    fr.payload_variant.node_info.position.longitude_i = -740060000;
    fr.payload_variant.node_info.position.location_source =
        meshtastic_Position_LocSource_LOC_MANUAL;

    uint8_t buf[300];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));

    uint8_t frame[400];
    uint16_t flen = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
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

    TEST_ASSERT_EQUAL_INT(1, cap.node_count);
    TEST_ASSERT_TRUE(cap.nodes[0].has_position);
    TEST_ASSERT_EQUAL_INT((int)MC_LOC_MANUAL, (int)cap.nodes[0].position.loc_source);
    /* Unchanged pre-existing behavior, re-pinned here because #33's whole
     * premise is that this replay has no reception time of its own. */
    TEST_ASSERT_FALSE(cap.nodes[0].position.has_rx_time);
}

/* NodeInfo's own hop summary uses explicit presence, so absent must read
 * UNKNOWN rather than being folded into "0 hops away = direct". */
static void run_nodeinfo_hops_case(bool has_hops, uint32_t hops, bool via_mqtt, mc_rx_path_t expect)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = 0x0D0D0D0Du;
    fr.payload_variant.node_info.has_hops_away = has_hops;
    fr.payload_variant.node_info.hops_away = hops;
    fr.payload_variant.node_info.via_mqtt = via_mqtt;

    uint8_t buf[300];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));

    uint8_t frame[400];
    uint16_t flen = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
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

    TEST_ASSERT_EQUAL_INT(1, cap.node_count);
    TEST_ASSERT_EQUAL_INT((int)expect, (int)cap.nodes[0].rx_path);
}

static void S03_AC9_nodeinfo_absent_hops_away_is_unknown_path(void)
{
    run_nodeinfo_hops_case(false, 0u, false, MC_RX_PATH_UNKNOWN);
}

static void S03_AC9_nodeinfo_zero_hops_away_is_direct_path(void)
{
    run_nodeinfo_hops_case(true, 0u, false, MC_RX_PATH_DIRECT);
}

static void S03_AC9_nodeinfo_nonzero_hops_away_is_indirect_path(void)
{
    run_nodeinfo_hops_case(true, 2u, false, MC_RX_PATH_INDIRECT);
}

static void S03_AC9_nodeinfo_via_mqtt_is_indirect_even_at_zero_hops(void)
{
    run_nodeinfo_hops_case(true, 0u, true, MC_RX_PATH_INDIRECT);
}

/* -------------------------------------------------------------------- */
/* AC10 — per-packet RSSI/SNR + hop path (issue #35)                     */
/* -------------------------------------------------------------------- */

static void S03_AC10_rx_meta_carries_rssi_and_snr(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .has_rx_rssi = true,
        .rx_rssi = -47,
        .rx_snr = 6.25f,
        .hop_start = 3u,
        .hop_limit = 3u,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_EQUAL_UINT32(0x0A0A0A0Au, cap.rx_metas[0].from);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_rssi);
    TEST_ASSERT_EQUAL_INT16(-47, cap.rx_metas[0].meta.rssi_dbm);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_snr);
    TEST_ASSERT_EQUAL_FLOAT(6.25f, cap.rx_metas[0].meta.snr_db);
    TEST_ASSERT_EQUAL_INT((int)MC_RX_PATH_DIRECT, (int)cap.rx_metas[0].meta.rx_path);
}

/* The absent-field case for RSSI: a packet with no rx_rssi must report
 * has_rssi == false, NOT a plausible-looking 0 dBm. */
static void S03_AC10_absent_rssi_is_flagged_absent_not_zero(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .has_rx_rssi = false,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_rssi);
}

/* The reason has_rssi exists at all rather than an in-band sentinel: 0 dBm
 * is a real reading some radios genuinely report, so it must survive as a
 * present value. This is the test that would fail if someone "simplified"
 * the flag away into a magic number. */
static void S03_AC10_rssi_of_exactly_zero_dbm_is_a_present_reading(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .has_rx_rssi = true,
        .rx_rssi = 0,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_rssi);
    TEST_ASSERT_EQUAL_INT16(0, cap.rx_metas[0].meta.rssi_dbm);
}

/* SNR has no presence flag on the wire, so 0.0 is unrecoverable. Pinning
 * the documented under-claiming choice: report unknown, never fabricate. */
static void S03_AC10_snr_of_exactly_zero_reports_unknown(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .rx_snr = 0.0f,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_snr);
}

/* Builds a float from its IEEE-754 bits without type-punning UB, so the
 * NaN/infinity cases below are exact rather than compiler-dependent. */
static float float_from_bits(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void run_snr_case(float wire_snr, bool expect_present)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .rx_snr = wire_snr,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    if (expect_present) {
        TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_snr);
        TEST_ASSERT_EQUAL_FLOAT(wire_snr, cap.rx_metas[0].meta.snr_db);
    } else {
        TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_snr);
        /* Absent must also mean benign: a caller that ignores the flag
         * reads 0, never NaN. Equality here doubles as a NaN check —
         * NaN == 0.0f is false. */
        TEST_ASSERT_TRUE(cap.rx_metas[0].meta.snr_db == 0.0f);
    }
}

/* PR #39 review finding F1. NaN compares unequal to everything, including
 * 0.0f, so a bare `!= 0.0f` presence test admits it and the library ends up
 * asserting "this is a measurement" for a non-number arriving from
 * untrusted RF — which then fails silently downstream, since every
 * comparison against NaN is false and any running mean is poisoned. */
static void S03_AC10_nan_snr_reports_unknown(void)
{
    run_snr_case(float_from_bits(0x7FC00000u), false); /* quiet NaN */
}

static void S03_AC10_signalling_nan_snr_reports_unknown(void)
{
    run_snr_case(float_from_bits(0x7F800001u), false);
}

static void S03_AC10_positive_infinity_snr_reports_unknown(void)
{
    run_snr_case(float_from_bits(0x7F800000u), false);
}

static void S03_AC10_negative_infinity_snr_reports_unknown(void)
{
    run_snr_case(float_from_bits(0xFF800000u), false);
}

static void S03_AC10_out_of_range_snr_reports_unknown(void)
{
    run_snr_case(3.0e38f, false);
}

/* The other half of the guard: bounds are meant to exclude garbage, not to
 * second-guess the radio. Readings at the edge of anything physically
 * plausible must still come through as present. */
static void S03_AC10_extreme_but_plausible_snr_is_still_present(void)
{
    run_snr_case(-30.0f, true);
    run_snr_case(15.0f, true);
    run_snr_case(MC_SNR_MIN_DB, true);
    run_snr_case(MC_SNR_MAX_DB, true);
}

static void run_rssi_case(int32_t wire_rssi, bool expect_present)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .has_rx_rssi = true,
        .rx_rssi = wire_rssi,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    if (expect_present) {
        TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_rssi);
        TEST_ASSERT_EQUAL_INT16((int16_t)wire_rssi, cap.rx_metas[0].meta.rssi_dbm);
    } else {
        TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_rssi);
    }
}

/* PR #39 review finding F2: the out-of-range guard was correct but no test
 * pinned it, so deleting it was an invisible regression.
 *
 * 65536 is the case that makes it matter — `(int16_t)65536 == 0`, so an
 * unguarded truncation would surface malformed wire data as
 * `has_rssi == true, rssi_dbm == 0`: precisely the "genuine 0 dBm" reading
 * that S03_AC10_rssi_of_exactly_zero_dbm_is_a_present_reading exists to
 * protect. Asserting *absence* rather than saturation also pins the
 * stronger property — see the comment in mc_emit_rx_meta on why a clamped
 * INT16_MAX would fabricate a CLOSE lock just as a truncated 0 would. */
static void S03_AC10_out_of_range_rssi_reports_unknown_not_zero(void)
{
    run_rssi_case(65536, false);
}

static void S03_AC10_negative_out_of_range_rssi_reports_unknown(void)
{
    run_rssi_case(-100000, false);
}

static void S03_AC10_extreme_but_plausible_rssi_is_still_present(void)
{
    run_rssi_case(-150, true);
    run_rssi_case(20, true);
    run_rssi_case(MC_RSSI_MIN_DBM, true);
    run_rssi_case(MC_RSSI_MAX_DBM, true);
}

static void run_rx_path_case(uint32_t hop_start, uint32_t hop_limit, bool via_mqtt, bool set_bitfield,
                              mc_rx_path_t expect)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .hop_start = hop_start,
        .hop_limit = hop_limit,
        .via_mqtt = via_mqtt,
        .set_bitfield = set_bitfield,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_EQUAL_INT((int)expect, (int)cap.rx_metas[0].meta.rx_path);
}

static void S03_AC10_hops_travelled_zero_is_direct(void)
{
    run_rx_path_case(3u, 3u, false, false, MC_RX_PATH_DIRECT);
}

static void S03_AC10_hops_travelled_nonzero_is_indirect(void)
{
    run_rx_path_case(3u, 1u, false, false, MC_RX_PATH_INDIRECT);
}

/* The trap this qualifier exists for: pre-2.3.0 firmware never populated
 * hop_start, so a bare hop_start == 0 must read UNKNOWN. Reading it as
 * DIRECT would attribute a relay's signal strength to a distant friend. */
static void S03_AC10_hop_start_zero_without_bitfield_is_unknown(void)
{
    run_rx_path_case(0u, 0u, false, false, MC_RX_PATH_UNKNOWN);
}

/* ...and the sender's bitfield (>= 2.5.0) is what licenses trusting it. */
static void S03_AC10_hop_start_zero_with_bitfield_is_direct(void)
{
    run_rx_path_case(0u, 0u, false, true, MC_RX_PATH_DIRECT);
}

/* Malformed: more hop_limit than we started with. Refuse to guess. */
static void S03_AC10_hop_limit_exceeding_hop_start_is_unknown(void)
{
    run_rx_path_case(2u, 5u, false, false, MC_RX_PATH_UNKNOWN);
}

/* Arrived over the internet — our radio never heard this sender at all,
 * so no hop arithmetic can make it direct. */
static void S03_AC10_via_mqtt_is_indirect_even_when_hops_say_direct(void)
{
    run_rx_path_case(3u, 3u, true, true, MC_RX_PATH_INDIRECT);
}

/* Breadth is the point: RSSI samples must not be limited to the position
 * path, which broadcasts far too slowly to feed a 5 s trend window. */
static void S03_AC10_rx_meta_fires_for_out_of_scope_portnum(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_TELEMETRY_APP,
        .has_rx_rssi = true,
        .rx_rssi = -80,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_rssi);
    TEST_ASSERT_EQUAL_INT16(-80, cap.rx_metas[0].meta.rssi_dbm);
    /* Still counted as out of decode scope — meta is additive, it does not
     * change what "decoded" means. */
    TEST_ASSERT_EQUAL_INT(0, cap.text_count);
    TEST_ASSERT_EQUAL_INT(0, cap.position_count);
}

static void S03_AC10_rx_meta_fires_for_encrypted_packet(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .encrypted = true,
        .has_rx_rssi = true,
        .rx_rssi = -55,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_rssi);
    TEST_ASSERT_EQUAL_INT16(-55, cap.rx_metas[0].meta.rssi_dbm);
    /* No decoded Data means no bitfield to consult. */
    TEST_ASSERT_EQUAL_INT((int)MC_RX_PATH_UNKNOWN, (int)cap.rx_metas[0].meta.rx_path);
}

/* Nobody to attribute the reading to. */
static void S03_AC10_rx_meta_does_not_fire_when_sender_unknown(void)
{
    pkt_spec_t s = {
        .from = 0u,
        .portnum = (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
        .has_rx_rssi = true,
        .rx_rssi = -55,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(0, cap.rx_meta_count);
}

/* The documented ordering guarantee, so S16's wiring can correlate meta
 * with the payload event by `from` without buffering. */
static void S03_AC10_rx_meta_precedes_the_payload_event(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_POSITION_APP,
        .has_rx_rssi = true,
        .rx_rssi = -33,
        .rx_time = 1700000101u,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_EQUAL_INT(1, cap.position_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].seq < cap.first_position_seq);
}

/* A client that never installs on_rx_meta must be entirely unaffected —
 * the callback is additive, not a new requirement. */
static void S03_AC10_null_rx_meta_callback_is_safe(void)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_POSITION_APP,
        .has_rx_rssi = true,
        .rx_rssi = -33,
    };
    uint8_t frame[512];
    uint16_t flen = build_spec_frame(&s, frame, sizeof(frame));
    TEST_ASSERT_TRUE(flen > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = flen;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_events_t ev = make_events(&cap);
    ev.on_rx_meta = NULL;

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, ev, &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(0, cap.rx_meta_count);
    TEST_ASSERT_EQUAL_INT(1, cap.position_count);
}

/* -------------------------------------------------------------------- */
/* AC11 — coordinate precision (issue #47)                               */
/* -------------------------------------------------------------------- */

/* The 2.7 km measurement behind this: a position truncated to the default
 * channel's 13 bits arrives as two ordinary-looking doubles with a fresh
 * timestamp. precision_bits is the only wire-level tell, so losing it (or
 * fabricating it) defeats the field's entire purpose. */

static void run_precision_case(uint32_t wire_bits, bool expect_present)
{
    pkt_spec_t s = {
        .from = 0x0A0A0A0Au,
        .portnum = (uint32_t)meshtastic_PortNum_POSITION_APP,
        .precision_bits = wire_bits,
    };
    events_capture_t cap;
    run_spec(&s, &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.position_count);
    if (expect_present) {
        TEST_ASSERT_TRUE(cap.positions[0].pos.has_precision_bits);
        TEST_ASSERT_EQUAL_UINT32(wire_bits, cap.positions[0].pos.precision_bits);
    } else {
        TEST_ASSERT_FALSE(cap.positions[0].pos.has_precision_bits);
        /* Absent must also be benign: a caller that ignores the flag reads
         * 0, not a leftover wire value that could pass a `>= 24` gate. */
        TEST_ASSERT_EQUAL_UINT32(0u, cap.positions[0].pos.precision_bits);
    }
}

/* The measured hardware case: the default public channel states 13 bits
 * (~5.8 km grid). This is the value the Radar face must someday refuse to
 * render a confident metre-level distance from. */
static void S03_AC11_precision_13_bits_is_carried_through(void)
{
    run_precision_case(13u, true);
}

/* The value the Firefly channel will state (~3 m). */
static void S03_AC11_precision_24_bits_is_carried_through(void)
{
    run_precision_case(24u, true);
}

/* Range boundaries. 1 is the least a sender can state; 32 is untruncated.
 * Both are statements, not garbage, and must survive. */
static void S03_AC11_precision_lower_boundary_1_is_present(void)
{
    run_precision_case(1u, true);
}

static void S03_AC11_precision_upper_boundary_32_is_present(void)
{
    run_precision_case(32u, true);
}

/* Wire 0 and an absent field are the same bytes (proto3 implicit presence),
 * and 0 never legitimately accompanies coordinates ("position disabled" in
 * channel config), so both read absent. This test IS the absent-field test:
 * a 0 knob encodes to nothing, and no builder flag could make it encode
 * differently — pinned so nobody adds a has_ flag claiming to tell apart
 * two identical byte streams. */
static void S03_AC11_precision_zero_or_absent_reads_absent(void)
{
    run_precision_case(0u, false);
}

/* Untrusted RF: >32 bits of a 32-bit coordinate is not a precision. Absent,
 * not clamped — a clamp to 32 would assert "full precision" for a malformed
 * packet, the exact confident-but-wrong reading this field exists to
 * prevent. 33 is the first bad value; the huge one guards against a
 * mod-32 "sanitizer" (0xFFFFFFFFu % 32 == 31, which would read present). */
static void S03_AC11_precision_33_reads_absent_not_clamped(void)
{
    run_precision_case(33u, false);
}

static void S03_AC11_precision_huge_wire_value_reads_absent(void)
{
    run_precision_case(0xFFFFFFFFu, false);
}

/* The NodeInfo-replay path decodes the field identically when the wire
 * carries it (stock firmware today does not replay it — see the path
 * caveat on mc_position_t.precision_bits — but the wire format allows it
 * and this library must not be the component that drops it). */
static void run_nodeinfo_precision_case(uint32_t wire_bits, bool expect_present)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = 0x0E0E0E0Eu;
    fr.payload_variant.node_info.has_position = true;
    fr.payload_variant.node_info.position.has_latitude_i = true;
    fr.payload_variant.node_info.position.latitude_i = 407128000;
    fr.payload_variant.node_info.position.has_longitude_i = true;
    fr.payload_variant.node_info.position.longitude_i = -740060000;
    fr.payload_variant.node_info.position.precision_bits = wire_bits;

    uint8_t buf[300];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));

    uint8_t frame[400];
    uint16_t flen = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
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

    TEST_ASSERT_EQUAL_INT(1, cap.node_count);
    TEST_ASSERT_TRUE(cap.nodes[0].has_position);
    if (expect_present) {
        TEST_ASSERT_TRUE(cap.nodes[0].position.has_precision_bits);
        TEST_ASSERT_EQUAL_UINT32(wire_bits, cap.nodes[0].position.precision_bits);
    } else {
        TEST_ASSERT_FALSE(cap.nodes[0].position.has_precision_bits);
        TEST_ASSERT_EQUAL_UINT32(0u, cap.nodes[0].position.precision_bits);
    }
}

static void S03_AC11_nodeinfo_position_carries_precision_bits(void)
{
    run_nodeinfo_precision_case(13u, true);
}

/* Today's stock-firmware reality: the replay omits the field. Absent must
 * read absent — NOT be defaulted to "full precision" because a live packet
 * from the same node once stated a value. (That correlation, if anyone
 * wants it, is consumer policy; this library reports the wire.) */
static void S03_AC11_nodeinfo_absent_precision_bits_reads_absent(void)
{
    run_nodeinfo_precision_case(0u, false);
}

/* PR #52 review finding F1: the pkt_spec_t knob is a uint32_t, but field 23
 * on the wire is a varint that can state values a uint32_t cannot — so the
 * ">32 never reads present" property, for that whole input class, rests on
 * what the DECODER does with the overflow, and nothing above pins it. This
 * test says the unsayable input with raw bytes.
 *
 * Why it exists / what it pins: vendored nanopb rejects the entire Position
 * for an oversized uint32 varint ("integer too large", pb_decode.c) —
 * decode_errors++, no event, honest. That rejection is LOAD-BEARING and
 * NONSTANDARD: mainline protobuf C++ *truncates* oversized uint32 varints,
 * under which 2^32+32 decodes to precision_bits == 32 — present, full
 * precision, fabricated from untrusted RF — while every knob-driven AC11
 * test stays green. If a nanopb upgrade or decoder swap makes this test
 * fail, the right response is adding a pre-decode range guard in front of
 * mc_position_from_pb(), NOT deleting the test.
 *
 * (Same shape as PR #39's NaN finding — a value class the presence test
 * never met — one level down: a value class the test harness itself could
 * never construct.)
 *
 * A wire varint wider than 32 bits in precision_bits — 2^32+32, which a
 * truncating decoder would read as 32 = full precision. nanopb must keep
 * rejecting the whole Position instead ("integer too large"); this pins
 * that, since pkt_spec_t's uint32 knob cannot state the input. */
static void S03_AC11_precision_overflow_varint_yields_no_position(void)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *pkt = &fr.payload_variant.packet;
    pkt->from = 0x0A0A0A0Au;
    pkt->to = MC_ADDR_BROADCAST;
    pkt->id = 4242u;
    pkt->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    meshtastic_Data *d = &pkt->payload_variant.decoded;
    d->portnum = meshtastic_PortNum_POSITION_APP;

    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = true;
    pos.latitude_i = 407128000;
    pos.has_longitude_i = true;
    pos.longitude_i = -740060000;
    uint8_t pbuf[64];
    pb_ostream_t pos_os = pb_ostream_from_buffer(pbuf, sizeof(pbuf));
    TEST_ASSERT_TRUE(pb_encode(&pos_os, meshtastic_Position_fields, &pos));
    size_t n = pos_os.bytes_written;
    /* field 23, wiretype 0, value 2^32 + 32 */
    uint8_t raw[7] = {0xB8, 0x01, 0xA0, 0x80, 0x80, 0x80, 0x10};
    memcpy(pbuf + n, raw, sizeof(raw));
    n += sizeof(raw);
    d->payload.size = (pb_size_t)n;
    memcpy(d->payload.bytes, pbuf, n);

    uint8_t buf[400];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));
    uint8_t frame[512];
    uint16_t flen = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io},
            make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(0, cap.position_count);
    TEST_ASSERT_EQUAL_UINT32(1u, c.stats.decode_errors);
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
    RUN_TEST(I123_on_private_carries_to_address_verbatim);

    RUN_TEST(S03_AC6_silence_30s_reconnects_ready_disconnected_handshake);
    RUN_TEST(S03_AC6_transport_error_triggers_reconnect);

    RUN_TEST(S03_AC7_zero_core_or_app_includes);

    RUN_TEST(S03_AC8_fuzz_smoke_10k_random_frames_no_crash);

    RUN_TEST(S03_AC9_position_loc_source_manual_is_carried_through);
    RUN_TEST(S03_AC9_position_loc_source_internal_is_carried_through);
    RUN_TEST(S03_AC9_position_loc_source_external_is_carried_through);
    RUN_TEST(S03_AC9_position_absent_loc_source_is_unknown_not_internal);
    RUN_TEST(S03_AC9_position_explicit_loc_unset_is_unknown);
    RUN_TEST(S03_AC9_unknown_wire_loc_source_folds_to_unknown);
    RUN_TEST(S03_AC9_nodeinfo_position_carries_loc_source);
    RUN_TEST(S03_AC9_nodeinfo_absent_hops_away_is_unknown_path);
    RUN_TEST(S03_AC9_nodeinfo_zero_hops_away_is_direct_path);
    RUN_TEST(S03_AC9_nodeinfo_nonzero_hops_away_is_indirect_path);
    RUN_TEST(S03_AC9_nodeinfo_via_mqtt_is_indirect_even_at_zero_hops);

    RUN_TEST(S03_AC10_rx_meta_carries_rssi_and_snr);
    RUN_TEST(S03_AC10_absent_rssi_is_flagged_absent_not_zero);
    RUN_TEST(S03_AC10_rssi_of_exactly_zero_dbm_is_a_present_reading);
    RUN_TEST(S03_AC10_snr_of_exactly_zero_reports_unknown);
    RUN_TEST(S03_AC10_nan_snr_reports_unknown);
    RUN_TEST(S03_AC10_signalling_nan_snr_reports_unknown);
    RUN_TEST(S03_AC10_positive_infinity_snr_reports_unknown);
    RUN_TEST(S03_AC10_negative_infinity_snr_reports_unknown);
    RUN_TEST(S03_AC10_out_of_range_snr_reports_unknown);
    RUN_TEST(S03_AC10_extreme_but_plausible_snr_is_still_present);
    RUN_TEST(S03_AC10_out_of_range_rssi_reports_unknown_not_zero);
    RUN_TEST(S03_AC10_negative_out_of_range_rssi_reports_unknown);
    RUN_TEST(S03_AC10_extreme_but_plausible_rssi_is_still_present);
    RUN_TEST(S03_AC10_hops_travelled_zero_is_direct);
    RUN_TEST(S03_AC10_hops_travelled_nonzero_is_indirect);
    RUN_TEST(S03_AC10_hop_start_zero_without_bitfield_is_unknown);
    RUN_TEST(S03_AC10_hop_start_zero_with_bitfield_is_direct);
    RUN_TEST(S03_AC10_hop_limit_exceeding_hop_start_is_unknown);
    RUN_TEST(S03_AC10_via_mqtt_is_indirect_even_when_hops_say_direct);
    RUN_TEST(S03_AC10_rx_meta_fires_for_out_of_scope_portnum);
    RUN_TEST(S03_AC10_rx_meta_fires_for_encrypted_packet);
    RUN_TEST(S03_AC10_rx_meta_does_not_fire_when_sender_unknown);
    RUN_TEST(S03_AC10_rx_meta_precedes_the_payload_event);
    RUN_TEST(S03_AC10_null_rx_meta_callback_is_safe);

    RUN_TEST(S03_AC11_precision_13_bits_is_carried_through);
    RUN_TEST(S03_AC11_precision_24_bits_is_carried_through);
    RUN_TEST(S03_AC11_precision_lower_boundary_1_is_present);
    RUN_TEST(S03_AC11_precision_upper_boundary_32_is_present);
    RUN_TEST(S03_AC11_precision_zero_or_absent_reads_absent);
    RUN_TEST(S03_AC11_precision_33_reads_absent_not_clamped);
    RUN_TEST(S03_AC11_precision_huge_wire_value_reads_absent);
    RUN_TEST(S03_AC11_nodeinfo_position_carries_precision_bits);
    RUN_TEST(S03_AC11_nodeinfo_absent_precision_bits_reads_absent);
    RUN_TEST(S03_AC11_precision_overflow_varint_yields_no_position);

    return UNITY_END();
}
