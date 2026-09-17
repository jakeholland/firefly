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

    /* S03-meshclient debt fix: write() backpressure. When > 0, mock_write
     * returns 0 ("accepted nothing, try again later" — never an error)
     * and decrements this instead of accepting bytes; once it reaches 0,
     * writes accept normally (or fail, per tx_fail, checked first).
     * Simulates a transport like a UART TX ring that is momentarily full
     * for exactly this many write() *calls*. */
    uint32_t write_zero_countdown;
    /* Never recovers: every write() call returns 0, forever (checked
     * before write_zero_countdown, and ignores it). Distinct from
     * write_zero_countdown so a "budget exhausted" test isn't pinning a
     * number that only holds by coincidence of the mock recovering at
     * exactly the budget boundary — this transport never would recover. */
    bool write_always_zero;
    uint32_t write_calls; /* total mock_write invocations, for budget pinning */
    uint32_t read_calls;  /* total mock_read invocations, for read-call-count pinning */
} mock_io_t;

static void mock_io_reset(mock_io_t *m)
{
    memset(m, 0, sizeof(*m));
}

static int mock_read(void *io, uint8_t *buf, size_t maxlen)
{
    mock_io_t *m = (mock_io_t *)io;
    m->read_calls++;
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
    m->write_calls++;
    if (m->tx_fail) {
        return -1;
    }
    if (m->write_always_zero) {
        return 0; /* never recovers — see the field's doc comment */
    }
    if (m->write_zero_countdown > 0) {
        m->write_zero_countdown--;
        return 0; /* "try again later" — not an error, per the write()
                   * backpressure contract in mc_client.h */
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
        char long_name[64];
        char short_name[16];
    } owners[4];
    int owner_count;

    struct {
        uint32_t request_id;
        bool ok;
    } routing_acks[4];
    int routing_ack_count;

    struct {
        uint32_t from;
        mc_rx_meta_t meta;
        int seq; /* dispatch order, shared with positions/texts below */
    } rx_metas[8];
    int rx_meta_count;

    /* [api] A02 slice D — the want_config channel table. */
    mc_channel_t channels[8];
    int channel_count;

    /* [api] A02 slice D2 — the LoRa region, and whether it was reported
     * at all (0 IS a region value — UNSET — so a count is the only way
     * to tell "reported UNSET" from "never reported"). */
    uint32_t lora_region;
    int      lora_region_count;

    /* Monotonic counter stamped by every callback that participates in the
     * on_rx_meta ordering guarantee, so a test can assert "meta first". */
    int seq_next;
    int first_position_seq;
    int first_text_seq;
    int first_private_seq;

    /* Bench finding 2026-09-14 — a LIVE NODEINFO_APP reply, as opposed
     * to `nodes[]`/`node_count` above (the want_config replay). */
    struct {
        uint32_t        from;
        mc_user_reply_t user;
    } nodeinfo_replies[8];
    int nodeinfo_reply_count;
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

static void cap_on_owner(void *u, char const *long_name, char const *short_name)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->owner_count < (int)(sizeof(c->owners) / sizeof(c->owners[0]))) {
        snprintf(c->owners[c->owner_count].long_name, sizeof(c->owners[0].long_name), "%s",
                 (long_name != NULL) ? long_name : "");
        snprintf(c->owners[c->owner_count].short_name, sizeof(c->owners[0].short_name), "%s",
                 (short_name != NULL) ? short_name : "");
        c->owner_count++;
    }
}

static void cap_on_routing_ack(void *u, uint32_t request_id, bool ok)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->routing_ack_count < (int)(sizeof(c->routing_acks) / sizeof(c->routing_acks[0]))) {
        c->routing_acks[c->routing_ack_count].request_id = request_id;
        c->routing_acks[c->routing_ack_count].ok = ok;
        c->routing_ack_count++;
    }
}

static void cap_on_channel(void *u, mc_channel_t const *ch)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->channel_count < (int)(sizeof(c->channels) / sizeof(c->channels[0]))) {
        c->channels[c->channel_count++] = *ch;
    }
}

static void cap_on_lora_region(void *u, uint32_t region)
{
    events_capture_t *c = (events_capture_t *)u;
    c->lora_region = region;
    c->lora_region_count++;
}

static void cap_on_nodeinfo_reply(void *u, uint32_t from, mc_user_reply_t const *user)
{
    events_capture_t *c = (events_capture_t *)u;
    if (c->nodeinfo_reply_count < (int)(sizeof(c->nodeinfo_replies) / sizeof(c->nodeinfo_replies[0]))) {
        c->nodeinfo_replies[c->nodeinfo_reply_count].from = from;
        c->nodeinfo_replies[c->nodeinfo_reply_count].user = *user;
        c->nodeinfo_reply_count++;
    }
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
    ev.on_owner = cap_on_owner;
    ev.on_routing_ack = cap_on_routing_ack;
    ev.on_channel = cap_on_channel;
    ev.on_lora_region = cap_on_lora_region;
    ev.on_nodeinfo_reply = cap_on_nodeinfo_reply;
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
        if (mc_framer_feed(&f, fixture[i], (uint32_t)i, &out, &out_len)) {
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
        if (mc_framer_feed(&f, garbage[i], (uint32_t)i, &out, &out_len)) {
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
        if (mc_framer_feed(&f, oversize[i], (uint32_t)i, &out, &out_len)) {
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
        if (mc_framer_feed(&f, framed[i], (uint32_t)i, &out, &out_len)) {
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
        TEST_ASSERT_FALSE(mc_framer_feed(&f, oversize_513_hdr[i], (uint32_t)i, &out, &out_len));
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
        if (mc_framer_feed(&f, text[i], (uint32_t)(sizeof(oversize_513_hdr) + i), &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(text_len - 4), out_len);
            TEST_ASSERT_EQUAL_MEMORY(text + 4, out, out_len);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, complete_count);

    free(text);
}

/* -------------------------------------------------------------------- */
/* AC1 (debt/link-churn-2026-09-16) — mid-frame resync timeout            */
/*                                                                        */
/* Direct repro of the report's mechanism: a byte gap mid-frame (light-  */
/* sleep RX loss, per docs/specs/S26-device-lifecycle.md's own "the RX   */
/* bytes ... are lost") must cost exactly the ONE frame it interrupted,  */
/* never the frame after it. See MC_FRAMER_RESYNC_TIMEOUT_MS's own doc   */
/* comment (mc_framing.h) for the timeout value and its justification.   */
/* -------------------------------------------------------------------- */

/* Builds a MC_MAX_FRAME-safe framed buffer with a deterministic,
 * distinguishable payload pattern (so a test can tell two different
 * frames' payloads apart at a glance in a failure message), returning
 * the total framed length (header + payload). `seed` picks the pattern. */
static uint16_t build_pattern_frame(uint8_t seed, uint16_t payload_len, uint8_t *out, size_t out_cap)
{
    uint8_t payload[64];
    TEST_ASSERT_TRUE(payload_len <= sizeof(payload));
    for (uint16_t i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)((i * 3u) + seed);
    }
    return mc_frame_encode(out, out_cap, payload, payload_len);
}

static void S03_AC1_timeout_frame_interrupted_past_timeout_is_discarded_and_resyncs(void)
{
    uint8_t frame_a[24];
    uint16_t const frame_a_len = build_pattern_frame(0x11u, 20u, frame_a, sizeof(frame_a));
    TEST_ASSERT_TRUE(frame_a_len > 0);

    uint8_t frame_b[24];
    uint16_t const frame_b_len = build_pattern_frame(0x77u, 20u, frame_b, sizeof(frame_b));
    TEST_ASSERT_TRUE(frame_b_len > 0);

    mc_framer_t f;
    mc_framer_init(&f);
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    int complete_count = 0;

    /* Frame A: header (4B) plus only HALF its payload arrives — the rest
     * is simply never delivered (the light-sleep RX-loss model: bytes are
     * LOST, not merely delayed, so nothing ever completes frame A). */
    uint16_t const delivered_a = 4u + (frame_a_len - 4u) / 2u;
    uint32_t now_ms = 0u;
    for (uint16_t i = 0; i < delivered_a; i++) {
        TEST_ASSERT_FALSE(mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len));
        now_ms += 1u;
    }
    TEST_ASSERT_EQUAL(MC_FRAMER_PAYLOAD, f.state);
    TEST_ASSERT_EQUAL_UINT32(0u, f.timeout_discards);

    /* The gap: well past MC_FRAMER_RESYNC_TIMEOUT_MS with no byte at all —
     * exactly what a light-sleep wake window looks like from the framer's
     * side. Measured from the LAST BYTE ACTUALLY FED (now_ms - 1, since
     * the loop above advances now_ms once more than bytes fed), not from
     * `now_ms` itself. */
    now_ms = (now_ms - 1u) + MC_FRAMER_RESYNC_TIMEOUT_MS + 1u;

    /* Frame B arrives next, in full, at the wire's normal pace. Today
     * (pre-fix) frame B's own 0x94 0xC3 header would be swallowed as
     * "more of frame A's payload" and BOTH frames would be lost. With the
     * fix, the stale frame A is discarded the moment frame B's first byte
     * breaks the silence, and frame B is recognized and parsed cleanly. */
    for (uint16_t i = 0; i < frame_b_len; i++) {
        if (mc_framer_feed(&f, frame_b[i], now_ms, &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(frame_b_len - 4u), out_len);
            TEST_ASSERT_EQUAL_MEMORY(frame_b + 4, out, out_len);
        }
        now_ms += 1u;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, complete_count,
                                   "frame B must be recovered even though frame A stalled right before it");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, f.timeout_discards,
                                      "exactly ONE stall was discarded — frame A's, and only once");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, f.resync_count,
                                      "this is a clean timeout discard, not garbage-scanning — resync_count "
                                      "(a DIFFERENT counter, see its own doc comment) must stay untouched");
}

/* The other half of the same fix: a pause SHORTER than the timeout is
 * exactly what "legitimately slow-but-continuous sender" means, and must
 * not be punished — the frame completes normally once the rest of its
 * payload resumes. */
static void S03_AC1_timeout_frame_interrupted_within_timeout_still_completes(void)
{
    uint8_t frame_a[24];
    uint16_t const frame_a_len = build_pattern_frame(0x22u, 20u, frame_a, sizeof(frame_a));
    TEST_ASSERT_TRUE(frame_a_len > 0);

    mc_framer_t f;
    mc_framer_init(&f);
    uint8_t const *out = NULL;
    uint16_t out_len = 0;

    uint16_t const half = 4u + (frame_a_len - 4u) / 2u;
    uint32_t now_ms = 0u;
    for (uint16_t i = 0; i < half; i++) {
        TEST_ASSERT_FALSE(mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len));
        now_ms += 1u;
    }

    /* Pause, but strictly LESS than the timeout — comfortably so (half
     * the timeout), to keep this test clearly distinct from the
     * exact-boundary one below rather than merely one off it. Measured
     * from the LAST BYTE ACTUALLY FED (now_ms - 1; see the equivalent
     * comment in the timeout test above for why). */
    TEST_ASSERT_TRUE(MC_FRAMER_RESYNC_TIMEOUT_MS >= 2u);
    now_ms = (now_ms - 1u) + MC_FRAMER_RESYNC_TIMEOUT_MS / 2u;

    int complete_count = 0;
    for (uint16_t i = half; i < frame_a_len; i++) {
        if (mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)(frame_a_len - 4u), out_len);
            TEST_ASSERT_EQUAL_MEMORY(frame_a + 4, out, out_len);
        }
        now_ms += 1u;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, complete_count, "a pause under the timeout must not lose the frame");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, f.timeout_discards, "no discard — the gap never exceeded the timeout");
}

/* Pins the exact comparison the timeout uses: a gap of EXACTLY
 * MC_FRAMER_RESYNC_TIMEOUT_MS is still within budget (">", not ">="), one
 * more millisecond is not. Both halves live in one test so a future
 * change to the comparison operator cannot flip one half green by
 * accident while this test still nominally "covers the boundary". */
static void S03_AC1_timeout_gap_exactly_at_boundary_is_not_a_timeout(void)
{
    /* Exactly-at-boundary case: gap == timeout must NOT discard. */
    {
        uint8_t frame_a[24];
        uint16_t const frame_a_len = build_pattern_frame(0x33u, 20u, frame_a, sizeof(frame_a));
        TEST_ASSERT_TRUE(frame_a_len > 0);

        mc_framer_t f;
        mc_framer_init(&f);
        uint8_t const *out = NULL;
        uint16_t out_len = 0;

        uint16_t const half = 4u + (frame_a_len - 4u) / 2u;
        uint32_t now_ms = 0u;
        for (uint16_t i = 0; i < half; i++) {
            (void)mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len);
            now_ms += 1u;
        }
        /* gap == timeout, exactly, measured from the LAST BYTE ACTUALLY
         * FED (now_ms - 1; the loop above leaves now_ms one past it). */
        now_ms = (now_ms - 1u) + MC_FRAMER_RESYNC_TIMEOUT_MS;

        int complete_count = 0;
        for (uint16_t i = half; i < frame_a_len; i++) {
            if (mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len)) {
                complete_count++;
            }
            now_ms += 1u;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, complete_count, "gap == timeout must complete, not discard");
        TEST_ASSERT_EQUAL_UINT32(0u, f.timeout_discards);
    }

    /* One millisecond further: gap == timeout + 1 MUST discard. */
    {
        uint8_t frame_a[24];
        uint16_t const frame_a_len = build_pattern_frame(0x44u, 20u, frame_a, sizeof(frame_a));
        TEST_ASSERT_TRUE(frame_a_len > 0);
        uint8_t frame_b[24];
        uint16_t const frame_b_len = build_pattern_frame(0x55u, 20u, frame_b, sizeof(frame_b));
        TEST_ASSERT_TRUE(frame_b_len > 0);

        mc_framer_t f;
        mc_framer_init(&f);
        uint8_t const *out = NULL;
        uint16_t out_len = 0;

        uint16_t const half = 4u + (frame_a_len - 4u) / 2u;
        uint32_t now_ms = 0u;
        for (uint16_t i = 0; i < half; i++) {
            (void)mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len);
            now_ms += 1u;
        }
        /* one past the boundary, again measured from the LAST BYTE
         * ACTUALLY FED (now_ms - 1). */
        now_ms = (now_ms - 1u) + MC_FRAMER_RESYNC_TIMEOUT_MS + 1u;

        int complete_count = 0;
        for (uint16_t i = 0; i < frame_b_len; i++) {
            if (mc_framer_feed(&f, frame_b[i], now_ms, &out, &out_len)) {
                complete_count++;
            }
            now_ms += 1u;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, complete_count, "frame B still recovered after the discard");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, f.timeout_discards, "gap == timeout+1 must discard");
    }
}

/* Regression guard: ordinary back-to-back frames (no artificial gap at
 * all between frame A's last byte and frame B's first) must keep working
 * exactly as before this fix — the timeout must never fire on healthy,
 * continuous traffic. */
