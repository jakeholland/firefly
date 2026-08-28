/**
 * test_shell.c — S16 slices b1/b2/c2: the app shell.
 *
 * Criteria covered (docs/specs/S16-app-shell.md, "Acceptance criteria"):
 *   AC4(a) — the dirty bit is exact against the RENDERED projection: a
 *            fully idle shell returns false for 1000 consecutive ticks.
 *            Plus the measurement that makes that non-vacuous (see
 *            "the proxy" below).
 *   AC5a   — a packet from a never-heard sender produces no feed item,
 *            no new crew slot, and exactly one ff_heard entry.
 *   AC5b   — a packet from a known-but-unpaired member produces no feed
 *            item and no new ff_heard entry.
 *   AC5c   — a position from a node not in the roster is dropped and the
 *            sender noted, asserted by roster count before and after.
 *   AC7    — FF_INTENT_CANNED_REPLY sends to the newest feed item's
 *            sender, or broadcasts with an empty feed; captured through
 *            the same real-transport pipeline AC6 uses, decoding the
 *            outbound frame's destination (slice c2).
 *   AC9    — a transport drop moves link state to reconnecting, and the
 *            reconnect's want_config replay does not refresh any
 *            position's age.
 *   AC11   — should_alert fires the haptic during quiet hours; a
 *            feed-push haptic during quiet hours does not.
 *   AC13   — ff_app_state_t.active_face is never FF_APP_FACE_FLARE, in
 *            any projection, including while a takeover is active.
 *
 * Everything named S16_b1_* is not a numbered criterion but a rule this
 * slice must hold for the criteria above to mean anything: the wall
 * clock's UNKNOWN discipline, "a position age never comes from the local
 * clock", self-traffic filtering, RSSI attribution, the fp_pack_t
 * placement decision, and the Now-face projection.
 *
 * No transport, no socket, no handshake anywhere in this file. Every
 * inbound event is injected through `ff_shell_events()` with synthetic
 * values — the same "mock event injector" seam `ff_wiring.h` documents
 * for its own two handlers, extended to all seven callbacks
 * (`mc_events_t` has seven, not the five the spec's slice table
 * used to say — see S16's Amendments).
 *
 * ---------------------------------------------------------------------
 * THE PROXY, stated up front (docs/review/code-review.md, item 6)
 * ---------------------------------------------------------------------
 * Two of these criteria are easy to pass for the wrong reason:
 *
 *  - AC4(a)'s proxy is "the tick returned false". A whole-struct memcmp
 *    over `ff_app_state_t` passes the idle test perfectly and still
 *    returns true on every single frame the moment anything real is on
 *    screen — the failure S16 names explicitly. So the AC4 test does not
 *    stop at "idle is quiet": it runs a live scene and MEASURES the raw
 *    whole-struct comparison alongside the shell's own answer, asserting
 *    the raw one differs on every tick while the shell's differs only at
 *    second boundaries. If the reduction were removed, that assertion
 *    fails; a green idle test alone would not have noticed.
 *
 *  - AC11's proxy is "the haptic count was 0 during quiet hours". A
 *    haptic that never fires at all satisfies that. Every quiet-hours
 *    assertion here is therefore paired with a positive control at a
 *    non-quiet wall time, on the same shell, through the same path.
 *
 * Reference timestamps are Lost Lands 2026 (Sep 18-20). Sep 18 2026 is
 * day-of-year 261; the festival day rolls at 06:00 local, so 05:00 local
 * belongs to day 260 at now_min 1740 (ff_sched's contract, implemented
 * in ff_wall).
 */
#include <string.h>

#include "unity.h"

#include "ff_shell.h"

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_heard.h"
#include "ff_proto.h"
#include "ff_radar.h"
#include "ff_settings.h"
#include "ff_wall.h"

/* S16 slice b2's transport-driven AC6 test is the ONE exception to this
 * file's "no transport anywhere" rule, deliberately: AC6 is about the
 * cutover — every inbound event routed through the same shell entry
 * points via a real mc_client_t decode — so it hand-encodes synthetic
 * Meshtastic frames over a scripted in-memory transport, the same
 * technique the retired targets/sim/tests/test_live.c used (itself
 * borrowed from meshclient/tests/test_meshclient.c). */
#include "mc_framing.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include "meshtastic/mesh.pb.h"

/* ------------------------------------------------------------------- */
/* reference times                                                      */
/* ------------------------------------------------------------------- */

/* 2026-09-18T22:00:00Z — comfortably inside ff_wall's plausibility
 * window [FF_WALL_EPOCH_FLOOR, FF_WALL_EPOCH_CEILING). */
#define U_EVENING ((uint32_t)1789768800u)
/* 2026-09-18T05:00:00Z — 05:00 UTC. With a settings offset of 0 that is
 * 05:00 local: inside the default quiet-hours window [240, 600). */
#define U_QUIET ((uint32_t)1789707600u)
/* 2026-09-18T20:00:00Z — 20:00 local at offset 0, comfortably awake. */
#define U_AWAKE ((uint32_t)1789761600u)

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

/* ------------------------------------------------------------------- */
/* in-memory ff_store_t (settings round-trip, no filesystem)            */
/* ------------------------------------------------------------------- */

typedef struct {
    uint8_t buf[512];
    size_t len;
    bool present;
} mem_store_t;

static int mem_get(void *io, char const *key, void *buf, size_t n)
{
    mem_store_t *st = (mem_store_t *)io;
    (void)key;
    if (!st->present || st->len > n) return -1;
    memcpy(buf, st->buf, st->len);
    return (int)st->len;
}

static int mem_set(void *io, char const *key, void const *buf, size_t n)
{
    mem_store_t *st = (mem_store_t *)io;
    (void)key;
    if (n > sizeof(st->buf)) return -1;
    memcpy(st->buf, buf, n);
    st->len = n;
    st->present = true;
    return (int)n;
}

static ff_store_t mem_store(mem_store_t *st)
{
    ff_store_t s;
    s.get = mem_get;
    s.set = mem_set;
    s.io = st;
    return s;
}

/* ------------------------------------------------------------------- */
/* haptic spy                                                           */
/* ------------------------------------------------------------------- */

typedef struct {
    int count;
} haptic_spy_t;

static void spy_haptic(void *user)
{
    ((haptic_spy_t *)user)->count++;
}

/* ------------------------------------------------------------------- */
/* harness                                                              */
/* ------------------------------------------------------------------- */

typedef struct {
    fake_clock_t clk;
    ff_clock_t clock;
    haptic_spy_t haptic;
    mem_store_t store_mem;
    ff_store_t store;
    fp_pack_t pack;
    ff_shell_t shell;
    mc_events_t ev;
} harness_t;

static harness_t H;

#define MY_ID 0x00001000u
#define DANA 0x0000DA1Au
#define KEV_ID 0x0000CEE0u
#define STRANGER 0x0000AAAAu
#define STRANGER2 0x0000BBBBu

/**
 * Bring up a shell with no transport (events are injected directly) and
 * no store unless `with_store`. Returns with the clock at `t0_ms`.
 */
static void harness_init(uint32_t t0_ms, bool with_store)
{
    /* Preserve anything harness_seed_settings wrote: the store is the
     * one piece of state that must survive an init, since that is
     * exactly what persistence means. */
    mem_store_t const seeded = H.store_mem;
    memset(&H, 0, sizeof(H));
    if (with_store) H.store_mem = seeded;
    H.clk.t = t0_ms;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;
    H.store = mem_store(&H.store_mem);

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.store = with_store ? &H.store : NULL;
    cfg.haptic = spy_haptic;
    cfg.haptic_user = &H.haptic;
    cfg.pack = &H.pack;
    /* cfg.transport left zeroed: the documented "no transport" case. */

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    H.ev = ff_shell_events(&H.shell);
}

/** Persist a settings blob so the NEXT harness_init(.., true) loads it. */
static void harness_seed_settings(int16_t utc_offset_min)
{
    memset(&H.store_mem, 0, sizeof(H.store_mem));
    H.store = mem_store(&H.store_mem);

    ff_settings_t s;
    ff_settings_load(&s, NULL); /* exact defaults: haptics on, quiet [240, 600) */
    s.utc_offset_min = utc_offset_min;
    s.utc_offset_set = true;
    ff_settings_save(&s, &H.store);
}

static void advance(uint32_t dt_ms)
{
    H.clk.t += dt_ms;
}

/* --- synthetic event builders -------------------------------------- */

static void inject_my_info(uint32_t id)
{
    H.ev.on_my_info(H.ev.user, id);
}

static mc_nodeinfo_t nodeinfo(uint32_t node, char const *short_name, uint32_t last_heard)
{
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = node;
    if (short_name != NULL) {
        n.has_short_name = true;
        strncpy(n.short_name, short_name, sizeof(n.short_name) - 1);
    }
    n.last_heard = last_heard;
    return n;
}

/** The want_config replay's exact shape: on_node carrying has_position +
 *  last_heard, and NO rx_time — mc_client.c:222 hardcodes
 *  has_rx_time = false on the NodeInfo path. */
static void inject_node_with_position(uint32_t node, uint32_t last_heard, double lat, double lon)
{
    mc_nodeinfo_t n = nodeinfo(node, NULL, last_heard);
    n.has_position = true;
    n.position.lat = lat;
    n.position.lon = lon;
    n.position.has_rx_time = false; /* the whole point of AC9 */
    H.ev.on_node(H.ev.user, &n);
}

/** Same shape as inject_node_with_position, plus loc_source/precision —
 * issue #33/#47's shell-boundary translation. `has_precision_bits`
 * false always leaves `precision_bits` at 0 (the "sender didn't say"
 * shape mc_client.c itself produces — see mc_position_t's doc comment). */
static void inject_node_with_position_ex(uint32_t node, uint32_t last_heard, double lat, double lon,
                                          mc_loc_source_t loc_source, bool has_precision_bits,
                                          uint8_t precision_bits)
{
    mc_nodeinfo_t n = nodeinfo(node, NULL, last_heard);
    n.has_position = true;
    n.position.lat = lat;
    n.position.lon = lon;
    n.position.has_rx_time = false; /* the whole point of AC9 */
    n.position.loc_source = loc_source;
    n.position.has_precision_bits = has_precision_bits;
    n.position.precision_bits = has_precision_bits ? precision_bits : 0u;
    H.ev.on_node(H.ev.user, &n);
}

static void inject_node(uint32_t node, char const *short_name, uint32_t last_heard)
{
    mc_nodeinfo_t n = nodeinfo(node, short_name, last_heard);
    H.ev.on_node(H.ev.user, &n);
}

/** A live over-the-air position: on_position with rx_time set. */
static void inject_position(uint32_t node, uint32_t rx_time, double lat, double lon)
{
    mc_position_t p;
    memset(&p, 0, sizeof(p));
    p.lat = lat;
    p.lon = lon;
    p.has_rx_time = (rx_time != 0u);
    p.rx_time = rx_time;
    H.ev.on_position(H.ev.user, node, &p);
}

/** Same shape as inject_position, plus loc_source/precision — issue
 * #33/#47's shell-boundary translation, exercised on the LIVE-packet
 * path (as opposed to inject_node_with_position_ex's NodeInfo replay
 * path — the two are deliberately kept distinguishable in these tests,
 * mirroring mc_client.h's documented path asymmetry). */
static void inject_position_ex(uint32_t node, uint32_t rx_time, double lat, double lon, mc_loc_source_t loc_source,
                                bool has_precision_bits, uint8_t precision_bits)
{
    mc_position_t p;
    memset(&p, 0, sizeof(p));
    p.lat = lat;
    p.lon = lon;
    p.has_rx_time = (rx_time != 0u);
    p.rx_time = rx_time;
    p.loc_source = loc_source;
    p.has_precision_bits = has_precision_bits;
    p.precision_bits = has_precision_bits ? precision_bits : 0u;
    H.ev.on_position(H.ev.user, node, &p);
}

static void inject_rx_meta(uint32_t from, mc_rx_path_t path, bool has_rssi, int16_t rssi)
{
    mc_rx_meta_t m;
    memset(&m, 0, sizeof(m));
    m.rx_path = path;
    m.has_rssi = has_rssi;
    m.rssi_dbm = rssi;
    H.ev.on_rx_meta(H.ev.user, from, &m);
}

static void inject_text(uint32_t from, char const *text)
{
    H.ev.on_text(H.ev.user, from, MY_ID, text, strlen(text));
}

static void inject_pulse(uint32_t from)
{
    uint8_t buf[FF_PROTO_ENVELOPE_LEN];
    int n = ff_proto_encode_pulse(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, FF_PORTNUM, buf, (size_t)n);
}

static void inject_flare(uint32_t from, uint16_t dur_s)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_flare(buf, sizeof(buf), dur_s);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, FF_PORTNUM, buf, (size_t)n);
}

static ff_crew_member_t const *member(uint32_t node)
{
    return ff_crew_find(ff_shell_crew(&H.shell), node);
}

void setUp(void) {}
void tearDown(void) {}

/* =================================================================== */
/* AC5a — a never-heard sender                                          */
/* =================================================================== */

static void S16_AC5a_unknown_sender_no_feed_no_crew_slot_one_heard_entry(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    /* Everything one real inbound packet actually produces: the radio
     * metadata event fires first (mc_client.h's documented ordering),
     * then the payload event. */
    inject_rx_meta(STRANGER, MC_RX_PATH_DIRECT, true, -55);
    inject_pulse(STRANGER);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_NULL(member(STRANGER));

    /* Exactly ONE entry, not one per callback: ff_heard_note refreshes
     * an already-tracked id in place. */
    TEST_ASSERT_EQUAL_UINT8(1, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER));

    /* Same for the other inbound shapes. */
    inject_text(STRANGER, "hey");
    inject_position(STRANGER, U_EVENING, 39.0, -82.0);
    inject_node(STRANGER, "STR", U_EVENING);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_EQUAL_UINT8(1, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);
}

/* =================================================================== */
/* AC5b — a known-but-unpaired member                                   */
/* =================================================================== */

static void S16_AC5b_known_unpaired_sender_no_feed_and_no_new_heard_entry(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    /* ff_shell_pair is the ONE entry point that may grow the roster.
     * Pairing then un-pairing models "known, deliberately not trusted". */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, false));
    TEST_ASSERT_EQUAL_UINT8(1, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_NOT_NULL(member(DANA));
    TEST_ASSERT_FALSE(member(DANA)->paired);

    inject_pulse(DANA);
    inject_text(DANA, "still here");

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);
    /* The sender is already tracked in the roster, so it is NOT also
     * pushed into the heard list. */
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(1, ff_shell_crew(&H.shell)->count);

    /* Positive control: the same path with the same member PAIRED does
     * push, so the assertions above are about the filter and not about a
     * feed that never works. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_text(DANA, "omw");
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
}

/* =================================================================== */
/* AC5c — a position from a node not in the roster                      */
/* =================================================================== */

static void S16_AC5c_position_from_non_roster_node_is_dropped_and_noted(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Latch the wall clock so the drops below cannot be blamed on "no
     * honest age was available" — the positive control at the end of
     * this test proves a position IS recordable at this instant. (The
     * derived wall clock itself still reads UNKNOWN here: nothing has
     * supplied a UTC offset. The latch and the offset are independent,
     * and only the latch is needed to age a position.) */
    inject_node(DANA, "DANA", U_EVENING);

    uint8_t const before = ff_shell_crew(&H.shell)->count;
    TEST_ASSERT_EQUAL_UINT8(1, before);

    /* ff_crew_on_position calls crew_find_or_create internally, so an
     * implementation that calls it straight from on_position satisfies
     * "we never call ff_crew_upsert" while growing the roster off a bare
     * Position — no name, no handshake. Roster count is the assertion
     * precisely because the function name is not. */
    inject_position(STRANGER, U_EVENING, 39.0, -82.0);
    inject_position(STRANGER2, U_EVENING, 39.1, -82.1);

    TEST_ASSERT_EQUAL_UINT8(before, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_NULL(member(STRANGER));
    TEST_ASSERT_NULL(member(STRANGER2));
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER));
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER2));

    /* Positive control: the identical call for a ROSTER member does
     * record, so "dropped" above is the trust filter and not a position
     * path that never works. */
    inject_position(DANA, U_EVENING, 39.2, -82.2);
    TEST_ASSERT_NOT_NULL(member(DANA));
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
}

