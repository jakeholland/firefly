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

static void S03_AC8_fuzz_smoke_10k_random_frames_no_crash(void)
{
    static uint8_t random_bytes[10000];
    uint32_t rng = 0xC0FFEEu; /* fixed seed: deterministic */
    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        random_bytes[i] = (uint8_t)(rng & 0xFFu);
    }

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = random_bytes;
    io.rx_len = sizeof(random_bytes);

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
     * manually — see the S03 PR body) is the point of this test. */
    TEST_ASSERT_EQUAL_UINT(io.rx_len, io.rx_pos);
}

/* -------------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S03_AC1_byte_dribble_yields_frame_once);
    RUN_TEST(S03_AC1_garbage_prefix_yields_frame_once);
    RUN_TEST(S03_AC1_oversize_len_resyncs_without_overflow);

    RUN_TEST(S03_AC2_connect_sends_want_config_and_enters_handshake);
    RUN_TEST(S03_AC2_handshake_dump_reaches_ready_with_node_and_myinfo);

    RUN_TEST(S03_AC3_position_packet_decodes_with_1e7_conversion_and_rx_time);

    RUN_TEST(S03_AC4_send_text_matches_byte_golden);

    RUN_TEST(S03_AC5_on_private_fires_for_portnum_256_511_untouched);

    RUN_TEST(S03_AC6_silence_30s_reconnects_ready_disconnected_handshake);
    RUN_TEST(S03_AC6_transport_error_triggers_reconnect);

    RUN_TEST(S03_AC7_zero_core_or_app_includes);

    RUN_TEST(S03_AC8_fuzz_smoke_10k_random_frames_no_crash);

    return UNITY_END();
}