static void S03_AC1_timeout_back_to_back_frames_no_gap_not_regressed(void)
{
    uint8_t frame_a[24];
    uint16_t const frame_a_len = build_pattern_frame(0x66u, 20u, frame_a, sizeof(frame_a));
    TEST_ASSERT_TRUE(frame_a_len > 0);
    uint8_t frame_b[24];
    uint16_t const frame_b_len = build_pattern_frame(0x99u, 20u, frame_b, sizeof(frame_b));
    TEST_ASSERT_TRUE(frame_b_len > 0);

    mc_framer_t f;
    mc_framer_init(&f);
    uint8_t const *out = NULL;
    uint16_t out_len = 0;
    int complete_count = 0;
    uint32_t now_ms = 0u;

    for (uint16_t i = 0; i < frame_a_len; i++) {
        if (mc_framer_feed(&f, frame_a[i], now_ms, &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_MEMORY(frame_a + 4, out, out_len);
        }
        now_ms += 1u;
    }
    for (uint16_t i = 0; i < frame_b_len; i++) {
        if (mc_framer_feed(&f, frame_b[i], now_ms, &out, &out_len)) {
            complete_count++;
            TEST_ASSERT_EQUAL_MEMORY(frame_b + 4, out, out_len);
        }
        now_ms += 1u;
    }

    TEST_ASSERT_EQUAL_INT(2, complete_count);
    TEST_ASSERT_EQUAL_UINT32(0u, f.timeout_discards);
    TEST_ASSERT_EQUAL_UINT32(0u, f.resync_count);
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

/* NAME-in-Settings reboot-session-loss fix (bench finding, 2026-09-06) —
 * a `FromRadio.rebooted` frame, Meshtastic's explicit "the radio just
 * rebooted" tell (mesh.pb.h tag 8). */
static uint16_t build_rebooted_frame(uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    fr.payload_variant.rebooted = true;

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

    int rc = mc_send_text(&c, 0x0A0A0A0Au, "hi", NULL);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT(golden_len, io.tx_len);
    TEST_ASSERT_EQUAL_MEMORY(golden, io.tx_buf, golden_len);

    free(golden);
}

/* Forward declaration — defined below (feat/s10-flare-want-ack section),
 * needed here first: decodes a single outbound ToRadio frame's
 * MeshPacket.want_ack bit. See that definition's own doc comment. */
static bool decode_tx_want_ack(mock_io_t const *io);

/* -------------------------------------------------------------------- */
/* Outbox delivery status feature (2026-09-07) — mc_send_text's new      */
/* out_packet_id, and the want_ack-by-destination rule it now lets a     */
/* caller correlate an ack against.                                     */
/* -------------------------------------------------------------------- */

static void feat_send_text_direct_returns_packet_id_and_wants_ack(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint32_t packet_id = 0xDEADBEEFu; /* poisoned — must be overwritten on success */
    int rc = mc_send_text(&c, 0x0A0A0A0Au, "hi", &packet_id);

    TEST_ASSERT_EQUAL_INT(0, rc);
    /* mc_init's default seed is 1 and nothing else has sent yet. */
    TEST_ASSERT_EQUAL_UINT32(1u, packet_id);
    TEST_ASSERT_TRUE_MESSAGE(decode_tx_want_ack(&io),
                              "a direct (non-broadcast) text must request a routing ack to correlate against");
}

static void feat_send_text_broadcast_returns_packet_id_but_no_ack_requested(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint32_t packet_id = 0;
    int rc = mc_send_text(&c, MC_ADDR_BROADCAST, "hi", &packet_id);

    TEST_ASSERT_EQUAL_INT(0, rc);
    /* out_packet_id is still populated for a broadcast — harmless, the
     * mesh just never sends an ack back for the caller to match it
     * against — see mc_send_text's own doc comment. */
    TEST_ASSERT_EQUAL_UINT32(1u, packet_id);
    TEST_ASSERT_FALSE_MESSAGE(decode_tx_want_ack(&io),
                               "a broadcast never requests a routing ack — the mesh gives it none");
}

static void feat_send_text_out_packet_id_is_optional(void)
{
    /* NULL out_packet_id must not crash — every pre-existing caller in
     * the tree before this feature passed none (mirrors
     * feat_set_owner_out_packet_id_is_optional's own test). */
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, 1u, "hi", NULL));
}

static void feat_send_text_fails_when_not_ready_leaves_out_packet_id_untouched(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    /* c.state left at its mc_init default (DISCONNECTED) — not READY. */

    uint32_t packet_id = 0xDEADBEEFu;
    int rc = mc_send_text(&c, 1u, "hi", &packet_id);

    TEST_ASSERT_TRUE(rc < 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xDEADBEEFu, packet_id, "a failed send must not touch the caller's out param");
}

/* -------------------------------------------------------------------- */
/* feat/s10-flare-want-ack — mc_send_private's want_ack reaches the wire */
/* -------------------------------------------------------------------- */

/* Decode a single outbound ToRadio frame written into `io->tx_buf` (magic
 * + 2-byte len header, then the protobuf) and return its
 * MeshPacket.want_ack — the nanopb-level check the task asks for: not
 * "mc_send_private accepted a bool" but "the bit actually landed in the
 * encoded ToRadio bytes a real radio would receive". */
static bool decode_tx_want_ack(mock_io_t const *io)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, io->tx_len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, io->tx_buf[0]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, io->tx_buf[1]);
    uint16_t flen = (uint16_t)((io->tx_buf[2] << 8) | io->tx_buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(io->tx_len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io->tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);
    return tr.payload_variant.packet.want_ack;
}

static void S03_want_ack_mc_send_private_true_sets_meshpacket_want_ack(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint8_t const payload[3] = {0x01, 0x2C, 0x01}; /* arbitrary bytes; content is irrelevant here */
    int rc = mc_send_private(&c, MC_ADDR_BROADCAST, 269u, payload, sizeof(payload), /*want_ack=*/true);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(decode_tx_want_ack(&io));
}

static void S03_want_ack_mc_send_private_false_leaves_meshpacket_want_ack_unset(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint8_t const payload[3] = {0x01, 0x2C, 0x01};
    int rc = mc_send_private(&c, MC_ADDR_BROADCAST, 269u, payload, sizeof(payload), /*want_ack=*/false);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(decode_tx_want_ack(&io));
}

/* -------------------------------------------------------------------- */
/* S14 hardening pass, item 5 (test-gap sweep) — mc_send_position() had
 * ZERO direct test coverage anywhere in this tree (confirmed by cross-
 * referencing every public core/meshclient function's declared name
 * against every *.c test file's call sites — the only one of 192 with
 * no hit). Its siblings (mc_send_text/mc_send_private/mc_send_set_owner)
 * all have direct byte-level coverage above and in this file's other
 * sections; this closes the one real gap the sweep found.
 * -------------------------------------------------------------------- */

/* Decode a single outbound ToRadio frame (mirrors decode_tx_want_ack's
 * own framing-strip logic just above) and return its decoded
 * MeshPacket — the fuller sibling that test needed only a single bit
 * from; this one hands back the whole packet so a caller can check
 * dest/portnum/want_ack AND decode the Data.payload as a Position in
 * one place. */
static meshtastic_MeshPacket decode_tx_packet(mock_io_t const *io)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, io->tx_len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, io->tx_buf[0]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, io->tx_buf[1]);
    uint16_t flen = (uint16_t)((io->tx_buf[2] << 8) | io->tx_buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(io->tx_len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io->tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);
    return tr.payload_variant.packet;
}

/* Not READY: rejected outright, exactly like every other mc_send_*
 * function's documented "not READY" failure — and, just as importantly,
 * nothing at all reaches the transport (a caller that ignores the
 * negative return must never have leaked a partial/stale position onto
 * the wire). */
static void S14_mc_send_position_not_ready_returns_error_writes_nothing(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    /* c.state left at mc_init's own default (MC_STATE_DISCONNECTED) — deliberately never set READY. */

    ff_latlon_t const p = {.lat = 41.1234567, .lon = -84.7654321};
    int rc = mc_send_position(&c, p);

    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_EQUAL_UINT(0, io.tx_len);
    TEST_ASSERT_EQUAL_UINT32(0, io.write_calls);
}

/* Success path: broadcast, POSITION_APP, no ack, and — the part a bare
 * "rc == 0" assertion would never catch — the actual encoded
 * latitude_i/longitude_i survive the real i1e7 round-trip
 * (mc_send_position's own `p.lat * 1e7 + 0.5` rounding, decoded back
 * here via nanopb, the same asymmetric encoder mc_position_from_pb's
 * OWN i1e7_to_deg on the receive side would decode) to within one
 * fixed-point unit (~1.1 cm) of the original double. */
static void S14_mc_send_position_encodes_broadcast_position_app_no_ack(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    ff_latlon_t const p = {.lat = 41.1234567, .lon = -84.7654321};
    int rc = mc_send_position(&c, p);
    TEST_ASSERT_EQUAL_INT(0, rc);

    meshtastic_MeshPacket pkt = decode_tx_packet(&io);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, pkt.to);
    TEST_ASSERT_FALSE(pkt.want_ack);
    TEST_ASSERT_EQUAL_INT(meshtastic_MeshPacket_decoded_tag, pkt.which_payload_variant);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)meshtastic_PortNum_POSITION_APP, (uint32_t)pkt.payload_variant.decoded.portnum);

    meshtastic_Position pos = meshtastic_Position_init_zero;
    pb_istream_t is = pb_istream_from_buffer(pkt.payload_variant.decoded.payload.bytes,
                                              pkt.payload_variant.decoded.payload.size);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_Position_fields, &pos));
    TEST_ASSERT_TRUE(pos.has_latitude_i);
    TEST_ASSERT_TRUE(pos.has_longitude_i);
    TEST_ASSERT_INT32_WITHIN(1, (int32_t)(41.1234567 * 1e7), pos.latitude_i);
    TEST_ASSERT_INT32_WITHIN(1, (int32_t)(-84.7654321 * 1e7), pos.longitude_i);
}

/* Negative-coordinate rounding: mc_send_position's `p.lat >= 0.0 ? 0.5 :
 * -0.5` ternary only ever takes its `-0.5` arm for a negative value
 * (southern-hemisphere lat / western-hemisphere lon) — a "forgot the
 * sign, always +0.5" bug would silently round every such coordinate 1
 * unit (~1.1 cm) toward zero instead of to the nearest unit. -12.0 is
 * exactly representable in a double (a small integer) and so is
 * -12.0 * 1e7 = -120000000.0 — no fractional part at all, so this
 * isn't testing rounding-to-nearest at a tie (which floating-point
 * representation noise could make non-deterministic across platforms —
 * deliberately avoided), it is testing which of two ADJACENT, exactly-
 * representable integers a "no fractional remainder" input still lands
 * on: correctly-signed rounding keeps it at exactly -120000000; the
 * "always +0.5" bug would instead produce -119999999 (0.5 truncated
 * toward zero) — the two disagree by exactly the encoded value this
 * test asserts, not a fuzzy tolerance. */
static void S14_mc_send_position_rounds_negative_coordinates_away_from_zero(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    ff_latlon_t const p = {.lat = -12.0, .lon = -12.0};
    int rc = mc_send_position(&c, p);
    TEST_ASSERT_EQUAL_INT(0, rc);

    meshtastic_MeshPacket pkt = decode_tx_packet(&io);
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pb_istream_t is = pb_istream_from_buffer(pkt.payload_variant.decoded.payload.bytes,
                                              pkt.payload_variant.decoded.payload.size);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_Position_fields, &pos));
    TEST_ASSERT_EQUAL_INT32(-120000000, pos.latitude_i);
    TEST_ASSERT_EQUAL_INT32(-120000000, pos.longitude_i);
}

/* -------------------------------------------------------------------- */
/* fix/meshclient-packet-id-seed — outgoing packet-id generator          */
/* -------------------------------------------------------------------- */

/* Decode one outbound ToRadio frame starting at byte offset `*offset` of
 * `io->tx_buf` (io->tx_buf accumulates every frame a test's mc_send_*
 * calls have written, back to back — mock_write() appends, never
 * overwrites), return its MeshPacket.id, and advance `*offset` past the
 * frame so a caller can walk several sends in one test without resetting
 * the mock transport between them. Same magic+len framing decode_tx_
 * want_ack already uses, generalized to a cursor. */
static uint32_t decode_tx_packet_id_advance(mock_io_t const *io, size_t *offset)
{
    size_t off = *offset;
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(off + 4u, io->tx_len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, io->tx_buf[off]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, io->tx_buf[off + 1]);
    uint16_t flen = (uint16_t)((io->tx_buf[off + 2] << 8) | io->tx_buf[off + 3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(io->tx_len - off - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io->tx_buf + off + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    *offset = off + 4u + (size_t)flen;
    return tr.payload_variant.packet.id;
}

/* Not calling mc_seed_packet_ids() at all must reproduce the exact
 * pre-fix sequence (1, 2, 3, ...) — every pre-existing test, including
 * the AC4 byte-golden fixture, relies on this. This is the compatibility
 * half of the fix: the new API is opt-in. */
static void S03_packet_id_unseeded_matches_legacy_sequence(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "a", NULL));
    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "b", NULL));
    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "c", NULL));

    size_t off = 0;
    TEST_ASSERT_EQUAL_UINT32(1u, decode_tx_packet_id_advance(&io, &off));
    TEST_ASSERT_EQUAL_UINT32(2u, decode_tx_packet_id_advance(&io, &off));
    TEST_ASSERT_EQUAL_UINT32(3u, decode_tx_packet_id_advance(&io, &off));
}

/* Calling mc_seed_packet_ids() moves the starting point: ids start at the
 * seed and increment by 1 per send, and the counter is shared across
 * mc_send_text/mc_send_private (the S04 firefly-protocol path) — this is
 * the "every sender path goes through one generator" audit the task
 * calls for, proven by observation rather than by reading the source. */
static void S03_packet_id_seed_sets_starting_point_and_increments(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    mc_seed_packet_ids(&c, 1000u);
    c.state = MC_STATE_READY;

    uint8_t const payload[2] = {0xAB, 0xCD};
    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "hi", NULL));
    TEST_ASSERT_EQUAL_INT(0, mc_send_private(&c, MC_ADDR_BROADCAST, 269u, payload, sizeof(payload), false));
    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "again", NULL));

    size_t off = 0;
    TEST_ASSERT_EQUAL_UINT32(1000u, decode_tx_packet_id_advance(&io, &off));
    TEST_ASSERT_EQUAL_UINT32(1001u, decode_tx_packet_id_advance(&io, &off));
    TEST_ASSERT_EQUAL_UINT32(1002u, decode_tx_packet_id_advance(&io, &off));
}

/* 0 is never a valid Meshtastic packet id (the wire's "unset" sentinel),
 * so a counter that wraps past UINT32_MAX must skip straight to 1
 * instead of handing a caller id 0. Also proves seeding with 0 itself is
 * treated as 1, same as mc_init()'s own default. */
static void S03_packet_id_skips_zero_on_wrap_and_on_zero_seed(void)
{
    /* Wrap case: seed at UINT32_MAX, first id is UINT32_MAX itself
     * (a seed is a legitimate id, not something to be skipped), second
     * id is 1, not 0. */
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
        mc_seed_packet_ids(&c, 0xFFFFFFFFu);
        c.state = MC_STATE_READY;

        TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "x", NULL));
        TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "y", NULL));

        size_t off = 0;
        TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, decode_tx_packet_id_advance(&io, &off));
        TEST_ASSERT_EQUAL_UINT32(1u, decode_tx_packet_id_advance(&io, &off));
    }

    /* Zero-seed case: mc_seed_packet_ids(c, 0) behaves exactly like never
     * calling it — starts at 1 — rather than handing out id 0. */
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
        mc_seed_packet_ids(&c, 0u);
        c.state = MC_STATE_READY;

        TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, MC_ADDR_BROADCAST, "z", NULL));

        size_t off = 0;
        TEST_ASSERT_EQUAL_UINT32(1u, decode_tx_packet_id_advance(&io, &off));
    }
}

/* The actual regression this fix closes: two client "lifetimes" (e.g. two
 * boots of the same puck) that mc_init() alone would both start at
 * packet id 1 — a real collision in the mesh router's (from, id) history,
 * per the bench log in this fix's PR body — produce disjoint ids once
 * each lifetime is seeded distinctly (standing in here for "the platform
 * picked a different random seed each boot"). Fails to even compile
 * against pre-fix mc_client.h/.c (no mc_seed_packet_ids()); restoring the
 * old two-line mc_client.c (bare `c->next_packet_id++`, no
 * mc_seed_packet_ids()) and hand-adapting this test to call it directly
 * on next_packet_id would still fail the wrap/zero assertions above —
 * see the fix's PR body for the revert-and-rerun proof. */
static void S03_packet_id_different_seeds_produce_disjoint_ids(void)
{
    mock_io_t io_a, io_b;
    mock_io_reset(&io_a);
    mock_io_reset(&io_b);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap_a, cap_b;
    memset(&cap_a, 0, sizeof(cap_a));
    memset(&cap_b, 0, sizeof(cap_b));

    mc_client_t a, b;
    mc_init(&a, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io_a}, make_events(&cap_a),
            &clock);
    mc_seed_packet_ids(&a, 0x10000000u);
    a.state = MC_STATE_READY;

    mc_init(&b, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io_b}, make_events(&cap_b),
            &clock);
    mc_seed_packet_ids(&b, 0x80000000u);
    b.state = MC_STATE_READY;

    uint32_t ids_a[3], ids_b[3];
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(0, mc_send_text(&a, MC_ADDR_BROADCAST, "a", NULL));
        TEST_ASSERT_EQUAL_INT(0, mc_send_text(&b, MC_ADDR_BROADCAST, "b", NULL));
    }
    size_t off_a = 0, off_b = 0;
    for (int i = 0; i < 3; i++) {
        ids_a[i] = decode_tx_packet_id_advance(&io_a, &off_a);
        ids_b[i] = decode_tx_packet_id_advance(&io_b, &off_b);
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            TEST_ASSERT_FALSE(ids_a[i] == ids_b[j]);
        }
    }
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