/* =================================================================== */
/* AC9 — reconnect must not refresh position ages                       */
/* =================================================================== */

static void S16_AC9_transport_drop_moves_link_state_to_reconnecting(void)
{
    harness_init(100000u, false);

    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_NONE, ff_shell_link(&H.shell));

    H.ev.on_state(H.ev.user, MC_STATE_HANDSHAKE);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_RECONNECTING, ff_shell_link(&H.shell));

    H.ev.on_state(H.ev.user, MC_STATE_READY);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));

    /* The drop. mc_client's own failure path is
     * mc_fail_and_schedule_reconnect -> DISCONNECTED with a retry armed,
     * so DISCONNECTED after a successful link IS "reconnecting". */
    H.ev.on_state(H.ev.user, MC_STATE_DISCONNECTED);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_RECONNECTING, ff_shell_link(&H.shell));

    /* A stale view during reconnect must not present itself as live. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.mesh_ok);

    H.ev.on_state(H.ev.user, MC_STATE_READY);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->radar.mesh_ok);
}

static void S16_AC9_want_config_replay_does_not_refresh_position_age(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Latch the wall clock, then take a LIVE position at T. */
    inject_node(DANA, "DANA", U_EVENING);
    inject_position(DANA, U_EVENING, 39.0, -82.0);
    TEST_ASSERT_NOT_NULL(member(DANA));
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));

    /* Drop at T+40 s. */
    H.ev.on_state(H.ev.user, MC_STATE_READY);
    advance(40000u);
    H.ev.on_state(H.ev.user, MC_STATE_DISCONNECTED);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_RECONNECTING, ff_shell_link(&H.shell));

    /* Reconnect at T+5 min. The want_config replay arrives as on_node
     * carrying has_position + last_heard (the cached value, still T) and
     * NO rx_time. Stamping it with the local clock would read LIVE. */
    advance(260000u); /* now T+5 min */
    H.ev.on_state(H.ev.user, MC_STATE_HANDSHAKE);
    inject_node_with_position(DANA, U_EVENING, 39.0, -82.0);
    H.ev.on_state(H.ev.user, MC_STATE_READY);

    TEST_ASSERT_EQUAL_INT(FF_FRESH_STALE, ff_crew_freshness(member(DANA), H.clk.t));

    /* The same replay again at T+12 min. */
    advance(420000u); /* now T+12 min */
    inject_node_with_position(DANA, U_EVENING, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, ff_crew_freshness(member(DANA), H.clk.t));

    /* And — the trap underneath the trap — the replay must not have
     * dragged the WALL CLOCK backwards to the moment of the replay.
     * `last_heard` is by construction <= now, so offering a cached one to
     * ff_wall_observe disagrees with the latch by the node's staleness
     * and re-latches, pinning the puck's idea of "now" to the reconnect
     * instant. The ages above would still be right; every LATER one
     * would be wrong.
     *
     * Measured rather than reasoned about: a genuinely current position
     * (rx_time == the real now, 12 minutes after the original latch)
     * must read LIVE. If the wall had been dragged back, its age would
     * compute as -720 s — the future — and the fix would be dropped
     * entirely, leaving DANA LOST. */
    inject_position(DANA, U_EVENING + 720u, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
}

static void S16_AC9_replayed_position_with_no_last_heard_reads_never(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    /* Latch the wall from a POSITIONLESS NodeInfo, so the latch exists
     * independently of any position being aged against it, and prove it
     * works by replaying an OLDER cached position that the latch can
     * honestly age. KEV's outcome below is then about KEV's missing
     * timestamp and nothing else.
     *
     * This setup used to be `inject_node_with_position(DANA, U_EVENING)`
     * asserted LIVE — which was the D1 defect asserted as correct: DANA
     * both defined the latch and was aged against it, so age 0 was
     * guaranteed rather than measured. (PR #46 review, D1.) */
    inject_node(DANA, "DANA", U_EVENING);
    inject_node_with_position(DANA, U_EVENING - 120u, 39.0, -82.0);
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_STALE, ff_crew_freshness(member(DANA), H.clk.t)); /* 2 min: 45 s..10 min */

    /* last_heard == 0 is mc_client.h's "unknown". The position is not
     * recorded at all: ff_crew_on_position sets has_pos, and a recorded
     * position with a fabricated age is exactly the lie AC9 forbids. */
    inject_node_with_position(KEV_ID, 0u, 39.0, -82.0);

    TEST_ASSERT_NOT_NULL(member(KEV_ID));
    TEST_ASSERT_FALSE(member(KEV_ID)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(KEV_ID), H.clk.t));
    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(KEV_ID), H.clk.t));
}

static void S16_AC9_cold_boot_replay_is_never_stamped_fresh(void)
{
    /* THE CASE THE WARM TEST ABOVE CANNOT REACH (PR #46 review, D1).
     * `S16_AC9_want_config_replay_does_not_refresh_position_age` latches
     * the wall before the drop, so the replay is always aged against a
     * clock established earlier. On a COLD BOOT there is no such clock:
     * the shell learns the time from the very burst it is trying to age.
     *
     * The durable, ordering-independent form: whichever node carries the
     * greatest `last_heard` in the burst defines the latch, so
     * ff_wall_unix_now() returns exactly its timestamp and its position
     * ages to zero — LIVE, however stale it really is. Here the entire
     * nodeDB is six hours cold: the puck has heard nobody since last
     * night, and there is nothing fresh anywhere in the burst. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);
    inject_node_with_position(DANA, U_EVENING - 21600u, 39.0, -82.0); /* 6 h cold */

    /* Not recorded at all: we have just learned the time FROM this node,
     * so we have no independent evidence of how old its fix is. NEVER is
     * the honest answer, and ff_radar.h's renderer contract turns it into
     * "NO FIX YET" rather than a fabricated "LAST SEEN". */
    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(DANA), H.clk.t));
    TEST_ASSERT_FALSE(member(DANA)->has_pos);
}

static void S16_AC9_cold_boot_burst_ages_against_the_running_maximum(void)
{
    /* The other half, and the reason the fix is "don't age from the
     * reading that defined the latch" rather than "don't age on a cold
     * boot": a node whose `last_heard` does NOT move the latch is aged
     * normally, against the best estimate of "now" the puck has.
     *
     * Descending burst — the freshest node first, which is what pins the
     * clock; the older ones behind it are then genuinely measurable. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, STRANGER, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_node_with_position(STRANGER, U_EVENING - 120u, 39.2, -82.2);   /* newest: defines the latch */
    inject_node_with_position(KEV_ID, U_EVENING - 3600u, 39.1, -82.1);    /* 58 min behind it */
    inject_node_with_position(DANA, U_EVENING - 10800u, 39.0, -82.0);     /* 2 h 58 min behind it */

    /* The latch-definer is the one we cannot place in time. */
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(STRANGER), H.clk.t));

    /* The other two are measured against it and read honestly stale —
     * so the fix is not "record nothing", it is "record nothing we would
     * have to invent a number for". */
    TEST_ASSERT_TRUE(member(KEV_ID)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, ff_crew_freshness(member(KEV_ID), H.clk.t));
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, ff_crew_freshness(member(DANA), H.clk.t));

    /* Ascending order is the pessimistic case and must still never lie:
     * each node in turn advances the latch, so each is unplaceable. The
     * outcome is ordering-dependent; its HONESTY is not. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, STRANGER, true));

    inject_node_with_position(DANA, U_EVENING - 10800u, 39.0, -82.0);
    inject_node_with_position(KEV_ID, U_EVENING - 3600u, 39.1, -82.1);
    inject_node_with_position(STRANGER, U_EVENING - 120u, 39.2, -82.2);

    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(KEV_ID), H.clk.t));
    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(STRANGER), H.clk.t));

    /* And a live position afterwards still lands normally, so the burst
     * being unplaceable does not poison the session. */
    inject_position(DANA, U_EVENING, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
}

/* =================================================================== */
/* S18 slice b — settle-then-age the cold-boot replay burst (#50)        */
/* =================================================================== */

/** Drive the replay burst to its settle: the link reaches READY, then the
 *  first tick re-ages the buffer against the settled latch. Mirrors the
 *  want_config handshake completing, which is the burst-end signal. */
static void reach_ready_then_settle(void)
{
    H.ev.on_state(H.ev.user, MC_STATE_READY);
    ff_shell_tick(&H.shell, H.clk.t);
}

/**
 * AC1 (the headline #50 fix): ascending and descending replay of the SAME
 * node set produce the SAME freshness after the settle pass. Before slice b
 * the ascending order left every node NEVER (each in turn moved the latch
 * and was dropped) while descending read correctly — the ordering-dependent
 * pessimism this closes.
 */
static void S18b_AC1_ascending_and_descending_replay_agree_after_settle(void)
{
    uint32_t const t0 = 100000u;

    /* Descending burst — freshest first (the order that already read right). */
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, STRANGER, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node_with_position(STRANGER, U_EVENING, 39.2, -82.2);        /* newest: settles the latch */
    inject_node_with_position(KEV_ID, U_EVENING - 3600u, 39.1, -82.1);  /* 1 h behind */
    inject_node_with_position(DANA, U_EVENING - 10800u, 39.0, -82.0);   /* 3 h behind */
    reach_ready_then_settle();
    ff_freshness_t const desc_str = ff_crew_freshness(member(STRANGER), H.clk.t);
    ff_freshness_t const desc_kev = ff_crew_freshness(member(KEV_ID), H.clk.t);
    ff_freshness_t const desc_dana = ff_crew_freshness(member(DANA), H.clk.t);

    /* Ascending burst — oldest first (the #50-pessimistic order). Same nodes,
     * same last_heard, roster paired in the opposite order too. */
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, STRANGER, true));
    inject_node_with_position(DANA, U_EVENING - 10800u, 39.0, -82.0);
    inject_node_with_position(KEV_ID, U_EVENING - 3600u, 39.1, -82.1);
    inject_node_with_position(STRANGER, U_EVENING, 39.2, -82.2);
    reach_ready_then_settle();

    /* Order-independence: identical freshness for every node. */
    TEST_ASSERT_EQUAL_INT(desc_str, ff_crew_freshness(member(STRANGER), H.clk.t));
    TEST_ASSERT_EQUAL_INT(desc_kev, ff_crew_freshness(member(KEV_ID), H.clk.t));
    TEST_ASSERT_EQUAL_INT(desc_dana, ff_crew_freshness(member(DANA), H.clk.t));

    /* Pin the actual values so "agree" cannot be satisfied by both orders
     * being wrong the same way (the proxy check): the latch-definer stays
     * NEVER (we learned the time FROM it — D1), the two older nodes age
     * honestly against the settled clock. */
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, desc_str);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, desc_kev);  /* 1 h old */
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, desc_dana); /* 3 h old */
}

/**
 * AC2: a 3-hour-cached node in a burst whose maximum last_heard is "now"
 * reads LOST after the settle pass — not the pre-slice-b NEVER. The stronger
 * outcome the reviewer's PROBE_cold_boot_replay_stamps_cached_positions
 * asked for: precision recovered without ever claiming LIVE.
 */
static void S18b_AC2_three_hour_cached_node_reads_lost_after_settle(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    /* Ascending: the 3-hour node arrives first and momentarily defines the
     * latch (so pre-slice-b it stayed NEVER), then "now" supersedes it. */
    inject_node_with_position(DANA, U_EVENING - 10800u, 39.0, -82.0); /* 3 h cached */
    inject_node_with_position(KEV_ID, U_EVENING, 39.1, -82.1);        /* max last_heard = "now" */

    /* Pre-settle: the pessimistic NEVER, proving the defect is present to
     * be fixed (measure, don't assume). */
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(DANA), H.clk.t));
    TEST_ASSERT_FALSE(member(DANA)->has_pos);

    reach_ready_then_settle();

    /* After settle: LOST (3 h > FF_CREW_LOST_MS), aged against the node that
     * settled the latch at "now" — recovered, and honest. */
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LOST, ff_crew_freshness(member(DANA), H.clk.t));

    /* KEV defined the settled latch, so its own fix is still unplaceable. */
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(KEV_ID), H.clk.t));
    TEST_ASSERT_FALSE(member(KEV_ID)->has_pos);
}

/**
 * AC3: a node genuinely older than the window still reads NEVER after the
 * settle pass — precision recovery must never over-claim. Eight days is past
 * FF_WALL_LATCH_MAX_AGE_MS, where the monotonic delta stops being
 * unambiguous, so shell_rx_ms_from_unix refuses it and the fix is not
 * recorded at all (an honest under-claim, the direction this repo errs in).
 */
static void S18b_AC3_node_older_than_the_window_stays_never_after_settle(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    uint32_t const eight_days_s = 8u * 24u * 3600u; /* > FF_WALL_LATCH_MAX_AGE_MS (7 d) */
    inject_node_with_position(DANA, U_EVENING - eight_days_s, 39.0, -82.0);
    inject_node_with_position(KEV_ID, U_EVENING, 39.1, -82.1); /* settles the latch at "now" */

    reach_ready_then_settle();

    /* DANA is younger than the settled latch base, so it is NOT excluded as a
     * definer — it reaches shell_rx_ms_from_unix and is refused there for
     * being past the window. Stays NEVER, no fabricated fix. */
    TEST_ASSERT_FALSE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(DANA), H.clk.t));
}

/**
 * AC4: the settle buffer is FF_CREW_MAX-bounded and allocation-free, and an
 * overflow drops the oldest entry while bumping a bench-visible counter (the
 * same one `targets/sim/ctl_loop.c`'s `state` dump surfaces) — never a silent
 * discard.
 */
static void S18b_AC4_replay_buffer_is_bounded_and_surfaces_overflow(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_replay_overflow_count(&H.shell));

    /* One roster node re-sent FF_CREW_MAX + 1 times in a single burst, each
     * with a strictly newer last_heard so each moves the still-settling latch
     * and is buffered. Inbound traffic never grows the roster past
     * FF_CREW_MAX and only roster members are buffered, so a re-send storm on
     * one node is the way to exceed the bound. The fixed buffer allocates
     * nothing, so the (FF_CREW_MAX+1)th arrival drops the OLDEST and bumps
     * the counter. Spaced an hour apart so no surviving duplicate reads
     * fresh. */
    for (int i = 0; i <= FF_CREW_MAX; i++) {
        uint32_t const back_s = (uint32_t)(FF_CREW_MAX + 1 - i) * 3600u;
        inject_node_with_position(DANA, U_EVENING - back_s, 39.0, -82.0);
    }

    /* Bounded + visible: exactly one drop, surfaced, not silent. */
    TEST_ASSERT_EQUAL_UINT32(1, ff_shell_replay_overflow_count(&H.shell));

    /* And the truncated buffer still settles into a consistent, non-fabricated
     * state — DANA is at best hours stale here, never LIVE. */
    reach_ready_then_settle();
    TEST_ASSERT_NOT_NULL(member(DANA));
    TEST_ASSERT_NOT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
}

/* =================================================================== */
/* AC11 — haptics vs quiet hours                                        */
/* =================================================================== */

static void S16_AC11_should_alert_fires_during_quiet_hours_feed_push_does_not(void)
{
    harness_seed_settings(0); /* UTC, so U_QUIET's 05:00Z is 05:00 local */
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    TEST_ASSERT_TRUE(ff_shell_settings(&H.shell)->haptics);
    TEST_ASSERT_TRUE(ff_shell_settings(&H.shell)->utc_offset_set);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Put the puck inside quiet hours, and prove it rather than assume
     * it: 05:00 local is before the 06:00 festival-day roll, so it
     * belongs to the previous day at now_min 1740 — which ff_quiet_now
     * normalizes to 300, inside the default [240, 600) window. */
    inject_node(DANA, "DANA", U_QUIET);
    ff_wall_t w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1740, w.now_min);
    TEST_ASSERT_TRUE(ff_quiet_now(ff_shell_settings(&H.shell), w.now_min));

    /* A feed push during quiet hours: item lands, buzz does not. */
    H.haptic.count = 0;
    inject_text(DANA, "you up");
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);

    /* A flare during the same quiet hours: should_alert overrides. */
    H.haptic.count = 0;
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_INT(1, H.haptic.count);
}

