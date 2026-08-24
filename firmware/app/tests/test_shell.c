/**
 * test_shell.c — S16 slice b1: the app shell.
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

static void S16_b1_now_projection_needs_both_a_pack_and_a_known_clock(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON, sizeof(PACK_JSON) - 1u));

    /* A pack is loaded, but the puck does not know what time it is yet.
     * INTERPRETATION (flagged in the PR): now_state_t has no member for
     * that, so the projection uses NOW_NO_PACK — the least-claiming
     * member, which scr_now renders as "nothing loaded". It under-claims
     * (we do have a pack) rather than over-claiming: NOW_TBD would
     * assert the day's set times are unknown, which is a statement about
     * the DATA and would be a lie. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_WALL_UNKNOWN, ff_shell_wall(&H.shell).src);
    TEST_ASSERT_EQUAL_INT(NOW_NO_PACK, ff_shell_view(&H.shell)->now.state);

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

static void S16_b1_loading_a_pack_does_not_fabricate_my_position(void)
{
    /* targets/sim/live.c adopts the pack's venue origin as "my
     * position". That is a dev-harness affordance: the venue centre is
     * not where the wearer is standing, and asserting it as a fix would
     * fabricate a position (CLAUDE.md: "never fake ... positions"). The
     * shell must not inherit it — a target that wants it for development
     * calls ff_shell_set_my_pos itself, visibly. */
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
     * If a later slice folds a pack in, this fails loudly. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT(FF_SHELL_BYTES, sizeof(ff_shell_t));
    TEST_ASSERT_LESS_THAN_UINT(sizeof(fp_pack_t), sizeof(ff_shell_t));
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
    return (int)len;
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
    ff_shell_dev_wall_observe(&H.shell, (int64_t)U_EVENING);
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

    RUN_TEST(S16_AC11_should_alert_fires_during_quiet_hours_feed_push_does_not);
    RUN_TEST(S16_AC11_feed_push_haptic_does_fire_outside_quiet_hours);
    RUN_TEST(S16_AC11_the_haptics_master_switch_silences_even_a_flare_alert);
    RUN_TEST(S16_AC11_unpaired_flare_neither_alerts_nor_takes_over);

    RUN_TEST(S16_AC13_active_face_is_never_flare_even_during_a_takeover);

    RUN_TEST(S16_AC4a_idle_shell_is_not_dirty_for_1000_ticks);
    RUN_TEST(S16_AC4a_dirty_is_the_rendered_projection_not_the_raw_struct);

    RUN_TEST(S16_b1_wall_is_unknown_until_a_plausible_timestamp_arrives);
    RUN_TEST(S16_b1_positions_are_never_stamped_from_the_local_clock);
    RUN_TEST(S16_b1_own_traffic_is_not_treated_as_inbound);
    RUN_TEST(S16_b1_rssi_is_attributed_only_on_a_direct_path);
    RUN_TEST(S16_b1_now_projection_needs_both_a_pack_and_a_known_clock);
    RUN_TEST(S16_b1_loading_a_pack_does_not_fabricate_my_position);
    RUN_TEST(S16_b1_our_own_nodeinfo_can_bootstrap_the_wall_clock);
    RUN_TEST(S16_b1_a_flare_on_a_foreign_portnum_raises_no_takeover);
    RUN_TEST(S16_b1_shell_footprint_excludes_the_pack);
    RUN_TEST(S16_b1_failed_pack_load_does_not_outrank_the_settings_offset);

    RUN_TEST(S16_AC6_nodeinfo_plus_position_via_real_transport_produce_zero_feed_items);
    RUN_TEST(S16_AC6_dev_trust_all_auto_pairs_on_nodeinfo_only);
    RUN_TEST(S16_AC6_dev_trust_all_lets_the_single_dev_node_play_a_crew_member);
    RUN_TEST(S16_b2_my_info_purges_our_own_id_from_heard);

    return UNITY_END();
}