/* -------------------------------------------------------------------- */
/* debt/meshclient-contracts — honest decode_errors on the live Position */
/* path: a well-formed "no fix yet" broadcast is not corruption; only a  */
/* pb_decode() failure is.                                               */
/* -------------------------------------------------------------------- */

static void S03_debt_position_no_fix_yet_does_not_count_as_decode_error(void)
{
    /* A perfectly well-formed Position that states no coordinates — the
     * legitimate "no GPS fix yet" broadcast (proto3 implicit presence:
     * has_latitude_i/has_longitude_i simply false). Give it *some* other
     * field so the payload isn't literally empty. */
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.time = 1700000005u; /* implicit presence (no has_time on the wire) */

    uint8_t pbuf[32];
    pb_ostream_t pos_os = pb_ostream_from_buffer(pbuf, sizeof(pbuf));
    TEST_ASSERT_TRUE(pb_encode(&pos_os, meshtastic_Position_fields, &pos));

    uint8_t frame[128];
    uint16_t flen = build_data_packet_frame(0x0A0A0A0Au, MC_ADDR_BROADCAST,
                                             (uint32_t)meshtastic_PortNum_POSITION_APP, pbuf,
                                             pos_os.bytes_written, frame, sizeof(frame));
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

    /* The frame WAS framed and decoded fine — this isn't a proxy pass
     * where the packet never reached the path under test. */
    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.frames_ok);

    /* The bug: this used to land in the same decode_errors++ as real
     * corruption. It must not — nothing was malformed here. Mirrors the
     * NodeInfo replay path (mc_client.c's `ni->has_position &&
     * ni->position.has_latitude_i && ni->position.has_longitude_i` check,
     * no else branch), which emits nothing at all — no event, no counter
     * bumped — for the same missing-coordinates condition. */
    TEST_ASSERT_EQUAL_INT(0, cap.position_count);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.decode_errors);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.decode_skipped);
}

static void S03_debt_position_corrupt_protobuf_counts_decode_error(void)
{
    /* Genuinely malformed wire data: a varint field tag (field 1,
     * wiretype 0 -> latitude_i) with the stream ending before its value.
     * pb_decode() must fail outright — this is the ONE case
     * decode_errors is allowed to count. */
    uint8_t const garbage[] = {0x08};

    uint8_t frame[64];
    uint16_t flen = build_data_packet_frame(0x0A0A0A0Au, MC_ADDR_BROADCAST,
                                             (uint32_t)meshtastic_PortNum_POSITION_APP, garbage,
                                             sizeof(garbage), frame, sizeof(frame));
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

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.frames_ok); /* framed and decoded as FromRadio fine */
    TEST_ASSERT_EQUAL_INT(0, cap.position_count);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.decode_errors);
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
/* debt/S03-reboot-session-loss — NAME-in-Settings bench finding,        */
/* 2026-09-06, real puck + comms brain, Meshtastic 2.7.26.               */
/*                                                                       */
/* Root cause (see mc_client.h's mc_tick() doc comment for the full      */
/* mechanism): Meshtastic's AdminModule reboots the comms brain a few    */
/* seconds after a set_owner admin write. The PhoneAPI session on the    */
/* other side of that reboot is fresh and silently ignores this client's */
/* packets until a new want_config handshake — but mc_client's OWN 30s   */
/* no-RX-bytes watchdog never noticed, because other FromRadio traffic   */
/* (queueStatus) kept arriving right through the reboot. FromRadio.      */
/* rebooted (tag 8) is the explicit tell fixed here: an immediate        */
/* session loss, not a silence timeout.                                  */
/* -------------------------------------------------------------------- */

static void S03_debt_reboot_frame_after_ready_drops_state_and_reissues_want_config(void)
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
    c.has_my_node_id = true;
    c.my_node_id = 0x42u;
    uint32_t const old_want_config_id = 0xDEADBEEFu;
    c.want_config_id = old_want_config_id;

    uint8_t frame[32];
    uint16_t flen = build_rebooted_frame(frame, sizeof(frame));
    TEST_ASSERT_TRUE(flen > 0);
    io.rx_data = frame;
    io.rx_len = flen;

    mc_tick(&c, 5000);

    TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_HANDSHAKE, mc_state(&c),
                              "a rebooted frame must drop READY straight into a fresh handshake");
    TEST_ASSERT_EQUAL_INT(1, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(old_want_config_id, c.want_config_id,
                                  "a fresh handshake must pick a NEW nonce, never reuse the pre-reboot one");

    /* Same device, same session identity — a reboot doesn't change who we are. */
    TEST_ASSERT_TRUE(c.has_my_node_id);
    TEST_ASSERT_EQUAL_UINT32(0x42u, c.my_node_id);

    /* Not treated as a transport failure or a backoff reconnect: the wire
     * is fine, only the session on the other end is gone. */
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.reconnects);
    TEST_ASSERT_FALSE(c.reconnect_pending);

    /* A real want_config frame was actually re-issued onto the wire. */
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

/* "Frames before the new config_complete are ignored/handled per the
 * handshake rules": a config_complete naming the STALE, pre-reboot nonce
 * must not complete the NEW handshake — same rule
 * S03_AC2_handshake_wrong_nonce_stays_in_handshake already pins for an
 * ordinary connect, now proven across a reboot's nonce rotation too. */
static void S03_debt_reboot_stale_config_complete_is_ignored_per_handshake_rules(void)
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
    uint32_t const old_want_config_id = 0xABCDu;
    c.want_config_id = old_want_config_id;

    uint8_t frame1[32];
    uint16_t f1_len = build_rebooted_frame(frame1, sizeof(frame1));
    TEST_ASSERT_TRUE(f1_len > 0);

    uint8_t frame2[32];
    /* The PRE-reboot nonce — stale by the time this (synthetic) frame
     * arrives, since the reboot handling above already picked a fresh one. */
    uint16_t f2_len = build_config_complete_frame(old_want_config_id, frame2, sizeof(frame2));
    TEST_ASSERT_TRUE(f2_len > 0);

    uint8_t combined[64];
    TEST_ASSERT_TRUE((size_t)f1_len + f2_len <= sizeof(combined));
    memcpy(combined, frame1, f1_len);
    memcpy(combined + f1_len, frame2, f2_len);
    io.rx_data = combined;
    io.rx_len = (size_t)f1_len + f2_len;

    mc_tick(&c, 100);

    TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_HANDSHAKE, mc_state(&c),
                              "a config_complete naming the PRE-reboot nonce must not complete the NEW handshake");
    TEST_ASSERT_NOT_EQUAL(old_want_config_id, c.want_config_id);
    mc_stats_t const stats = mc_get_stats(&c);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.decode_skipped);
}

/* The full round trip: rebooted -> fresh handshake -> the NEW
 * config_complete (matching the nonce the reboot handling just picked)
 * reaches READY again, exactly like an ordinary cold connect. */
static void S03_debt_reboot_then_matching_config_complete_reaches_ready_again(void)
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
    c.want_config_id = 0x1111u;

    uint8_t reboot_frame[32];
    uint16_t rf_len = build_rebooted_frame(reboot_frame, sizeof(reboot_frame));
    TEST_ASSERT_TRUE(rf_len > 0);
    io.rx_data = reboot_frame;
    io.rx_len = rf_len;

    mc_tick(&c, 100); /* READY -> HANDSHAKE, fresh want_config_id */
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    uint32_t const new_want_config_id = c.want_config_id;

    uint8_t cc_frame[32];
    uint16_t cc_len = build_config_complete_frame(new_want_config_id, cc_frame, sizeof(cc_frame));
    TEST_ASSERT_TRUE(cc_len > 0);
    io.rx_data = cc_frame;
    io.rx_len = cc_len;
    io.rx_pos = 0;

    mc_tick(&c, 200); /* HANDSHAKE -> READY */

    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_EQUAL_INT(2, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);
    TEST_ASSERT_EQUAL(MC_STATE_READY, cap.states[1]);
}


/* -------------------------------------------------------------------- */
/* debt/S15c-handshake-stall — bench finding, 2026-09-13, real puck +    */
/* XIAO comms brain (Meshtastic 2.7.26) over UART1 GPIO43/44.           */
/*                                                                      */
/* Symptom: ~7 minutes after boot the link dropped once and then sat in */
/* RECONNECTING for 20+ minutes. Throughout, `diag` showed frames_ok    */
/* climbing steadily (84 -> 92 -> 138), decode_err 0, last_frame_ms     */
/* 0.4-11 s, reconnects STUCK at 1, and the heard list still gaining    */
/* new nodes — so FromRadio frames were arriving, framing, decoding and */
/* dispatching the whole time. Only `send`/`dm` never transmitted       */
/* ("queued"), because the shell gates sends on READY. A reset restored */
/* CONNECTED in 20 ms.                                                  */
/*                                                                      */
/* Root cause: mc_tick()'s only liveness watchdog was "no bytes read    */
/* for 30 s", and `last_rx_ms` is refreshed by ANY inbound byte in ANY  */
/* state. A handshake whose want_config went unanswered therefore kept  */
/* its own watchdog fed by the radio's unrelated traffic, and nothing   */
/* ever re-issued want_config — the client only sent one per handshake. */
/* Same blind spot FromRadio.rebooted handling was added for, reached   */
/* without a reboot to announce.                                        */
/*                                                                      */
/* These tests drive the client with a scripted FromRadio stream:       */
/* handshake OK -> drop -> frames RESUME without a config_complete, and */
/* pin that the client re-asks and converges to READY once the stream   */
/* answers. The load-bearing assertion in each is that `reconnects`     */
/* does NOT move during the stall: the recovery must come from the      */
/* handshake watchdog, not from the 30 s silence watchdog quietly       */
/* re-dialling (which the flowing traffic must keep suppressed — if it  */
/* fired, these tests would pass while the real bug stood).             */
/* -------------------------------------------------------------------- */

/* An ordinary nodeDB frame — the "frames are flowing" traffic. Chosen
 * because it is what the bench actually saw keeping the heard list
 * updating, and because mc_process_from_radio dispatches it regardless of
 * state, so it exercises the real path rather than a decode_skipped stub. */
static uint16_t build_nodeinfo_frame(uint32_t num, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = num;

    uint8_t payload[128];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
}

/* Walk every complete frame the client has written and count the
 * want_config ones, recording the last nonce seen. Counting on the WIRE
 * (rather than trusting mc_stats_t.handshake_retries, which the code
 * under test also maintains) is deliberate: a retry that bumps a counter
 * without actually re-asking the radio would fix nothing, and is exactly
 * the mutation a stats-only assertion would wave through. */
static uint32_t tx_want_config_count(mock_io_t const *io, uint32_t *out_last_nonce)
{
    uint32_t count = 0;
    size_t pos = 0;
    while (pos + 4u <= io->tx_len) {
        TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, io->tx_buf[pos]);
        TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, io->tx_buf[pos + 1u]);
        uint16_t const flen = (uint16_t)((io->tx_buf[pos + 2u] << 8) | io->tx_buf[pos + 3u]);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(io->tx_len - pos - 4u, (size_t)flen);

        meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
        pb_istream_t is = pb_istream_from_buffer(io->tx_buf + pos + 4u, flen);
        TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
        if (tr.which_payload_variant == meshtastic_ToRadio_want_config_id_tag) {
            count++;
            if (out_last_nonce != NULL) {
                *out_last_nonce = tr.payload_variant.want_config_id;
            }
        }
        pos += 4u + (size_t)flen;
    }
    return count;
}

/* Hand the client exactly one frame and tick it at `t_ms`. */
static void feed_frame(mc_client_t *c, mock_io_t *io, uint8_t const *frame, uint16_t len, uint32_t t_ms)
{
    io->rx_data = frame;
    io->rx_len = len;
    io->rx_pos = 0;
    mc_tick(c, t_ms);
}

/* THE REPRO. Handshake completes; the link drops; frames resume with no
 * config_complete in them. Before the fix this hung in HANDSHAKE for as
 * long as the traffic kept flowing (20+ minutes on the bench, unbounded
 * in principle). After it, the client re-asks on a deadline and reaches
 * READY the moment the stream answers. */
static void S03_debt_handshake_stall_reissues_want_config_and_reaches_ready(void)
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

    uint8_t node_frame[200];
    uint16_t const node_len = build_nodeinfo_frame(0x0D0D0D0Du, node_frame, sizeof(node_frame));
    TEST_ASSERT_TRUE(node_len > 0);

    /* --- 1. A healthy handshake, exactly as on a fresh boot. --- */
    mc_connect(&c);
    uint8_t cc[32];
    uint16_t cc_len = build_config_complete_frame(c.want_config_id, cc, sizeof(cc));
    TEST_ASSERT_TRUE(cc_len > 0);
    feed_frame(&c, &io, cc, cc_len, 100u);
    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32(1u, tx_want_config_count(&io, NULL));

    /* --- 2. The drop: the radio goes quiet long enough for the 30 s
     * silence watchdog, then the client re-dials (reconnects -> 1) and a
     * SECOND want_config goes out. This is the bench's `reconnects=1`. */
    io.rx_data = NULL;
    io.rx_len = 0;
    io.rx_pos = 0;
    mc_tick(&c, 30200u); /* -> DISCONNECTED, retry armed 2 s out */
    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));
    mc_tick(&c, 32300u); /* -> HANDSHAKE, want_config #2 */
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32(1u, mc_get_stats(&c).reconnects);
    TEST_ASSERT_EQUAL_UINT32(2u, tx_want_config_count(&io, NULL));

    uint32_t const frames_at_stall_start = mc_get_stats(&c).frames_ok;

    /* --- 3. Frames RESUME — but the handshake is never answered. One
     * NodeInfo per second for 37 s — the bench's "heard list still
     * updating" while `link=RECONNECTING`. The window deliberately stops
     * just short of the retry budget running out (which would escalate to
     * a second re-dial, a different path pinned by its own test below), so
     * what recovers the link here is unambiguously the handshake retry. --- */
    for (uint32_t t = 33000u; t <= 70000u; t += 1000u) {
        feed_frame(&c, &io, node_frame, node_len, t);
    }

    mc_stats_t const during = mc_get_stats(&c);

    /* The premise the whole test rests on: traffic really was flowing and
     * really was decoding, so the 30 s silence watchdog stayed asleep.
     * If `reconnects` had moved, recovery below would prove nothing about
     * the handshake watchdog — it would just be the old re-dial path. */
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(frames_at_stall_start + 30u, during.frames_ok,
                                             "the scripted stream must actually be framing");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, during.decode_errors, "the scripted stream must decode cleanly");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(1, cap.node_count, "NodeInfo must still dispatch mid-handshake");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, during.reconnects,
                                      "flowing traffic must keep the 30 s silence watchdog asleep — "
                                      "recovery has to come from the handshake watchdog, not a re-dial");

    /* The fix: want_config was actually RE-ASKED on the wire, bounded. */
    uint32_t last_nonce = 0u;
    uint32_t const want_configs = tx_want_config_count(&io, &last_nonce);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u + MC_HANDSHAKE_MAX_RETRIES, want_configs,
                                      "a stalled handshake must re-issue want_config, and stop at the budget");
    TEST_ASSERT_EQUAL_UINT32(MC_HANDSHAKE_MAX_RETRIES, during.handshake_retries);
    TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_HANDSHAKE, mc_state(&c),
                              "still handshaking — nothing has answered yet");

    /* --- 4. The stream finally answers the request that is actually
     * outstanding. The link must converge to READY. --- */
    cc_len = build_config_complete_frame(last_nonce, cc, sizeof(cc));
    TEST_ASSERT_TRUE(cc_len > 0);
    feed_frame(&c, &io, cc, cc_len, 71000u);

    TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_READY, mc_state(&c),
                              "a drop must converge back to READY while the radio is alive");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, mc_get_stats(&c).reconnects,
                                      "and it must get there without a single extra re-dial");
}

/* The retry deliberately reuses the CURRENT nonce rather than rotating to
 * a fresh one, so an answer to the ORIGINAL request — merely slow, not
 * lost — still completes the handshake instead of being discarded at the
 * nonce gate and costing another full timeout. This pins that choice:
 * the nonce the client asked with first is still the nonce it accepts
 * after a retry. */