static void S16_AC11_feed_push_haptic_does_fire_outside_quiet_hours(void)
{
    /* The positive control for the test above. Without it, "0 buzzes
     * during quiet hours" is equally satisfied by a haptic that is
     * simply never wired up. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_node(DANA, "DANA", U_AWAKE); /* 20:00 local */
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1200, w.now_min);
    TEST_ASSERT_FALSE(ff_quiet_now(ff_shell_settings(&H.shell), w.now_min));

    H.haptic.count = 0;
    inject_text(DANA, "at the main stage");
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_INT(1, H.haptic.count);

    /* And one flare is still exactly one buzz, not two, outside quiet
     * hours — the feed-push buzz is suppressed for a FLARE so the alert
     * is the one that lands. */
    H.haptic.count = 0;
    inject_flare(DANA, 300);
    TEST_ASSERT_EQUAL_INT(1, H.haptic.count);
}

static void S16_AC11_the_haptics_master_switch_silences_even_a_flare_alert(void)
{
    /* THE INTERPRETATION, PINNED (PR #46 review, D2). `should_alert`
     * overrides QUIET HOURS — a schedule. It does not override
     * `ff_settings_t.haptics`, the user's "stop buzzing" switch, because
     * those are different statements and the second one is not about a
     * time window at all. The flare is silenced, never swallowed: the
     * takeover still renders full-screen.
     *
     * This is the judgement call in this slice a reader is most likely
     * to disagree with, so it gets a test rather than only a paragraph —
     * otherwise a later slice can flip it either way and nothing says so.
     * Whether critical alerts should ignore the master switch is a real
     * product question, and S11/S12 owns it; it is not settled here by
     * implication. */
    ff_settings_t s;
    ff_settings_load(&s, NULL);
    s.haptics = false;
    s.utc_offset_min = 0;
    s.utc_offset_set = true;
    memset(&H.store_mem, 0, sizeof(H.store_mem));
    H.store = mem_store(&H.store_mem);
    ff_settings_save(&s, &H.store);

    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_FALSE(ff_shell_settings(&H.shell)->haptics);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_node(DANA, "DANA", U_AWAKE); /* 20:00 local — NOT quiet hours */
    TEST_ASSERT_FALSE(ff_quiet_now(ff_shell_settings(&H.shell), ff_shell_wall(&H.shell).now_min));

    H.haptic.count = 0;
    inject_text(DANA, "where you at");
    inject_flare(DANA, 300);

    /* Silenced — both of them, and outside quiet hours, so this is the
     * master switch and nothing else. */
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);

    /* But NOT swallowed: the takeover is up and the feed items landed. */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_UINT8(2, ff_feed_count(ff_shell_feed(&H.shell)));

    /* Positive control: the identical scene with the switch ON buzzes,
     * so "0" above is the switch and not a broken path. */
    ff_settings_load(&s, NULL);
    s.haptics = true;
    s.utc_offset_min = 0;
    s.utc_offset_set = true;
    memset(&H.store_mem, 0, sizeof(H.store_mem));
    H.store = mem_store(&H.store_mem);
    ff_settings_save(&s, &H.store);

    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_AWAKE);
    H.haptic.count = 0;
    inject_flare(DANA, 300);
    TEST_ASSERT_EQUAL_INT(1, H.haptic.count);
}

static void S16_AC11_unpaired_flare_neither_alerts_nor_takes_over(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    H.haptic.count = 0;
    inject_flare(STRANGER, 300);

    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
}

/* =================================================================== */
/* AC13 — active_face is never FLARE                                    */
/* =================================================================== */

static void S16_AC13_active_face_is_never_flare_even_during_a_takeover(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face);

    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* Across the whole life of the takeover: raised, held, expired. */
    for (int i = 0; i < 60; i++) {
        advance(5000u);
        ff_shell_tick(&H.shell, H.clk.t);
        ff_app_state_t const *v = ff_shell_view(&H.shell);
        TEST_ASSERT_NOT_EQUAL_INT(FF_APP_FACE_FLARE, v->active_face);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, v->active_face);
        /* The takeover DOES reach the screen — as ff_flare_t's single
         * fact, which face_dispatch.c already reads. If it did not, the
         * assertion above would be passing for the wrong reason. */
        TEST_ASSERT_EQUAL_INT(ff_shell_flare(&H.shell)->takeover_active, v->flare.takeover_active);
    }

    /* It really did go up and really did come down over that span. */
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
}

/* =================================================================== */
/* AC3b — SEND_TEXT/BACK rejected during a takeover; draft survives     */
/* (S16 slice c3)                                                       */
/* =================================================================== */

/**
 * S16 spec, AC3b: "While a takeover is visible, SEND_TEXT and BACK are
 * rejected and the draft is unchanged; clearing the takeover restores
 * dispatch to Compose with the draft intact." Unimplementable before
 * this slice per the Intents section ("scr_compose.c's `static ff_t9_t
 * s_t9`... has not moved yet") — the draft is typed through the real T9
 * intent seam (the same path a real keypress takes), not poked directly
 * into shell internals, so this is also the first test that exercises
 * T9_KEY/T9_SPACE end to end at the shell level.
 */
static void S16_AC3b_send_text_and_back_rejected_during_takeover_draft_survives(void)
{
    harness_init(U_EVENING, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    /* S08 addendum: the composer now opens in predictive mode; this test is
     * about the MULTITAP draft surviving a takeover, so switch to ABC first
     * (one T9_MODE press: PRED -> ABC). */
    ff_intent_t mode = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&H.shell, &mode);

    /* Type "a " via the real keypad intents: key 2 once (pending 'a'),
     * then space (commits the pending char, appends the space). */
    ff_intent_t key = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    key.u.t9_key = 2;
    ff_shell_intent(&H.shell, &key);
    ff_intent_t space = {.kind = FF_INTENT_T9_SPACE, .u = {0}};
    ff_shell_intent(&H.shell, &space);

    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    char draft_before[FF_APP_COMPOSE_TEXT_LEN];
    strncpy(draft_before, ff_shell_view(&H.shell)->compose.text, sizeof(draft_before) - 1u);
    draft_before[sizeof(draft_before) - 1u] = '\0';
    TEST_ASSERT_EQUAL_STRING("a ", draft_before);

    /* A flare arrives: the takeover is now the visible face (routing rule
     * 4), not Compose. */
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_intent_t send = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    ff_intent_t back = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(&H.shell, &back);

    (void)ff_shell_tick(&H.shell, H.clk.t);
    /* Rejected, not partially consumed: still Compose underneath, and the
     * exact same draft — not sent, not cleared, not popped. */
    TEST_ASSERT_EQUAL_STRING(draft_before, ff_shell_view(&H.shell)->compose.text);

    /* Clearing the takeover restores dispatch to Compose, draft intact. */
    ff_intent_t dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};
    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_STRING(draft_before, ff_shell_view(&H.shell)->compose.text);

    /* And SEND genuinely works once the seam is open again — proving the
     * earlier rejection was specifically "not while a takeover is up",
     * not "SEND_TEXT never does anything". */
    ff_shell_intent(&H.shell, &send);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face); /* SEND pops back to base */
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.text);       /* draft reset on send */
}

/* =================================================================== */
/* AC4(a) — the dirty bit over the RENDERED projection                  */
/* =================================================================== */

static void S16_AC4a_idle_shell_is_not_dirty_for_1000_ticks(void)
{
    harness_init(100000u, false);

    /* The first tick after init always reports a change: there is no
     * previous frame to be identical to. */
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));

    /* The clock genuinely advances — 1000 ticks at 20 ms is 20 s of real
     * elapsed time, not a frozen clock. A shell with nothing on it has
     * nothing whose rendering depends on that. */
    for (int i = 0; i < 1000; i++) {
        advance(20u);
        TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));
    }
}

static void S16_AC4a_dirty_is_the_rendered_projection_not_the_raw_struct(void)
{
    /* THE MEASUREMENT (docs/review/code-review.md, item 6: instrument it,
     * do not reason harder). The idle test above passes just as happily
     * against a whole-struct memcmp, which is the implementation S16
     * names as wrong. So run a LIVE scene and compare the shell's answer
     * against what a raw whole-struct comparison would have said, on the
     * same frames.
     *
     * The scene has exactly two rendered quantities that move with time:
     * the takeover countdown (whole seconds, what flare_fmt prints) and
     * the feed item's age_str. Each crosses at most once per second, so
     * over N seconds at 50 Hz the shell may report at most 2N changes —
     * while the raw struct differs on every single tick, because
     * takeover_expires_in_ms drops by 20 every time. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_flare(DANA, 300);

    ff_shell_tick(&H.shell, H.clk.t); /* first frame: always dirty */

    int const seconds = 4;
    int const ticks = seconds * 50;
    int shell_dirty = 0;
    int raw_dirty = 0;

    ff_app_state_t prev_raw;
    memcpy(&prev_raw, ff_shell_view(&H.shell), sizeof(prev_raw));

    for (int i = 0; i < ticks; i++) {
        advance(20u);
        if (ff_shell_tick(&H.shell, H.clk.t)) shell_dirty++;

        ff_app_state_t const *v = ff_shell_view(&H.shell);
        if (memcmp(v, &prev_raw, sizeof(prev_raw)) != 0) raw_dirty++;
        memcpy(&prev_raw, v, sizeof(prev_raw));
    }

    /* A whole-struct memcmp would have repainted every single frame. */
    TEST_ASSERT_EQUAL_INT(ticks, raw_dirty);

    /* The shell repainted only when something rendered actually changed:
     * at most one crossing per second per moving string. */
    TEST_ASSERT_LESS_OR_EQUAL_INT(2 * seconds, shell_dirty);
    /* ...and it did not go silent either — the countdown really is
     * ticking, so at least one change per second must be reported or the
     * screen would freeze with a stale number on it. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(seconds, shell_dirty);
}

/* =================================================================== */
/* Supporting behaviour the ACs above depend on                         */
/* =================================================================== */

static void S16_b1_wall_is_unknown_until_a_plausible_timestamp_arrives(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* An uncorrected pre-GPS-lock RTC is below the plausibility floor and
     * must not latch. */
    inject_node(DANA, "DANA", 1000000u);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* With no latch there is no honest clock string — scr_radar renders
     * an empty clock_str as "--:--", never an invented time. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->radar.clock_str);

    /* A plausible timestamp latches, but the wall is STILL unknown:
     * nothing has supplied a UTC offset, and a defaulted guess is not an
     * answer (ff_wall_resolve_offset). The latch and the offset are
     * independent facts and both are required. */
    inject_node(DANA, "DANA", U_EVENING);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->radar.clock_str);

    /* Positive control: with a configured offset, the same latch
     * resolves — so "UNKNOWN" above is the honesty rule, not a wall
     * clock that never works. 22:00Z at UTC+0 is 22:00 local. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    inject_node(DANA, "DANA", U_EVENING);

    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1320, w.now_min);
    TEST_ASSERT_EQUAL_UINT16(261, w.day_doy);

    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("22:00", ff_shell_view(&H.shell)->radar.clock_str);
}

static void S16_b1_positions_are_never_stamped_from_the_local_clock(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* A live over-the-air position with NO rx_time. The local clock says
     * "it arrived now" and that is exactly the answer S16 forbids: with
     * no receive timestamp there is no honest age, so the fix is not
     * recorded and freshness stays NEVER. */
    inject_position(DANA, 0u, 39.0, -82.0);
    TEST_ASSERT_FALSE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_NEVER, ff_crew_freshness(member(DANA), H.clk.t));

    /* An rx_time below the plausibility floor is an uncorrected RTC, not
     * a time: it neither latches the wall nor ages the fix. */
    inject_position(DANA, 1000000u, 39.0, -82.0);
    TEST_ASSERT_FALSE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* Positive control: a plausible rx_time both latches and ages, so
     * the two drops above are about the timestamps and not about a
     * position path that never works. */
    inject_position(DANA, U_EVENING, 39.0, -82.0);
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));
}

static void S16_b1_own_traffic_is_not_treated_as_inbound(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    /* mc_client.h warns that self-packets are NOT filtered by the
     * library: an echoed packet arrives with from == my_node_id and
     * would otherwise claim a roster or heard slot for ourselves. */
    inject_rx_meta(MY_ID, MC_RX_PATH_DIRECT, true, -30);
    inject_position(MY_ID, U_EVENING, 39.0, -82.0);
    inject_text(MY_ID, "echo");
    inject_pulse(MY_ID);

    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT32(MY_ID, ff_shell_my_node_id(&H.shell));
}

static void S16_b1_rssi_is_attributed_only_on_a_direct_path(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* A relayed packet's RSSI belongs to the RELAY, not to `from`.
     * ff_crew_close_range turns exactly this number into a CLOSE lock,
     * so attributing it would put a distant friend "next to you". */
    inject_rx_meta(DANA, MC_RX_PATH_INDIRECT, true, -40);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, member(DANA)->rssi_dbm);

    inject_rx_meta(DANA, MC_RX_PATH_UNKNOWN, true, -40);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, member(DANA)->rssi_dbm);

    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -40);
    TEST_ASSERT_EQUAL_INT16(-40, member(DANA)->rssi_dbm);
}


/**
 * #35 remainder — the has_rssi gate, the third and previously UNPINNED
 * condition (PR #67 review: mutating it out passed all 35 tests, because
 * nothing ever injected meta without a reading). A DIRECT packet from a
 * paired peer whose meta carries NO rssi (has_rssi=false, per #39's
 * plausibility gate: NaN, out-of-range, or simply absent on the wire)
 * must not record anything — rssi_dbm's INT16_MIN "never direct" state
 * survives, and whatever garbage rides the value field is never read.
 */
static void S16_b1_rssi_absent_reading_records_nothing_even_direct_and_paired(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, false, -40);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, member(DANA)->rssi_dbm);

    /* Positive control: same call, flag true, records. */
    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -40);
    TEST_ASSERT_EQUAL_INT16(-40, member(DANA)->rssi_dbm);
}

/**
 * #35 remainder — the paired gate. A roster slot can exist and be
 * unpaired: known-but-never-trusted, or paired-then-unpaired
 * (S16_AC5b above models exactly this with ff_shell_pair(..., false)).
 * ff_wiring.c:42 applies the identical rule to feed pushes
 * ("`if (!m->paired) return;` known ... but not trusted"); on_rx_meta
 * must apply it too, or a merely-heard node's radio signal could satisfy
 * ff_crew_close_range's CLOSE predicate for someone the wearer never
 * chose to trust — the standing trust rule (CLAUDE.md / AGENTS.md
 * "never create" via ff_crew_find), extended to the RSSI wire. */
static void S16_b1_rssi_never_recorded_for_a_known_but_unpaired_sender(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, false));
    TEST_ASSERT_FALSE(member(DANA)->paired);

    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -40);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, member(DANA)->rssi_dbm);

    /* Positive control: re-pairing the SAME id and re-sending the SAME
     * DIRECT packet does record it, so the drop above is about the
     * paired gate specifically, not about a path that never works. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -40);
    TEST_ASSERT_EQUAL_INT16(-40, member(DANA)->rssi_dbm);
}


/**
 * #35 remainder — the full chain, end to end. Earlier tests in this
 * file pin the two gates individually (DIRECT-only, paired-only) at the
 * `ff_crew_member_t.rssi_dbm` level; this one drives the SAME
 * `on_rx_meta` entry point all the way through `ff_radar_compute` to
 * `radar.mode`, because "the field got set" and "CLOSE actually fires"
 * are different claims (the proxy this project's standing brief warns
 * about — an input could satisfy the first without the second, e.g. if
 * ff_radar_compute's own NOFIX/CLOSE priority order ever regressed).
 *
 * DANA's own GPS fix is deliberately never sent: CLOSE must come from
 * RSSI alone, matching the S06 spec's documented case ("a member can be
 * RSSI-close even with a GPS-stale/lost/never position", ff_radar.h).
 * My own position/heading ARE set, since RADAR_NOFIX outranks CLOSE
 * (ff_radar.h's priority order) — without them CLOSE could never show
 * regardless of RSSI, which would make this test meaningless.
 */