static void S03_debt_handshake_retry_keeps_nonce_so_a_late_answer_still_lands(void)
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
    uint32_t const original_nonce = c.want_config_id;

    uint8_t node_frame[200];
    uint16_t const node_len = build_nodeinfo_frame(0x0E0E0E0Eu, node_frame, sizeof(node_frame));
    TEST_ASSERT_TRUE(node_len > 0);

    /* Traffic flows; no config_complete. Cross one timeout so exactly one
     * retry goes out. */
    for (uint32_t t = 1000u; t <= 11000u; t += 1000u) {
        feed_frame(&c, &io, node_frame, node_len, t);
    }
    TEST_ASSERT_EQUAL_UINT32(1u, mc_get_stats(&c).handshake_retries);

    uint32_t retry_nonce = 0u;
    TEST_ASSERT_EQUAL_UINT32(2u, tx_want_config_count(&io, &retry_nonce));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(original_nonce, retry_nonce,
                                      "a retry re-asks with the SAME nonce — rotating would throw away a "
                                      "slow answer to the original request");

    /* The original request's answer, arriving late. It must still land. */
    uint8_t cc[32];
    uint16_t const cc_len = build_config_complete_frame(original_nonce, cc, sizeof(cc));
    TEST_ASSERT_TRUE(cc_len > 0);
    feed_frame(&c, &io, cc, cc_len, 11500u);

    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, mc_get_stats(&c).reconnects,
                                      "a late answer is not a link failure");
}

/* Bounded, and still converging: a handshake nobody ever answers must
 * spend its retry budget and then escalate to the ordinary reconnect path
 * — a fresh nonce, a fresh budget — rather than retrying the same dead
 * session forever. The retry budget bounds one ATTEMPT; the reconnect
 * loop outside it is what never gives up. */
static void S03_debt_handshake_retry_budget_escalates_to_a_fresh_reconnect(void)
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
    uint32_t const first_nonce = c.want_config_id;

    uint8_t node_frame[200];
    uint16_t const node_len = build_nodeinfo_frame(0x0F0F0F0Fu, node_frame, sizeof(node_frame));
    TEST_ASSERT_TRUE(node_len > 0);

    /* Never answered, but never silent either — one frame per second past
     * the whole budget (4 x 10 s) and the 2 s reconnect backoff. */
    for (uint32_t t = 1000u; t <= 45000u; t += 1000u) {
        feed_frame(&c, &io, node_frame, node_len, t);
    }

    mc_stats_t const s = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(MC_HANDSHAKE_MAX_RETRIES, s.handshake_retries,
                                      "retries are bounded per handshake");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, s.reconnects,
                                      "a spent budget escalates to a full reconnect, so the link keeps trying");
    TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_HANDSHAKE, mc_state(&c), "and lands in a brand new handshake");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(first_nonce, c.want_config_id,
                                  "a full reconnect rotates the nonce — the old session is presumed gone");

    /* Budget reset with the new handshake: this one can retry again. */
    TEST_ASSERT_EQUAL_UINT32(0u, c.handshake_retry_count);

    /* And the new handshake completes normally when answered. */
    uint8_t cc[32];
    uint16_t const cc_len = build_config_complete_frame(c.want_config_id, cc, sizeof(cc));
    TEST_ASSERT_TRUE(cc_len > 0);
    feed_frame(&c, &io, cc, cc_len, 46000u);
    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
}

/* debt/link-churn-2026-09-16 (fix #2's bound): a handshake that NEVER
 * completes must not be able to inhibit light sleep forever. The new
 * sleep_inhibit source (app_main.c) is gated on `mc_state() ==
 * MC_STATE_HANDSHAKE` via `ff_shell_handshake_in_flight()` (ff_shell.h)
 * — this test proves, at the mc_client level that accessor reads
 * straight through to, that MC_STATE_HANDSHAKE is NEVER held
 * continuously: the existing S15c handshake-stall ladder (this file's
 * own S03_debt_handshake_retry_budget_escalates_to_a_fresh_reconnect,
 * immediately above) already forces a drop to MC_STATE_DISCONNECTED for
 * the ~2s reconnect backoff every time a handshake's retry budget is
 * spent — and this is not a one-off: it recurs every cycle, for as long
 * as the handshake keeps failing to complete. That recurring, guaranteed
 * release is the bound; this test exercises THREE full cycles (never
 * just one) to demonstrate it keeps recurring rather than being a fluke
 * of the first escalation. Cycle length is
 * `MC_HANDSHAKE_TIMEOUT_MS * (MC_HANDSHAKE_MAX_RETRIES + 1)` (40s: the
 * initial send plus 3 retries, each waiting the full timeout) plus the 2s
 * reconnect backoff = 42s; the DISCONNECTED window is the last 2s of
 * each 42s cycle. */
static void S03_debt_handshake_never_completing_does_not_inhibit_sleep_forever(void)
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

    uint8_t node_frame[200];
    uint16_t const node_len = build_nodeinfo_frame(0x1A1A1A1Au, node_frame, sizeof(node_frame));
    TEST_ASSERT_TRUE(node_len > 0);

    /* Traffic flows continuously (framer happy, frames_ok climbing) but
     * config_complete NEVER arrives — the exact "session handshake never
     * lands" shape MC_HANDSHAKE_TIMEOUT_MS exists for. Walk 3 full
     * escalation cycles (42s each = 126s), asserting the state at the
     * three moments that matter: mid-cycle (still legitimately
     * negotiating — sleep SHOULD be inhibited) and inside each cycle's
     * guaranteed 2s DISCONNECTED window (sleep must NOT be inhibited). */
    uint32_t const cycle_ms = 42000u;
    for (uint32_t cycle = 0; cycle < 3u; cycle++) {
        uint32_t const base = cycle * cycle_ms;

        for (uint32_t t = base + 1000u; t <= base + 39000u; t += 1000u) {
            feed_frame(&c, &io, node_frame, node_len, t);
        }
        TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_HANDSHAKE, mc_state(&c),
                                   "still legitimately negotiating mid-cycle — inhibit should hold here");

        /* t = base+40000: the retry budget is spent this tick, escalating
         * to mc_fail_and_schedule_reconnect() — state drops to
         * DISCONNECTED with a 2s reconnect backoff pending. */
        feed_frame(&c, &io, node_frame, node_len, base + 40000u);
        feed_frame(&c, &io, node_frame, node_len, base + 41000u);
        TEST_ASSERT_EQUAL_MESSAGE(MC_STATE_DISCONNECTED, mc_state(&c),
                                   "the guaranteed release window — sleep_inhibit MUST go false here, every "
                                   "cycle, or a never-completing handshake would be a permanent battery leak");

        /* t = base+42000: reconnect fires, a fresh handshake begins (new
         * nonce, fresh retry budget) — back to MC_STATE_HANDSHAKE for the
         * next cycle. */
        feed_frame(&c, &io, node_frame, node_len, base + 42000u);
        TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3u, mc_get_stats(&c).reconnects,
                                      "one reconnect per cycle, exactly as many cycles as walked — the "
                                      "release keeps recurring, it is not a one-time fluke of the first "
                                      "escalation");
}

/* The watchdog must not fire on a handshake that is simply BUSY. A
 * whole-mesh NodeInfo dump can take several ticks to drain
 * (MC_TICK_MAX_FRAMES); nothing about that is a stall, and a client that
 * re-asked mid-dump would restart the dump forever. */
static void S03_debt_handshake_answered_within_timeout_never_retries(void)
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

    uint8_t node_frame[200];
    uint16_t const node_len = build_nodeinfo_frame(0x11111111u, node_frame, sizeof(node_frame));
    TEST_ASSERT_TRUE(node_len > 0);

    /* A dump that runs right up to — but not past — the deadline. */
    for (uint32_t t = 500u; t < MC_HANDSHAKE_TIMEOUT_MS; t += 500u) {
        feed_frame(&c, &io, node_frame, node_len, t);
    }
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, mc_get_stats(&c).handshake_retries,
                                      "a busy handshake is not a stalled one");
    TEST_ASSERT_EQUAL_UINT32(1u, tx_want_config_count(&io, NULL));

    uint8_t cc[32];
    uint16_t const cc_len = build_config_complete_frame(c.want_config_id, cc, sizeof(cc));
    TEST_ASSERT_TRUE(cc_len > 0);
    feed_frame(&c, &io, cc, cc_len, MC_HANDSHAKE_TIMEOUT_MS - 100u);

    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32(0u, mc_get_stats(&c).handshake_retries);

    /* And READY is not subject to the handshake deadline at all: long
     * past it, with traffic flowing, nothing re-asks. */
    for (uint32_t t = MC_HANDSHAKE_TIMEOUT_MS; t <= 3u * MC_HANDSHAKE_TIMEOUT_MS; t += 1000u) {
        feed_frame(&c, &io, node_frame, node_len, t);
    }
    TEST_ASSERT_EQUAL(MC_STATE_READY, mc_state(&c));
    TEST_ASSERT_EQUAL_UINT32(0u, mc_get_stats(&c).handshake_retries);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, tx_want_config_count(&io, NULL),
                                      "a READY link must never re-issue want_config");
}

/* -------------------------------------------------------------------- */
/* debt/meshclient-contracts — write() backpressure contract             */
/*                                                                       */
/* mc_write_bytes() retries a write() returning 0 ("try again later",   */
/* NOT an error — see the mc_transport_t.write() contract in            */
/* mc_client.h) up to MC_WRITE_ZERO_RETRY_BUDGET (64, mc_client.c)      */
/* times before giving up. These tests drive mc_connect(), which sends  */
/* the want_config frame through exactly this path, against a fake      */
/* transport whose write() returns 0 for a controlled number of calls   */
/* before accepting — the shape the S15 UART transport is expected to   */
/* produce for a momentarily-full TX ring.                              */
/* -------------------------------------------------------------------- */

static void S03_debt_write_backpressure_below_budget_sends_frame_no_reconnect(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    /* One call short of the budget: the 64th write() call is the first
     * that actually accepts. Still within budget, so mc_write_bytes()
     * must retry through all 63 "try again later" calls and succeed. */
    io.write_zero_countdown = 63;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c);

    /* Frame sent: still in HANDSHAKE (mc_begin_handshake never fell back
     * to mc_fail_and_schedule_reconnect), and the want_config frame
     * landed byte-for-byte, same as the ordinary AC2 connect test. */
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, mc_state(&c));
    TEST_ASSERT_EQUAL_INT(1, cap.state_count);
    TEST_ASSERT_EQUAL(MC_STATE_HANDSHAKE, cap.states[0]);
    TEST_ASSERT_FALSE(c.reconnect_pending);

    TEST_ASSERT_TRUE(io.tx_len >= 4);
    TEST_ASSERT_EQUAL_UINT8(0x94, io.tx_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xC3, io.tx_buf[1]);
    uint16_t plen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, plen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL(meshtastic_ToRadio_want_config_id_tag, tr.which_payload_variant);
    TEST_ASSERT_EQUAL_UINT32(c.want_config_id, tr.payload_variant.want_config_id);

    /* Stats untouched by the retry itself — no reconnect was scheduled or
     * counted, this was ordinary (if belated) success. */
    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(0, stats.reconnects);
    TEST_ASSERT_EQUAL_UINT32(64, io.write_calls); /* 63 zero + 1 accepting */
}

static void S03_debt_write_backpressure_budget_exhausted_triggers_reconnect(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    /* Exactly at budget: the 64th zero-return trips the "budget
     * exhausted" branch before ever attempting a 65th call. A
     * permanently-stuck transport (this one would keep returning 0
     * forever, per write_zero_countdown, but the retry loop must not
     * find that out empirically) must fail in a bounded number of calls. */
    io.write_zero_countdown = 64;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c);

    /* mc_begin_handshake set HANDSHAKE first, then the exhausted retry
     * budget fell through to mc_fail_and_schedule_reconnect() — the
     * documented failure path, same as any other send failure. */
    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));
    TEST_ASSERT_TRUE(c.reconnect_pending);
    TEST_ASSERT_EQUAL_UINT32(0, io.tx_len); /* nothing was ever accepted */
    TEST_ASSERT_EQUAL_UINT32(64, io.write_calls); /* budget spent, not exceeded */

    /* stats.reconnects itself only increments when mc_tick's scheduled
     * backoff actually redials (see S03_AC6) — this path only *schedules*
     * one, so it stays 0 here; the scheduling is what reconnect_pending
     * above already pins. */
    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(0, stats.reconnects);
}

static void S03_debt_write_backpressure_transport_stuck_forever_fails_at_exactly_budget(void)
{
    /* Unlike the budget-exhausted test above (which uses a countdown that
     * happens to line up with the budget), this transport NEVER recovers
     * — write() returns 0 on every single call, with no upper bound on
     * how long it would keep doing so if the retry loop kept calling it.
     * The only thing that can stop this loop is the budget itself, and
     * the literal call count pins exactly where it stops. */
    mock_io_t io;
    mock_io_reset(&io);
    io.write_always_zero = true;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c);

    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));
    TEST_ASSERT_TRUE(c.reconnect_pending);
    TEST_ASSERT_EQUAL_UINT32(0, io.tx_len);
    /* The literal budget, pinned: exactly 64 calls, every one of them
     * returning 0 forever — the loop had no way to know that in advance,
     * only the call-count budget bounds it. */
    TEST_ASSERT_EQUAL_UINT32(64, io.write_calls);
}

static void S03_debt_write_negative_return_fails_immediately(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    io.tx_fail = true; /* write() returns -1: hard transport failure */

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap),
             &clock);

    mc_connect(&c);

    TEST_ASSERT_EQUAL(MC_STATE_DISCONNECTED, mc_state(&c));
    TEST_ASSERT_TRUE(c.reconnect_pending);
    /* Negative is a hard failure, never retried: exactly one write() call,
     * not the full backpressure budget. */
    TEST_ASSERT_EQUAL_UINT32(1, io.write_calls);
}

/* -------------------------------------------------------------------- */
/* debt/meshclient-contracts — bounded mc_tick() drain                   */
/*                                                                       */
/* Feeds more complete frames than MC_TICK_MAX_FRAMES in one shot and    */
/* checks that a single mc_tick() call decodes exactly the cap, the      */
/* transport keeps the remainder buffered (never lost), and a second     */
/* call finishes the drain with ordering preserved throughout.           */
/* -------------------------------------------------------------------- */

typedef struct {
    uint16_t seen[64]; /* sequence index encoded in each frame's payload */
    int count;
} drain_capture_t;

static void drain_on_private(void *u, uint32_t from, uint32_t to, uint32_t portnum,
                              uint8_t const *payload, size_t len)
{
    (void)from;
    (void)to;
    (void)portnum;
    drain_capture_t *d = (drain_capture_t *)u;
    if (d->count < (int)(sizeof(d->seen) / sizeof(d->seen[0])) && len >= 2) {
        d->seen[d->count] = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    }
    d->count++;
}