static void S16_b1_close_mode_triggers_live_via_direct_rssi_from_paired_peer(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.936, -82.414});

    ff_shell_tick(&H.shell, H.clk.t);
    /* Baseline: paired, my own fix known, DANA never sent a fix or an
     * RSSI sample — FF_FRESH_NEVER folds into RADAR_LOST (ff_radar.h),
     * distinguishable from a real stale fix by the empty age_str. Not
     * CLOSE: nothing has told the radar DANA is close yet. */
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, ff_shell_view(&H.shell)->radar.mode);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->radar.age_str);

    /* Negative control: an INDIRECT packet, even a loud one, must not
     * move the mode — a relay's signal is not a distance proxy for the
     * originator (the whole reason mc_rx_path_t exists; issue #35's
     * hardware findings caught exactly this on a live mesh). */
    inject_rx_meta(DANA, MC_RX_PATH_INDIRECT, true, -40);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, ff_shell_view(&H.shell)->radar.mode);

    /* The real thing: a live DIRECT packet from a paired peer, through
     * the actual on_rx_meta callback — no test seam bypassing it. */
    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -50);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_CLOSE, ff_shell_view(&H.shell)->radar.mode);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.arrow_valid);

    /* Age-out: FF_CREW_CLOSE_RANGE_RSSI_AGE_MS later with no fresh
     * sample, the RSSI leg of ff_crew_close_range goes stale and CLOSE
     * must release — stale RSSI must not hold a CLOSE lock forever.
     * Falls back to the same paired-but-never-fixed LOST reading as the
     * baseline, since DANA still has no position fix of her own. */
    advance(FF_CREW_CLOSE_RANGE_RSSI_AGE_MS);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_LOST, ff_shell_view(&H.shell)->radar.mode);
}

/* ------------------------------------------------------------------- */
/* a tiny in-line festpack (S05 schema v0.1)                            */
/* ------------------------------------------------------------------- */

/* One stage; one set with known times on 2026-09-18 (day-of-year 261),
 * one set on the same day whose times are still unknown — the "some
 * known, most still not" shape S07's NOW_MIXED exists for, and the
 * expected near-term state of the real Lost Lands pack. `utc_offset_min`
 * is STATED (0), so it outranks any settings offset. */
static char const PACK_JSON[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Headliner\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"21:00\",\"end\":\"23:00\"},"
    "{\"artist\":\"TBA Act\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":null,\"end\":null}]}";

/* 2026-08-24 amendment (S07-now-face.md ## Amendments, "starts-only set
 * grids") — same shape as PACK_JSON above except the headliner's `end`
 * is null, matching the real Bass Canyon 2026 pack's actual publishing
 * convention (82 starts, zero ends). Before this amendment,
 * shell_project_now's lineup filter required BOTH start_min and
 * end_min before excluding a set from the unknown-time lineup, so this
 * exact shape put the headliner in `lineup` even though its start time
 * IS known — the bug this test locks in the fix for. */
static char const PACK_JSON_STARTS_ONLY[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Headliner\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"21:00\",\"end\":null},"
    "{\"artist\":\"TBA Act\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":null,\"end\":null}]}";

static void S07_2026_08_24_starts_only_set_is_live_not_lineup(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_STARTS_ONLY, sizeof(PACK_JSON_STARTS_ONLY) - 1u));

    /* 22:00 local on day-of-year 261 — same clock latch as the sibling
     * PACK_JSON test above. */
    inject_node(DANA, "DANA", U_EVENING);
    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_now_t const *n = &ff_shell_view(&H.shell)->now;

    /* Headliner has a real, known start_min: it belongs in `rows`, live,
     * NOT in the unknown-time lineup — even though its end_min is null.
     * No other set shares its stage/day, so its true end is genuinely
     * unknowable: pct_valid must be false, not silently 0-and-claimed. */
    TEST_ASSERT_EQUAL_INT(NOW_MIXED, n->state);
    TEST_ASSERT_EQUAL_UINT8(1, n->n_rows);
    TEST_ASSERT_EQUAL_STRING("Headliner", n->rows[0].artist);
    TEST_ASSERT_FALSE(n->rows[0].pct_valid);

    /* Only the genuinely timeless act is in the lineup. */
    TEST_ASSERT_EQUAL_UINT8(1, n->n_lineup);
    TEST_ASSERT_EQUAL_STRING("TBA Act", n->lineup[0].artist);
}

/* S09 — a pack whose map.features carries one traced polygon, for
 * shell_project_map's live-projection coverage below. Venue matches
 * PACK_JSON's (39.936, -82.414) so the two share an origin story. */
static char const PACK_JSON_MAP[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Solo Act\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"20:00\",\"end\":\"21:00\"}],"
    "\"map\":{\"features\":["
    "{\"kind\":\"water\",\"label\":\"Pond\",\"polygon\":["
    "[39.937,-82.414],"
    "[39.937,-82.413],"
    "[39.936,-82.413]]}"
    "]}}";

/**
 * S09 — shell_project_map's live projection: a pack's own feature
 * (already carrying its projected east/north from fp_parse), a paired
 * crew member's position (projected here against the SAME origin), and
 * my own position/heading, all reaching `ff_app_state_t.map` through one
 * tick. Pins that Map is wired into the live shell, not only the
 * fixture-driven golden path — the honesty-mapped freshness/imprecision
 * flags (issue #33/#47's vocabulary reused per the task brief) are
 * covered by the `stale`/`imprecise` assertions below.
 */
static void S09_shell_projects_map_from_pack_crew_and_my_position(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);

    /* No pack yet: the map is honestly empty, not a guessed origin. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_view(&H.shell)->map.n_features);
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_view(&H.shell)->map.n_crew);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->map.you_has_pos);

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_MAP, sizeof(PACK_JSON_MAP) - 1u));

    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(DANA, "DANA", U_EVENING); /* latches the wall clock too */
    inject_position(DANA, U_EVENING, 39.9365, -82.4135); /* LIVE — fresh fix near the venue */
    inject_node(KEV_ID, "KEV", U_EVENING); /* name only — latch already set, this doesn't move it */
    /* A want_config REPLAY (not another on_position) for KEV's stale fix,
     * deliberately: on_position's rx_time re-latches the wall
     * UNCONDITIONALLY in both directions (S16's own documented rule —
     * "the authoritative re-latch source... offered unconditionally"),
     * so a second inject_position() with an OLDER rx_time here would
     * drag the just-latched wall clock backward with it instead of
     * producing an honestly-older KEV fix. The NodeInfo replay path is
     * guarded the other way (an EARLIER-than-latched reading is ignored,
     * not relatched), which is exactly the cached/replayed-position
     * shape this assertion wants. */
    inject_node_with_position(KEV_ID, U_EVENING - 300u, 39.935, -82.415); /* STALE — 5 min older than the latch */

    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.936, -82.414}); /* exactly the venue */
    ff_shell_set_heading(&H.shell, 90.0f);

    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_map_t const *m = &ff_shell_view(&H.shell)->map;

    /* The feature: copied straight through from the pack, already
     * projected at parse time. */
    TEST_ASSERT_EQUAL_UINT8(1, m->n_features);
    TEST_ASSERT_EQUAL_INT(FF_APP_MAP_KIND_WATER, m->features[0].kind);
    TEST_ASSERT_EQUAL_STRING("Pond", m->features[0].label);
    TEST_ASSERT_EQUAL_UINT8(3, m->features[0].n_pts);

    /* Crew: only PAIRED members with a position reach the map, same gate
     * ff_radar_compute's dots[] uses. Both DANA and KEV have one. */
    TEST_ASSERT_EQUAL_UINT8(2, m->n_crew);
    bool saw_dana = false, saw_kev = false;
    for (uint8_t i = 0; i < m->n_crew; i++) {
        TEST_ASSERT_TRUE(m->crew[i].has_pos);
        if (m->crew[i].initial == 'D') {
            saw_dana = true;
            TEST_ASSERT_FALSE(m->crew[i].stale); /* fresh fix, same tick as the latch */
        } else if (m->crew[i].initial == 'K') {
            saw_kev = true;
            TEST_ASSERT_TRUE(m->crew[i].stale); /* 5 min old at freshness-check time */
        }
    }
    TEST_ASSERT_TRUE(saw_dana);
    TEST_ASSERT_TRUE(saw_kev);

    /* YOU: my own position/heading, projected against the same origin —
     * exactly at the venue, so east/north read ~0. */
    TEST_ASSERT_TRUE(m->you_has_pos);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, m->you_east_m);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, m->you_north_m);
    TEST_ASSERT_TRUE(m->you_heading_valid);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, m->you_heading_deg);

    /* AC5's negative half: clearing my position drops the fix and the
     * heading claim together — "no fix" is the honest state, not a
     * stale arrow pointed at a stale spot. */
    ff_shell_clear_my_pos(&H.shell);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->map.you_has_pos);
}

/* S09 — a pack whose venue is UNKNOWN (S05's own honesty rule: any
 * projected east_m/north_m is meaningless without one). Every feature
 * still parses, but the map must render nothing rather than trust a
 * (0,0)-origin guess. */
static char const PACK_JSON_MAP_NO_VENUE[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":null,\"lon\":null}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Solo Act\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"20:00\",\"end\":\"21:00\"}],"
    "\"map\":{\"features\":["
    "{\"kind\":\"water\",\"label\":\"Pond\",\"polygon\":["
    "[39.937,-82.414],"
    "[39.937,-82.413],"
    "[39.936,-82.413]]}"
    "]}}";

static void S09_shell_map_stays_empty_without_a_known_venue(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_MAP_NO_VENUE, sizeof(PACK_JSON_MAP_NO_VENUE) - 1u));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);
    inject_position(DANA, U_EVENING, 39.9365, -82.4135);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.936, -82.414});

    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_map_t const *m = &ff_shell_view(&H.shell)->map;
    TEST_ASSERT_EQUAL_UINT8(0, m->n_features);
    TEST_ASSERT_EQUAL_UINT8(0, m->n_crew);
    TEST_ASSERT_FALSE(m->you_has_pos);
}

/* PR #73 review finding #5 (LOW, mutation-uncaught): the paired-only
 * crew gate in shell_project_map (`if (!m->paired || !m->has_pos)
 * continue;`) had no test proving the PAIRED half of that condition
 * does anything — mutating it to `if (!m->has_pos) continue;` still
 * passed 100% before this test existed. STRANGER has a real position
 * but is never paired; DANA is both. */
static void S09_shell_map_excludes_unpaired_members_even_with_a_position(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_MAP, sizeof(PACK_JSON_MAP) - 1u));

    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);
    inject_position(DANA, U_EVENING, 39.9365, -82.4135);

    /* STRANGER gets a KNOWN-BUT-UNPAIRED roster slot FIRST (ff_shell_pair
     * returns true — it succeeds at "set paired = false", which for a
     * brand-new slot also means "create it unpaired"; roster growth only
     * ever happens through this one audited path — shell_pair's doc
     * comment). That slot has to exist BEFORE inject_node/inject_position
     * can attach anything to it: shell_member (both callbacks' shared
     * lookup) only MUTATES an existing slot, matching S16 AC5a/5b's
     * roster-trust policy — an unknown sender's NodeInfo/Position both go
     * to ff_heard instead, which would silently make this test assert
     * nothing at all if the ordering were reversed. This exercises the
     * `paired` flag specifically, not merely "unknown sender". */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, STRANGER, false));
    inject_node(STRANGER, "STRANGER", U_EVENING);
    inject_position(STRANGER, U_EVENING, 39.937, -82.412);

    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_map_t const *m = &ff_shell_view(&H.shell)->map;

    TEST_ASSERT_EQUAL_UINT8(1, m->n_crew); /* DANA only */
    TEST_ASSERT_EQUAL_CHAR('D', m->crew[0].initial);
}

/* PR #73 review finding #1 (BLOCKING): a synthetic pack with MORE
 * features than FF_APP_MAP_MAX_FEATURES (20) — but still within
 * fp_pack.h's own FP_MAX_FEATURES (24), so `fp_parse` itself accepts it
 * cleanly — proving `shell_project_map` caps at exactly 20, keeps them
 * in pack order, and sets `truncated`/`features_omitted` rather than
 * silently dropping the rest. Built at runtime (22 near-identical
 * one-point features) rather than hand-typed, since the point is the
 * COUNT, not any individual feature's content. */
static void build_pack_json_with_n_map_features(char *buf, size_t bufsz, int n_features)
{
    int off = snprintf(buf, bufsz,
                        "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
                        "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
                        "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
                        "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
                        "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
                        "\"schedule\":[{\"artist\":\"Solo Act\",\"stage\":\"a\",\"day\":\"2026-09-18\","
                        "\"start\":\"20:00\",\"end\":\"21:00\"}],"
                        "\"map\":{\"features\":[");
    for (int i = 0; i < n_features && off > 0 && (size_t)off < bufsz; i++) {
        off += snprintf(buf + off, bufsz - (size_t)off,
                         "%s{\"kind\":\"poi\",\"label\":\"F%d\",\"polygon\":[[39.93%d,-82.41%d]]}", i > 0 ? "," : "",
                         i, i % 10, i % 10);
    }
    if (off > 0 && (size_t)off < bufsz) {
        snprintf(buf + off, bufsz - (size_t)off, "]}}");
    }
}

static void S09_shell_caps_features_at_the_view_limit_and_surfaces_truncation(void)
{
    char pack_json[4096];
    int const n_real_features = 22; /* > FF_APP_MAP_MAX_FEATURES(20), <= FP_MAX_FEATURES(24) */
    build_pack_json_with_n_map_features(pack_json, sizeof(pack_json), n_real_features);

    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, pack_json, strlen(pack_json)));

    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_map_t const *m = &ff_shell_view(&H.shell)->map;

    TEST_ASSERT_EQUAL_UINT8(FF_APP_MAP_MAX_FEATURES, m->n_features); /* capped, not silently fewer */
    TEST_ASSERT_TRUE(m->truncated);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(n_real_features - FF_APP_MAP_MAX_FEATURES), m->features_omitted);
    /* Kept in pack order — the first FF_APP_MAP_MAX_FEATURES labels,
     * not an arbitrary subset. */
    TEST_ASSERT_EQUAL_STRING("F0", m->features[0].label);
    TEST_ASSERT_EQUAL_STRING("F19", m->features[FF_APP_MAP_MAX_FEATURES - 1].label);
}

/* A pack within both caps must NOT claim truncation — the negative
 * control for the test above (an always-true `truncated` would pass the
 * positive test just as well). */
static void S09_shell_does_not_claim_truncation_for_a_pack_within_caps(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_MAP, sizeof(PACK_JSON_MAP) - 1u));
    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_map_t const *m = &ff_shell_view(&H.shell)->map;
    TEST_ASSERT_FALSE(m->truncated);
    TEST_ASSERT_EQUAL_UINT8(0, m->features_omitted);
}

static void S16_b1_now_projection_needs_both_a_pack_and_a_known_clock(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON, sizeof(PACK_JSON) - 1u));

    /* A pack is loaded, but the puck does not know what time it is yet —
     * the normal cold-boot path. Issue #48 (resolved): the projection
     * uses NOW_TIME_UNKNOWN, a distinct member naming the actual missing
     * fact (the clock), not NOW_NO_PACK (which would mis-claim there's no
     * pack) or NOW_TBD (which would be a claim about the DATA the
     * projection never even looked at). See shell_project_now's doc
     * comment and now_state_t's own (ff_app_state.h). */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);
    TEST_ASSERT_EQUAL_INT(NOW_TIME_UNKNOWN, ff_shell_view(&H.shell)->now.state);

    /* Latch the clock. The pack's STATED offset resolves it with no
     * settings involvement at all. 22:00Z at UTC+0 is 22:00 local on
     * day-of-year 261. */
    inject_node(DANA, "DANA", U_EVENING);
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_FALSE(w.offset_assumed);
    TEST_ASSERT_EQUAL_UINT16(261, w.day_doy);
    TEST_ASSERT_EQUAL_INT16(1320, w.now_min);

    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_now_t const *n = &ff_shell_view(&H.shell)->now;

    /* One set playing, one still timeless: NOW_MIXED, and the timeless
     * one is LISTED alongside rather than disappearing. */
    TEST_ASSERT_EQUAL_INT(NOW_MIXED, n->state);
    TEST_ASSERT_EQUAL_UINT8(1, n->n_rows);
    TEST_ASSERT_EQUAL_STRING("Headliner", n->rows[0].artist);
    TEST_ASSERT_EQUAL_STRING("A Stage", n->rows[0].stage_name);
    TEST_ASSERT_TRUE(n->rows[0].stage_color_valid);
    TEST_ASSERT_EQUAL_HEX32(0x0000FF00u, n->rows[0].stage_color_rgb);
    TEST_ASSERT_EQUAL_UINT8(50, n->rows[0].pct_done); /* 22:00 of 21:00-23:00 */

    TEST_ASSERT_EQUAL_UINT8(1, n->n_lineup);
    TEST_ASSERT_EQUAL_STRING("TBA Act", n->lineup[0].artist);

    /* Nothing is starred, so there is no honest "next". */
    TEST_ASSERT_FALSE(n->next.valid);

    /* And the clock now renders. */
    TEST_ASSERT_EQUAL_STRING("22:00", ff_shell_view(&H.shell)->radar.clock_str);
}