static void S03_debt_mc_tick_bounded_drain_caps_frames_per_call(void)
{
    /* 32 + 8 = 40: the cap (32) then the remainder (8), pinned exactly —
     * matches the review's own burst shape (a 40-frame burst). */
    uint32_t const total_frames = MC_TICK_MAX_FRAMES + 8u;
    TEST_ASSERT_TRUE(total_frames <= 64u); /* fits drain_capture_t.seen */

    static uint8_t frames[8192];
    size_t pos = 0;
    for (uint32_t i = 0; i < total_frames; i++) {
        uint8_t payload[2] = {(uint8_t)(i & 0xFFu), (uint8_t)((i >> 8) & 0xFFu)};
        uint16_t flen = build_data_packet_frame(0x0A0A0A0Au, MC_ADDR_BROADCAST, 300u /* private */,
                                                 payload, sizeof(payload), frames + pos,
                                                 sizeof(frames) - pos);
        TEST_ASSERT_TRUE(flen > 0);
        pos += flen;
    }

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frames;
    io.rx_len = pos;

    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};

    drain_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    mc_events_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.on_private = drain_on_private;
    ev.user = &cap;

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, ev, &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    /* Exactly the cap, no more, in one call — and the transport still has
     * the rest buffered (nothing lost, nothing skipped). */
    TEST_ASSERT_EQUAL_INT((int)MC_TICK_MAX_FRAMES, cap.count);
    TEST_ASSERT_TRUE(io.rx_pos < io.rx_len);
    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(MC_TICK_MAX_FRAMES, stats.frames_ok);
    for (uint32_t i = 0; i < MC_TICK_MAX_FRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)i, cap.seen[i]);
    }

    mc_tick(&c, 6);

    /* The remainder (8 frames) arrives on the next call, ordering
     * preserved throughout, and the transport is now fully drained. */
    TEST_ASSERT_EQUAL_INT((int)total_frames, cap.count);
    TEST_ASSERT_EQUAL_UINT(io.rx_len, io.rx_pos);
    stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(total_frames, stats.frames_ok);
    for (uint32_t i = 0; i < total_frames; i++) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)i, cap.seen[i]);
    }

    /* Read-call-count regression pin (review finding on #170): a
     * chunked-read implementation reads `pos` total bytes in
     * MC_TICK_READ_CHUNK-sized chunks, plus exactly one final zero-length
     * read to learn the transport is empty — regardless of how many
     * mc_tick() calls that spans, because the per-tick frame cap only
     * ever stops mid-CHUNK (carried over, never re-read), never causes a
     * chunk to be read twice. So the total read() calls across both
     * mc_tick() calls above is EXACTLY ceil(pos / MC_TICK_READ_CHUNK) + 1
     * — an equality, not just the review's "<=" bound (which a byte-at-a-
     * time implementation would blow past: that regression measured 4161
     * calls for a similarly-sized burst, one read() per byte). */
    /* Literal pin (review finding on #170's regression numbers): this
     * fixture is 40 frames / 1000 bytes total. A chunked-read
     * implementation needs ceil(1000/64) = 16 chunk reads to cover every
     * byte, plus exactly one final zero-length read to learn the
     * transport is empty = 17 read() calls, however many mc_tick() calls
     * that spans — the frame cap only ever pauses mid-chunk (carried
     * over, never re-read), it never causes a chunk to be read twice or
     * adds an extra transport call. Both the formula and the literal are
     * asserted so a change to the fixture size (which would legitimately
     * move the literal) still has the formula catch a REAL regression. */
    TEST_ASSERT_EQUAL_UINT(1000u, pos);
    uint32_t const expected_reads = (uint32_t)((pos + MC_TICK_READ_CHUNK - 1u) / MC_TICK_READ_CHUNK) + 1u;
    TEST_ASSERT_EQUAL_UINT32(17u, expected_reads);
    TEST_ASSERT_EQUAL_UINT32(17u, io.read_calls);
    TEST_ASSERT_TRUE(io.read_calls <= expected_reads); /* the review's own <= bound, restated */
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

    /* mc_tick() now caps frames decoded per call at MC_TICK_MAX_FRAMES
     * (bounded-drain fix), so a single call no longer necessarily drains
     * the whole 10 KB buffer — call it in a loop until the mock transport
     * is exhausted. The guard bound is generous purely so a real
     * regression (mc_tick stuck, never draining) hangs the test instead
     * of looping forever. */
    for (int guard = 0; guard < 10000 && io.rx_pos < io.rx_len; guard++) {
        mc_tick(&c, (uint32_t)(1 + guard));
    }

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
/* NAME in Settings — mc_send_set_owner (AdminMessage.set_owner)        */
/* -------------------------------------------------------------------- */

#include "meshtastic/admin.pb.h"

/* Decode one outbound ToRadio frame out of `io->tx_buf`, assert it carries
 * a MeshPacket on ADMIN_APP addressed to `expect_dest` with `want_ack`
 * true, decode ITS payload as an AdminMessage, and hand back the
 * set_owner User it carries — the same "decode the actual wire bytes a
 * real radio would receive", not just "the call returned 0", discipline
 * `decode_tx_want_ack` above already established for mc_send_private. */
static meshtastic_User decode_tx_set_owner(mock_io_t const *io, uint32_t expect_dest)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, io->tx_len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, io->tx_buf[0]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, io->tx_buf[1]);
    uint16_t flen = (uint16_t)((io->tx_buf[2] << 8) | io->tx_buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(io->tx_len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(io->tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    meshtastic_MeshPacket const *pkt = &tr.payload_variant.packet;
    TEST_ASSERT_EQUAL_UINT32(expect_dest, pkt->to);
    TEST_ASSERT_TRUE(pkt->want_ack);
    TEST_ASSERT_EQUAL_INT(meshtastic_MeshPacket_decoded_tag, pkt->which_payload_variant);
    TEST_ASSERT_EQUAL_INT((int)meshtastic_PortNum_ADMIN_APP, (int)pkt->payload_variant.decoded.portnum);
    /* Confirmation-fix round 2: want_response is a get_owner_request-only
     * concern (AdminModule::handleGetOwner gates ITS reply on it) — a
     * set_owner WRITE getting it set too would be harmless on a real
     * AdminModule (handleSetOwner never checks it) but is not something
     * this library does, so pin it false here rather than leave it
     * unasserted. */
    TEST_ASSERT_FALSE_MESSAGE(pkt->payload_variant.decoded.want_response,
                              "set_owner is a write, not a get_*_request — want_response is not this call's concern");

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    pb_istream_t admin_is =
        pb_istream_from_buffer(pkt->payload_variant.decoded.payload.bytes, pkt->payload_variant.decoded.payload.size);
    TEST_ASSERT_TRUE(pb_decode(&admin_is, meshtastic_AdminMessage_fields, &admin));
    TEST_ASSERT_EQUAL_INT(meshtastic_AdminMessage_set_owner_tag, admin.which_payload_variant);
    return admin.payload_variant.set_owner;
}

static void feat_set_owner_encodes_long_and_short_name(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint32_t packet_id = 0xDEADBEEFu; /* poisoned — must be overwritten on success */
    int rc = mc_send_set_owner(&c, 0x0A0A0A0Au, "Jake", "JAKE", &packet_id);

    TEST_ASSERT_EQUAL_INT(0, rc);
    meshtastic_User const owner = decode_tx_set_owner(&io, 0x0A0A0A0Au);
    TEST_ASSERT_EQUAL_STRING("Jake", owner.long_name);
    TEST_ASSERT_EQUAL_STRING("JAKE", owner.short_name);
    /* Confirmation-fix follow-up: out_packet_id receives the SAME id the
     * outgoing MeshPacket actually carried (mc_init's default seed is 1
     * and nothing else has sent yet), not left at its poisoned value. */
    TEST_ASSERT_EQUAL_UINT32(1u, packet_id);
}

static void feat_set_owner_out_packet_id_is_optional(void)
{
    /* NULL out_packet_id must not crash — every pre-existing caller in
     * the tree before this follow-up passed none. */
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 1u, "Jake", "JAKE", NULL));
}

static void feat_set_owner_dest_is_whatever_the_caller_passes(void)
{
    /* mc_send_set_owner does not itself assert dest == self — that
     * policy (the "local admin, no key" path only works for a message
     * addressed to this node's OWN id) is documented as the CALLER's
     * job (mc_client.h's own doc comment); this test pins that this
     * library forwards `dest` verbatim rather than silently rewriting
     * it, using an arbitrary node id `has_my_node_id` was never set to. */
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 0x12345678u, "Taylor", "TAYL", NULL));
    (void)decode_tx_set_owner(&io, 0x12345678u); /* asserts dest == 0x12345678 internally */
}

static void feat_set_owner_null_short_name_leaves_it_unset(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 1u, "Jo", NULL, NULL));
    meshtastic_User const owner = decode_tx_set_owner(&io, 1u);
    TEST_ASSERT_EQUAL_STRING("Jo", owner.long_name);
    TEST_ASSERT_EQUAL_STRING("", owner.short_name);
}

static void feat_set_owner_uses_the_seeded_packet_id_counter(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    mc_seed_packet_ids(&c, 777u);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 1u, "Jake", "JAKE", NULL));

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    uint16_t flen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_UINT32(777u, tr.payload_variant.packet.id);
}

static void feat_set_owner_fails_when_not_ready(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    /* c.state left at mc_init's default (MC_STATE_DISCONNECTED). */

    uint32_t packet_id = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_INT(-1, mc_send_set_owner(&c, 1u, "Jake", "JAKE", &packet_id));
    TEST_ASSERT_EQUAL_UINT32(0u, io.tx_len); /* nothing written to the wire */
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, packet_id); /* untouched on failure */
}

/* -------------------------------------------------------------------- */
/* Confirmation-fix follow-up (bench finding, 2026-09-06):              */
/* mc_send_get_owner_request, get_owner_response -> on_owner, and       */
/* ROUTING_APP replies -> on_routing_ack.                                */
/* -------------------------------------------------------------------- */

static void feat_get_owner_request_encodes_the_request(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    TEST_ASSERT_EQUAL_INT(0, mc_send_get_owner_request(&c, 0x0A0A0A0Au));

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    uint16_t flen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    meshtastic_MeshPacket const *pkt = &tr.payload_variant.packet;
    TEST_ASSERT_EQUAL_UINT32(0x0A0A0A0Au, pkt->to);
    TEST_ASSERT_FALSE_MESSAGE(pkt->want_ack, "a read request is best-effort — the response IS the confirmation");
    /* Confirmation-fix round 2 (bench finding, 2026-09-06): a real
     * AdminModule (meshtastic/firmware v2.7.26.54e0d8d0,
     * AdminModule::handleGetOwner) only builds a get_owner_response
     * `if (req.decoded.want_response)` — this bit was never set before
     * this fix, so a real node never replied at all. Decoded straight
     * back off the wire here, not asserted against a mocked struct. */
    TEST_ASSERT_TRUE_MESSAGE(pkt->payload_variant.decoded.want_response,
                             "a real AdminModule only answers get_owner_request when want_response is set "
                             "(AdminModule::handleGetOwner, meshtastic/firmware v2.7.26.54e0d8d0)");
    TEST_ASSERT_EQUAL_INT((int)meshtastic_PortNum_ADMIN_APP, (int)pkt->payload_variant.decoded.portnum);

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    pb_istream_t admin_is =
        pb_istream_from_buffer(pkt->payload_variant.decoded.payload.bytes, pkt->payload_variant.decoded.payload.size);
    TEST_ASSERT_TRUE(pb_decode(&admin_is, meshtastic_AdminMessage_fields, &admin));
    TEST_ASSERT_EQUAL_INT(meshtastic_AdminMessage_get_owner_request_tag, admin.which_payload_variant);
    TEST_ASSERT_TRUE(admin.payload_variant.get_owner_request);
}

static void feat_get_owner_request_fails_when_not_ready(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);

    TEST_ASSERT_EQUAL_INT(-1, mc_send_get_owner_request(&c, 1u));
    TEST_ASSERT_EQUAL_UINT32(0u, io.tx_len);
}

/* -------------------------------------------------------------------- */
/* Bench finding 2026-09-14 — mc_send_nodeinfo_request, and a LIVE       */
/* NODEINFO_APP MeshPacket -> on_nodeinfo_reply (as opposed to on_node's */
/* want_config replay).                                                  */
/* -------------------------------------------------------------------- */

static void feat_nodeinfo_request_encodes_want_response_on_nodeinfo_app(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint32_t packet_id = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_INT(0, mc_send_nodeinfo_request(&c, 0x0B0B0B0Bu, &packet_id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0xDEADBEEFu, packet_id); /* overwritten on success */

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    uint16_t flen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    meshtastic_MeshPacket const *pkt = &tr.payload_variant.packet;
    TEST_ASSERT_EQUAL_UINT32(0x0B0B0B0Bu, pkt->to);
    TEST_ASSERT_EQUAL_UINT32(packet_id, pkt->id);
    TEST_ASSERT_FALSE_MESSAGE(pkt->want_ack,
                              "the value here is the reply's User payload, not a routing receipt");
    TEST_ASSERT_EQUAL_INT((int)meshtastic_PortNum_NODEINFO_APP, (int)pkt->payload_variant.decoded.portnum);
    /* The bit NodeInfoModule::allocReply (via MeshModule's generic reply
     * mechanism) requires to answer at all — see mc_send_nodeinfo_
     * request's own doc comment (mc_client.h) for the v2.7.26.54e0d8d
     * citation. */
    TEST_ASSERT_TRUE_MESSAGE(pkt->payload_variant.decoded.want_response,
                             "NodeInfoModule only replies to a NODEINFO_APP packet whose want_response bit is set");

    /* The payload is a meshtastic_User. Nothing has told this client its
     * own owner names yet, so every field is unset — which proto3
     * encodes as ZERO bytes ("we have said nothing"), never as a claim
     * that our name is the empty string. */
    meshtastic_User user = meshtastic_User_init_zero;
    pb_istream_t uis = pb_istream_from_buffer(pkt->payload_variant.decoded.payload.bytes,
                                              pkt->payload_variant.decoded.payload.size);
    TEST_ASSERT_TRUE(pb_decode(&uis, meshtastic_User_fields, &user));
    TEST_ASSERT_EQUAL_STRING("", user.long_name);
    TEST_ASSERT_EQUAL_STRING("", user.short_name);
    TEST_ASSERT_EQUAL_UINT32(0u, pkt->payload_variant.decoded.payload.size);
}

/* A want_config nodeDB REPLAY frame carrying names (build_nodeinfo_frame
 * above is the nameless "traffic is flowing" variant) — the only path
 * this library learns its OWN owner names from. */
static uint16_t build_named_nodeinfo_replay_frame(uint32_t num, char const *long_name, char const *short_name,
                                                  uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = num;
    fr.payload_variant.node_info.has_user = true;
    snprintf(fr.payload_variant.node_info.user.long_name, sizeof(fr.payload_variant.node_info.user.long_name), "%s",
             long_name);
    snprintf(fr.payload_variant.node_info.user.short_name, sizeof(fr.payload_variant.node_info.user.short_name), "%s",
             short_name);

    uint8_t payload[160];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) {
        return 0;
    }
    return mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
}

/* The payload a real peer writes STRAIGHT INTO ITS NODEDB
 * (NodeInfoModule::handleReceivedProtobuf -> NodeDB::updateUser). An
 * empty User there is not a payload-free ask: it is a claim that this
 * node has no name, and a peer holding no public key for us stores it —
 * blanking the record this whole feature exists to fill in. So the
 * request must carry what the RADIO said our owner is. */
static void feat_nodeinfo_request_carries_our_own_user(void)
{
    uint8_t frame[300];
    uint16_t flen = build_named_nodeinfo_replay_frame(0x1234u, "Jake's Puck", "JKP", frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    /* The replay names our OWN node — the one path this library learns
     * its owner names from (there is no other honest source). */
    c.my_node_id = 0x1234u;
    c.has_my_node_id = true;

    mc_tick(&c, 5);
    TEST_ASSERT_EQUAL_INT(1, cap.node_count);

    io.tx_len = 0; /* only the request itself is under the microscope */
    TEST_ASSERT_EQUAL_INT(0, mc_send_nodeinfo_request(&c, 0x0B0B0B0Bu, NULL));

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    uint16_t txlen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, txlen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));

    meshtastic_Data const *d = &tr.payload_variant.packet.payload_variant.decoded;
    TEST_ASSERT_EQUAL_INT((int)meshtastic_PortNum_NODEINFO_APP, (int)d->portnum);
    TEST_ASSERT_TRUE(d->want_response);

    meshtastic_User user = meshtastic_User_init_zero;
    pb_istream_t uis = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
    TEST_ASSERT_TRUE(pb_decode(&uis, meshtastic_User_fields, &user));
    TEST_ASSERT_EQUAL_STRING("Jake's Puck", user.long_name);
    TEST_ASSERT_EQUAL_STRING("JKP", user.short_name);
}

/* A replay for SOMEBODY ELSE never becomes our own identity — the
 * mutation this guards is dropping the `ni->num == my_node_id` test. */
static void feat_nodeinfo_request_never_borrows_another_nodes_name(void)
{
    uint8_t frame[300];
    uint16_t flen = build_named_nodeinfo_replay_frame(0x9999u, "Somebody Else", "SBE", frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    c.my_node_id = 0x1234u;
    c.has_my_node_id = true;

    mc_tick(&c, 5);
    TEST_ASSERT_EQUAL_INT(1, cap.node_count);

    io.tx_len = 0; /* only the request itself is under the microscope */
    TEST_ASSERT_EQUAL_INT(0, mc_send_nodeinfo_request(&c, 0x0B0B0B0Bu, NULL));

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    uint16_t txlen = (uint16_t)((io.tx_buf[2] << 8) | io.tx_buf[3]);
    pb_istream_t is = pb_istream_from_buffer(io.tx_buf + 4, txlen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));

    meshtastic_Data const *d = &tr.payload_variant.packet.payload_variant.decoded;
    meshtastic_User user = meshtastic_User_init_zero;
    pb_istream_t uis = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
    TEST_ASSERT_TRUE(pb_decode(&uis, meshtastic_User_fields, &user));
    TEST_ASSERT_EQUAL_STRING("", user.long_name);
    TEST_ASSERT_EQUAL_STRING("", user.short_name);
}

static void feat_nodeinfo_request_fails_when_not_ready(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);

    uint32_t packet_id = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_INT(-1, mc_send_nodeinfo_request(&c, 1u, &packet_id));
    TEST_ASSERT_EQUAL_UINT32(0u, io.tx_len);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, packet_id); /* untouched on failure */
}