/* Issue #48 — the narrowed-back-down half of the ruling: with NO pack
 * loaded at all, the projection is NOW_NO_PACK regardless of clock
 * state, even once the clock DOES latch. Pins that NOW_TIME_UNKNOWN is
 * never reachable without a pack (it means "pack loaded, clock not" —
 * not "clock not", full stop) and that NOW_NO_PACK's meaning is back to
 * literally "no pack loaded". Mutation check: swapping
 * shell_project_now's two early-return checks (clock check before the
 * pack check) would make this fail the moment the clock latches below,
 * since a swapped order falls through past the pack-missing return once
 * wall.src != FF_WALL_UNKNOWN.
 *
 * The clock needs a resolvable UTC offset to ever report FF_WALL_MESH
 * (ff_wall_now bails to UNKNOWN if ff_wall_resolve_offset fails — see
 * that function in ff_wall.c), and with no pack loaded the only source
 * left is settings — same seed-then-load pattern
 * S16_b1_failed_pack_load_does_not_outrank_the_settings_offset uses just
 * above. */
static void S48_now_no_pack_holds_regardless_of_clock_state(void)
{
    harness_seed_settings(-300); /* UTC-5, arbitrary but non-zero */
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    /* No ff_shell_load_pack call at all — clock still unknown too. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);
    TEST_ASSERT_EQUAL_INT(NOW_NO_PACK, ff_shell_view(&H.shell)->now.state);

    /* Latch the clock via a mesh NodeInfo. Still no pack loaded. */
    inject_node(DANA, "DANA", U_EVENING);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, ff_shell_wall(&H.shell).src);

    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(NOW_NO_PACK, ff_shell_view(&H.shell)->now.state);
}

/* S18 slice c (#40) — the shell wires ff_shell_load_pack to tighten the
 * wall-clock plausibility window to the loaded pack's festival dates. The
 * derivation and the core seam are unit-tested in test_wall_window.c /
 * test_wall.c; this pins that the SHELL actually calls ff_wall_set_window
 * on load, using the real load_pack path and the dev-clock observe hook
 * (the same hook, and the same plausibility gate, a live rx_time hits).
 *
 * Discriminator: 2029-09-18 22:00 UTC (1884463200) is INSIDE the fixed
 * [FLOOR, CEILING) window (< 2030-08-01) but OUTSIDE the Lost Lands 2026
 * window. So its acceptance flips on whether the window tightened. */
#define U_SEP2029 ((int64_t)1884463200) /* 2029-09-18 22:00 UTC */

static void S18c_no_pack_keeps_the_fixed_window(void)
{
    /* AC1: with no pack loaded the fixed bootstrap window is in force, so a
     * 2029 stamp (inside it) is accepted. This is the control that proves
     * the 2029 rejection below is the tightening, not some other gate. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_dev_wall_observe(&H.shell, U_SEP2029));
}

static void S18c_loading_lost_lands_tightens_the_window(void)
{
    /* AC2: PACK_JSON carries Lost Lands' Sep 18-20 2026 dates. After load,
     * the window is [Sep 4, Oct 5 2026]-ish: a Sep 2026 stamp is plausible,
     * a Sep 2029 stamp is rejected — even though 2029 cleared the fixed
     * window in S18c_no_pack_keeps_the_fixed_window above. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON, sizeof(PACK_JSON) - 1u));

    /* 2029 is now outside the tightened window: the gate refuses it. */
    TEST_ASSERT_FALSE(ff_shell_dev_wall_observe(&H.shell, U_SEP2029));
    /* A genuine Sep 2026 festival time is still accepted — narrowing never
     * rejects a real festival-time reading (S18 honest-data brief). */
    TEST_ASSERT_TRUE(ff_shell_dev_wall_observe(&H.shell, (int64_t)U_EVENING));
}

static void S16_b1_loading_a_pack_does_not_fabricate_my_position(void)
{
    /* The retired targets/sim/live.c adopted the pack's venue origin
     * as "my position". That is a dev-harness affordance: the venue
     * centre is not where the wearer is standing, and asserting it as a
     * fix would fabricate a position (CLAUDE.md: "never fake ...
     * positions"). The shell must not inherit it — a target that wants
     * it for development calls ff_shell_set_my_pos itself, visibly,
     * which is exactly what main.c's --pack block now does. */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON, sizeof(PACK_JSON) - 1u));
    inject_node(DANA, "DANA", U_EVENING);
    inject_position(DANA, U_EVENING, 39.937, -82.415);
    TEST_ASSERT_TRUE(member(DANA)->has_pos);

    /* DANA has a fresh fix and there is a pack with a known venue, yet
     * the radar honestly reports NOFIX: "no position of MINE to compare
     * against" is a true statement. */
    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_NOFIX, ff_shell_view(&H.shell)->radar.mode);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.arrow_valid);

    /* Positive control: told explicitly where we are, it computes. */
    ff_latlon_t const me = {39.936, -82.414};
    ff_shell_set_my_pos(&H.shell, me);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, ff_shell_view(&H.shell)->radar.mode);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->radar.arrow_valid);
}

static void S16_b1_our_own_nodeinfo_can_bootstrap_the_wall_clock(void)
{
    /* shell_ev_node offers `last_heard` to the latch BEFORE the
     * self-check, deliberately: our own node's NodeInfo carries the
     * freshest `last_heard` in the dump (meshtasticd keeps it current),
     * so it is the tightest bootstrap available and the one that best
     * pins the "running maximum" every other node's age is measured
     * against. Moving the observe below the self-check broke nothing in
     * this suite (PR #46 review, D4) — and after D1 made latch
     * provenance load-bearing, that gap is worth closing. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* Only our OWN NodeInfo arrives. It must still latch, while
     * correctly claiming no roster or heard slot for ourselves. */
    inject_node(MY_ID, "ME", U_EVENING);

    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1320, w.now_min);
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(ff_shell_heard(&H.shell)));
}

/* =================================================================== */
/* S18 slice a — trust-gated wall-clock re-latch (issue #49)            */
/* =================================================================== */

static void S18_AC1_shell_bootstrap_from_unpaired_stranger_still_works(void)
{
    /* Cold start through the real shell entry points: STRANGER has never
     * been heard from, is not in the roster, and its live Position still
     * bootstraps the wall — establishing accepts any tier. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    inject_position(STRANGER, U_EVENING, 39.0, -82.0);

    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1320, w.now_min); /* 22:00 */
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S18_AC2_second_unpaired_stranger_cannot_move_the_wall_clock(void)
{
    /* The exact #49 repro, end to end through the real shell entry
     * points: a first unpaired stranger's live Position bootstraps the
     * latch (necessarily BOOTSTRAP-tier — that is what a cold start is),
     * and a SECOND, entirely different unpaired stranger's Position then
     * disagrees by more than FF_WALL_RELATCH_DELTA_S. shell_wall_trust_for
     * classifies BOTH as BOOTSTRAP via ff_crew_find (which never
     * creates — neither is ever in the roster). The headline fix: the
     * second stranger's disagreeing reading is REJECTED, now_min is
     * UNCHANGED, and the rejection is counted (AC5). This must hold
     * specifically because the ORIGINATING latch was itself
     * stranger-bootstrapped — the gate does not care what tier
     * established the latch, only the tier of the incoming reading. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));

    /* Stranger #1 bootstraps: 22:00 UTC == 22:00 local at offset 0. */
    inject_position(STRANGER, U_EVENING, 39.0, -82.0);
    ff_wall_t const w0 = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w0.src);
    TEST_ASSERT_EQUAL_INT16(1320, w0.now_min);

    /* Stranger #2 — a completely different, never-heard node — reports a
     * time TWO HOURS behind: issue #49's exact measured shape ("now_min
     * shifted back", a two-hour backward step from an unpaired sender). */
    inject_position(STRANGER2, U_AWAKE, 39.1, -82.1);

    ff_wall_t const w1 = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w1.src);
    TEST_ASSERT_EQUAL_INT16(1320, w1.now_min); /* UNCHANGED — the whole point */

    /* AC5: bench-visible, not a silent no-op. */
    TEST_ASSERT_EQUAL_UINT32(1, ff_shell_wall_rejected_relatches(&H.shell));

    /* Neither stranger was paired or promoted by any of this. */
    TEST_ASSERT_NULL(member(STRANGER));
    TEST_ASSERT_NULL(member(STRANGER2));
}

static void S18_AC3_a_paired_members_backward_correction_relatches(void)
{
    /* AC3 + the spec's "wiring the TRUSTED sources" note: a paired member
     * is TRUSTED, so a live on_position disagreeing BACKWARD by more than
     * the delta DOES move an established latch — the backwards-GPS-step
     * correction case the trust gate must not close off. Deliberately via
     * on_position, not NodeInfo: see
     * S18_paired_members_backward_nodeinfo_reading_is_still_ignored below
     * for why the same reading through NodeInfo stays ignored regardless
     * of trust (the #46 rule). */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_position(DANA, U_EVENING, 39.0, -82.0); /* bootstraps, 22:00 */
    TEST_ASSERT_EQUAL_INT16(1320, ff_shell_wall(&H.shell).now_min);

    /* DANA, PAIRED, reports a time two hours behind — a genuine backwards
     * GPS correction from a trusted source. */
    inject_position(DANA, U_AWAKE, 39.0, -82.0);

    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1200, w.now_min); /* 20:00 — moved */
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S18_AC4_self_position_latches_the_wall_but_stays_dropped_for_crew(void)
{
    /* The headline reorder: shell_ev_position now offers self's own
     * rx_time to ff_wall_observe BEFORE the self-drop returns, exactly
     * like shell_ev_node already did for NodeInfo (D1) — bringing the two
     * into the same shape. Self is TRUSTED (shell_wall_trust_for), so its
     * own GPS-disciplined reading both bootstraps AND can move an
     * existing latch, while ff_crew_find/ff_heard_note/ff_crew_on_position
     * stay gated behind shell_drop_as_self exactly as
     * S16_b1_own_traffic_is_not_treated_as_inbound already pins for the
     * non-wall side of this same event. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* Only our own live Position arrives — no NodeInfo, no peer. */
    inject_position(MY_ID, U_EVENING, 39.0, -82.0);

    ff_wall_t const w0 = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w0.src);
    TEST_ASSERT_EQUAL_INT16(1320, w0.now_min);

    /* And it moves an EXISTING latch too, not just a bootstrap — self is
     * TRUSTED, so a later disagreeing self-reading re-latches. */
    inject_position(MY_ID, U_AWAKE, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT16(1200, ff_shell_wall(&H.shell).now_min);

    /* Still never treated as inbound crew/feed traffic. */
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S18_self_trust_is_independent_of_dev_trust_all(void)
{
    /* Pins the independence the S18a PR body calls load-bearing and no
     * other test caught (review of PR #90): shell_wall_trust_for
     * classifies self via shell_is_self, NOT shell_drop_as_self. The two
     * diverge only under --dev-trust-all (sim-only), where
     * shell_drop_as_self is suspended and returns false for every node.
     * If the classifier used shell_drop_as_self, self would fall through
     * to BOOTSTRAP under the flag and could no longer MOVE an established
     * latch — silently demoting self's own GPS-disciplined anchor exactly
     * when the bench harness is driving. So: with the flag ON, self must
     * still be TRUSTED enough to re-latch. This is S18_AC4 with
     * --dev-trust-all set; AC4 alone (flag off) can't catch the swap
     * because the two helpers are identical when the flag is off. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    ff_shell_dev_trust_all(&H.shell, true);
    inject_my_info(MY_ID);

    /* Self bootstraps the latch at 22:00 (any tier establishes a latch,
     * so the bootstrap alone does not distinguish the mutation). */
    inject_position(MY_ID, U_EVENING, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT16(1320, ff_shell_wall(&H.shell).now_min);

    /* Self, still TRUSTED despite the flag, moves the established latch
     * two hours backward. The swap-to-drop_as_self mutation demotes self
     * to BOOTSTRAP here and would REJECT this instead (now_min stays 1320,
     * rejected count ticks to 1) — so this assertion, and the counter one,
     * both fail under the swap. */
    inject_position(MY_ID, U_AWAKE, 39.0, -82.0);
    TEST_ASSERT_EQUAL_INT16(1200, ff_shell_wall(&H.shell).now_min);
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S18_paired_members_backward_nodeinfo_reading_is_still_ignored(void)
{
    /* The layering note the S18 spec insists on stating: a paired
     * member's backward correction flows through live on_position
     * (S18_AC3 above), NEVER through NodeInfo — trust does not change the
     * #46 forward-only rule (shell_observe_wall_nodeinfo). If trust ever
     * overrode "forward-only", a paired member's stale cached last_heard
     * on a reconnect replay would drag the wall clock backward by the
     * node's staleness — AC9's exact defect, reintroduced one layer up. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_node(DANA, "DANA", U_EVENING); /* bootstraps, 22:00 */
    TEST_ASSERT_EQUAL_INT16(1320, ff_shell_wall(&H.shell).now_min);

    /* DANA, PAIRED and therefore TRUSTED, sends a NodeInfo with a
     * last_heard two hours EARLIER. Forward-only wins regardless of
     * trust: this tells us nothing new about the clock and is ignored
     * before ff_wall_observe even runs. */
    inject_node(DANA, "DANA", (uint32_t)U_AWAKE);

    TEST_ASSERT_EQUAL_INT16(1320, ff_shell_wall(&H.shell).now_min); /* unmoved */
    /* Ignored, not "rejected" — the forward-only guard never calls
     * ff_wall_observe at all, so it cannot be counted as a trust-gate
     * rejection either. */
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S18_expired_latch_relatches_trust_blind_through_the_shell(void)
{
    /* The one branch S18 slice a deliberately does not gate, pinned at
     * the shell layer too, not just inside ff_wall_observe: once the
     * latch has expired, a fresh BOOTSTRAP-tier reading from a totally
     * unpaired stranger still re-latches it. */
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_position(DANA, U_EVENING, 39.0, -82.0); /* TRUSTED bootstrap, 22:00 */
    TEST_ASSERT_EQUAL_INT16(1320, ff_shell_wall(&H.shell).now_min);

    advance(FF_WALL_LATCH_MAX_AGE_MS + 1u);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);

    /* A totally unpaired stranger re-latches it anyway. */
    inject_position(STRANGER, U_AWAKE, 39.0, -82.0);

    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1200, w.now_min);
    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_wall_rejected_relatches(&H.shell));
}

static void S16_b1_a_flare_on_a_foreign_portnum_raises_no_takeover(void)
{
    /* The shell's own flare branch does not go through ff_wiring, so
     * ff_wiring's portnum check does not cover it: without the check in
     * shell_ev_private a well-formed FLARE envelope arriving on any other
     * private portnum would raise a full-screen takeover. Currently
     * unreachable through mc_client, but it is real logic and it had no
     * test (PR #46 review, mutation survivor). */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int const n = ff_proto_encode_flare(buf, sizeof(buf), 300);
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    H.haptic.count = 0;
    H.ev.on_private(H.ev.user, DANA, FF_PORTNUM + 1u, buf, (size_t)n);

    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));

    /* Positive control: the same bytes on the right portnum do. */
    H.ev.on_private(H.ev.user, DANA, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
}