/* Builds an inbound live NODEINFO_APP MeshPacket carrying a meshtastic_User
 * — the shape a real reply (or an unsolicited re-announcement) arrives
 * as, as opposed to build_nodeinfo_frame's want_config FromRadio.node_info
 * replay shape (below). */
static uint16_t build_live_nodeinfo_frame(uint32_t from, char const *long_name, char const *short_name,
                                          uint8_t *out, size_t out_cap)
{
    meshtastic_User user = meshtastic_User_init_zero;
    if (long_name != NULL) snprintf(user.long_name, sizeof(user.long_name), "%s", long_name);
    if (short_name != NULL) snprintf(user.short_name, sizeof(user.short_name), "%s", short_name);

    uint8_t payload[128];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_User_fields, &user));

    return build_data_packet_frame(from, MC_ADDR_BROADCAST, (uint32_t)meshtastic_PortNum_NODEINFO_APP, payload,
                                    os.bytes_written, out, out_cap);
}

static void feat_live_nodeinfo_app_decodes_to_on_nodeinfo_reply(void)
{
    uint8_t frame[300];
    uint16_t flen = build_live_nodeinfo_frame(0x0C0C0C0Cu, "Stranger Danger", "STR", frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.decode_errors);

    /* Fires the NEW event, never the replay one — a live packet must
     * never be mistaken for a want_config nodeDB dump. */
    TEST_ASSERT_EQUAL_INT(0, cap.node_count);
    TEST_ASSERT_EQUAL_INT(1, cap.nodeinfo_reply_count);
    TEST_ASSERT_EQUAL_UINT32(0x0C0C0C0Cu, cap.nodeinfo_replies[0].from);
    TEST_ASSERT_TRUE(cap.nodeinfo_replies[0].user.has_long_name);
    TEST_ASSERT_EQUAL_STRING("Stranger Danger", cap.nodeinfo_replies[0].user.long_name);
    TEST_ASSERT_TRUE(cap.nodeinfo_replies[0].user.has_short_name);
    TEST_ASSERT_EQUAL_STRING("STR", cap.nodeinfo_replies[0].user.short_name);
}

static void feat_live_nodeinfo_app_with_no_name_still_fires_with_both_flags_false(void)
{
    /* A well-formed User that states neither name (proto3 implicit
     * presence: "" and "never set" are the same bytes) — a legitimate,
     * if unusual, reply. Mirrors the Position "well-formed, nothing to
     * report" precedent EXCEPT that this event still fires (see its own
     * doc comment in mc_client.h for why: a caller's own "did I get an
     * answer at all" bookkeeping needs to see it). */
    uint8_t frame[300];
    uint16_t flen = build_live_nodeinfo_frame(0x0D0D0D0Du, NULL, NULL, frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.decode_errors);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.decode_skipped);
    TEST_ASSERT_EQUAL_INT(1, cap.nodeinfo_reply_count);
    TEST_ASSERT_FALSE(cap.nodeinfo_replies[0].user.has_long_name);
    TEST_ASSERT_FALSE(cap.nodeinfo_replies[0].user.has_short_name);
}

static void feat_live_nodeinfo_app_corrupt_protobuf_counts_decode_error(void)
{
    /* Genuinely malformed wire data — same technique
     * S03_debt_position_corrupt_protobuf_counts_decode_error uses: a
     * varint field tag whose value the stream ends before. */
    uint8_t const garbage[] = {0x08}; /* field 1, varint wiretype, no value byte follows */
    uint8_t frame[300];
    uint16_t flen = build_data_packet_frame(0x0E0E0E0Eu, MC_ADDR_BROADCAST,
                                             (uint32_t)meshtastic_PortNum_NODEINFO_APP, garbage, sizeof(garbage),
                                             frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    mc_tick(&c, 5);

    mc_stats_t stats = mc_get_stats(&c);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.decode_errors);
    TEST_ASSERT_EQUAL_INT(0, cap.nodeinfo_reply_count);
}

/* Builds an inbound FromRadio.packet frame on `portnum` carrying `request_id`
 * plus arbitrary payload bytes — the same shape build_data_packet_frame
 * (above) already establishes, extended with request_id since neither
 * ADMIN_APP's get_owner_response nor ROUTING_APP's ack needs `from`/`to`
 * populated for this library to dispatch them. */
static uint16_t build_admin_or_routing_frame(uint32_t portnum, uint32_t request_id, uint8_t const *payload,
                                              size_t len, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.payload_variant.packet.payload_variant.decoded.portnum = (meshtastic_PortNum)portnum;
    fr.payload_variant.packet.payload_variant.decoded.request_id = request_id;
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

static uint16_t build_owner_response_frame(char const *long_name, char const *short_name, uint8_t *out,
                                            size_t out_cap)
{
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    meshtastic_User *owner = &admin.payload_variant.get_owner_response;
    if (long_name != NULL) {
        snprintf(owner->long_name, sizeof(owner->long_name), "%s", long_name);
    }
    if (short_name != NULL) {
        snprintf(owner->short_name, sizeof(owner->short_name), "%s", short_name);
    }

    uint8_t admin_buf[128];
    pb_ostream_t os = pb_ostream_from_buffer(admin_buf, sizeof(admin_buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_AdminMessage_fields, &admin));

    return build_admin_or_routing_frame((uint32_t)meshtastic_PortNum_ADMIN_APP, 0u, admin_buf, os.bytes_written,
                                         out, out_cap);
}

static void feat_get_owner_response_fires_on_owner(void)
{
    uint8_t frame[400];
    uint16_t flen = build_owner_response_frame("Jake", "JAKE", frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.owner_count);
    TEST_ASSERT_EQUAL_STRING("Jake", cap.owners[0].long_name);
    TEST_ASSERT_EQUAL_STRING("JAKE", cap.owners[0].short_name);
    /* Not counted anywhere — a recognized, well-formed AdminMessage. */
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.decode_errors);
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.decode_skipped);
}

/* A get_owner_response with an unset short_name (proto3 implicit
 * presence) must still fire on_owner, with short_name reported as "" —
 * never NULL, never fabricated from the long name. */
static void feat_get_owner_response_unset_short_name_reports_empty(void)
{
    uint8_t frame[400];
    uint16_t flen = build_owner_response_frame("Jo", NULL, frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.owner_count);
    TEST_ASSERT_EQUAL_STRING("Jo", cap.owners[0].long_name);
    TEST_ASSERT_EQUAL_STRING("", cap.owners[0].short_name);
}

/* Every OTHER AdminMessage variant (this device asks for nothing else
 * yet) decodes cleanly and fires nothing — not a decode error, mirroring
 * the Position "well-formed, nothing to report" precedent. */
static void feat_admin_other_variant_is_silently_ignored(void)
{
    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    admin.which_payload_variant = meshtastic_AdminMessage_get_config_request_tag;
    admin.payload_variant.get_config_request = meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG;

    uint8_t admin_buf[128];
    pb_ostream_t os = pb_ostream_from_buffer(admin_buf, sizeof(admin_buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_AdminMessage_fields, &admin));

    uint8_t frame[400];
    uint16_t flen = build_admin_or_routing_frame((uint32_t)meshtastic_PortNum_ADMIN_APP, 0u, admin_buf,
                                                  os.bytes_written, frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(0, cap.owner_count);
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.decode_errors);
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.decode_skipped);
}

static uint16_t build_routing_ack_frame(uint32_t request_id, bool nak, uint8_t *out, size_t out_cap)
{
    meshtastic_Routing routing = meshtastic_Routing_init_zero;
    if (nak) {
        routing.which_variant = meshtastic_Routing_error_reason_tag;
        routing.variant.error_reason = meshtastic_Routing_Error_NO_RESPONSE;
    }
    /* else: which_variant left at its zero-init default — no error_reason
     * present at all, matching real Meshtastic's plain-ACK wire shape
     * (see mc_client.c's mc_process_mesh_packet ROUTING_APP comment). */

    uint8_t routing_buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(routing_buf, sizeof(routing_buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_Routing_fields, &routing));

    return build_admin_or_routing_frame((uint32_t)meshtastic_PortNum_ROUTING_APP, request_id, routing_buf,
                                         os.bytes_written, out, out_cap);
}

static void feat_routing_ack_none_reports_ok(void)
{
    uint8_t frame[400];
    uint16_t flen = build_routing_ack_frame(42u, /*nak=*/false, frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.routing_ack_count);
    TEST_ASSERT_EQUAL_UINT32(42u, cap.routing_acks[0].request_id);
    TEST_ASSERT_TRUE(cap.routing_acks[0].ok);
}

static void feat_routing_nak_reports_not_ok(void)
{
    uint8_t frame[400];
    uint16_t flen = build_routing_ack_frame(42u, /*nak=*/true, frame, sizeof(frame));
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
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 5);

    TEST_ASSERT_EQUAL_INT(1, cap.routing_ack_count);
    TEST_ASSERT_EQUAL_UINT32(42u, cap.routing_acks[0].request_id);
    TEST_ASSERT_FALSE(cap.routing_acks[0].ok);
}

/* Outbox delivery status feature (2026-09-07) — a routing ACK for a
 * direct text's own packet id reaches on_routing_ack exactly like the
 * pre-existing set_owner case (this is mc_client's existing dispatch,
 * unchanged by this feature); the new fact this feature adds is only
 * that mc_send_text now HANDS OUT a packet id for a caller (ff_shell.c)
 * to match one against. End-to-end ff_shell-layer behavior (feed status
 * transitions to DELIVERED/NO_ACK) is covered in test_shell.c; this one
 * test pins that the meshclient-level plumbing itself needs no
 * feature-specific change beyond the out_packet_id parameter. */
static void feat_send_text_direct_packet_id_matches_a_later_routing_ack(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    uint32_t packet_id = 0;
    TEST_ASSERT_EQUAL_INT(0, mc_send_text(&c, 0x0A0A0A0Au, "hi", &packet_id));

    uint8_t frame[400];
    uint16_t flen = build_routing_ack_frame(packet_id, /*nak=*/false, frame, sizeof(frame));
    TEST_ASSERT_TRUE(flen > 0);
    io.rx_data = frame;
    io.rx_len = flen;
    io.rx_pos = 0;
    mc_tick(&c, 0);

    TEST_ASSERT_EQUAL_INT(1, cap.routing_ack_count);
    TEST_ASSERT_EQUAL_UINT32(packet_id, cap.routing_acks[0].request_id);
    TEST_ASSERT_TRUE(cap.routing_acks[0].ok);
}

/**
 * Bench finding (2026-09-06, real puck + Meshtastic 2.7.26 comms brain,
 * AFTER commit eb1cb06): the FIRST NAME push after boot confirmed
 * perfectly (ack=ok, reply within 3s); every push after that, in the SAME
 * boot session, got ack=none reply=none forever, even after the app's
 * retry budget was exhausted, despite the comms brain's owner really
 * changing each time. Reproduced here at the mc_client wire level — two
 * full set_owner + get_owner_request round trips through REAL encode and
 * REAL decode (not the app-layer spy), each with its own routing ack and
 * get_owner_response fed back in as raw FromRadio bytes — to check
 * whether this library itself, not just the app's own bookkeeping,
 * correlates a SECOND round trip's reply/ack correctly in the same
 * mc_client_t session.
 */
static void feat_two_consecutive_set_owner_round_trips_in_one_session_both_confirm(void)
{
    mock_io_t io;
    mock_io_reset(&io);
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;

    /* --- Round 1: "Jake H" --- */
    uint32_t packet_id_1 = 0;
    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 0x0A0A0A0Au, "Jake H", "JAKE", &packet_id_1));
    TEST_ASSERT_EQUAL_INT(0, mc_send_get_owner_request(&c, 0x0A0A0A0Au));

    uint8_t frame1a[400];
    uint16_t f1a_len = build_routing_ack_frame(packet_id_1, /*nak=*/false, frame1a, sizeof(frame1a));
    TEST_ASSERT_TRUE(f1a_len > 0);
    io.rx_data = frame1a;
    io.rx_len = f1a_len;
    io.rx_pos = 0;
    mc_tick(&c, 100);

    uint8_t frame1b[400];
    uint16_t f1b_len = build_owner_response_frame("Jake H", "JAKE", frame1b, sizeof(frame1b));
    TEST_ASSERT_TRUE(f1b_len > 0);
    io.rx_data = frame1b;
    io.rx_len = f1b_len;
    io.rx_pos = 0;
    mc_tick(&c, 200);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cap.routing_ack_count, "round 1's own routing ack must be delivered");
    TEST_ASSERT_EQUAL_UINT32(packet_id_1, cap.routing_acks[0].request_id);
    TEST_ASSERT_TRUE(cap.routing_acks[0].ok);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cap.owner_count, "round 1's own get_owner_response must be delivered");
    TEST_ASSERT_EQUAL_STRING("Jake H", cap.owners[0].long_name);

    /* --- Round 2: "Jake", SAME mc_client_t, SAME session, no reboot --- */
    uint32_t packet_id_2 = 0;
    TEST_ASSERT_EQUAL_INT(0, mc_send_set_owner(&c, 0x0A0A0A0Au, "Jake", "JAKE", &packet_id_2));
    TEST_ASSERT_EQUAL_INT(0, mc_send_get_owner_request(&c, 0x0A0A0A0Au));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(packet_id_1, packet_id_2,
                                  "each push must get its own fresh outgoing packet id");

    uint8_t frame2a[400];
    uint16_t f2a_len = build_routing_ack_frame(packet_id_2, /*nak=*/false, frame2a, sizeof(frame2a));
    TEST_ASSERT_TRUE(f2a_len > 0);
    io.rx_data = frame2a;
    io.rx_len = f2a_len;
    io.rx_pos = 0;
    mc_tick(&c, 300);

    uint8_t frame2b[400];
    uint16_t f2b_len = build_owner_response_frame("Jake", "JAKE", frame2b, sizeof(frame2b));
    TEST_ASSERT_TRUE(f2b_len > 0);
    io.rx_data = frame2b;
    io.rx_len = f2b_len;
    io.rx_pos = 0;
    mc_tick(&c, 400);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cap.routing_ack_count, "round 2's OWN routing ack must ALSO be delivered");
    TEST_ASSERT_EQUAL_UINT32(packet_id_2, cap.routing_acks[1].request_id);
    TEST_ASSERT_TRUE(cap.routing_acks[1].ok);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, cap.owner_count, "round 2's OWN get_owner_response must ALSO be delivered");
    TEST_ASSERT_EQUAL_STRING("Jake", cap.owners[1].long_name);

    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.decode_errors);
}

/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* [api] A02 slice D — rx-meta's three new facts, and the channel table  */
/* (docs/specs/S02-core-crew.md's 2026-09-13 amendment §B)               */
/* -------------------------------------------------------------------- */

/* Encode callbacks, mirroring the decode side in mc_client.c: the two
 * ChannelSettings fields this library cares about are unbounded in
 * channel.proto and therefore pb_callback_t in the generated code. */
typedef struct {
    uint8_t const *data;
    size_t         len;
} a02_blob_t;

static bool a02_enc_bytes(pb_ostream_t *stream, pb_field_t const *field, void *const *arg)
{
    a02_blob_t const *b = (a02_blob_t const *)(*arg);
    if (b == NULL || b->len == 0u) return true; /* absent, not empty */
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, b->data, b->len);
}

/* A `FromRadio.channel` frame, with an honest name/psk on the wire. */
static uint16_t a02_build_channel_frame(uint8_t index, char const *name, uint8_t const *psk,
                                         size_t psk_len, bool primary, uint8_t *out, size_t out_cap)
{
    a02_blob_t name_blob = {(uint8_t const *)name, (name != NULL) ? strlen(name) : 0u};
    a02_blob_t psk_blob = {psk, psk_len};

    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_channel_tag;
    fr.payload_variant.channel.index = (int32_t)index;
    fr.payload_variant.channel.role =
        primary ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
    fr.payload_variant.channel.has_settings = true;
    fr.payload_variant.channel.settings.name.funcs.encode = a02_enc_bytes;
    fr.payload_variant.channel.settings.name.arg = &name_blob;
    fr.payload_variant.channel.settings.psk.funcs.encode = a02_enc_bytes;
    fr.payload_variant.channel.settings.psk.arg = &psk_blob;

    uint8_t buf[200];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) return 0;
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

/* A data packet with the extra MeshPacket fields the admission rule
 * reads. build_data_packet_frame's signature deliberately stays as it
 * is — every existing caller means "defaults", and widening it would
 * have quietly changed what a dozen other tests are asserting about. */
static uint16_t a02_build_packet_frame(uint32_t from, uint32_t channel, bool via_mqtt,
                                        bool pki_encrypted, uint32_t portnum, uint8_t *out,
                                        size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.from = from;
    fr.payload_variant.packet.to = MC_ADDR_BROADCAST;
    fr.payload_variant.packet.channel = channel;
    fr.payload_variant.packet.via_mqtt = via_mqtt;
    fr.payload_variant.packet.pki_encrypted = pki_encrypted;
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.payload_variant.packet.payload_variant.decoded.portnum = (meshtastic_PortNum)portnum;
    fr.payload_variant.packet.payload_variant.decoded.payload.size = 1u;
    fr.payload_variant.packet.payload_variant.decoded.payload.bytes[0] = 0x42u;

    uint8_t buf[300];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) return 0;
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

static void a02_feed(uint8_t const *frame, uint16_t len, events_capture_t *cap)
{
    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = len;
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    memset(cap, 0, sizeof(*cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io},
             make_events(cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 10u);
}

static void A02_rx_meta_carries_channel_index_mqtt_and_portnum(void)
{
    uint8_t frame[300];
    uint16_t const len = a02_build_packet_frame(0x0A0A0A0Au, 3u, false, false,
                                                 (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
                                                 frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_channel_index);
    TEST_ASSERT_EQUAL_UINT32(3u, cap.rx_metas[0].meta.channel_index);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.via_mqtt);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_portnum);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
                              cap.rx_metas[0].meta.portnum);
}

static void A02_rx_meta_channel_zero_is_present_not_absent(void)
{
    /* proto3 implicit presence means a packet on the PRIMARY channel
     * serializes `channel` as nothing at all — and mesh.proto states
     * outright that "If unset, packet was on the primary channel". So 0
     * here is a real reading, not a default. Getting this backwards
     * would make the crew channel (normally index 0) admit nobody,
     * which is the exact inverse of the bug the presence flag exists to
     * prevent, and just as silent. */
    uint8_t frame[300];
    uint16_t const len = a02_build_packet_frame(0x0A0A0A0Au, 0u, false, false,
                                                 (uint32_t)meshtastic_PortNum_POSITION_APP,
                                                 frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.has_channel_index);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.rx_metas[0].meta.channel_index);
}

static void A02_rx_meta_via_mqtt_is_reported_unfolded(void)
{
    /* rx_path already folds via_mqtt into INDIRECT — but plenty of
     * ordinary relayed LoRa packets are INDIRECT too, and the admission
     * rule needs the un-folded fact. */
    uint8_t frame[300];
    uint16_t const len = a02_build_packet_frame(0x0A0A0A0Au, 0u, true, false,
                                                 (uint32_t)meshtastic_PortNum_NODEINFO_APP,
                                                 frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_TRUE(cap.rx_metas[0].meta.via_mqtt);
    TEST_ASSERT_EQUAL_INT(MC_RX_PATH_INDIRECT, cap.rx_metas[0].meta.rx_path);
}

static void A02_rx_meta_pki_dm_reports_no_channel_index(void)
{
    /* A PKI-encrypted DM proves possession of a key PAIR, not of the
     * crew channel's key. Its `channel` is 0 on the wire, which is the
     * crew's usual slot — so reporting it present would admit anyone who
     * could DM this puck. */
    uint8_t frame[300];
    uint16_t const len = a02_build_packet_frame(0x0A0A0A0Au, 0u, false, true,
                                                 (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
                                                 frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_channel_index);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.rx_metas[0].meta.channel_index); /* zeroed, not stale */
}

static void A02_rx_meta_encrypted_packet_reports_no_index_and_no_portnum(void)
{
    /* An encrypted-variant packet carries the channel HASH in that field
     * (mesh.pb.h says so), not an index — a hash that happened to equal
     * the crew's index would otherwise admit a sender we could not even
     * decrypt. And there is no decoded payload, so no portnum. */
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.from = 0x0B0B0B0Bu;
    fr.payload_variant.packet.to = MC_ADDR_BROADCAST;
    fr.payload_variant.packet.channel = 0u; /* the hash, which happens to be 0 */
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;

    uint8_t buf[200];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));
    uint8_t frame[300];
    uint16_t const len = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.rx_meta_count);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_channel_index);
    TEST_ASSERT_FALSE(cap.rx_metas[0].meta.has_portnum);
}

static void A02_channel_table_is_delivered_with_name_and_psk(void)
{
    uint8_t psk[32];
    for (int i = 0; i < 32; i++) psk[i] = (uint8_t)(0xA0 + i);

    uint8_t frame[300];
    uint16_t const len = a02_build_channel_frame(0u, "FIRE-4K9M7X", psk, sizeof(psk), true, frame,
                                                  sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_EQUAL_UINT8(0u, cap.channels[0].index);
    TEST_ASSERT_EQUAL_STRING("FIRE-4K9M7X", cap.channels[0].name);
    TEST_ASSERT_EQUAL_UINT8(32u, cap.channels[0].psk_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(psk, cap.channels[0].psk, sizeof(psk));
    TEST_ASSERT_TRUE(cap.channels[0].is_primary);
}

static void A02_channel_at_a_nonzero_index_keeps_its_index(void)
{
    /* The whole reason this event exists: a radio provisioned by CLI or
     * the stock app can hold the crew channel anywhere, and the index is
     * "inherently a local concept". Never assume 0. */
    uint8_t psk[16];
    memset(psk, 0x5A, sizeof(psk));
    uint8_t frame[300];
    uint16_t const len = a02_build_channel_frame(5u, "FIRE-ZZZZZZ", psk, sizeof(psk), false, frame,
                                                  sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_EQUAL_UINT8(5u, cap.channels[0].index);
    TEST_ASSERT_EQUAL_STRING("FIRE-ZZZZZZ", cap.channels[0].name);
    TEST_ASSERT_EQUAL_UINT8(16u, cap.channels[0].psk_len);
    TEST_ASSERT_FALSE(cap.channels[0].is_primary);
}

static void A02_channel_with_no_psk_reports_none_not_zeros(void)
{
    /* `psk_len == 0` is "this channel stated no key", which must not be
     * confused with an all-zero 32-byte key — a crew-index match against
     * a fabricated zero key could succeed for the wrong channel. */
    uint8_t frame[300];
    uint16_t const len = a02_build_channel_frame(1u, "LongFast", NULL, 0u, false, frame,
                                                  sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_EQUAL_UINT8(0u, cap.channels[0].psk_len);
    TEST_ASSERT_EQUAL_STRING("LongFast", cap.channels[0].name);
}

static void A02_oversized_channel_name_reports_no_name_rather_than_truncating(void)
{
    /* Longer than Meshtastic's own 11-byte budget, so not a Firefly crew
     * channel and not describable by mc_channel_t. Reported with an
     * EMPTY name — truncating would produce a name that is not the
     * channel's name, which a name-and-PSK match could then match on. */
    uint8_t frame[300];
    uint16_t const len = a02_build_channel_frame(2u, "a-very-long-channel-name", NULL, 0u, false,
                                                  frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_EQUAL_STRING("", cap.channels[0].name);
    TEST_ASSERT_EQUAL_UINT8(2u, cap.channels[0].index);
}

static void A02_channel_event_is_optional(void)
{
    /* Every existing caller installs no on_channel. Nothing may crash,
     * and the frame must still be accounted for. */
    uint8_t frame[300];
    uint16_t const len = a02_build_channel_frame(0u, "FIRE-4K9M7X", NULL, 0u, true, frame,
                                                  sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = len;
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    mc_events_t ev = make_events(&cap);
    ev.on_channel = NULL;

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, ev, &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 10u);
    TEST_ASSERT_EQUAL_UINT32(1u, mc_get_stats(&c).frames_ok);
    TEST_ASSERT_EQUAL_INT(0, cap.channel_count);
}

/* -------------------------------------------------------------------- */
/* [api] A02 slice D2 — the admin channel write, the table snapshot,     */
/* and the LoRa region (docs/specs/S02-core-crew.md's 2026-09-14         */
/* amendment)                                                            */
/* -------------------------------------------------------------------- */

/* A `FromRadio.channel` frame that also states a position_precision.
 * Separate from a02_build_channel_frame rather than a widened signature,
 * for the same reason that helper gives: every existing caller means
 * "defaults", and widening would quietly change what they assert. */
static uint16_t d2_build_channel_frame_with_precision(uint8_t index, char const *name, uint8_t const *psk,
                                                       size_t psk_len, bool primary, bool has_precision,
                                                       uint32_t precision, uint8_t *out, size_t out_cap)
{
    a02_blob_t name_blob = {(uint8_t const *)name, (name != NULL) ? strlen(name) : 0u};
    a02_blob_t psk_blob = {psk, psk_len};

    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_channel_tag;
    fr.payload_variant.channel.index = (int32_t)index;
    fr.payload_variant.channel.role =
        primary ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
    fr.payload_variant.channel.has_settings = true;
    fr.payload_variant.channel.settings.name.funcs.encode = a02_enc_bytes;
    fr.payload_variant.channel.settings.name.arg = &name_blob;
    fr.payload_variant.channel.settings.psk.funcs.encode = a02_enc_bytes;
    fr.payload_variant.channel.settings.psk.arg = &psk_blob;
    fr.payload_variant.channel.settings.has_module_settings = has_precision;
    fr.payload_variant.channel.settings.module_settings.position_precision = precision;

    uint8_t buf[200];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) return 0;
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

/* Decode the AdminMessage a set_channel write put on the wire, with the
 * two callback fields hooked up so the name and the psk are actually
 * readable — a test that only checked the index would pass for a write
 * that shipped an empty channel. */
typedef struct {
    uint8_t buf[64];
    size_t  len;
    bool    ok;
} d2_sink_t;

static bool d2_dec_bytes(pb_istream_t *stream, pb_field_t const *field, void **arg)
{
    (void)field;
    d2_sink_t *s = (d2_sink_t *)(*arg);
    size_t const n = stream->bytes_left;
    if (s == NULL || n > sizeof(s->buf)) return pb_read(stream, NULL, n);
    if (!pb_read(stream, s->buf, n)) return false;
    s->len = n;
    s->ok = true;
    return true;
}

static meshtastic_Channel d2_decode_tx_set_channel(mock_io_t const *io, d2_sink_t *name, d2_sink_t *psk)
{
    meshtastic_MeshPacket const pkt = decode_tx_packet(io);
    TEST_ASSERT_EQUAL_INT(meshtastic_MeshPacket_decoded_tag, pkt.which_payload_variant);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)meshtastic_PortNum_ADMIN_APP, (uint32_t)pkt.payload_variant.decoded.portnum);

    memset(name, 0, sizeof(*name));
    memset(psk, 0, sizeof(*psk));

    meshtastic_AdminMessage admin = meshtastic_AdminMessage_init_zero;
    /* The oneof's union is memset when the variant is recognized, so the
     * callbacks cannot be installed before the decode (mc_client.c's own
     * comment). Decode the AdminMessage's set_channel submessage
     * directly instead, by walking for tag 33. */
    pb_istream_t top = pb_istream_from_buffer(pkt.payload_variant.decoded.payload.bytes,
                                               pkt.payload_variant.decoded.payload.size);
    meshtastic_Channel ch = meshtastic_Channel_init_zero;
    bool found = false;
    for (;;) {
        pb_wire_type_t wire = PB_WT_VARINT;
        uint32_t tag = 0u;
        bool eof = false;
        if (!pb_decode_tag(&top, &wire, &tag, &eof) || eof) break;
        if (tag != (uint32_t)meshtastic_AdminMessage_set_channel_tag || wire != PB_WT_STRING) {
            TEST_ASSERT_TRUE(pb_skip_field(&top, wire));
            continue;
        }
        pb_istream_t sub;
        TEST_ASSERT_TRUE(pb_make_string_substream(&top, &sub));
        ch.settings.name.funcs.decode = d2_dec_bytes;
        ch.settings.name.arg = name;
        ch.settings.psk.funcs.decode = d2_dec_bytes;
        ch.settings.psk.arg = psk;
        TEST_ASSERT_TRUE(pb_decode(&sub, meshtastic_Channel_fields, &ch));
        (void)pb_close_string_substream(&top, &sub);
        found = true;
        break;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "no AdminMessage.set_channel on the wire");
    (void)admin;
    return ch;
}

/* A READY client over a capturing transport. */
static void d2_ready_client(mc_client_t *c, mock_io_t *io, mock_clock_t *clk, ff_clock_t *clock,
                             events_capture_t *cap)
{
    mock_io_reset(io);
    clk->t = 0;
    clock->now_ms = mock_now;
    clock->user = clk;
    memset(cap, 0, sizeof(*cap));
    mc_init(c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = io}, make_events(cap), clock);
    c->state = MC_STATE_READY;
}

static mc_channel_t d2_crew_channel(void)
{
    /* A02's own vector 1, so the bytes on the wire are the fixture's. */
    static uint8_t const psk[32] = {0x74, 0x3c, 0xc9, 0x83, 0xba, 0x32, 0x68, 0x92, 0xfb, 0x91, 0xb6,
                                     0x70, 0x0b, 0x9f, 0xf3, 0xd0, 0x8d, 0x7e, 0x00, 0x25, 0x68, 0xb3,
                                     0xf2, 0x78, 0xdb, 0x43, 0x24, 0x4a, 0x24, 0x9e, 0x71, 0xda};
    mc_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.index = 0u;
    snprintf(ch.name, sizeof(ch.name), "FIRE-4K9M7X");
    memcpy(ch.psk, psk, sizeof(psk));
    ch.psk_len = 32u;
    ch.is_primary = true;
    ch.has_position_precision = true;
    ch.position_precision = 32u;
    return ch;
}

static void D2_set_channel_encodes_the_whole_channel(void)
{
    mock_io_t io;
    mock_clock_t clk;
    ff_clock_t clock;
    events_capture_t cap;
    mc_client_t c;
    d2_ready_client(&c, &io, &clk, &clock, &cap);

    mc_channel_t const want = d2_crew_channel();
    uint32_t pid = 0u;
    TEST_ASSERT_EQUAL_INT(0, mc_client_set_channel(&c, 0x1234u, &want, &pid));
    TEST_ASSERT_NOT_EQUAL(0u, pid);

    d2_sink_t name, psk;
    meshtastic_Channel const got = d2_decode_tx_set_channel(&io, &name, &psk);

    TEST_ASSERT_EQUAL_INT32(0, got.index);
    TEST_ASSERT_EQUAL_INT(meshtastic_Channel_Role_PRIMARY, got.role);
    TEST_ASSERT_TRUE(got.has_settings);
    /* The NAME is on the wire, in full — this is the assertion that
     * fails for a write that encoded an empty channel and looked fine. */
    TEST_ASSERT_TRUE(name.ok);
    TEST_ASSERT_EQUAL_size_t(11u, name.len);
    TEST_ASSERT_EQUAL_MEMORY("FIRE-4K9M7X", name.buf, 11u);
    /* ...and so is the KEY, byte for byte against A02's vector 1. */
    TEST_ASSERT_TRUE(psk.ok);
    TEST_ASSERT_EQUAL_size_t(32u, psk.len);
    TEST_ASSERT_EQUAL_MEMORY(want.psk, psk.buf, 32u);
    /* A02 §1.5: always explicitly present, and 32. */
    TEST_ASSERT_TRUE(got.settings.has_module_settings);
    TEST_ASSERT_EQUAL_UINT32(32u, got.settings.module_settings.position_precision);
    /* A crew is never bridged to MQTT. */
    TEST_ASSERT_FALSE(got.settings.uplink_enabled);
    TEST_ASSERT_FALSE(got.settings.downlink_enabled);
}

static void D2_set_channel_requests_an_ack(void)
{
    /* An admin write is worth the mesh stack's retries and a routing
     * reply, exactly like set_owner — that reply is what the crew-start
     * machine correlates against `out_packet_id`. */
    mock_io_t io;
    mock_clock_t clk;
    ff_clock_t clock;
    events_capture_t cap;
    mc_client_t c;
    d2_ready_client(&c, &io, &clk, &clock, &cap);

    mc_channel_t const want = d2_crew_channel();
    TEST_ASSERT_EQUAL_INT(0, mc_client_set_channel(&c, 0x1234u, &want, NULL));
    TEST_ASSERT_TRUE(decode_tx_want_ack(&io));
}

static void D2_set_channel_always_emits_module_settings_even_for_zero(void)
{
    /* THE TRAP. Meshtastic reads an ABSENT module_settings as the
     * default precision (32), so a LEAVE restoring precision 0 that
     * omitted the submessage would silently ship full precision — the
     * exact inverse of what the wearer asked for, and invisible. */
    mock_io_t io;
    mock_clock_t clk;
    ff_clock_t clock;
    events_capture_t cap;
    mc_client_t c;
    d2_ready_client(&c, &io, &clk, &clock, &cap);

    mc_channel_t restore;
    memset(&restore, 0, sizeof(restore));
    restore.index = 0u;
    restore.psk[0] = 0x01u;
    restore.psk_len = 1u;
    restore.is_primary = true;
    restore.has_position_precision = true;
    restore.position_precision = 0u;

    TEST_ASSERT_EQUAL_INT(0, mc_client_set_channel(&c, 0x1234u, &restore, NULL));
    d2_sink_t name, psk;
    meshtastic_Channel const got = d2_decode_tx_set_channel(&io, &name, &psk);
    TEST_ASSERT_TRUE(got.settings.has_module_settings);
    TEST_ASSERT_EQUAL_UINT32(0u, got.settings.module_settings.position_precision);
    /* An empty name IS channel.proto's default channel; nothing is
     * written for it, and that is not the same as a zero-length field. */
    TEST_ASSERT_FALSE(name.ok);
    TEST_ASSERT_TRUE(psk.ok);
    TEST_ASSERT_EQUAL_size_t(1u, psk.len);
    TEST_ASSERT_EQUAL_UINT8(0x01u, psk.buf[0]);
}

static void D2_set_channel_refuses_out_of_bounds_input(void)
{
    mock_io_t io;
    mock_clock_t clk;
    ff_clock_t clock;
    events_capture_t cap;
    mc_client_t c;
    d2_ready_client(&c, &io, &clk, &clock, &cap);

    mc_channel_t bad = d2_crew_channel();
    bad.index = (uint8_t)MC_CHANNEL_MAX;
    TEST_ASSERT_LESS_THAN_INT(0, mc_client_set_channel(&c, 1u, &bad, NULL));

    /* A 7-byte key is not a shorter key, it is a corrupt one. */
    bad = d2_crew_channel();
    bad.psk_len = 7u;
    TEST_ASSERT_LESS_THAN_INT(0, mc_client_set_channel(&c, 1u, &bad, NULL));

    /* A name field with no terminator anywhere in it. */
    bad = d2_crew_channel();
    memset(bad.name, 'A', sizeof(bad.name));
    TEST_ASSERT_LESS_THAN_INT(0, mc_client_set_channel(&c, 1u, &bad, NULL));

    TEST_ASSERT_LESS_THAN_INT(0, mc_client_set_channel(&c, 1u, NULL, NULL));

    /* Nothing was written for any of them. */
    TEST_ASSERT_EQUAL_size_t(0u, io.tx_len);
}

static void D2_set_channel_refuses_before_ready(void)
{
    mock_io_t io;
    mock_clock_t clk;
    ff_clock_t clock;
    events_capture_t cap;
    mc_client_t c;
    d2_ready_client(&c, &io, &clk, &clock, &cap);
    c.state = MC_STATE_HANDSHAKE;

    mc_channel_t const want = d2_crew_channel();
    uint32_t pid = 0xAAAAu;
    TEST_ASSERT_LESS_THAN_INT(0, mc_client_set_channel(&c, 1u, &want, &pid));
    TEST_ASSERT_EQUAL_UINT32(0xAAAAu, pid); /* untouched on failure */
    TEST_ASSERT_EQUAL_size_t(0u, io.tx_len);
}

static void D2_position_precision_is_presence_flagged_on_read(void)
{
    uint8_t frame[300];
    uint16_t len = d2_build_channel_frame_with_precision(0u, "FIRE-4K9M7X", NULL, 0u, true, true, 32u,
                                                          frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);
    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_TRUE(cap.channels[0].has_position_precision);
    TEST_ASSERT_EQUAL_UINT32(32u, cap.channels[0].position_precision);

    /* A channel that stated NO precision is not a channel that stated 0
     * — the snapshot has to be able to restore the difference. */
    len = d2_build_channel_frame_with_precision(0u, "FIRE-4K9M7X", NULL, 0u, true, false, 0u, frame,
                                                 sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.channel_count);
    TEST_ASSERT_FALSE(cap.channels[0].has_position_precision);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.channels[0].position_precision);
}

static void D2_channel_snapshot_reports_only_what_this_handshake_said(void)
{
    uint8_t frame[300];
    uint16_t const len = d2_build_channel_frame_with_precision(0u, "LongFast", NULL, 0u, true, true, 13u,
                                                                frame, sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);

    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = len;
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);

    mc_channel_t snap;
    /* Nothing reported yet is "there is no snapshot", NOT "it is
     * probably the default". */
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(&c, 0u, &snap));

    c.state = MC_STATE_READY;
    mc_tick(&c, 10u);

    TEST_ASSERT_TRUE(mc_client_get_channel_snapshot(&c, 0u, &snap));
    TEST_ASSERT_EQUAL_STRING("LongFast", snap.name);
    TEST_ASSERT_TRUE(snap.has_position_precision);
    TEST_ASSERT_EQUAL_UINT32(13u, snap.position_precision);

    /* An index nobody reported, and one past the table, both say no. */
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(&c, 1u, &snap));
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(&c, (uint8_t)MC_CHANNEL_MAX, &snap));
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(&c, 0u, NULL));
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(NULL, 0u, &snap));
}