static void S16_b1_shell_footprint_excludes_the_pack(void)
{
    /* The fp_pack_t decision, pinned rather than described: the pack is
     * beside the shell, so the shell's stated budget is about the shell.
     * If a later slice folds a pack in, this fails loudly.
     *
     * S22 update: the old proxy was "shell < pack" (a folded pack would
     * blow past the pack's own size). That stopped holding when the
     * reworked Signals face made ff_app_state_t embed the 1,816-byte
     * ff_sigview_t view-model — the shell holds TWO copies of that state
     * (view + prev_key render key; the persistent send target is only an
     * 8-byte holder, not a third copy) and so legitimately grew PAST
     * sizeof(fp_pack_t) on those two view copies alone, with no pack
     * anywhere near it (see ff_shell.h's FF_SHELL_BYTES comment). The guard
     * is kept, just expressed correctly: the shell's size is explained by a
     * small number of ff_app_state_t-sized view copies plus bookkeeping, NOT
     * a pack. A folded fp_pack_t would add ~23.7 KB — far past this bound —
     * so it is still caught loudly. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT(FF_SHELL_BYTES, sizeof(ff_shell_t));
    /* A few view-copies' worth, not a pack: the real shell is ~3x an
     * ff_app_state_t; a folded pack would push it past 6x. */
    TEST_ASSERT_LESS_THAN_UINT(4u * sizeof(ff_app_state_t), sizeof(ff_shell_t));
}

/* S22 slice b — the Signals send target is the ONLY persistent Signals state,
 * held in an 8-byte shell holder because view.signals is memset + rebuilt from
 * scratch every tick. This pins the whole reason that holder exists: a target
 * set by a SELECT intent must SURVIVE the per-tick rebuild, a CLEAR returns to
 * WHOLE_CREW, and a tap that can't legitimately target (unpaired node, or the
 * Signals face not visible under a takeover) leaves it unchanged.
 *
 * This also guards the shell_project_signals refactor's key step: the "survives
 * a rebuild" assertion FAILS if the projection stops re-applying the holder
 * after ff_sigview_build (mutation — the target would reset to WHOLE_CREW every
 * tick). */
static void S22b_signals_target_survives_rebuild_and_is_gated(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    /* Default after a build: WHOLE_CREW. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->signals.target_kind);

    /* SELECT a paired member, then rebuild TWICE — the target must still hold.
     * (Mutation guard: without the holder re-apply in shell_project_signals it
     * resets to WHOLE_CREW on the very next tick.) */
    ff_intent_t sel = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);
    ff_shell_tick(&H.shell, H.clk.t);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->signals.target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->signals.target_node);

    /* CLEAR returns to WHOLE_CREW. */
    ff_intent_t clr = {.kind = FF_INTENT_SIG_CLEAR_TARGET, .u = {0}};
    ff_shell_intent(&H.shell, &clr);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->signals.target_kind);

    /* Re-target KEV, then an UNPAIRED node's SELECT is rejected (roster
     * validation in ff_sigview_target_select) — target unchanged. */
    sel.u.node_id = KEV_ID;
    ff_shell_intent(&H.shell, &sel);
    ff_intent_t bad = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    bad.u.node_id = STRANGER; /* never paired */
    ff_shell_intent(&H.shell, &bad);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->signals.target_kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->signals.target_node);

    /* A stray SELECT while a TAKEOVER is up is gated (the Signals face is not
     * the visible face) — target stays KEV, does not jump to DANA. */
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int const n = ff_proto_encode_flare(buf, sizeof(buf), 300);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, KEV_ID, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_intent_t stray = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    stray.u.node_id = DANA;
    ff_shell_intent(&H.shell, &stray);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->signals.target_kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->signals.target_node);
}

static void S16_b1_failed_pack_load_does_not_outrank_the_settings_offset(void)
{
    /* fp_parse zeroes *out on any failure, and a zeroed fp_pack_t reads
     * as a deliberately STATED UTC offset of 0 — utc_offset_assumed is
     * false. An implementation that sets "a pack is loaded" before
     * checking the parse result therefore hands ff_wall_resolve_offset a
     * stated offset of UTC, which outranks the user's configured one and
     * silently puts the puck in London.
     *
     * Measured with two offsets that differ: settings say UTC-5, the
     * failed pack would say UTC. 22:00Z is 17:00 at UTC-5 (now_min 1020)
     * and 22:00 at UTC (now_min 1320). */
    harness_seed_settings(-300);
    harness_init(100000u, true);

    TEST_ASSERT_LESS_THAN_INT(0, ff_shell_load_pack(&H.shell, "{not json", 9));

    inject_node(DANA, "DANA", U_EVENING);
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_EQUAL_INT16(1020, w.now_min);
    TEST_ASSERT_FALSE(w.offset_assumed);
}

/* =================================================================== */

/* =================================================================== */
/* S16 slice b2 — AC6: the sim cutover                                  */
/* =================================================================== */

/**
 * Scripted in-memory transport: `rx` is the radio's reply stream, `tx`
 * captures everything mc_client writes (so the test can echo back the
 * REAL want_config nonce — it is randomly generated per connect and
 * cannot be guessed; see mc_begin_handshake).
 */
typedef struct {
    uint8_t rx[4096];
    size_t rx_len, rx_pos;
    uint8_t tx[1024];
    size_t tx_len;
} pipe_io_t;

static pipe_io_t P;

static int pipe_read(void *io, uint8_t *buf, size_t maxlen)
{
    pipe_io_t *p = (pipe_io_t *)io;
    size_t remaining = p->rx_len - p->rx_pos;
    if (remaining == 0) return 0;
    size_t n = (remaining < maxlen) ? remaining : maxlen;
    memcpy(buf, p->rx + p->rx_pos, n);
    p->rx_pos += n;
    return (int)n;
}

static int pipe_write(void *io, uint8_t const *buf, size_t len)
{
    pipe_io_t *p = (pipe_io_t *)io;
    size_t room = sizeof(p->tx) - p->tx_len;
    size_t n = (len < room) ? len : room;
    memcpy(p->tx + p->tx_len, buf, n);
    p->tx_len += n;
    /* Report what was actually stored (review N4): a handshake that ever
     * outgrew `tx` must surface as a short-write transport failure the
     * client makes loud, not silently corrupt the nonce parse. */
    return (int)n;
}

/** The want_config nonce from the FIRST frame mc_connect wrote. */
static uint32_t pipe_want_config_id(pipe_io_t const *p)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, p->tx_len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, p->tx[0]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, p->tx[1]);
    uint16_t len = (uint16_t)((p->tx[2] << 8) | p->tx[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(p->tx_len - 4u, len);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(p->tx + 4, len);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_want_config_id_tag, tr.which_payload_variant);
    return tr.payload_variant.want_config_id;
}

static uint16_t frame_from_radio(meshtastic_FromRadio const *fr, uint8_t *out, size_t out_cap)
{
    uint8_t payload[512];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_FromRadio_fields, fr));
    uint16_t n = mc_frame_encode(out, out_cap, payload, (uint16_t)os.bytes_written);
    TEST_ASSERT_GREATER_THAN_UINT16(0, n);
    return n;
}

static size_t frame_my_info(uint8_t *out, size_t cap, uint32_t my_node_num)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    fr.payload_variant.my_info.my_node_num = my_node_num;
    return frame_from_radio(&fr, out, cap);
}

static size_t frame_nodeinfo_with_position(uint8_t *out, size_t cap, uint32_t node, char const *short_name,
                                            uint32_t last_heard, double lat, double lon)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    fr.payload_variant.node_info.num = node;
    fr.payload_variant.node_info.has_user = true;
    (void)snprintf(fr.payload_variant.node_info.user.short_name,
                    sizeof(fr.payload_variant.node_info.user.short_name), "%s", short_name);
    fr.payload_variant.node_info.has_position = true;
    fr.payload_variant.node_info.position.has_latitude_i = true;
    fr.payload_variant.node_info.position.latitude_i = (int32_t)(lat * 1e7);
    fr.payload_variant.node_info.position.has_longitude_i = true;
    fr.payload_variant.node_info.position.longitude_i = (int32_t)(lon * 1e7);
    fr.payload_variant.node_info.last_heard = last_heard;
    return frame_from_radio(&fr, out, cap);
}

static size_t frame_config_complete(uint8_t *out, size_t cap, uint32_t nonce)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    fr.payload_variant.config_complete_id = nonce;
    return frame_from_radio(&fr, out, cap);
}

static size_t frame_position_packet(uint8_t *out, size_t cap, uint32_t from, uint32_t rx_time, double lat,
                                     double lon)
{
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = true;
    pos.latitude_i = (int32_t)(lat * 1e7);
    pos.has_longitude_i = true;
    pos.longitude_i = (int32_t)(lon * 1e7);

    uint8_t pos_bytes[64];
    pb_ostream_t os = pb_ostream_from_buffer(pos_bytes, sizeof(pos_bytes));
    TEST_ASSERT_TRUE(pb_encode(&os, meshtastic_Position_fields, &pos));

    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.from = from;
    fr.payload_variant.packet.to = 0xFFFFFFFFu;
    fr.payload_variant.packet.id = 77;
    fr.payload_variant.packet.has_rx_time = true;
    fr.payload_variant.packet.rx_time = rx_time;
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.payload_variant.packet.payload_variant.decoded.portnum = meshtastic_PortNum_POSITION_APP;
    fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)os.bytes_written;
    memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, pos_bytes, os.bytes_written);
    return frame_from_radio(&fr, out, cap);
}

/** An inbound TEXT_MESSAGE_APP packet — raw UTF-8 bytes, no sub-message
 *  to encode (unlike Position above). Used by the AC7 test below to seed
 *  a feed item whose sender the canned-reply intent should target. */
static size_t frame_text_packet(uint8_t *out, size_t cap, uint32_t from, char const *utf8)
{
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    fr.payload_variant.packet.from = from;
    fr.payload_variant.packet.to = MY_ID;
    fr.payload_variant.packet.id = 78;
    fr.payload_variant.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    fr.payload_variant.packet.payload_variant.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t const len = strlen(utf8);
    fr.payload_variant.packet.payload_variant.decoded.payload.size = (pb_size_t)len;
    memcpy(fr.payload_variant.packet.payload_variant.decoded.payload.bytes, utf8, len);
    return frame_from_radio(&fr, out, cap);
}

/** Decode a single outbound ToRadio frame (as written by mc_send_text /
 *  mc_send_private into P.tx) and return its MeshPacket.to — the
 *  destination the AC7 test below is the whole point of capturing. */
static uint32_t decode_packet_to(uint8_t const *buf, size_t len)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, len);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC1, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(MC_FRAME_MAGIC2, buf[1]);
    uint16_t flen = (uint16_t)((buf[2] << 8) | buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);
    return tr.payload_variant.packet.to;
}

/** Decode a single outbound ToRadio frame's decoded payload bytes as a
 *  NUL-terminated string into `out` (capacity `out_cap`) — the SEND_TEXT
 *  test below's way of checking WHAT was sent, not just to whom. */
static void decode_packet_text(uint8_t const *buf, size_t len, char *out, size_t out_cap)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, len);
    uint16_t flen = (uint16_t)((buf[2] << 8) | buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    meshtastic_MeshPacket const *pkt = &tr.payload_variant.packet;
    TEST_ASSERT_EQUAL_INT(meshtastic_MeshPacket_decoded_tag, pkt->which_payload_variant);
    size_t n = (size_t)pkt->payload_variant.decoded.payload.size;
    if (n >= out_cap) n = out_cap - 1u;
    memcpy(out, pkt->payload_variant.decoded.payload.bytes, n);
    out[n] = '\0';
}

/** Decode a single outbound ToRadio frame's PRIVATE (ff_proto) payload: it
 *  MUST be on FF_PORTNUM, and its bytes are run through ff_proto_decode so
 *  the S22 AC4 tests can assert the encoded TYPE (PULSE vs RALLY), not just
 *  that "a send happened" — the proxy trap the brief warns about. Returns
 *  the ff_proto type (positive) and, for RALLY, fills *out_msg. */
static int decode_packet_private(uint8_t const *buf, size_t len, ff_proto_msg_t *out_msg)
{
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(5u, len);
    uint16_t flen = (uint16_t)((buf[2] << 8) | buf[3]);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(len - 4u, flen);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf + 4, flen);
    TEST_ASSERT_TRUE(pb_decode(&is, meshtastic_ToRadio_fields, &tr));
    TEST_ASSERT_EQUAL_INT(meshtastic_ToRadio_packet_tag, tr.which_payload_variant);

    meshtastic_MeshPacket const *pkt = &tr.payload_variant.packet;
    TEST_ASSERT_EQUAL_INT(meshtastic_MeshPacket_decoded_tag, pkt->which_payload_variant);
    /* Honest-data: a Signals action is an ff_proto packet — it must ride the
     * private portnum, never masquerade as a TEXT/POSITION app message. */
    TEST_ASSERT_EQUAL_UINT32(FF_PORTNUM, (uint32_t)pkt->payload_variant.decoded.portnum);

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int const type = ff_proto_decode(pkt->payload_variant.decoded.payload.bytes,
                                     (size_t)pkt->payload_variant.decoded.payload.size, &msg);
    TEST_ASSERT_GREATER_THAN_INT(0, type);
    if (out_msg != NULL) *out_msg = msg;
    return type;
}

/** Stand a shell up on the scripted P-pipe transport and drive it to
 *  CONNECTED — the shared preamble every S22 AC4 send test needs (the
 *  same handshake S16_c3's send test open-codes). MY_ID becomes our node. */
static void s22_connect_shell(void)
{
    memset(&P, 0, sizeof(P));
    memset(&H, 0, sizeof(H));
    H.clk.t = 100000u;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.transport.read = pipe_read;
    cfg.transport.write = pipe_write;
    cfg.transport.io = &P;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    uint32_t const nonce = pipe_want_config_id(&P);

    size_t n = 0;
    n += frame_my_info(P.rx + n, sizeof(P.rx) - n, MY_ID);
    n += frame_config_complete(P.rx + n, sizeof(P.rx) - n, nonce);
    P.rx_len = n;

    advance(20u);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));
}

/* =================================================================== */
/* S22 slice d — AC4: action send wiring + rally-to-crew confirm        */
/* =================================================================== */

/* AC4 — PULSE addresses a member vs the whole crew, and encodes TYPE PULSE.
 * The proxy the brief names ("a send happened" without asserting node+type)
 * is refused here: every send asserts BOTH the destination NODE and the
 * decoded ff_proto TYPE. */
static void S22_AC4_pulse_addresses_member_vs_whole_crew(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Member: SELECT DANA, then PULSE -> addressed to DANA, type PULSE. */
    ff_intent_t sel = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t tx_before = P.tx_len;
    ff_intent_t pulse = {.kind = FF_INTENT_SIG_PULSE, .u = {0}};
    ff_shell_intent(&H.shell, &pulse);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_PULSE, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));

    /* The send reset the target to WHOLE_CREW (AC3). A second PULSE now
     * broadcasts. */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->signals.target_kind);
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &pulse);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_PULSE, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));
}

/* AC4 — RALLY to a single member sends on the FIRST tap (no confirm), is
 * addressed to that member, and encodes TYPE RALLY carrying a place name. */
static void S22_AC4_rally_to_member_sends_first_tap(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_latlon_t const here = {.lat = 39.7392, .lon = -104.9903};
    ff_shell_set_my_pos(&H.shell, here);

    ff_intent_t sel = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_SIG_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally); /* first tap: a member rally sends immediately */
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    ff_proto_msg_t msg;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY,
                          decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, &msg));
    /* No pack loaded -> the honest default place name, never a fabricated
     * landmark. */
    TEST_ASSERT_EQUAL_STRING("MY SPOT", msg.body.rally.name);

    /* Reset-after-send (AC3). */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->signals.target_kind);
}

/* AC4 — RALLY with an UNKNOWN own position sends NOTHING: a rally carries a
 * lat/lon, and fabricating {0,0} is the honesty violation the repo forbids. */
static void S22_AC4_rally_without_my_pos_sends_nothing(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* my_pos deliberately unset. */
    ff_intent_t sel = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t const tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_SIG_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* nothing sent */
    /* Target unchanged (no send, so no AC3 reset) — still DANA. */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->signals.target_kind);
}

/* AC4 — RALLY to WHOLE_CREW is the one loud broadcast: the first tap ARMS
 * (no send, button shows armed), a second tap within the window SENDS to
 * broadcast and disarms, and an intervening action disarms instead. */