static void D2_channel_snapshot_is_cleared_by_a_fresh_handshake(void)
{
    /* An admin channel write REBOOTS the comms brain, which starts a new
     * handshake. A row that survived that would be a memory of a radio
     * that no longer exists — and it is the pre-crew snapshot somebody
     * would restore. */
    uint8_t frame[300];
    uint16_t const len = d2_build_channel_frame_with_precision(0u, "LongFast", NULL, 0u, true, true, 13u,
                                                                frame, sizeof(frame));
    mock_io_t io;
    mock_io_reset(&io);
    io.rx_data = frame;
    io.rx_len = len;
    mock_clock_t clk = {.t = 0};
    ff_clock_t clock = {.now_ms = mock_now, .user = &clk};
    events_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    mc_client_t c;
    mc_init(&c, (mc_transport_t){.write = mock_write, .read = mock_read, .io = &io}, make_events(&cap), &clock);
    c.state = MC_STATE_READY;
    mc_tick(&c, 10u);

    mc_channel_t snap;
    TEST_ASSERT_TRUE(mc_client_get_channel_snapshot(&c, 0u, &snap));

    mc_connect(&c); /* a fresh want_config */
    TEST_ASSERT_FALSE(mc_client_get_channel_snapshot(&c, 0u, &snap));
}

/* A `FromRadio.config` frame carrying a LoRaConfig. */
static uint16_t d2_build_lora_config_frame(uint32_t region, uint8_t *out, size_t out_cap)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_tag;
    fr.payload_variant.config.which_payload_variant = meshtastic_Config_lora_tag;
    fr.payload_variant.config.payload_variant.lora.region = (meshtastic_Config_LoRaConfig_RegionCode)region;
    fr.payload_variant.config.payload_variant.lora.use_preset = true;

    uint8_t buf[200];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_FromRadio_fields, &fr)) return 0;
    return mc_frame_encode(out, out_cap, buf, (uint16_t)os.bytes_written);
}

static void D2_lora_region_is_reported_including_unset(void)
{
    uint8_t frame[300];
    uint16_t len = d2_build_lora_config_frame((uint32_t)meshtastic_Config_LoRaConfig_RegionCode_US, frame,
                                                sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);
    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.lora_region_count);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)meshtastic_Config_LoRaConfig_RegionCode_US, cap.lora_region);

    /* UNSET is 0 and proto3 drops zero values, so the frame carries no
     * `region` field at all — and it MUST still be reported, because
     * "this radio has no region" is the whole question a crew start
     * asks. A client that only fired on a present field would leave the
     * start flow thinking the region was fine. */
    len = d2_build_lora_config_frame((uint32_t)meshtastic_Config_LoRaConfig_RegionCode_UNSET, frame,
                                      sizeof(frame));
    TEST_ASSERT_TRUE(len > 0);
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(1, cap.lora_region_count);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.lora_region);
}

static void D2_non_lora_config_is_skipped_not_reported(void)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_tag;
    fr.payload_variant.config.which_payload_variant = meshtastic_Config_device_tag;
    fr.payload_variant.config.payload_variant.device.node_info_broadcast_secs = 900u;

    uint8_t buf[200];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, &fr));
    uint8_t frame[300];
    uint16_t const len = mc_frame_encode(frame, sizeof(frame), buf, (uint16_t)os.bytes_written);
    TEST_ASSERT_TRUE(len > 0);

    events_capture_t cap;
    a02_feed(frame, len, &cap);
    TEST_ASSERT_EQUAL_INT(0, cap.lora_region_count);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S03_AC1_byte_dribble_yields_frame_once);
    RUN_TEST(S03_AC1_garbage_prefix_yields_frame_once);
    RUN_TEST(S03_AC1_oversize_len_resyncs_without_overflow);
    RUN_TEST(S03_AC1_frame_length_exactly_512_is_accepted);
    RUN_TEST(S03_AC1_frame_length_513_resyncs);

    RUN_TEST(S03_AC1_timeout_frame_interrupted_past_timeout_is_discarded_and_resyncs);
    RUN_TEST(S03_AC1_timeout_frame_interrupted_within_timeout_still_completes);
    RUN_TEST(S03_AC1_timeout_gap_exactly_at_boundary_is_not_a_timeout);
    RUN_TEST(S03_AC1_timeout_back_to_back_frames_no_gap_not_regressed);

    RUN_TEST(S03_AC2_connect_sends_want_config_and_enters_handshake);
    RUN_TEST(S03_AC2_handshake_dump_reaches_ready_with_node_and_myinfo);
    RUN_TEST(S03_AC2_handshake_wrong_nonce_stays_in_handshake);

    RUN_TEST(S03_AC3_position_packet_decodes_with_1e7_conversion_and_rx_time);

    RUN_TEST(S03_AC4_send_text_matches_byte_golden);
    RUN_TEST(feat_send_text_direct_returns_packet_id_and_wants_ack);
    RUN_TEST(feat_send_text_broadcast_returns_packet_id_but_no_ack_requested);
    RUN_TEST(feat_send_text_out_packet_id_is_optional);
    RUN_TEST(feat_send_text_fails_when_not_ready_leaves_out_packet_id_untouched);
    RUN_TEST(S03_want_ack_mc_send_private_true_sets_meshpacket_want_ack);
    RUN_TEST(S03_want_ack_mc_send_private_false_leaves_meshpacket_want_ack_unset);

    RUN_TEST(S14_mc_send_position_not_ready_returns_error_writes_nothing);
    RUN_TEST(S14_mc_send_position_encodes_broadcast_position_app_no_ack);
    RUN_TEST(S14_mc_send_position_rounds_negative_coordinates_away_from_zero);

    RUN_TEST(S03_packet_id_unseeded_matches_legacy_sequence);
    RUN_TEST(S03_packet_id_seed_sets_starting_point_and_increments);
    RUN_TEST(S03_packet_id_skips_zero_on_wrap_and_on_zero_seed);
    RUN_TEST(S03_packet_id_different_seeds_produce_disjoint_ids);

    RUN_TEST(S03_AC5_on_private_fires_for_portnum_256_511_untouched);
    RUN_TEST(S03_AC5_private_portnum_boundary_255_is_not_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_256_is_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_511_is_private);
    RUN_TEST(S03_AC5_private_portnum_boundary_512_is_not_private);
    RUN_TEST(I123_on_private_carries_to_address_verbatim);

    RUN_TEST(S03_AC6_silence_30s_reconnects_ready_disconnected_handshake);
    RUN_TEST(S03_AC6_transport_error_triggers_reconnect);

    RUN_TEST(S03_debt_reboot_frame_after_ready_drops_state_and_reissues_want_config);
    RUN_TEST(S03_debt_reboot_stale_config_complete_is_ignored_per_handshake_rules);
    RUN_TEST(S03_debt_reboot_then_matching_config_complete_reaches_ready_again);

    RUN_TEST(S03_debt_handshake_stall_reissues_want_config_and_reaches_ready);
    RUN_TEST(S03_debt_handshake_retry_keeps_nonce_so_a_late_answer_still_lands);
    RUN_TEST(S03_debt_handshake_retry_budget_escalates_to_a_fresh_reconnect);
    RUN_TEST(S03_debt_handshake_never_completing_does_not_inhibit_sleep_forever);
    RUN_TEST(S03_debt_handshake_answered_within_timeout_never_retries);

    RUN_TEST(S03_debt_write_backpressure_below_budget_sends_frame_no_reconnect);
    RUN_TEST(S03_debt_write_backpressure_budget_exhausted_triggers_reconnect);
    RUN_TEST(S03_debt_write_backpressure_transport_stuck_forever_fails_at_exactly_budget);
    RUN_TEST(S03_debt_write_negative_return_fails_immediately);

    RUN_TEST(S03_debt_position_no_fix_yet_does_not_count_as_decode_error);
    RUN_TEST(S03_debt_position_corrupt_protobuf_counts_decode_error);

    RUN_TEST(S03_debt_mc_tick_bounded_drain_caps_frames_per_call);

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

    RUN_TEST(feat_set_owner_encodes_long_and_short_name);
    RUN_TEST(feat_set_owner_out_packet_id_is_optional);
    RUN_TEST(feat_set_owner_dest_is_whatever_the_caller_passes);
    RUN_TEST(feat_set_owner_null_short_name_leaves_it_unset);
    RUN_TEST(feat_set_owner_uses_the_seeded_packet_id_counter);
    RUN_TEST(feat_set_owner_fails_when_not_ready);

    RUN_TEST(feat_get_owner_request_encodes_the_request);
    RUN_TEST(feat_get_owner_request_fails_when_not_ready);
    RUN_TEST(feat_nodeinfo_request_encodes_want_response_on_nodeinfo_app);
    RUN_TEST(feat_nodeinfo_request_carries_our_own_user);
    RUN_TEST(feat_nodeinfo_request_never_borrows_another_nodes_name);
    RUN_TEST(feat_nodeinfo_request_fails_when_not_ready);
    RUN_TEST(feat_live_nodeinfo_app_decodes_to_on_nodeinfo_reply);
    RUN_TEST(feat_live_nodeinfo_app_with_no_name_still_fires_with_both_flags_false);
    RUN_TEST(feat_live_nodeinfo_app_corrupt_protobuf_counts_decode_error);
    RUN_TEST(feat_get_owner_response_fires_on_owner);
    RUN_TEST(feat_get_owner_response_unset_short_name_reports_empty);
    RUN_TEST(feat_admin_other_variant_is_silently_ignored);
    RUN_TEST(feat_routing_ack_none_reports_ok);
    RUN_TEST(feat_routing_nak_reports_not_ok);
    RUN_TEST(feat_send_text_direct_packet_id_matches_a_later_routing_ack);
    RUN_TEST(feat_two_consecutive_set_owner_round_trips_in_one_session_both_confirm);

    RUN_TEST(A02_rx_meta_carries_channel_index_mqtt_and_portnum);
    RUN_TEST(A02_rx_meta_channel_zero_is_present_not_absent);
    RUN_TEST(A02_rx_meta_via_mqtt_is_reported_unfolded);
    RUN_TEST(A02_rx_meta_pki_dm_reports_no_channel_index);
    RUN_TEST(A02_rx_meta_encrypted_packet_reports_no_index_and_no_portnum);
    RUN_TEST(A02_channel_table_is_delivered_with_name_and_psk);
    RUN_TEST(A02_channel_at_a_nonzero_index_keeps_its_index);
    RUN_TEST(A02_channel_with_no_psk_reports_none_not_zeros);
    RUN_TEST(A02_oversized_channel_name_reports_no_name_rather_than_truncating);
    RUN_TEST(A02_channel_event_is_optional);
    RUN_TEST(D2_set_channel_encodes_the_whole_channel);
    RUN_TEST(D2_set_channel_requests_an_ack);
    RUN_TEST(D2_set_channel_always_emits_module_settings_even_for_zero);
    RUN_TEST(D2_set_channel_refuses_out_of_bounds_input);
    RUN_TEST(D2_set_channel_refuses_before_ready);
    RUN_TEST(D2_position_precision_is_presence_flagged_on_read);
    RUN_TEST(D2_channel_snapshot_reports_only_what_this_handshake_said);
    RUN_TEST(D2_channel_snapshot_is_cleared_by_a_fresh_handshake);
    RUN_TEST(D2_lora_region_is_reported_including_unset);
    RUN_TEST(D2_non_lora_config_is_skipped_not_reported);

    return UNITY_END();
}