static void S22_AC4_rally_whole_crew_confirm(void)
{
    s22_connect_shell();
    ff_latlon_t const here = {.lat = 39.7392, .lon = -104.9903};
    ff_shell_set_my_pos(&H.shell, here);
    /* Default target is WHOLE_CREW. */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->signals.target_kind);

    /* First tap: ARMS, sends nothing. */
    size_t tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_SIG_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* no send on first whole-crew tap */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->signals.rally_confirm_armed); /* button shows armed */

    /* Second tap within the window: SENDS to broadcast, type RALLY, disarms. */
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->signals.rally_confirm_armed);

    /* Arm again, then an intervening PULSE DISARMS: the next rally tap must
     * arm afresh, not send. */
    ff_shell_intent(&H.shell, &rally); /* arm */
    ff_intent_t pulse = {.kind = FF_INTENT_SIG_PULSE, .u = {0}};
    ff_shell_intent(&H.shell, &pulse); /* intervening action disarms (and itself sends a pulse) */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->signals.rally_confirm_armed);

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally); /* this is a fresh FIRST tap: arms, no send */
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len);
}

/* AC4 — the confirm arms on the first tap but LAPSES after the window: a
 * tap past FF_SIG_RALLY_CONFIRM_MS is a fresh first tap (arms), never a
 * silent send of the loud broadcast. */
static void S22_AC4_rally_confirm_lapses_after_window(void)
{
    s22_connect_shell();
    ff_latlon_t const here = {.lat = 39.7392, .lon = -104.9903};
    ff_shell_set_my_pos(&H.shell, here);

    ff_intent_t rally = {.kind = FF_INTENT_SIG_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally); /* arm at t0 */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->signals.rally_confirm_armed);

    /* Let the window lapse, then tick: the arm expires (button clears). */
    advance(5000u); /* > FF_SIG_RALLY_CONFIRM_MS (4000) */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->signals.rally_confirm_armed);

    /* A tap now is a fresh first tap: arms, sends nothing. */
    size_t const tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len);
}

/* AC4 — COMPOSE opens the composer with its TO set to the current target
 * and switches to the compose face; the Signals target is NOT reset (only
 * a direct PULSE/RALLY send resets it). */
static void S22_AC4_compose_sets_dest_and_switches_face(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* WHOLE_CREW target: COMPOSE opens broadcast (dest 0). */
    ff_intent_t compose = {.kind = FF_INTENT_SIG_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &compose);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_compose_to_node(&H.shell));

    /* Back out, target a member, COMPOSE: TO is that member. */
    ff_intent_t back = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(&H.shell, &back);
    ff_intent_t sel = {.kind = FF_INTENT_SIG_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);
    ff_shell_intent(&H.shell, &compose);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_compose_to_node(&H.shell));
    /* COMPOSE did not reset the Signals target (it is a navigation, not a
     * direct send). */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->signals.target_kind);
}

/**
 * AC6, the without-the-flag half, driven through the REAL pipeline the
 * cutover ships: a scripted transport into the shell's own mc_client_t.
 * A node that has only ever sent NodeInfo + Position produces zero feed
 * items, zero roster slots, and exactly one heard entry — the same
 * answer the injection-level AC5 tests give, now proven through the
 * decode path `ffsim --connect` actually uses (main.c has no other
 * path: live.c is gone).
 */
static void S16_AC6_nodeinfo_plus_position_via_real_transport_produce_zero_feed_items(void)
{
    memset(&P, 0, sizeof(P));
    memset(&H, 0, sizeof(H));
    H.clk.t = 100000u;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.transport.read = pipe_read;
    cfg.transport.write = pipe_write;
    cfg.transport.io = &P;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg)); /* mc_connect -> want_config in P.tx */
    uint32_t const nonce = pipe_want_config_id(&P);

    size_t n = 0;
    n += frame_my_info(P.rx + n, sizeof(P.rx) - n, MY_ID);
    n += frame_nodeinfo_with_position(P.rx + n, sizeof(P.rx) - n, STRANGER, "STR", U_EVENING, 39.0, -82.0);
    n += frame_config_complete(P.rx + n, sizeof(P.rx) - n, nonce);
    n += frame_position_packet(P.rx + n, sizeof(P.rx) - n, STRANGER, U_EVENING + 5u, 39.0005, -82.0);
    P.rx_len = n;

    advance(20u);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    /* The pipeline genuinely ran end to end... */
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));
    TEST_ASSERT_EQUAL_UINT32(MY_ID, ff_shell_my_node_id(&H.shell));

    /* ...and the trust policy held at every stage of it. */
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER));
    TEST_ASSERT_EQUAL_UINT8(1, ff_heard_count(ff_shell_heard(&H.shell)));
}

/**
 * AC6, the flag half: auto-pair on NodeInfo, and on NodeInfo ONLY. A
 * bare Position from an unknown node must not pair even on the dev
 * bench — pairing on the most untrusted packet on the mesh is this
 * spec's headline defect, and the dev affordance does not get to
 * reintroduce it.
 */
static void S16_AC6_dev_trust_all_auto_pairs_on_nodeinfo_only(void)
{
    harness_init(100000u, false);
    ff_shell_dev_trust_all(&H.shell, true);
    inject_my_info(MY_ID);

    /* NodeInfo -> a real paired roster slot, name and all... */
    inject_node(STRANGER, "STR", U_EVENING);
    TEST_ASSERT_EQUAL_UINT8(1, ff_shell_crew(&H.shell)->count);
    ff_crew_member_t const *m = member(STRANGER);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->paired);
    TEST_ASSERT_EQUAL_STRING("STR", m->name);
    TEST_ASSERT_FALSE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER));

    /* ...a bare Position does NOT pair; the sender goes to heard. */
    inject_position(STRANGER2, U_EVENING + 10u, 39.0, -82.0);
    TEST_ASSERT_EQUAL_UINT8(1, ff_shell_crew(&H.shell)->count);
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), STRANGER2));

    /* The pairing is real, not cosmetic: a PULSE from the paired node
     * reaches the feed; one from the position-only node still does not. */
    inject_pulse(STRANGER);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
    inject_pulse(STRANGER2);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
}

/**
 * AC6 amendment (recorded in S16's Amendments): the dockerized dev
 * meshtasticd is a SINGLE node whose id is also what on_my_info reports
 * as ours — the harness's one node plays every role. So under the flag
 * the self filter is suspended, and the sim offers the HOST's clock to
 * the wall latch so that single node's replayed position has an
 * independent clock to be aged against (without one, its own
 * `last_heard` defines the latch and the D1 rule — correctly — refuses
 * to age it). This test is firmware/tests/e2e/test_position_reaches_radar's
 * mechanism, pinned at unit level.
 */
static void S16_AC6_dev_trust_all_lets_the_single_dev_node_play_a_crew_member(void)
{
    /* Without the host-clock observation: the lone NodeInfo defines the
     * latch, so its position is honestly unplaceable (D1) — pinning WHY
     * ff_shell_dev_wall_observe is load-bearing, not decoration. */
    harness_init(100000u, false);
    ff_shell_dev_trust_all(&H.shell, true);
    inject_my_info(MY_ID);
    inject_node_with_position(MY_ID, U_EVENING - 30u, 39.002, -82.0);
    ff_crew_member_t const *m = member(MY_ID);
    TEST_ASSERT_NOT_NULL(m); /* paired (self filter suspended)... */
    TEST_ASSERT_TRUE(m->paired);
    TEST_ASSERT_FALSE(m->has_pos); /* ...but its fix defined the clock: not aged */

    /* With it — the exact ffsim --dev-trust-all startup sequence — the
     * same replay ages honestly against the host latch and reads LIVE. */
    harness_init(100000u, false);
    ff_shell_dev_trust_all(&H.shell, true);
    (void)ff_shell_dev_wall_observe(&H.shell, (int64_t)U_EVENING);
    inject_my_info(MY_ID);
    inject_node_with_position(MY_ID, U_EVENING - 30u, 39.002, -82.0);

    m = member(MY_ID);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE(m->paired);
    TEST_ASSERT_TRUE(m->has_pos);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(m, H.clk.t)); /* 30 s < FF_CREW_LIVE_MS */

    /* And the flag is opt-in: a fresh shell without it keeps the self
     * filter up (S16_b1_own_traffic_is_not_treated_as_inbound pins the
     * production path in full). */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    inject_node_with_position(MY_ID, U_EVENING, 39.002, -82.0);
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
}

/**
 * The b1 caveat (PR #46 review), closed: before on_my_info lands, our
 * own NodeInfo cannot be recognised as ours and used to note our own id
 * in ff_heard — where it lingered until LRU eviction, so S12's "add
 * from heard nodes" list would have offered the user their own puck.
 * on_my_info now purges it, which covers EVERY stream ordering (see
 * shell_ev_my_info for why the suggested mc_client_t read closes
 * nothing — the client's copy updates at the same instant this
 * callback fires).
 */
static void S16_b2_my_info_purges_our_own_id_from_heard(void)
{
    harness_init(100000u, false);

    /* The radio picked the caveat's ordering: our NodeInfo first. */
    inject_node(MY_ID, "ME", U_EVENING);
    TEST_ASSERT_TRUE(ff_heard_contains(ff_shell_heard(&H.shell), MY_ID)); /* the defect, reproduced */

    inject_my_info(MY_ID);
    TEST_ASSERT_FALSE(ff_heard_contains(ff_shell_heard(&H.shell), MY_ID)); /* purged on learning who we are */

    /* And once named, later self traffic never re-notes it. */
    inject_node(MY_ID, "ME", U_EVENING + 60u);
    inject_position(MY_ID, U_EVENING + 61u, 39.0, -82.0);
    TEST_ASSERT_FALSE(ff_heard_contains(ff_shell_heard(&H.shell), MY_ID));
    TEST_ASSERT_EQUAL_UINT8(0, ff_heard_count(ff_shell_heard(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(0, ff_shell_crew(&H.shell)->count);
}

/* =================================================================== */
/* S16 slice c2 — AC7: canned reply, mock sender captures dest          */
/* =================================================================== */

/**
 * AC7, driven through the REAL pipeline the same way AC6 is (S16 slice
 * b2): a scripted transport into the shell's own mc_client_t, which is
 * also the shell's canned-reply sender (`ff_wiring_init` binds
 * `w->sender` to `mc_send_text`/`mc_send_private` on that same client —
 * ff_wiring.h). Decoding the outbound frame IS the "mock sender captures
 * dest" the task brief asks for at this layer: `ff_wiring_send_canned_reply`
 * itself is already covered against a synthetic `ff_wiring_sender_t`
 * mock in test_wiring.c's S08 AC6 suite, so this test's job is narrower
 * and different — that `FF_INTENT_CANNED_REPLY`'s dispatch in
 * `ff_shell_intent` resolves the reply-context correctly (newest feed
 * item, or none) BEFORE handing it to that already-tested function.
 */
static void S16_AC7_canned_reply_uses_newest_feed_item_or_broadcasts(void)
{
    memset(&P, 0, sizeof(P));
    memset(&H, 0, sizeof(H));
    H.clk.t = 100000u;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.transport.read = pipe_read;
    cfg.transport.write = pipe_write;
    cfg.transport.io = &P;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    uint32_t const nonce = pipe_want_config_id(&P);

    size_t n = 0;
    n += frame_my_info(P.rx + n, sizeof(P.rx) - n, MY_ID);
    n += frame_config_complete(P.rx + n, sizeof(P.rx) - n, nonce);
    P.rx_len = n;

    advance(20u);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));

    /* Empty feed -> broadcast, BEFORE any sender is paired at all: proves
     * the broadcast half doesn't depend on there being a roster to fall
     * back to. */
    size_t tx_before = P.tx_len;
    ff_intent_t in = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    in.u.reply = FF_WIRING_REPLY_OMW;
    ff_shell_intent(&H.shell, &in);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    /* Now seed a feed item — DANA, paired, sends "hey" over the real
     * decode path — and the SAME intent must target DANA's node id, not
     * broadcast. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    n = frame_text_packet(P.rx, sizeof(P.rx), DANA, "hey");
    P.rx_len = n;
    P.rx_pos = 0;
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &in); /* the identical OMW intent */
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    /* And while a takeover is visible, the reply chips are not — the
     * shell must not send at all (routing rule 4, same principle
     * test_intent.c pins for Compose and FLARE_START). inject_flare uses
     * the H.ev this file's other tests share; set it once here since this
     * test built its own shell manually (real-transport bring-up, like
     * AC6) rather than through harness_init. */
    H.ev = ff_shell_events(&H.shell);
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &in);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* nothing sent */
}

/* =================================================================== */
/* S16 slice c3 — SEND_TEXT sends the shell-owned draft                 */
/* =================================================================== */

/**
 * SEND_TEXT, driven through the REAL pipeline the same way AC6/AC7 are:
 * a scripted transport into the shell's own `mc_client_t`, which is also
 * `ff_wiring_ctx_t.sender` — the seam SEND_TEXT sends through, same as
 * the canned replies. Types a draft via the real T9 intents, sends it,
 * and decodes the outbound frame for BOTH destination and text — AC7's
 * test only ever needed `decode_packet_to`; this one adds
 * `decode_packet_text` since "what was sent" is the whole point here.
 */
static void S16_c3_send_text_sends_the_shell_owned_draft(void)
{
    memset(&P, 0, sizeof(P));
    memset(&H, 0, sizeof(H));
    H.clk.t = 100000u;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.transport.read = pipe_read;
    cfg.transport.write = pipe_write;
    cfg.transport.io = &P;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    uint32_t const nonce = pipe_want_config_id(&P);

    size_t n = 0;
    n += frame_my_info(P.rx + n, sizeof(P.rx) - n, MY_ID);
    n += frame_config_complete(P.rx + n, sizeof(P.rx) - n, nonce);
    P.rx_len = n;

    advance(20u);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));

    /* Broadcast half: no explicit destination AND nobody paired yet (so
     * `ff_crew_selected` has nothing to self-heal to either) — same
     * "before any pairing exists" shape AC7's broadcast half uses, for
     * the same reason: pairing DANA first would make her the crew's own
     * self-healing selection and this half would pass for the wrong
     * reason. SEND on an empty draft must not fire at all. */
    size_t tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* empty draft: no-op */

    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open); /* node_id 0: broadcast (no selection, no explicit id) */
    /* S08 addendum: opens in predictive mode; this test drives the MULTITAP
     * path, so switch to ABC (one T9_MODE press: PRED -> ABC). */
    ff_intent_t to_abc = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&H.shell, &to_abc);

    ff_intent_t k5 = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    k5.u.t9_key = 5; /* 'j' */
    ff_shell_intent(&H.shell, &k5);
    ff_intent_t space = {.kind = FF_INTENT_T9_SPACE, .u = {0}};
    ff_shell_intent(&H.shell, &space); /* commits -> "j " */

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    char text[32];
    decode_packet_text(P.tx + tx_before, P.tx_len - tx_before, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("j ", text);

    /* Sending closed the composer and reset the draft (interpretation
     * call, noted in the PR body): back at the base face, and typing
     * again after re-opening starts from empty, not from "j ". */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.text);

    /* Explicit-destination half: OPEN_COMPOSE(DANA) -> SEND targets DANA,
     * not broadcast. Paired here, not before the broadcast half above. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_intent_t open_dana = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    open_dana.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open_dana);
    ff_shell_intent(&H.shell, &to_abc); /* S08 addendum: PRED -> ABC for the multitap path */
    ff_intent_t k2 = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    k2.u.t9_key = 2;
    ff_shell_intent(&H.shell, &k2);
    ff_shell_intent(&H.shell, &space);

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
}

/* =================================================================== */
/* S08 predictive addendum — SEND edge + festpack honesty at the shell   */
/* =================================================================== */

/* A predictive pack: artist "Gooders" deliberately shares keys 4-6-6-3
 * with the dictionary word "good" (test_t9pred.c's own overlap), so one
 * sequence proves BOTH pack-word ranking and the pointer-identity
 * from_pack flag. The stage "A Stage" (keys start 'a'=2) never collides. */
static char const PACK_JSON_T9[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Test Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.936,\"lon\":-82.414}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":["
    "{\"artist\":\"Gooders\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"21:00\",\"end\":\"23:00\"}]}";

/**
 * THE LOAD-BEARING EDGE: a user types a predicted word and hits SEND
 * WITHOUT first accepting it (no space, no tap). The visible word must be
 * sent, not lost. Driven through the same real transport pipeline as
 * S16_c3 (decoding the outbound frame's actual text).
 */
static void S08_pred_send_with_unaccepted_candidate_sends_the_visible_word(void)
{
    memset(&P, 0, sizeof(P));
    memset(&H, 0, sizeof(H));
    H.clk.t = 100000u;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.transport.read = pipe_read;
    cfg.transport.write = pipe_write;
    cfg.transport.io = &P;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    uint32_t const nonce = pipe_want_config_id(&P);

    size_t n = 0;
    n += frame_my_info(P.rx + n, sizeof(P.rx) - n, MY_ID);
    n += frame_config_complete(P.rx + n, sizeof(P.rx) - n, nonce);
    P.rx_len = n;
    advance(20u);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_SHELL_LINK_CONNECTED, ff_shell_link(&H.shell));

    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}}; /* opens in PRED */
    ff_shell_intent(&H.shell, &open);

    /* Type 4-6-6-3 -> predicted "good". Deliberately NO space/select: the
     * word is live and un-accepted. */
    ff_intent_t key = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    uint8_t const seq[] = {4, 6, 6, 3};
    for (size_t i = 0; i < sizeof(seq); i++) {
        key.u.t9_key = seq[i];
        ff_shell_intent(&H.shell, &key);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("good", ff_shell_view(&H.shell)->compose.word);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.text); /* not committed yet */

    /* SEND without accepting: the visible predicted word is folded in and
     * sent, not dropped. */
    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    char text[32];
    decode_packet_text(P.tx + tx_before, P.tx_len - tx_before, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("good", text); /* the whole point: the word survived */

    /* Sending closed the composer and reset draft + predictive session. */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.text);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.word);
}

/**
 * A loaded pack's headliner is a predictive candidate flagged from_pack by
 * POINTER IDENTITY (not name-matching), ranked above a same-keys dictionary
 * word which is honestly NOT from_pack — and honest no-match still holds for
 * a sequence in neither dictionary nor pack.
 */
static void S08_pred_festpack_from_pack_by_pointer_identity_and_honest_no_match(void)
{
    harness_init(100000u, false);
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_T9, sizeof(PACK_JSON_T9) - 1u));

    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open);

    ff_intent_t key = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    uint8_t const good[] = {4, 6, 6, 3};
    for (size_t i = 0; i < sizeof(good); i++) {
        key.u.t9_key = good[i];
        ff_shell_intent(&H.shell, &key);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_compose_t const *c = &ff_shell_view(&H.shell)->compose;
    TEST_ASSERT_EQUAL_STRING("Gooders", c->word);
    TEST_ASSERT_EQUAL_STRING("Gooders", c->cand[0].text);
    TEST_ASSERT_TRUE(c->cand[0].from_pack);       /* pack name, by pointer identity */
    TEST_ASSERT_EQUAL_STRING("good", c->cand[1].text);
    TEST_ASSERT_FALSE(c->cand[1].from_pack);      /* dictionary word, honestly not from pack */
    TEST_ASSERT_EQUAL_UINT16(12, c->total_cand);  /* dict 11 + the one pack word */

    /* Clear the session (four backspaces) and type "249": a sequence that
     * matches no dictionary word AND no pack name (Gooders starts 'g'=4).
     * Honest no-match must still hold — the pack does not rescue it, and
     * nothing is fabricated. */
    for (int i = 0; i < 4; i++) {
        ff_intent_t bs = {.kind = FF_INTENT_T9_BACKSPACE, .u = {0}};
        ff_shell_intent(&H.shell, &bs);
    }
    uint8_t const none[] = {2, 4, 9};
    for (size_t i = 0; i < sizeof(none); i++) {
        key.u.t9_key = none[i];
        ff_shell_intent(&H.shell, &key);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);
    c = &ff_shell_view(&H.shell)->compose;
    TEST_ASSERT_TRUE(c->word_nomatch);
    TEST_ASSERT_EQUAL_STRING("", c->word);
    TEST_ASSERT_EQUAL_UINT8(0, c->n_cand);
    TEST_ASSERT_EQUAL_UINT16(0, c->total_cand);
}

/* =================================================================== */
/* issue #33 — shell-boundary translation: LOC_MANUAL -> asserted        */
/* =================================================================== */

/* Full pipeline (mc_position_t -> shell -> crew -> radar view), the live-
 * packet path: LOC_MANUAL renders RADAR_PLACE, not RADAR_LIVE — and never
 * fabricates an age for it, even though `has_pos` is true. */
static void S33_live_loc_manual_renders_radar_place(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* latches the wall clock */

    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_MANUAL, false, 0);
    TEST_ASSERT_TRUE(member(DANA)->pos_asserted);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_ASSERTED, ff_crew_freshness(member(DANA), H.clk.t));

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);

    ff_radar_view_t const *r = &ff_shell_view(&H.shell)->radar;
    TEST_ASSERT_EQUAL_INT(RADAR_PLACE, r->mode);
    TEST_ASSERT_TRUE(r->arrow_valid); /* a real coordinate exists to point at */
    TEST_ASSERT_EQUAL_STRING("", r->age_str); /* never a fabricated "LAST SEEN" */
}

/* Mutation-conscious: the whole point of #33 is the freshness-axis
 * exclusion. Advance well past LOST (10 min) and re-tick — RADAR_PLACE
 * must hold, not decay to RADAR_LOST the way an ordinary LIVE fix would. */
static void S33_live_loc_manual_never_decays_to_lost(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);
    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_MANUAL, false, 0);

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */

    advance(3600u * 1000u); /* +1 hour: comfortably past FF_CREW_LOST_MS (10 min) */
    ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_EQUAL_INT(RADAR_PLACE, ff_shell_view(&H.shell)->radar.mode);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_ASSERTED, ff_crew_freshness(member(DANA), H.clk.t));
}

/* Regression/proxy guard: MC_LOC_UNKNOWN ("didn't say") must NOT be read
 * as an assertion — only the exact MC_LOC_MANUAL value does. A mutant
 * that defaulted `asserted` to true, or that folded UNKNOWN into
 * "asserted" alongside MANUAL, would satisfy S33_live_loc_manual_* alone;
 * this pins the other side. */
static void S33_live_loc_unknown_is_not_asserted(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_UNKNOWN, false, 0);
    TEST_ASSERT_FALSE(member(DANA)->pos_asserted);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_LIVE, ff_crew_freshness(member(DANA), H.clk.t));

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, ff_shell_view(&H.shell)->radar.mode);
}

/* The want_config REPLAY path translates LOC_MANUAL identically to the
 * live-packet path — same boundary function (shell_pos_meta), two call
 * sites. Matches issue #33's hardware finding: a fixed-position landmark
 * asserts LOC_MANUAL over the air, replay included. */
static void S33_replay_loc_manual_also_translates_to_asserted(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* latches the wall clock first (positionless) */

    inject_node_with_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_MANUAL, false, 0);
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_TRUE(member(DANA)->pos_asserted);
    TEST_ASSERT_EQUAL_INT(FF_FRESH_ASSERTED, ff_crew_freshness(member(DANA), H.clk.t));
}

/* =================================================================== */
/* issue #47 — shell-boundary translation: precision_bits                */
/* =================================================================== */

/* Live packet, KNOWN-DEGRADED precision (13 bits — issue #47's own
 * hardware measurement, the default public channel): the full pipeline
 * renders an area-scale distance, not a fabricated exact one, and
 * freshness/mode are untouched (precision and freshness are orthogonal
 * facts). */
static void S47_live_degraded_precision_renders_area_distance(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_UNKNOWN, true, 13);
    TEST_ASSERT_TRUE(member(DANA)->has_precision_bits);
    TEST_ASSERT_EQUAL_UINT8(13, member(DANA)->precision_bits);

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);

    ff_radar_view_t const *r = &ff_shell_view(&H.shell)->radar;
    TEST_ASSERT_EQUAL_INT(RADAR_LIVE, r->mode); /* freshness is untouched by precision */
    TEST_ASSERT_TRUE(r->dist_imprecise);
    TEST_ASSERT_EQUAL_CHAR('~', r->dist_str[0]); /* never a bare metre-looking number */
}

/* Positive control at the threshold: 24 bits (mc_client.h's own worked
 * example, ~3 m grid) is precise enough — normal exact-distance
 * rendering, no area caveat. */
static void S47_live_fine_precision_renders_normally(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_UNKNOWN, true, 24);

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.dist_imprecise);
}

/* issue #47's documented asymmetry, live side: absent precision on a
 * live packet renders EXACTLY as it did before this issue's fix — never
 * flagged degraded. "Didn't say" is not evidence of a coarse fix. */
static void S47_live_absent_precision_renders_normally(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    inject_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_UNKNOWN, false, 0);
    TEST_ASSERT_FALSE(member(DANA)->has_precision_bits);

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.dist_imprecise);
}

/* issue #47's documented asymmetry, REPLAY side — the mutation this test
 * actually guards against: a "fix" that treats replay-path-absent as
 * "must be degraded" (reasoning "the replay never carries it, so assume
 * the worst") would regress every ordinary reconnect to a blanket
 * "imprecise" label. Replay-absent and live-absent must behave
 * IDENTICALLY (both "unknown", neither "degraded") — see
 * mc_client.h's precision_bits doc comment. */
static void S47_replay_absent_precision_renders_normally_not_degraded(void)
{
    uint32_t const t0 = 100000u;
    harness_init(t0, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* latches the wall clock first (positionless) */

    inject_node_with_position_ex(DANA, U_EVENING, 39.0, -82.0, MC_LOC_UNKNOWN, false, 0);
    TEST_ASSERT_TRUE(member(DANA)->has_pos);
    TEST_ASSERT_FALSE(member(DANA)->has_precision_bits);

    ff_shell_set_heading(&H.shell, 0.0f);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.01}); /* ~865 m away: outside 30 m close range */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->radar.dist_imprecise);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_AC5a_unknown_sender_no_feed_no_crew_slot_one_heard_entry);
    RUN_TEST(S16_AC5b_known_unpaired_sender_no_feed_and_no_new_heard_entry);
    RUN_TEST(S16_AC5c_position_from_non_roster_node_is_dropped_and_noted);

    RUN_TEST(S16_AC9_transport_drop_moves_link_state_to_reconnecting);
    RUN_TEST(S16_AC9_want_config_replay_does_not_refresh_position_age);
    RUN_TEST(S16_AC9_replayed_position_with_no_last_heard_reads_never);
    RUN_TEST(S16_AC9_cold_boot_replay_is_never_stamped_fresh);
    RUN_TEST(S16_AC9_cold_boot_burst_ages_against_the_running_maximum);

    RUN_TEST(S18b_AC1_ascending_and_descending_replay_agree_after_settle);
    RUN_TEST(S18b_AC2_three_hour_cached_node_reads_lost_after_settle);
    RUN_TEST(S18b_AC3_node_older_than_the_window_stays_never_after_settle);
    RUN_TEST(S18b_AC4_replay_buffer_is_bounded_and_surfaces_overflow);

    RUN_TEST(S16_AC11_should_alert_fires_during_quiet_hours_feed_push_does_not);
    RUN_TEST(S16_AC11_feed_push_haptic_does_fire_outside_quiet_hours);
    RUN_TEST(S16_AC11_the_haptics_master_switch_silences_even_a_flare_alert);
    RUN_TEST(S16_AC11_unpaired_flare_neither_alerts_nor_takes_over);

    RUN_TEST(S16_AC13_active_face_is_never_flare_even_during_a_takeover);

    RUN_TEST(S16_AC3b_send_text_and_back_rejected_during_takeover_draft_survives);

    RUN_TEST(S16_AC4a_idle_shell_is_not_dirty_for_1000_ticks);
    RUN_TEST(S16_AC4a_dirty_is_the_rendered_projection_not_the_raw_struct);

    RUN_TEST(S16_b1_wall_is_unknown_until_a_plausible_timestamp_arrives);
    RUN_TEST(S16_b1_positions_are_never_stamped_from_the_local_clock);
    RUN_TEST(S16_b1_own_traffic_is_not_treated_as_inbound);
    RUN_TEST(S16_b1_rssi_is_attributed_only_on_a_direct_path);
    RUN_TEST(S16_b1_rssi_never_recorded_for_a_known_but_unpaired_sender);
    RUN_TEST(S16_b1_rssi_absent_reading_records_nothing_even_direct_and_paired);
    RUN_TEST(S16_b1_close_mode_triggers_live_via_direct_rssi_from_paired_peer);
    RUN_TEST(S07_2026_08_24_starts_only_set_is_live_not_lineup);
    RUN_TEST(S16_b1_now_projection_needs_both_a_pack_and_a_known_clock);
    RUN_TEST(S48_now_no_pack_holds_regardless_of_clock_state);
    RUN_TEST(S18c_no_pack_keeps_the_fixed_window);
    RUN_TEST(S18c_loading_lost_lands_tightens_the_window);
    RUN_TEST(S09_shell_projects_map_from_pack_crew_and_my_position);
    RUN_TEST(S09_shell_map_stays_empty_without_a_known_venue);
    RUN_TEST(S09_shell_map_excludes_unpaired_members_even_with_a_position);
    RUN_TEST(S09_shell_caps_features_at_the_view_limit_and_surfaces_truncation);
    RUN_TEST(S09_shell_does_not_claim_truncation_for_a_pack_within_caps);
    RUN_TEST(S16_b1_loading_a_pack_does_not_fabricate_my_position);
    RUN_TEST(S16_b1_our_own_nodeinfo_can_bootstrap_the_wall_clock);

    RUN_TEST(S18_AC1_shell_bootstrap_from_unpaired_stranger_still_works);
    RUN_TEST(S18_AC2_second_unpaired_stranger_cannot_move_the_wall_clock);
    RUN_TEST(S18_AC3_a_paired_members_backward_correction_relatches);
    RUN_TEST(S18_AC4_self_position_latches_the_wall_but_stays_dropped_for_crew);
    RUN_TEST(S18_self_trust_is_independent_of_dev_trust_all);
    RUN_TEST(S18_paired_members_backward_nodeinfo_reading_is_still_ignored);
    RUN_TEST(S18_expired_latch_relatches_trust_blind_through_the_shell);
    RUN_TEST(S16_b1_a_flare_on_a_foreign_portnum_raises_no_takeover);
    RUN_TEST(S16_b1_shell_footprint_excludes_the_pack);
    RUN_TEST(S22b_signals_target_survives_rebuild_and_is_gated);
    RUN_TEST(S22_AC4_pulse_addresses_member_vs_whole_crew);
    RUN_TEST(S22_AC4_rally_to_member_sends_first_tap);
    RUN_TEST(S22_AC4_rally_without_my_pos_sends_nothing);
    RUN_TEST(S22_AC4_rally_whole_crew_confirm);
    RUN_TEST(S22_AC4_rally_confirm_lapses_after_window);
    RUN_TEST(S22_AC4_compose_sets_dest_and_switches_face);
    RUN_TEST(S16_b1_failed_pack_load_does_not_outrank_the_settings_offset);

    RUN_TEST(S16_AC6_nodeinfo_plus_position_via_real_transport_produce_zero_feed_items);
    RUN_TEST(S16_AC6_dev_trust_all_auto_pairs_on_nodeinfo_only);
    RUN_TEST(S16_AC6_dev_trust_all_lets_the_single_dev_node_play_a_crew_member);
    RUN_TEST(S16_b2_my_info_purges_our_own_id_from_heard);

    RUN_TEST(S16_AC7_canned_reply_uses_newest_feed_item_or_broadcasts);
    RUN_TEST(S16_c3_send_text_sends_the_shell_owned_draft);
    RUN_TEST(S08_pred_send_with_unaccepted_candidate_sends_the_visible_word);
    RUN_TEST(S08_pred_festpack_from_pack_by_pointer_identity_and_honest_no_match);

    RUN_TEST(S33_live_loc_manual_renders_radar_place);
    RUN_TEST(S33_live_loc_manual_never_decays_to_lost);
    RUN_TEST(S33_live_loc_unknown_is_not_asserted);
    RUN_TEST(S33_replay_loc_manual_also_translates_to_asserted);

    RUN_TEST(S47_live_degraded_precision_renders_area_distance);
    RUN_TEST(S47_live_fine_precision_renders_normally);
    RUN_TEST(S47_live_absent_precision_renders_normally);
    RUN_TEST(S47_replay_absent_precision_renders_normally_not_degraded);

    return UNITY_END();
}
