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
#include "ff_multitap.h" /* S10 quick flare — FF_MULTITAP_MAX_GAP_MS, for the keep_awake-expiry test */
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
/* S27 sounds — play_sound spy (mirrors the haptic spy above, plus a    */
/* small event log so a test can assert not just "did a sound fire" but */
/* "which event, how many times" — the "which call site fired what,     */
/* exactly once" shape S27's own AC needs).                             */
/* ------------------------------------------------------------------- */

typedef struct {
    ff_sound_event_t events[32];
    int count; /* total pushes, INCLUDING any past the array's capacity —
                * see sound_count's own note on why that's still safe */
} sound_spy_t;

static void spy_play_sound(void *user, ff_sound_event_t ev)
{
    sound_spy_t *s = (sound_spy_t *)user;
    if (s->count < (int)(sizeof(s->events) / sizeof(s->events[0]))) {
        s->events[s->count] = ev;
    }
    s->count++;
}

/* ------------------------------------------------------------------- */
/* harness                                                              */
/* ------------------------------------------------------------------- */

typedef struct {
    fake_clock_t clk;
    ff_clock_t clock;
    haptic_spy_t haptic;
    sound_spy_t sound;
    mem_store_t store_mem;
    ff_store_t store;
    fp_pack_t pack;
    jsmntok_t toks[FP_MAX_TOKENS];
    ff_shell_t shell;
    mc_events_t ev;
} harness_t;

static harness_t H;

/** How many times `ev` was pushed to H.sound so far. Scans only the
 *  populated prefix of `events` (bounded by the spy's own capacity, not
 *  `count`, so an over-capacity test — none currently need more than
 *  32 — still can't read past the array). */
static int sound_count(ff_sound_event_t ev)
{
    int const cap = (int)(sizeof(H.sound.events) / sizeof(H.sound.events[0]));
    int const n = (H.sound.count < cap) ? H.sound.count : cap;
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (H.sound.events[i] == ev) c++;
    }
    return c;
}

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
    cfg.play_sound = spy_play_sound; /* S27 sounds */
    cfg.play_sound_user = &H.sound;
    cfg.pack = &H.pack;
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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

/* S26 banner-opens-conversation bugfix (2026-09-03) — the GROUP-message
 * sibling of inject_text above: `to == MC_ADDR_BROADCAST` (not MY_ID),
 * so ff_wiring_classify_dir reads FEED_DIR_BROADCAST and the item lands
 * in the CREW conversation, not `from`'s own 1:1 thread. */
static void inject_text_broadcast(uint32_t from, char const *text)
{
    H.ev.on_text(H.ev.user, from, MC_ADDR_BROADCAST, text, strlen(text));
}

/* 2026-09-02: this file used to have an inject_pulse(from) helper (PULSE,
 * empty body) for "some generic private-portnum inbound signal" — retired
 * along with the wire type (ff_proto.h's RESERVED_01 section). STATUS is
 * the drop-in stand-in everywhere that generic role was needed: like PULSE
 * it is feed-representable and NOT flare/rally-shaped, so it doesn't
 * trip the flare-takeover or rally-confirm machinery a careless PULSE
 * replacement could. */
static void inject_status(uint32_t from, char const *text)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_status(buf, sizeof(buf), text);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
}

static void inject_flare(uint32_t from, uint16_t dur_s)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_flare(buf, sizeof(buf), dur_s);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
}

/* A RESERVED_01 (retired PULSE) frame — hand-crafted, since there is no
 * encoder any more (ff_proto.h's RESERVED_01 section). */
static void inject_reserved01(uint32_t from)
{
    uint8_t buf[] = {(uint8_t)FF_PROTO_VERSION, (uint8_t)FF_PROTO_TYPE_RESERVED_01};
    H.ev.on_private(H.ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, sizeof(buf));
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
    inject_status(STRANGER, "hey");

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

    inject_status(DANA, "hey");
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
/* debt/batt-low-core — the shell projects ONE batt_pct that scr_radar.c */
/* and scr_launcher.c both read (ff_radar_view_t.batt_pct is a single    */
/* field, never duplicated per screen), and both call the SAME core      */
/* classifier (ff_radar_batt_is_low, ff_radar.h) on it. Proving the      */
/* shell's real projection path and the shared classifier agree here is  */
/* therefore sufficient to prove radar and launcher cannot disagree:     */
/* there is exactly one input and exactly one decision function — see    */
/* ff_radar.h's doc comment on ff_radar_batt_is_low for the full case.   */
/* =================================================================== */

static void S06_shell_projects_batt_pct_unknown_and_batt_low_agrees(void)
{
    /* No battery ADC on either target yet (ff_shell.c's own comment,
     * "no battery ADC on either target yet: honestly unknown") — the
     * real shell tick always projects batt_pct == -1 today. Pinning that
     * here (rather than only in core/tests/test_radar.c's classifier
     * boundary tests) proves the INTEGRATION: the real projection path
     * actually reaches -1, not just that -1-in gives false-out in
     * isolation. */
    harness_init(100000u, false);
    ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_EQUAL_INT8(-1, ff_shell_view(&H.shell)->radar.batt_pct);
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(ff_shell_view(&H.shell)->radar.batt_pct));
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
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* the boot default, S26e amended 2026-09-01 */

    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* Across the whole life of the takeover: raised, held, expired. */
    for (int i = 0; i < 60; i++) {
        advance(5000u);
        ff_shell_tick(&H.shell, H.clk.t);
        ff_app_state_t const *v = ff_shell_view(&H.shell);
        TEST_ASSERT_NOT_EQUAL_INT(FF_APP_FACE_FLARE, v->active_face);
        TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, v->active_face);
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* SEND pops back to base (the boot default) */
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->compose.text);       /* draft reset on send */
}

/* =================================================================== */
/* Repeat-flare dismiss (the live-demo report): a SECOND takeover must  */
/* be dismissable, and — the render-lifecycle half — arming a second    */
/* takeover after dismissing the first must mark the frame DIRTY so the */
/* device tears down the resting face and rebuilds the takeover (with a */
/* live DISMISS button). app_main only calls lv_obj_clean + rebuild on  */
/* a dirty tick; if the render key does not distinguish "resting face"  */
/* from "a fresh takeover", the takeover LVGL tree is never (re)built    */
/* and its DISMISS never becomes live — the on-glass symptom.           */
/* =================================================================== */

/* The shell's render key is file-private, so re-derive the device's
 * rebuild decision the way app_main does: capture the view, and treat a
 * tick whose dirty bit is true as "the device rebuilt this frame". These
 * tests assert on ff_shell_tick's own return value (the dirty bit) plus
 * the projected takeover state — exactly the two inputs app_main's
 * lv_obj_clean+ff_face_build path consumes. */
static void flare_second_takeover_dismisses(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    ff_intent_t const dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};

    /* First flare (member A) -> takeover, and the frame is dirty so the
     * device builds the takeover face. */
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* First dismiss clears it, and the frame is dirty (takeover -> resting
     * face rebuild). The report says this one works. */
    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    /* Second flare, a DIFFERENT member (B). This is the frame that must be
     * dirty: the device has to tear down the resting face and rebuild the
     * takeover so its DISMISS button exists and is wired. */
    advance(1000u);
    inject_flare(KEV_ID, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));       /* dirty: a new takeover is on screen */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* Second dismiss — the reported failure. */
    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    /* The demo repeats members: the SAME member A flares again. Still a
     * distinct dirty takeover, still dismissable. */
    advance(1000u);
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
}

/* The render-lifecycle guard, stated as its own property so a regression
 * that reintroduces the on-glass bug fails HERE even if the pure-state
 * test above still passes: dismissing a takeover and IMMEDIATELY arming a
 * new one (same member, identical 300 s duration — the coarsened
 * takeover countdown is byte-identical between the two) MUST still make
 * BOTH the dismiss frame and the re-arm frame dirty. If the render key
 * failed to distinguish the two takeover instances, the re-arm frame
 * would be clean and the device would keep showing the resting face with
 * no live DISMISS. */
static void flare_rearm_after_dismiss_is_dirty(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    ff_intent_t const dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};

    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* first takeover built */

    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* torn down to resting face */
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    /* Re-arm with the SAME sender and SAME duration, no clock advance: the
     * takeover countdown coarsens to the identical whole-second value the
     * first takeover had. The ONLY honest difference is "a takeover is up
     * now, and a moment ago it was not" — which the render key must carry
     * as a dirty frame. */
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* MUST be dirty: rebuild the takeover */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
}

/* THE ROOT CAUSE, isolated (the on-glass "second dismiss won't work").
 *
 * When a takeover is up it is the ONLY thing rendered — both the device
 * (targets/esp32s3/main/ff_face.c) and the sim (targets/sim/
 * face_dispatch.c) build the full-screen takeover and NOTHING beneath it.
 * So the rendered screen is a pure function of the takeover's own fields;
 * a changing radar arrow, an aging signals row, or a fresh feed item from
 * the busy live demo changes no pixel.
 *
 * The render key, though, keyed on the WHOLE projection. So all that
 * underlying churn (the live demo pushes feed items and jitters presence
 * continuously — ff_sigview_build reads the feed every tick) marked every
 * frame dirty. The device answers a dirty frame by lv_obj_clean() +
 * rebuild, which DESTROYS and recreates the takeover's DISMISS button. Run
 * that many times a second and any DISMISS tap whose press and release
 * straddle a rebuild is dropped by LVGL (its pressed object was deleted) —
 * the dismiss is lost. The first takeover often lands in a quiet moment;
 * by the second the demo's feed is churning steadily, so the takeover is
 * torn down continuously and the tap can't survive: "the second one won't
 * dismiss."
 *
 * The fix makes the takeover OPAQUE in the render key: while it is up, the
 * key depends only on the takeover-rendered fields, so the takeover tree —
 * and its live DISMISS button — is rebuilt only when the takeover ITSELF
 * changes, never from anything beneath it. This test pins that property,
 * with the AC4a-style positive control that keeps it honest. */
static void flare_takeover_is_opaque_to_underlying_churn(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    /* Distinct projected names so the takeover headline actually differs
     * between the two senders (positive control A below leans on that). */
    inject_node(DANA, "DANA", U_EVENING);
    inject_node(KEV_ID, "KEV", U_EVENING);

    ff_intent_t const dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};

    /* Raise a takeover from DANA and let it settle to a stable frame. */
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* dirty: takeover built */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    advance(20u);
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* stable takeover: nothing to redraw */

    /* Underlying churn while the takeover covers the screen: a fresh
     * inbound signal from KEV (grows the feed, moves the signals view) and
     * time advancing (ages move). None of it is rendered. The frame MUST
     * NOT be dirty — a rebuild here is exactly what clobbers the live
     * DISMISS button mid-press. */
    advance(1000u);
    inject_text(KEV_ID, "where you at");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "underlying activity rebuilt the takeover — its DISMISS button would be clobbered");
    advance(1000u);
    inject_status(KEV_ID, "hey");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a second underlying signal rebuilt the takeover");

    /* Positive control A: a change to the TAKEOVER's OWN appearance — a
     * new flare from a DIFFERENT sender (headline name changes) — IS
     * dirty, so the reduction has not disabled genuine takeover repaints. */
    advance(1000u);
    inject_flare(KEV_ID, 300);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a new sender's takeover did not repaint — DISMISS would show the wrong person");
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    /* Positive control B: dismissing (takeover -> underlying face) IS
     * dirty — the screen must rebuild back to what was beneath. */
    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);

    /* Positive control C: the SAME underlying activity that was inert
     * under the takeover DOES dirty the frame with no takeover up — proving
     * the assertions above are the takeover's opacity, not a dead dirty
     * bit (the AC4a proxy discipline). */
    advance(1000u);
    inject_text(KEV_ID, "still here");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "an inbound signal did not dirty the frame even with NO takeover up — dead dirty bit");
}

/* The bearing half of the same defect (PR #120 review, BLOCKING). The
 * takeover draws bearing ONLY as ff_flare_fmt_compass8's 1-of-8 compass
 * point (a 45-degree bucket), but takeover_bearing_deg is recomputed every
 * tick from live positions — so keying the raw float (even at 0.1 degree)
 * repaints on sub-octant jitter that moves no glyph, and on device that
 * repaint destroys the DISMISS button mid-tap: the exact clobber, from a
 * second live source. So the render key must fold bearing to its RENDERED
 * octant.
 *
 * A SUB-OCTANT sender move (bearing changes but the drawn compass point
 * does not) must NOT dirty; a CROSS-OCTANT move (the drawn point changes)
 * MUST. All three positions sit on the same 400 m radius so the rendered
 * distance string is identical across them — isolating bearing as the only
 * thing that could move. This bites the old raw-float keying: 308->332 deg
 * is a 24-degree swing that a 0.1-degree key reports dirty (FAIL); the
 * octant key keeps it clean (PASS). */
static void flare_takeover_bearing_keys_the_rendered_octant(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    ff_latlon_t const me = {39.0, -82.0};
    ff_shell_set_my_pos(&H.shell, me);

    ff_intent_t const dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};

    /* DANA at P1: bearing ~308 deg (NW), 400 m out. Flare -> takeover with a
     * valid bearing. */
    inject_position(DANA, U_EVENING, 39.002215, -82.003648);
    inject_flare(DANA, 300);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* takeover built */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_view(&H.shell)->flare.takeover_bearing_valid,
                             "bearing did not resolve — test needs both positions to compute a bearing");

    /* Capture the rendered distance; P2 keeps DANA on the same radius so
     * this string does not change — the guard that a NOT-dirty result below
     * is the octant holding, not the distance masking a real bearing move. */
    char dist_p1[sizeof(ff_shell_view(&H.shell)->flare.takeover_dist_str)];
    strncpy(dist_p1, ff_shell_view(&H.shell)->flare.takeover_dist_str, sizeof(dist_p1) - 1u);
    dist_p1[sizeof(dist_p1) - 1u] = '\0';
    float const bearing_p1 = ff_shell_view(&H.shell)->flare.takeover_bearing_deg;
    TEST_ASSERT_TRUE(bearing_p1 >= 292.5f && bearing_p1 < 337.5f); /* NW bucket */

    advance(20u);
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settle: stable takeover */

    /* SUB-OCTANT move -> P2: bearing ~332 deg, still NW, still 400 m. */
    advance(1000u);
    inject_position(DANA, U_EVENING + 1u, 39.003176, -82.002173);
    bool const dirty_sub = ff_shell_tick(&H.shell, H.clk.t);
    float const bearing_p2 = ff_shell_view(&H.shell)->flare.takeover_bearing_deg;
    /* Preconditions: the bearing really did move (a real sub-octant swing)
     * but stayed in the NW bucket, and the distance string held. */
    TEST_ASSERT_TRUE(bearing_p2 >= 292.5f && bearing_p2 < 337.5f);   /* still NW */
    TEST_ASSERT_TRUE((bearing_p2 - bearing_p1) > 10.0f);             /* a genuine swing, not a no-op */
    TEST_ASSERT_EQUAL_STRING(dist_p1, ff_shell_view(&H.shell)->flare.takeover_dist_str);
    TEST_ASSERT_FALSE_MESSAGE(dirty_sub,
                              "a sub-octant bearing move rebuilt the takeover — DISMISS would be clobbered mid-tap");

    /* CROSS-OCTANT move -> P3: bearing ~349 deg = N. The drawn compass point
     * changes NW -> N, so this MUST repaint. */
    advance(1000u);
    inject_position(DANA, U_EVENING + 2u, 39.003531, -82.000883);
    bool const dirty_cross = ff_shell_tick(&H.shell, H.clk.t);
    float const bearing_p3 = ff_shell_view(&H.shell)->flare.takeover_bearing_deg;
    TEST_ASSERT_TRUE(bearing_p3 >= 337.5f || bearing_p3 < 22.5f); /* N bucket */
    TEST_ASSERT_TRUE_MESSAGE(dirty_cross,
                             "a cross-octant bearing move did not repaint — the drawn compass point is stale");

    /* And it still dismisses. */
    ff_shell_intent(&H.shell, &dismiss);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
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
     * The scene has exactly one rendered quantity that moves with time:
     * a paired member's feed item age_str, what ff_fmt_age prints on the
     * Signals face. Sub-minute it reads the steady "now" (ONE render-key
     * bucket — the churn fix), so the string changes only when the age
     * crosses a MINUTE boundary ("now"->"1 MIN"->"2 MIN"...): at most once
     * per minute. Over M minutes at 50 Hz the shell may report ~M changes,
     * while the raw struct differs on EVERY single tick, because the
     * signals row's raw age_ms advances by 20 every time. That gap — a few
     * repaints against thousands of raw ticks — is the whole point.
     *
     * (Earlier this scene used an inbound flare and keyed on the takeover
     * countdown — but the receive takeover screen renders no countdown at
     * all, so that was a change to a NON-rendered field: exactly the
     * over-keying #flare-repeat-dismiss removed. A flare here now also
     * makes the takeover opaque, freezing the very change this measurement
     * needs, so the scene uses a plain rendered feed age.) */
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* NodeInfo only, no position -> DANA is LINKED, no presence age churn */
    inject_text(DANA, "omw"); /* a feed item whose age_str the Signals face renders */

    /* S26e amended 2026-09-01: the boot default is the launcher, whose
     * render key masks everything except the unread badge (the same
     * "opaque overlay" discipline the power menu uses), so the Signals
     * row's age_str this scene measures would never dirty the key from
     * there. Leave the launcher for the Signals face first. */
    {
        ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 2u}};
        ff_shell_intent(&H.shell, &leave_launcher);
    }

    ff_shell_tick(&H.shell, H.clk.t); /* first frame: always dirty */

    int const minutes = 3;
    int const ticks = minutes * 60 * 50; /* 9000 ticks of genuinely elapsing time */
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

    /* The shell repainted only when the rendered string actually changed —
     * one crossing per minute boundary, a tiny fraction of the raw ticks. */
    TEST_ASSERT_LESS_OR_EQUAL_INT(minutes + 1, shell_dirty);
    /* ...and it did not go silent either — the age really is advancing
     * across minute boundaries, so at least one repaint must be reported or
     * the screen would freeze with a stale age on it. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, shell_dirty);
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

    /* S21 amendment: default is 12-hour, so 22:00 local renders "10:00 pm"
     * (ff_fmt_clock, ff_wall.h) — not the bare 24-hour "22:00" this test
     * pinned before the clock-format setting existed. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("10:00 pm", ff_shell_view(&H.shell)->radar.clock_str);
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
    inject_status(MY_ID, "hey");

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

/* S26 slice (a) — ff_shell_load_pack is the shell's real production
 * pack-load path (not a one-shot boot helper), so the token scratch
 * `cfg.toks` points at (ff_shell_cfg_t.toks, stored as sh->toks at
 * ff_shell_init) must stay valid and correctly reusable across MORE
 * THAN ONE load on the same shell — the exact contract the esp32s3
 * target leans on by never freeing its PSRAM toks allocation (see
 * app_main.c). H.toks (the harness's fixed-size buffer) is the SAME
 * buffer both calls below go through; if fp_parse left any state
 * behind in it, or if a real bug ever reintroduced a stale/freed
 * pointer, the SECOND load — of a DIFFERENT pack, so a copy-paste
 * "first result cached" bug would also be caught — would be the one
 * to fail. */
static void S26_ff_shell_load_pack_twice_same_shell_same_toks_buffer(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON, sizeof(PACK_JSON) - 1u));
    TEST_ASSERT_EQUAL_UINT8(1, H.pack.n_stages);
    TEST_ASSERT_EQUAL_STRING("Headliner", H.pack.sets[0].artist);
    TEST_ASSERT_EQUAL_UINT8(0, H.pack.n_features);

    /* Second load, same shell, same H.toks buffer, a DIFFERENT pack
     * (one with a map.features section the first pack didn't have) —
     * proves H.toks (fp_parse's caller-supplied scratch, stored once at
     * harness_init and reused by every ff_shell_load_pack call on this
     * shell) parses correctly again rather than returning stale/garbage
     * tokens from the first call. */
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_MAP, sizeof(PACK_JSON_MAP) - 1u));
    TEST_ASSERT_EQUAL_STRING("Solo Act", H.pack.sets[0].artist);
    TEST_ASSERT_EQUAL_UINT8(1, H.pack.n_features);

    /* And the shell still functions normally after the reload — the
     * whole point of proving the buffer isn't stale/corrupted. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell) != NULL);
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

    /* And the clock now renders — 12-hour by default (S21 amendment). */
    TEST_ASSERT_EQUAL_STRING("10:00 pm", ff_shell_view(&H.shell)->radar.clock_str);
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
    H.ev.on_private(H.ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM + 1u, buf, (size_t)n);

    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_INT(0, H.haptic.count);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));

    /* Positive control: the same bytes on the right portnum do. */
    H.ev.on_private(H.ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
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
 * held in an 8-byte shell holder because view.inbox is memset + rebuilt from
 * scratch every tick. This pins the whole reason that holder exists: a target
 * set by a SELECT intent must SURVIVE the per-tick rebuild, a CLEAR returns to
 * WHOLE_CREW, and a tap that can't legitimately target (unpaired node, or the
 * Signals face not visible under a takeover) leaves it unchanged.
 *
 * This also guards the shell_project_inbox refactor's key step: the "survives
 * a rebuild" assertion FAILS if the projection stops re-applying the holder
 * after ff_sigview_build (mutation — the target would reset to WHOLE_CREW every
 * tick). */
static void S22b_inbox_target_survives_rebuild_and_is_gated(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    /* Default after a build: WHOLE_CREW. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);

    /* SELECT a paired member, then rebuild TWICE — the target must still hold.
     * (Mutation guard: without the holder re-apply in shell_project_inbox it
     * resets to WHOLE_CREW on the very next tick.) */
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);
    ff_shell_tick(&H.shell, H.clk.t);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->inbox.target_node);

    /* CLEAR returns to WHOLE_CREW. */
    ff_intent_t clr = {.kind = FF_INTENT_INBOX_CLEAR_TARGET, .u = {0}};
    ff_shell_intent(&H.shell, &clr);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);

    /* Re-target KEV, then an UNPAIRED node's SELECT is rejected (roster
     * validation in ff_sigview_target_select) — target unchanged. */
    sel.u.node_id = KEV_ID;
    ff_shell_intent(&H.shell, &sel);
    ff_intent_t bad = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    bad.u.node_id = STRANGER; /* never paired */
    ff_shell_intent(&H.shell, &bad);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->inbox.target_node);

    /* A stray SELECT while a TAKEOVER is up is gated (the Signals face is not
     * the visible face) — target stays KEV, does not jump to DANA. */
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int const n = ff_proto_encode_flare(buf, sizeof(buf), 300);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, KEV_ID, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_intent_t stray = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    stray.u.node_id = DANA;
    ff_shell_intent(&H.shell, &stray);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->inbox.target_node);
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
 *  the S22 AC4 tests can assert the encoded TYPE (FLARE vs RALLY), not just
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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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

/* (2026-09-02: this used to open with S22_AC4_pulse_addresses_member_vs_
 * whole_crew, PULSE addressing a member vs the whole crew and encoding TYPE
 * PULSE. PULSE is retired end to end — see ff_intent.h's header note — and
 * the test below, S24_flare_chip_addresses_member_vs_whole_crew_as_flare,
 * already covered the identical shape one FLARE_DEFAULT_DUR_S assertion
 * stronger, so the PULSE version is removed outright rather than kept
 * redundant.)
 *
 * The OUTBOUND quick signal is a FLARE, not a pulse (the maintainer's "in
 * send to crew we should have flare not pulse"). The 1:1 FLARE chip
 * (FF_INTENT_INBOX_FLARE) addresses a member vs broadcasts to the whole crew
 * through the SAME S22(d) seam, and the wire body is a FLARE at the S10
 * default duration. Every send asserts the destination NODE, the decoded
 * ff_proto TYPE, AND the duration — refusing the proxy the brief names ("a
 * flare send happened" while the wire still carries a PULSE would slip past
 * a feed-only check; the wire-type + dur assertions are what catch it). */
static void S24_flare_chip_addresses_member_vs_whole_crew_as_flare(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Member: SELECT DANA, then FLARE -> addressed to DANA, type FLARE at
     * the default duration. */
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t tx_before = P.tx_len;
    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    ff_proto_msg_t msg;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE,
                          decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, &msg));
    TEST_ASSERT_EQUAL_UINT16(FF_FLARE_DEFAULT_DUR_S, msg.body.flare.dur_s);

    /* The send reset the target to WHOLE_CREW (AC3, no thread open). A second
     * FLARE now broadcasts, still as a FLARE. */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE,
                          decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));

    /* The OUT item lands in the feed as a FLARE (not a pulse). */
    ff_feed_item_t const *it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_FLARE, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
}

/* AC4 / honest-data (Tier 3) — a send to a member who was JUST unpaired,
 * with NO intervening tick, is REFUSED. `ff_shell_pair(node, false)` clears
 * `paired` but leaves the node in the roster, so `ff_crew_find` still returns
 * it; between SELECT and this FLARE (2026-09-02: was PULSE, retired — see
 * ff_intent.h's header note) the per-tick projection that normally
 * downgrades a stale member target has not run, so the send-time `!m->paired`
 * re-validation in `shell_sig_dest` is the ONLY thing standing between the
 * user's drop and a directed send to a node they just removed. This is the
 * mutation guard for that guard: it PASSES with the check present and FAILS
 * (a FLARE goes to the dropped node) when `!m->paired` is dropped from
 * `shell_sig_dest`. */
static void S22_AC4_send_to_just_unpaired_member_is_refused(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* SELECT DANA as the target while she is still paired. */
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->inbox.target_node);

    /* Drop DANA — cleared `paired`, still in the roster — and do NOT tick, so
     * the projection's stale-target downgrade cannot run first. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, false));

    /* A FLARE now must send NOTHING: shell_sig_dest refuses a member target
     * that is no longer paired rather than addressing the dropped node. */
    size_t const tx_before = P.tx_len;
    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* refused — nothing on the wire */
}

/* AC4 — RALLY to a single member sends on the FIRST tap (no confirm), is
 * addressed to that member, and encodes TYPE RALLY carrying a place name. */
static void S22_AC4_rally_to_member_sends_first_tap(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_latlon_t const here = {.lat = 39.7392, .lon = -104.9903};
    ff_shell_set_my_pos(&H.shell, here);

    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
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
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);
}

/* AC4 — RALLY with an UNKNOWN own position sends NOTHING: a rally carries a
 * lat/lon, and fabricating {0,0} is the honesty violation the repo forbids. */
static void S22_AC4_rally_without_my_pos_sends_nothing(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* my_pos deliberately unset. */
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    size_t const tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* nothing sent */
    /* Target unchanged (no send, so no AC3 reset) — still DANA. */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
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
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);

    /* First tap: ARMS, sends nothing. */
    size_t tx_before = P.tx_len;
    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* no send on first whole-crew tap */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->inbox.rally_confirm_armed); /* button shows armed */

    /* Second tap within the window: SENDS to broadcast, type RALLY, disarms. */
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->inbox.rally_confirm_armed);

    /* Arm again, then an intervening FLARE DISARMS (2026-09-02: was PULSE,
     * retired — see ff_intent.h's header note): the next rally tap must arm
     * afresh, not send. */
    ff_shell_intent(&H.shell, &rally); /* arm */
    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare); /* intervening action disarms (and itself sends a flare) */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->inbox.rally_confirm_armed);

    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally); /* this is a fresh FIRST tap: arms, no send */
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len);
}

/* AC4 — the confirm arms on the first tap but LAPSES after the window: a
 * tap past FF_INBOX_RALLY_CONFIRM_MS is a fresh first tap (arms), never a
 * silent send of the loud broadcast. */
static void S22_AC4_rally_confirm_lapses_after_window(void)
{
    s22_connect_shell();
    ff_latlon_t const here = {.lat = 39.7392, .lon = -104.9903};
    ff_shell_set_my_pos(&H.shell, here);

    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally); /* arm at t0 */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->inbox.rally_confirm_armed);

    /* Let the window lapse, then tick: the arm expires (button clears). */
    advance(5000u); /* > FF_INBOX_RALLY_CONFIRM_MS (4000) */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->inbox.rally_confirm_armed);

    /* A tap now is a fresh first tap: arms, sends nothing. */
    size_t const tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &rally);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len);
}

/* AC4 — COMPOSE opens the composer with its TO set to the current target
 * and switches to the compose face; the Signals target is NOT reset (only
 * a direct FLARE/RALLY send resets it). */
static void S22_AC4_compose_sets_dest_and_switches_face(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* WHOLE_CREW target: COMPOSE opens broadcast (dest 0). */
    ff_intent_t compose = {.kind = FF_INTENT_INBOX_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &compose);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_compose_to_node(&H.shell));

    /* Back out, target a member, COMPOSE: TO is that member. */
    ff_intent_t back = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(&H.shell, &back);
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);
    ff_shell_intent(&H.shell, &compose);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_compose_to_node(&H.shell));
    /* COMPOSE did not reset the Signals target (it is a navigation, not a
     * direct send). */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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

    /* The pairing is real, not cosmetic: a STATUS from the paired node
     * reaches the feed; one from the position-only node still does not. */
    inject_status(STRANGER, "hey");
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
    inject_status(STRANGER2, "hey");
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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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
    /* 2 items as of S24: the broadcast OMW above pushed its own OUTGOING
     * feed item (FEED_DIR_OUT — threads show both sides), plus DANA's
     * "hey". The newest INBOUND item is what the next reply must target —
     * the OUT item is context-skipped (see the CANNED_REPLY handler). */
    TEST_ASSERT_EQUAL_UINT8(2, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, ff_feed_at(ff_shell_feed(&H.shell), 1)->dir);

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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* base underneath was the boot default */
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
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;
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
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* base underneath was the boot default */
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

/* =================================================================== */
/* S24 slice a — AC1: direction at the shell's push sites               */
/* =================================================================== */

/* An accepted FLARE send pushes its own OUTGOING feed item — dir OUT,
 * to_node = the resolved destination (member id, or 0 for the whole-crew
 * broadcast), unread false (my own send is never a badge). (2026-09-02:
 * this was S24_AC1_sig_pulse_send_pushes_outgoing_item, FF_INTENT_INBOX_
 * PULSE; retired along with the intent, see ff_intent.h's header note —
 * FLARE is the drop-in, same S22(d) scope+send seam.) */
static void S24_AC1_sig_flare_send_pushes_outgoing_item(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare);

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(feed));
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_EQUAL(FEED_FLARE, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(DANA, it->to_node);
    TEST_ASSERT_EQUAL_UINT32(0u, it->from_node);
    TEST_ASSERT_FALSE(it->unread);
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(feed));

    /* The target reset to WHOLE_CREW (AC3) — a second FLARE broadcasts,
     * and its outgoing item records the whole-crew sentinel to_node 0. */
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_EQUAL_UINT8(2, ff_feed_count(ff_shell_feed(&H.shell)));
    it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(0u, it->to_node);
}

/* An accepted RALLY send pushes an outgoing FEED_RALLY carrying the SAME
 * place name that went on the wire. */
static void S24_AC1_rally_send_pushes_outgoing_item_with_place_name(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.7392, -104.9903});

    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally); /* member rally: first tap sends */

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(feed));
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_EQUAL(FEED_RALLY, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(DANA, it->to_node);
    /* No pack loaded -> the honest default name, same as the wire body
     * (S22_AC4_rally_to_member_sends_first_tap decodes the packet). */
    TEST_ASSERT_EQUAL_STRING("MY SPOT", it->text);
    TEST_ASSERT_FALSE(it->unread);
}

/* A REFUSED send (rally with unknown own position) pushes nothing —
 * the outgoing item exists only for a message that was actually
 * accepted for transmission. */
static void S24_AC1_refused_rally_pushes_no_outgoing_item(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* my_pos deliberately unset. */
    ff_intent_t sel = {.kind = FF_INTENT_INBOX_SELECT_MEMBER, .u = {0}};
    sel.u.node_id = DANA;
    ff_shell_intent(&H.shell, &sel);

    ff_intent_t rally = {.kind = FF_INTENT_INBOX_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &rally);

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
}

/* The shell hands its my_info node id to the wiring, so an inbound text
 * addressed to US classifies as DIRECT — and one addressed to a THIRD
 * party stays honestly UNKNOWN (never guessed DIRECT or BROADCAST). */
static void S24_AC1_shell_classifies_inbound_text_direction_via_my_info(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    H.ev.on_text(H.ev.user, DANA, MY_ID, "for you", 7);
    H.ev.on_text(H.ev.user, DANA, MC_ADDR_BROADCAST, "for all", 7);
    H.ev.on_text(H.ev.user, DANA, KEV_ID, "for kev", 7);

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(3, ff_feed_count(feed));
    TEST_ASSERT_EQUAL(FEED_DIR_UNKNOWN, ff_feed_at(feed, 0)->dir);   /* third party */
    TEST_ASSERT_EQUAL(FEED_DIR_BROADCAST, ff_feed_at(feed, 1)->dir);
    TEST_ASSERT_EQUAL(FEED_DIR_DIRECT, ff_feed_at(feed, 2)->dir);    /* addressed to me */
}

/* Proxy check on the reply-context amendment: after replying once (which
 * pushes MY outgoing "omw" as the newest feed item), replying AGAIN must
 * still target the newest INBOUND sender — under the old ff_feed_at(0)
 * rule the context would be my own OUT item (from_node 0) and the reply
 * would be misaddressed to node 0. */
static void S24_AC1_reply_context_skips_my_own_outgoing_item(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    size_t n = frame_text_packet(P.rx, sizeof(P.rx), DANA, "hey");
    P.rx_len = n;
    P.rx_pos = 0;
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));

    ff_intent_t in = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    in.u.reply = FF_WIRING_REPLY_OMW;

    size_t tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &in);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    /* The reply pushed an outgoing item — it is now the newest. */
    TEST_ASSERT_EQUAL_UINT8(2, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, ff_feed_at(ff_shell_feed(&H.shell), 0)->dir);

    /* Reply again: still DANA, not node 0 / broadcast. */
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &in);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
}

/* An accepted composer SEND pushes its own OUTGOING feed item — the
 * fourth and last send site (PR #122 review: this one had no coverage,
 * and deleting its push left the whole suite green — the exact hole
 * where slice (c)'s 1:1 thread would show quick-chip replies while
 * composed texts silently vanish). Same S16_c3 real-pipeline drive:
 * OPEN_COMPOSE -> type via T9 intents -> SEND, for BOTH destinations —
 * broadcast (to_node 0) and an explicit member (to_node DANA). */
static void S24_AC1_composer_send_pushes_outgoing_item(void)
{
    s22_connect_shell();

    /* Broadcast half FIRST, before anyone is paired — S16_c3's own
     * ordering, for the same reason (a paired DANA would become the
     * crew's self-healing selection and this half would pass for the
     * wrong reason). */
    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    ff_intent_t to_abc = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&H.shell, &to_abc); /* S08 addendum: PRED -> ABC multitap */
    ff_intent_t k5 = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    k5.u.t9_key = 5; /* 'j' */
    ff_shell_intent(&H.shell, &k5);
    ff_intent_t space = {.kind = FF_INTENT_T9_SPACE, .u = {0}};
    ff_shell_intent(&H.shell, &space); /* commits -> "j " */
    ff_intent_t send = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_shell_intent(&H.shell, &send);

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(feed));
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_EQUAL(FEED_TEXT, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(0u, it->to_node); /* broadcast -> whole-crew sentinel */
    TEST_ASSERT_EQUAL_UINT32(0u, it->from_node);
    TEST_ASSERT_EQUAL_STRING("j ", it->text); /* the exact draft that went on the wire */
    TEST_ASSERT_FALSE(it->unread);
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(feed)); /* my send is no badge */

    /* Explicit-destination half: OPEN_COMPOSE(DANA) -> to_node == DANA. */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    ff_intent_t open_dana = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    open_dana.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open_dana);
    ff_shell_intent(&H.shell, &to_abc);
    ff_intent_t k2 = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    k2.u.t9_key = 2; /* 'a' */
    ff_shell_intent(&H.shell, &k2);
    ff_shell_intent(&H.shell, &space); /* commits -> "a " */
    ff_shell_intent(&H.shell, &send);

    TEST_ASSERT_EQUAL_UINT8(2, ff_feed_count(ff_shell_feed(&H.shell)));
    it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_TEXT, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(DANA, it->to_node);
    TEST_ASSERT_EQUAL_STRING("a ", it->text);
    TEST_ASSERT_FALSE(it->unread);
}

/* A REFUSED composer send pushes nothing: with no transport the shell's
 * mc_client is never READY, so send_text returns negative — the rc == 0
 * gate at the SEND_TEXT site must fabricate no "sent" item. (The other
 * send-site rc gates are pinned by S24_AC1_refused_rally_pushes_no_
 * outgoing_item and test_wiring's refusing-sender test; this closes the
 * composer's.) */
static void S24_AC1_composer_send_refused_pushes_no_item(void)
{
    harness_init(100000u, false); /* documented no-transport shell: sends refuse */

    ff_intent_t open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    ff_intent_t to_abc = {.kind = FF_INTENT_T9_MODE, .u = {0}};
    ff_shell_intent(&H.shell, &to_abc);
    ff_intent_t k5 = {.kind = FF_INTENT_T9_KEY, .u = {0}};
    k5.u.t9_key = 5;
    ff_shell_intent(&H.shell, &k5);
    ff_intent_t space = {.kind = FF_INTENT_T9_SPACE, .u = {0}};
    ff_shell_intent(&H.shell, &space);
    ff_intent_t send = {.kind = FF_INTENT_SEND_TEXT, .u = {0}};
    ff_shell_intent(&H.shell, &send); /* draft non-empty: the send IS attempted */

    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(ff_shell_feed(&H.shell)));
}

/* =================================================================== */
/* S24 slice b — the inbox screen's navigation + mark-read + render key */
/* =================================================================== */

/* Find one conversation in the projected inbox by its key (0 = CREW). */
static ff_inbox_conv_t const *view_conv(uint32_t node)
{
    ff_app_inbox_t const *sig = &ff_shell_view(&H.shell)->inbox;
    for (uint8_t i = 0; i < sig->inbox.conv_count; i++) {
        ff_inbox_conv_t const *cv = &sig->inbox.convs[i];
        bool const is_crew = (cv->kind == FF_CONV_CREW);
        if ((node == 0u && is_crew) || (node != 0u && !is_crew && cv->node_id == node)) {
            return cv;
        }
    }
    return NULL;
}

/* AC4 (the mark-read-on-open half): opening ONE thread clears exactly
 * that conversation's unread items — the other conversations' badges,
 * and the feed's own remaining unread count, survive item-by-item. Also
 * pins "the open thread IS the send scope": the S22(d) target holders
 * follow the open. */
static void S24_AC4_open_thread_marks_only_that_thread_read(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    inject_text(DANA, "you close?");                              /* DIRECT -> DANA thread, unread */
    H.ev.on_text(H.ev.user, KEV_ID, MC_ADDR_BROADCAST, "hi all", 6); /* BROADCAST -> CREW, unread */
    inject_text(KEV_ID, "just you");                              /* DIRECT -> KEV thread, unread */

    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(KEV_ID)->unread);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(0)->unread);
    TEST_ASSERT_EQUAL_UINT16(3, ff_feed_unread_count(ff_shell_feed(&H.shell)));

    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->inbox.thread_node);
    TEST_ASSERT_EQUAL_STRING("", ff_shell_view(&H.shell)->inbox.thread_name); /* no NodeInfo yet: honest "" */
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(DANA)->unread);   /* opened thread cleared */
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(KEV_ID)->unread); /* other threads survive */
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(0)->unread);
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(ff_shell_feed(&H.shell)));

    /* The open thread IS the send scope (S22(d) holders follow). */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->inbox.target_node);

    /* Opening the CREW thread clears the broadcast item only. */
    ff_intent_t open_crew = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    ff_shell_intent(&H.shell, &open_crew);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(0)->unread);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(KEV_ID)->unread);
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);
}

/* AC3 (navigation): the FAB opens the picker; a pick routes to the
 * thread for that scope (the pre-slice-d routing) and marks it read;
 * BACK pops thread/picker to the inbox; a pick naming an unknown node is
 * rejected whole (no navigation, no mark-read, no scope change). */
static void S24_AC3_fab_pick_and_back_navigate_subviews(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_text(KEV_ID, "oi");

    /* Navigate to the Signals face first — these controls only exist
     * there, and BACK's sub-view pop is (correctly) gated on the
     * Signals base face. S26 slice e: via the BOOT-button launcher
     * (HOME opens it from Radar, LAUNCHER_SELECT idx 2 = Signals, amended 2026-09-01 5-circle order). */
    ff_intent_t to_home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&H.shell, &to_home); /* RADAR -> LAUNCHER */
    ff_intent_t to_inbox = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    to_inbox.u.launcher_idx = 2u; /* Signals — index 2 as of the amended 5-circle order */
    ff_shell_intent(&H.shell, &to_inbox); /* LAUNCHER -> SIGNALS */

    ff_intent_t fab = {.kind = FF_INTENT_INBOX_NEW, .u = {0}};
    ff_intent_t back = {.kind = FF_INTENT_BACK, .u = {0}};

    ff_shell_intent(&H.shell, &fab);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_PICKER, ff_shell_view(&H.shell)->inbox.subview);

    ff_shell_intent(&H.shell, &back);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);

    /* S24 slice (d): PICK now routes to the action POPUP scoped to the
     * pick (over the thread), not straight to the thread — it still
     * establishes the full open-thread scope (target + mark-read +
     * thread_node) so closing the popup reveals the real thread. */
    ff_shell_intent(&H.shell, &fab);
    ff_intent_t pick = {.kind = FF_INTENT_INBOX_PICK, .u = {0}};
    pick.u.node_id = KEV_ID;
    ff_shell_intent(&H.shell, &pick);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_POPUP, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->inbox.thread_node);
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(KEV_ID)->unread); /* pick routes through the open transition */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);

    /* BACK steps the stack one level at a time: POPUP -> THREAD -> INBOX. */
    ff_shell_intent(&H.shell, &back);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
    ff_shell_intent(&H.shell, &back);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);

    /* Unknown recipient: rejected whole. Asserted BEFORE the next tick
     * (review Finding 4): the projection re-validates holders every tick
     * and would self-heal a handler that mutated first and checked
     * later, so the immediate view state is what pins the HANDLER's own
     * reject-before-mutate order. The target was MEMBER/KEV from the
     * pick above; a bad key must leave it exactly there. */
    inject_text(KEV_ID, "again");
    ff_intent_t bad = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    bad.u.node_id = 0xBADBEEFu;
    ff_shell_intent(&H.shell, &bad);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, ff_shell_view(&H.shell)->inbox.target_kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->inbox.target_node);
    TEST_ASSERT_EQUAL_UINT16(1, ff_feed_unread_count(ff_shell_feed(&H.shell))); /* no mark-read fired */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(KEV_ID)->unread); /* nothing was marked read */
}

/* Leaving the Signals face resets the sub-view: a fresh entry always
 * lands on the inbox, never a stale thread. (Signals is base index 2;
 * the route starts on RADAR.) */
static void S24_AC3_leaving_inbox_face_resets_subview_to_inbox(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* S26 slice e: via the BOOT-button launcher. */
    ff_intent_t home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_intent_t to_inbox = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    to_inbox.u.launcher_idx = 2u; /* Signals — index 2 as of the amended 5-circle order */
    ff_shell_intent(&H.shell, &home); /* RADAR -> LAUNCHER */
    ff_shell_intent(&H.shell, &to_inbox); /* LAUNCHER -> SIGNALS */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_INBOX, ff_shell_view(&H.shell)->active_face);

    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);

    ff_shell_intent(&H.shell, &home); /* SIGNALS -> RADAR (HOME: "from any app -> Radar") */
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_shell_intent(&H.shell, &home); /* RADAR -> LAUNCHER */
    ff_shell_intent(&H.shell, &to_inbox); /* back to SIGNALS */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_INBOX, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(0u, ff_shell_view(&H.shell)->inbox.thread_node);
}

/* AC8 — the inbox's render key covers exactly its rendered projection
 * (the flare-octant churn mold): a same-bucket age tick is CLEAN (no
 * rebuild to destroy the row under a finger); a cross-bucket tick, a new
 * item, and a badge change are each DIRTY (the positive controls that
 * keep the clean assertions honest — the AC4a proxy discipline). */
static void S24_AC8_inbox_key_same_bucket_age_tick_is_clean(void)
{
    harness_init(100000u, false);
    /* S26e amended 2026-09-01: the boot default is the launcher, whose
     * render key masks everything except the unread badge (the same
     * "opaque overlay" discipline the power menu uses) — and the
     * launcher does not render the Signals/inbox content at all
     * (face_dispatch.c dispatches it to ff_scr_launcher_build, not
     * scr_nav.c). Leave it for the Signals face first, so the
     * dirty-key assertions below are testing what they say they test. */
    {
        ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 2u}};
        ff_shell_intent(&H.shell, &leave_launcher);
    }
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "yo");
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* new item: dirty (control) */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    /* Same-bucket age tick: 400 ms leaves the item's rendered age in the
     * same ff_fmt_age bucket (sub-minute "now"). MUST be clean. */
    advance(400u);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a sub-bucket age tick rebuilt the inbox - the row under a finger would be destroyed");

    /* S26(d): the same inbound (paired) text ALSO pushed a BANNER
     * (FF_NOTIFY_MESSAGE) — a legitimate, SEPARATE dirty event 6s after it
     * was pushed (the banner's own auto-expiry, spec AC2: "a banner
     * appearing/expiring is a legitimate rebuild"), independent of the
     * inbox row's own age bucketing this test otherwise exercises. Cross
     * that expiry deliberately, before the "must stay clean" loop below,
     * so the loop tests exactly what its name says and nothing else. */
    advance(6000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the banner's own 6s auto-expiry did not repaint (S26(d) spec AC2)");
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled again */

    /* The churn fix's core: the WHOLE sub-minute span is ONE "now" bucket,
     * so an age ticking second-by-second under a minute must stay CLEAN.
     * Advance well into the sub-minute range a second at a time. */
    for (int s = 0; s < 40; s++) {
        advance(1000u);
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                                  "a sub-minute per-second age tick rebuilt the inbox - the \"now\" bucket leaked "
                                  "the raw second into the render key (the demo-LIVE churn source)");
    }

    /* Cross-bucket tick: crossing the 60 s boundary changes the rendered
     * string "now"->"1 MIN" -> MUST be dirty (positive control proving the
     * clean results above are bucketing, not a dead key). Age is ~46.4 s
     * (400ms + the 6s banner crossing above + 40s of loop); +20 s crosses
     * 60 s. */
    advance(20000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the now->1 MIN boundary did not repaint - the age on glass would go stale");
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled again */

    /* A new item is dirty. */
    inject_status(DANA, "hey");
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));

    /* A badge change (mark-read via the open transition) is dirty. */
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
}

/* Routing rule 4 for the INBOX_* intents (review Finding 3, the
 * S16_AC3b pattern): while a takeover owns the screen, a tap landing
 * where a conversation row was must NOT navigate, NOT mark anything
 * read, and NOT move the send scope. Dismissing the takeover restores
 * the intent (positive control). */
static void S24_AC3_inbox_intents_are_inert_under_a_takeover(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_text(DANA, "you close?"); /* DIRECT -> DANA thread, unread */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread);

    inject_flare(DANA, 300); /* takeover up */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread); /* nothing marked read */
    /* 2: DANA's text + the inbound FLARE's own feed item (dir UNKNOWN ->
     * the CREW conversation) — both still unread. */
    TEST_ASSERT_EQUAL_UINT16(2, ff_feed_unread_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_INT(FF_TARGET_WHOLE_CREW, ff_shell_view(&H.shell)->inbox.target_kind);

    /* Same for the FAB and a picker pick. */
    ff_intent_t fab = {.kind = FF_INTENT_INBOX_NEW, .u = {0}};
    ff_shell_intent(&H.shell, &fab);
    ff_intent_t pick = {.kind = FF_INTENT_INBOX_PICK, .u = {0}};
    pick.u.node_id = DANA;
    ff_shell_intent(&H.shell, &pick);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread);

    /* Positive control: dismissed, the same intent works. */
    ff_intent_t dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};
    ff_shell_intent(&H.shell, &dismiss);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(DANA)->unread);
}

/* AC8, the PRESENCE leg (review Finding 1 — the flare-lesson risk the
 * first churn test could not see because its member never reached SEEN
 * with a ticking age): a QUIET member's rendered second line IS the
 * presence text, so its key must follow exactly what ff_fmt_age puts on
 * glass and nothing rawer.
 *   - SEEN, same rendered bucket        => CLEAN  (bites "key the raw
 *     presence_age_ms": the raw value advances every tick);
 *   - SEEN, rendered bucket crossed     => DIRTY  (positive control);
 *   - LOST, any bucket crossed          => CLEAN  (bites "drop the
 *     SEEN-only gate": LOST renders the bare word, no age — a LOST
 *     member's ticking age must not repaint anything). */
static void S24_AC8_presence_age_keys_rendered_bucket_only(void)
{
    harness_init(100000u, false);
    /* S26e amended 2026-09-01: the boot default is the launcher, whose
     * render key masks everything except the unread badge (the same
     * "opaque overlay" discipline the power menu uses) — and the
     * launcher does not render the Signals/inbox content at all
     * (face_dispatch.c dispatches it to ff_scr_launcher_build, not
     * scr_nav.c). Leave it for the Signals face first, so the
     * dirty-key assertions below are testing what they say they test. */
    {
        ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {.launcher_idx = 2u}};
        ff_shell_intent(&H.shell, &leave_launcher);
    }
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Real SEEN evidence: a DIRECT packet's RSSI sighting (the honest
     * presence leg ff_sigview_presence reads). No feed items — DANA is a
     * quiet row, so presence is the rendered line. */
    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -55);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* row appears: dirty */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_SEEN, view_conv(DANA)->presence);

    /* SEEN, same sub-minute bucket ("SEEN now"): MUST be clean. */
    advance(400u);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a sub-bucket SEEN-age tick rebuilt the inbox - raw presence_age_ms leaked "
                              "into the render key (the flare-octant lesson, presence leg)");

    /* SEEN, bucket crossed (~0.4s -> ~60.4s): the rendered text changes
     * "SEEN now" -> "SEEN 1 MIN" (still well inside the SEEN window, which
     * only ends at FF_CREW_LOST_MS). */
    advance(60000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a rendered SEEN-bucket change did not repaint - the presence age on glass "
                             "would go stale");
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_SEEN, view_conv(DANA)->presence); /* still SEEN (precondition for below) */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    /* Cross into LOST (sighting age > FF_CREW_LOST_MS): a CATEGORY
     * change, rendered ("SEEN ..." -> "LOST") — dirty, then settle. */
    advance(FF_CREW_LOST_MS);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_LOST, view_conv(DANA)->presence);
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    /* LOST renders the bare word — no age — so a full minute-bucket
     * crossing while LOST must stay CLEAN. */
    advance(60000u);
    TEST_ASSERT_EQUAL_INT(FF_PRESENCE_LOST, view_conv(DANA)->presence); /* still LOST (precondition) */
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a LOST member's un-rendered age bucket dirtied the frame - the key must "
                              "carry presence age ONLY when it is drawn (SEEN)");
}

/* =================================================================== */
/* S24 slice c — thread screens: projection, quick chips, churn key     */
/* =================================================================== */

/* Navigate the route to Signals (the thread chip paths are gated on the
 * Signals base face, like every other base-face control). S26 slice e,
 * amended 2026-09-01: via the BOOT-button launcher — HOME (a no-op if
 * already on the launcher, which is the boot default) followed by
 * LAUNCHER_SELECT idx 2 (Signals — ff_intent.h's fixed 5-circle order).
 * This replaces the retired swipe-based route this helper used to take;
 * it works from ANY starting base (HOME always returns to the launcher
 * first), not just Radar. */
static void s24c_swipe_to_inbox(void)
{
    ff_intent_t home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&H.shell, &home); /* any base -> LAUNCHER (no-op if already there) */
    ff_intent_t select = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    select.u.launcher_idx = 2u; /* Signals — index 2 as of the amended 5-circle order */
    ff_shell_intent(&H.shell, &select); /* LAUNCHER -> SIGNALS */
}

/* AC4 — the thread PROJECTION: opening a thread builds `view.inbox.
 * thread` from the feed, oldest first, direction preserved (in vs out),
 * identity joined only for paired senders, and read on open. */
static void S24c_AC4_thread_projection_builds_messages_both_ways(void)
{
    s22_connect_shell();
    H.ev = ff_shell_events(&H.shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "you close?");                 /* DIRECT -> DANA thread */
    advance(1000u);

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    /* My own send, through the REAL chip path (thread scope = DANA), so
     * the thread has an OUT side pushed exactly the way the device does. */
    ff_intent_t omw = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    omw.u.reply = FF_WIRING_REPLY_OMW;
    ff_shell_intent(&H.shell, &omw);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_inbox_t const *sig = &ff_shell_view(&H.shell)->inbox;
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, sig->subview);
    TEST_ASSERT_EQUAL_UINT8(2, sig->thread.msg_count);

    /* Oldest first: DANA's inbound text, then my outgoing reply. */
    ff_inbox_msg_t const *in_msg = &sig->thread.msgs[0];
    TEST_ASSERT_EQUAL(FEED_TEXT, in_msg->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_DIRECT, in_msg->dir);
    TEST_ASSERT_TRUE(in_msg->identity_known); /* paired sender, joined */
    TEST_ASSERT_EQUAL_UINT32(DANA, in_msg->node_id);
    TEST_ASSERT_EQUAL_STRING("you close?", in_msg->text);
    TEST_ASSERT_FALSE(in_msg->unread); /* read on open (AC4) */

    ff_inbox_msg_t const *out_msg = &sig->thread.msgs[1];
    TEST_ASSERT_EQUAL(FEED_TEXT, out_msg->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, out_msg->dir);
    TEST_ASSERT_FALSE(out_msg->identity_known); /* my own send claims no joined identity */
    TEST_ASSERT_EQUAL_STRING("omw", out_msg->text);
}

/* AC4 — a signal arriving INTO the open, visible thread is on glass the
 * moment it is projected, so it is marked read then (the live-arrival
 * half of mark-read); under a takeover nothing is on glass and nothing
 * is marked. */
static void S24c_AC4_live_arrival_into_open_thread_marks_read_only_when_visible(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    inject_text(DANA, "here now");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_UINT16(0, ff_feed_unread_count(ff_shell_feed(&H.shell)));
    TEST_ASSERT_EQUAL_UINT8(1, ff_shell_view(&H.shell)->inbox.thread.msg_count);

    /* Takeover up: an arrival is NOT silently marked read (the user
     * cannot see it), and it surfaces as unread once the takeover ends. */
    inject_flare(KEV_ID, 300); /* unpaired sender's flare is dropped from feed... */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_flare(KEV_ID, 300);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    inject_text(DANA, "u see this?");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_GREATER_THAN_UINT16(0, ff_feed_unread_count(ff_shell_feed(&H.shell)));

    ff_intent_t dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};
    ff_shell_intent(&H.shell, &dismiss);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    /* Visible again: the open thread's items clear on this projection. */
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(DANA) != NULL ? view_conv(DANA)->unread : 999);
}

/* AC4 — the OMW / IN 5 MIN quick chips send to the THREAD's scope, not
 * to the newest feed item's sender (the S16 rule the thread supersedes),
 * and the accepted send lands in the thread as an OUT item end-to-end. */
static void S24c_AC4_quick_chip_canned_reply_targets_thread_scope(void)
{
    s22_connect_shell();
    H.ev = ff_shell_events(&H.shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    /* The proxy-killer: the NEWEST inbound item is from KEV — the old
     * reply-context rule would aim there. The open thread is DANA's. */
    inject_text(DANA, "you close?");
    inject_text(KEV_ID, "unrelated");
    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    size_t tx_before = P.tx_len;
    ff_intent_t omw = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    omw.u.reply = FF_WIRING_REPLY_OMW;
    ff_shell_intent(&H.shell, &omw);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    /* End-to-end: the accepted send appears in the OPEN thread as the
     * newest message, sided OUT. */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    ff_app_inbox_t const *sig = &ff_shell_view(&H.shell)->inbox;
    TEST_ASSERT_GREATER_THAN_UINT8(0, sig->thread.msg_count);
    ff_inbox_msg_t const *last = &sig->thread.msgs[sig->thread.msg_count - 1];
    TEST_ASSERT_EQUAL(FEED_TEXT, last->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, last->dir);
    TEST_ASSERT_EQUAL_STRING("omw", last->text);

    /* The scope survives the send (no S22 AC3 reset while a thread is
     * open — the chips must keep aiming at the thread). */
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, sig->target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, sig->target_node);

    /* CREW thread scope -> broadcast. */
    ff_intent_t back = {.kind = FF_INTENT_BACK, .u = {0}};
    ff_shell_intent(&H.shell, &back);
    ff_intent_t open_crew = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    ff_shell_intent(&H.shell, &open_crew);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    tx_before = P.tx_len;
    ff_intent_t fivemin = {.kind = FF_INTENT_CANNED_REPLY, .u = {0}};
    fivemin.u.reply = FF_WIRING_REPLY_5MIN;
    ff_shell_intent(&H.shell, &fivemin);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
}

/* AC4 — the FLARE chip: FF_INTENT_INBOX_FLARE with a thread open sends to
 * the THREAD's scope, keeps the scope (no reset-to-WHOLE_CREW while the
 * thread is open — a second flare must not silently broadcast), and the
 * OUT flare lands in the thread. (2026-09-02: this was S24c_AC4_pulse_
 * chip_sends_to_thread_scope_and_keeps_scope, FF_INTENT_INBOX_PULSE;
 * retired along with the intent, see ff_intent.h's header note — FLARE is
 * the only chip left, same shape.) */
static void S24c_AC4_flare_chip_sends_to_thread_scope_and_keeps_scope(void)
{
    s22_connect_shell();
    H.ev = ff_shell_events(&H.shell);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    size_t tx_before = P.tx_len;
    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    /* The scope stays the thread; a SECOND flare still goes to DANA (the
     * mutation this pins: shell_sig_reset_target here would make this
     * one a broadcast the user never chose). */
    tx_before = P.tx_len;
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));

    (void)ff_shell_tick(&H.shell, H.clk.t);
    ff_app_inbox_t const *sig = &ff_shell_view(&H.shell)->inbox;
    TEST_ASSERT_EQUAL_UINT8(2, sig->thread.msg_count);
    TEST_ASSERT_EQUAL(FEED_FLARE, sig->thread.msgs[1].kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, sig->thread.msgs[1].dir);
    TEST_ASSERT_EQUAL_INT(FF_TARGET_MEMBER, sig->target_kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, sig->target_node);
}

/* AC8 — the THREAD render key covers exactly the thread's rendered
 * projection (the slice-b churn mold): a same-bucket age tick is CLEAN;
 * a cross-bucket tick is DIRTY (positive control); a new item in THIS
 * thread is DIRTY; a new item in ANOTHER thread is CLEAN (the thread
 * renders nothing about other conversations — their churn must not
 * destroy the chip under a finger). */
static void S24c_AC8_thread_key_opaque_to_other_conversations_churn(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    s24c_swipe_to_inbox();
    inject_text(DANA, "yo");
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));  /* thread opened: dirty */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    /* S26(d): the earlier inject_text(DANA, "yo") also pushed a BANNER
     * (FF_NOTIFY_MESSAGE) — a legitimate, separate dirty tick 6s after it
     * was pushed (spec AC2: auto-expiry is a legitimate rebuild). Cross
     * that expiry before the "must stay clean" loop, so the loop tests
     * only the thread's own per-second age bucketing (this test's actual
     * subject), same fix as S24_AC8_inbox_key_same_bucket_age_tick_is_clean. */
    advance(6000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the banner's own 6s auto-expiry did not repaint (S26(d) spec AC2)");
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled again */

    /* Sub-minute age ticks (the whole "now" span is ONE bucket): a message
     * age ticking second-by-second under a minute must NOT rebuild the open
     * thread and destroy the chip under a finger. */
    for (int s = 0; s < 40; s++) {
        advance(1000u);
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                                  "a sub-minute per-second message-age tick rebuilt the open thread - the "
                                  "\"now\" bucket leaked the raw second into the render key");
    }

    /* Cross-bucket: crossing 60 s changes "now"->"1 MIN" -> dirty (positive
     * control proving the clean results above are bucketing), then settle.
     * Age is ~46 s (the 6s banner crossing above + 40s of loop); +21 s
     * crosses 60 s. */
    advance(21000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the now->1 MIN message-age boundary did not repaint - the age on glass "
                             "would go stale");
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));

    /* A new item in ANOTHER conversation (KEV's 1:1) while DANA's thread is
     * open: the THREAD CONTENT itself must stay opaque (KEV's item must
     * never leak into DANA's own message list — the property this test
     * names), but a paired KEV TEXT is ALSO a banner-eligible kind
     * (S26(d) spec: "MESSAGE or RALLY"), and a banner is a genuinely NEW
     * top-of-glass surface, not part of the inbox/thread content this
     * opacity rule protects (shell_render_key's comment on the popup/
     * rally mask makes the identical call the other way). So this DOES
     * dirty — once, for the banner's arrival — and the thread's own
     * projected content must be provably unchanged underneath it. */
    uint8_t const msg_count_before = ff_shell_view(&H.shell)->inbox.thread.msg_count;
    inject_text(KEV_ID, "elsewhere");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a banner from another paired conversation must dirty the key - it is a new "
                             "top-of-glass surface (S26(d))");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(msg_count_before, ff_shell_view(&H.shell)->inbox.thread.msg_count,
                                    "KEV's item leaked into DANA's own open thread - the THREAD CONTENT key "
                                    "must stay opaque to other conversations' traffic even though the banner "
                                    "overlay correctly is not");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "settled: no further churn from the banner");

    /* Positive control: a new item in THIS thread is dirty. */
    inject_text(DANA, "same thread");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a new message in the OPEN thread did not repaint - the conversation on "
                             "glass would go stale");
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));
}

/* AC8, the 1:1 header-presence leg: the header's "SEEN <age>" keys on
 * its rendered ff_fmt_age bucket — same-bucket ticks clean, bucket
 * crossings dirty. */
static void S24c_AC8_thread_header_presence_keys_rendered_bucket(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    /* Real SEEN evidence (a DIRECT packet's RSSI sighting): the header
     * line appears -> dirty, then settles. */
    inject_rx_meta(DANA, MC_RX_PATH_DIRECT, true, -55);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));

    advance(400u);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a sub-bucket SEEN-age tick rebuilt the thread - raw presence_age_ms leaked "
                              "into the thread key");
    /* Crossing 60 s changes the header's "SEEN now" -> "SEEN 1 MIN" -> dirty
     * (positive control; still SEEN — the window ends only at LOST_MS). */
    advance(60000u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the SEEN now->1 MIN boundary did not repaint the thread header");
}

/* Fix: a Signals 1:1 must render the MEMBER's own identity color, not a
 * default. color_idx is app-assigned (ff_crew.h); the shell assigns each
 * roster member its slot index when it first pairs (shell_pair), so paired
 * members get DISTINCT palette colors and the 1:1 thread header projects the
 * open member's real color_idx — before this, every member kept color_idx 0
 * and every 1:1 (and radar dot) rendered palette[0]. */
static void S24_inbox_1to1_projects_the_members_own_color(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));   /* roster slot 0 -> color 0 */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true)); /* roster slot 1 -> color 1 */

    /* Distinct, non-default identity colors (the bug was all-zero). */
    TEST_ASSERT_NOT_EQUAL_INT(member(DANA)->color_idx, member(KEV_ID)->color_idx);
    TEST_ASSERT_NOT_EQUAL_INT(0, member(KEV_ID)->color_idx);

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = KEV_ID;
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_inbox_t const *sig = &ff_shell_view(&H.shell)->inbox;
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, sig->subview);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, sig->thread_node);
    /* The 1:1 header dot/accent (scr_inbox.c reads sig->thread_color_idx)
     * carries KEV's real color, not the palette[0] default. */
    TEST_ASSERT_EQUAL_UINT8(member(KEV_ID)->color_idx, sig->thread_color_idx);
    TEST_ASSERT_NOT_EQUAL_INT(0, sig->thread_color_idx);

    /* And the inbox conversation row projects the same per-member color. */
    TEST_ASSERT_EQUAL_UINT8(member(KEV_ID)->color_idx, view_conv(KEV_ID)->color_idx);
}

/* =================================================================== */
/* S26 slice d — ff_notify + the message banner (docs/specs/            */
/* S26-device-lifecycle.md, "(d) ff_notify + message banner"). AC3: an  */
/* unpaired sender's MESSAGE/RALLY never enqueues a banner; a paired    */
/* one does. Also: the coalesce-through-the-shell effect, expiry, and   */
/* the FF_INTENT_BANNER_OPEN routing/mark-read/dismiss seam.            */
/* =================================================================== */

/* AC3 — a paired sender's text produces a BANNER, sender-name-prefixed
 * per the CREW-preview convention (scr_inbox.c's preview_from_known
 * pattern, S26(d) doc comment on shell_notify_push_banner). */
static void S26_AC3_paired_message_pushes_banner(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* NodeInfo name — projected separately, see below */

    inject_text(DANA, "you close?");
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_banner_t const *b = &ff_shell_view(&H.shell)->banner;
    TEST_ASSERT_TRUE(b->active);
    TEST_ASSERT_EQUAL_INT(FF_NOTIFY_MESSAGE, b->kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, b->node_id);
    /* name and text are SEPARATE fields (not baked into one string) —
     * scr_banner.c renders the name in the sender's crew color, distinct
     * from the plain preview body. */
    TEST_ASSERT_EQUAL_STRING("DANA", b->name);
    TEST_ASSERT_EQUAL_STRING("you close?", b->text);
}

/* AC3, the other half — an UNPAIRED sender's text must NEVER enqueue a
 * banner (the S22 stranger rule), even though ff_wiring still notes them
 * as heard. Proxy check: DANA is deliberately never paired here, so a
 * banner appearing would prove the gate is missing, not merely untested. */
/* S26 (c)+(d) wake hook — a pushed BANNER must light a dim/off screen.
 * The DECISION lives in the shell beside the push (ff_shell_take_wake pulses
 * true); the ff_idle owner only forwards it. One-shot: true exactly once,
 * then false until the next banner; several pushes in one tick collapse to
 * one true. Proxy check: an unpaired sender pushes no banner, so it must
 * also raise no wake — a wake without a banner would prove the pulse is
 * wired to the wrong event. */
static void S26_wake_pulse_true_once_after_banner_then_clear(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    TEST_ASSERT_FALSE(ff_shell_take_wake(&H.shell)); /* nothing yet */

    inject_text(DANA, "you close?");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    TEST_ASSERT_TRUE(ff_shell_take_wake(&H.shell));  /* exactly once */
    TEST_ASSERT_FALSE(ff_shell_take_wake(&H.shell)); /* cleared on read */

    /* Two banners between takes collapse to one pulse. */
    inject_text(DANA, "still there?");
    inject_text(DANA, "??");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_take_wake(&H.shell));
    TEST_ASSERT_FALSE(ff_shell_take_wake(&H.shell));
}

static void S26_wake_pulse_not_raised_by_unpaired_sender_or_null(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    /* DANA never paired: no banner (S22 stranger rule) => no wake either. */
    inject_text(DANA, "you close?");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_FALSE(ff_shell_take_wake(&H.shell));

    TEST_ASSERT_FALSE(ff_shell_take_wake(NULL)); /* NULL-safe */
}

static void S26_AC3_unpaired_message_pushes_no_banner(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    /* No ff_shell_pair call: DANA is a stranger. */

    inject_text(DANA, "hi, who are you");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* AC3 — a paired sender's RALLY also produces a BANNER (spec: "an
 * incoming MESSAGE or RALLY"), distinct kind. */
static void S26_AC3_paired_rally_pushes_banner(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING); /* NodeInfo name, for the sender-prefixed preview */

    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_rally(buf, sizeof(buf), (ff_latlon_t){39.0, -82.0}, "Main Stage");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_banner_t const *b = &ff_shell_view(&H.shell)->banner;
    TEST_ASSERT_TRUE(b->active);
    TEST_ASSERT_EQUAL_INT(FF_NOTIFY_RALLY, b->kind);
    TEST_ASSERT_EQUAL_UINT32(DANA, b->node_id);
    TEST_ASSERT_EQUAL_STRING("DANA", b->name);
    TEST_ASSERT_EQUAL_STRING("Main Stage", b->text);
}

/* An unpaired sender's RALLY also produces no banner (same gate, the
 * other banner-eligible kind — proxy-check symmetry with the MESSAGE
 * case above). */
static void S26_AC3_unpaired_rally_pushes_no_banner(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_rally(buf, sizeof(buf), (ff_latlon_t){39.0, -82.0}, "Main Stage");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, DANA, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* A FLARE or a RESERVED_01 (retired PULSE) frame from a paired sender must
 * NOT produce a banner — only MESSAGE/RALLY are banner-eligible per spec;
 * FLARE keeps its own, untouched takeover path, and RESERVED_01 is
 * honestly nothing (no feed item either, see ff_wiring's own unit tests) —
 * bench-visible instead via `ff_shell_retired_frame_count` (2026-09-02:
 * this test used to be S26_flare_and_pulse_do_not_push_banners, injecting
 * a real PULSE; PULSE is retired end to end, see ff_proto.h's RESERVED_01
 * section). */
static void S26_flare_and_reserved01_do_not_push_banners(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    TEST_ASSERT_EQUAL_UINT32(0, ff_shell_retired_frame_count(&H.shell));
    inject_reserved01(DANA);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_UINT32(1, ff_shell_retired_frame_count(&H.shell)); /* bench-visible instead */

    inject_flare(DANA, 300);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active); /* the FLARE takeover is separate, untouched */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
}

/* AC1's coalesce rule, observed THROUGH the shell: a second text from the
 * SAME paired sender within 2s replaces the queue's head IN PLACE (not a
 * second, newer entry) — proven by the head still showing the coalesced
 * (latest) text rather than the original one a plain "newest wins" queue
 * would also produce (see ff_notify.h's top comment on why position is
 * preserved). */
static void S26_coalesce_within_2s_updates_head_in_place(void)
{
    harness_init(100000u, false);
    /* S26e amended 2026-09-01: the boot default is the launcher. Corrected
     * 2026-09-02 — this comment used to claim "the launcher does not
     * render the banner overlay at all"; that stopped being true with
     * #157 (the message banner strip moved on top of every face,
     * ff_scr_launcher_build calls ff_scr_banner_build unconditionally same
     * as every other face's builder), and the render-key mask that made
     * it LOOK true here (a stale `key->banner` bug — the launcher branch
     * of shell_render_key zeroed the banner through its memset and never
     * restored it, so a banner-only change never dirtied the key while on
     * the launcher) is now fixed; see the launcher-specific
     * S26_launcher_* tests below for that behavior directly. This test
     * still leaves the launcher first, for a DIFFERENT reason: it is
     * checking the banner's own dirty-key semantics (arrival / coarsened-
     * age churn / expiry) in isolation, on an ordinary base face, rather
     * than re-proving them through the launcher's extra unread-badge
     * masking every time. */
    ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    ff_shell_intent(&H.shell, &leave_launcher);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);

    inject_text(DANA, "first");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("first", ff_shell_view(&H.shell)->banner.text);

    advance(1000u); /* within the 2000ms coalesce window */
    inject_text(DANA, "second");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    ff_app_banner_t const *b = &ff_shell_view(&H.shell)->banner;
    TEST_ASSERT_TRUE(b->active);
    TEST_ASSERT_EQUAL_STRING("second", b->text);

    /* The coalesced entry's freshness reset too (ff_notify.h: the push
     * overwrites at_ms/expiry_ms in place). The differentiator: hold at
     * exactly the FIRST push's original deadline (100000+6000=106000) —
     * still active only because the coalesce reset the clock to the
     * SECOND push (101000+6000=107000); a coalesce that failed to reset
     * at_ms would have already expired here. */
    advance(4999u); /* t = 105999, before either deadline */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    advance(1u); /* t = 106000 exactly — the FIRST push's original deadline */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_view(&H.shell)->banner.active,
                             "banner expired at the ORIGINAL push's deadline - the coalesce did not reset "
                             "expiry_ms to the SECOND push's at_ms");

    advance(1000u); /* t = 107000 — the coalesced (SECOND push) deadline: now due */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "the coalesced entry's own expiry did not repaint");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* AC1/AC2 — the banner auto-expires 6s after at_ms (inclusive boundary,
 * matching ff_flare's own convention), driven by ff_shell_tick. */
static void S26_AC1_banner_auto_expires_after_6s(void)
{
    harness_init(100000u, false);
    /* S26e amended 2026-09-01: the boot default is the launcher. Corrected
     * 2026-09-02 — this comment used to claim "the launcher does not
     * render the banner overlay at all"; that stopped being true with
     * #157 (the message banner strip moved on top of every face,
     * ff_scr_launcher_build calls ff_scr_banner_build unconditionally same
     * as every other face's builder), and the render-key mask that made
     * it LOOK true here (a stale `key->banner` bug — the launcher branch
     * of shell_render_key zeroed the banner through its memset and never
     * restored it, so a banner-only change never dirtied the key while on
     * the launcher) is now fixed; see the launcher-specific
     * S26_launcher_* tests below for that behavior directly. This test
     * still leaves the launcher first, for a DIFFERENT reason: it is
     * checking the banner's own dirty-key semantics (arrival / coarsened-
     * age churn / expiry) in isolation, on an ordinary base face, rather
     * than re-proving them through the launcher's extra unread-badge
     * masking every time. */
    ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    ff_shell_intent(&H.shell, &leave_launcher);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "hi");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    advance(5999u);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_view(&H.shell)->banner.active, "expired one tick early");

    advance(1u); /* exactly 6000ms */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "the banner's own expiry did not repaint");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* AC2 — the coarsened-age discipline (spec: "its age uses the coarsened-
 * age discipline — shell_coarsen_age_ms — so per-second churn does not
 * rebuild"), the same S24_AC8 churn mold applied to the banner: a
 * banner's whole 6s life sits inside ff_fmt_age's one sub-minute "now"
 * bucket, so ticking it second-by-second must NOT dirty the render key
 * (a rebuild mid-press would destroy the strip's own tap target — the
 * exact clobber class this discipline exists to prevent) until the
 * banner's own content genuinely changes (a new push, or its expiry). */
static void S26_AC2_banner_age_same_bucket_ticks_are_clean(void)
{
    harness_init(100000u, false);
    /* S26e amended 2026-09-01: the boot default is the launcher. Corrected
     * 2026-09-02 — this comment used to claim "the launcher does not
     * render the banner overlay at all"; that stopped being true with
     * #157 (the message banner strip moved on top of every face,
     * ff_scr_launcher_build calls ff_scr_banner_build unconditionally same
     * as every other face's builder), and the render-key mask that made
     * it LOOK true here (a stale `key->banner` bug — the launcher branch
     * of shell_render_key zeroed the banner through its memset and never
     * restored it, so a banner-only change never dirtied the key while on
     * the launcher) is now fixed; see the launcher-specific
     * S26_launcher_* tests below for that behavior directly. This test
     * still leaves the launcher first, for a DIFFERENT reason: it is
     * checking the banner's own dirty-key semantics (arrival / coarsened-
     * age churn / expiry) in isolation, on an ordinary base face, rather
     * than re-proving them through the launcher's extra unread-badge
     * masking every time. */
    ff_intent_t const leave_launcher = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    ff_shell_intent(&H.shell, &leave_launcher);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "hi");
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* new banner: dirty (control) */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    /* The banner's entire 6s life is inside the sub-minute "now" bucket —
     * every one-second tick up to (but not including) its own expiry must
     * be clean. */
    for (int s = 0; s < 5; s++) {
        advance(1000u);
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                                  "a sub-bucket banner-age tick rebuilt the strip - the tap target under a "
                                  "finger would be destroyed");
    }

    /* Positive control: the banner's OWN expiry (a genuine content change,
     * active true -> false) still dirties. */
    advance(1000u); /* t = push + 6000ms */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "the banner's own expiry did not repaint");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* =================================================================== */
/* The stale-launcher-banner bug (this PR's P0 fix), reproduced directly */
/* =================================================================== */

/* The bug itself: `ff_shell_tick`'s dirty bit is exactly a memcmp of
 * `shell_render_key`'s output against the previous tick's. Before this
 * fix, the FF_APP_FACE_LAUNCHER branch of that function zeroed
 * `key->banner` through its `memset(key, 0, sizeof(*key))` and restored
 * only `active_face` and the unread badge — so a banner-only transition
 * (most importantly its own 6 s auto-expiry) left the key byte-identical
 * to the previous tick's while the launcher was showing, and
 * `ff_shell_tick` never reported dirty: an expired banner stayed on the
 * glass, stale, until something UNRELATED (e.g. the unread count)
 * happened to dirty the key. `ff_scr_launcher_build` (scr_launcher.c)
 * calls `ff_scr_banner_build` unconditionally, exactly like every other
 * face's builder (#157), so the bug was purely in this file's render
 * key, never in what the launcher actually draws. Deliberately NOT
 * calling the `leave_launcher` intent the tests above use — staying ON
 * the launcher (the boot default, ff_route_init) is the whole point:
 * these tests fail on the pre-fix mask and pass once key->banner is
 * stashed/restored the same way `af`/`unread_total` already were. */
static void S26_launcher_AC_banner_expiry_dirties_key(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    (void)ff_shell_tick(&H.shell, H.clk.t); /* the view is a zeroed struct (active_face NONE) until the
                                              * first project — settle the roster setup above before
                                              * reading it back */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* stay ON the launcher */

    inject_text(DANA, "hi");
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* new banner: the pre-existing arrival path, unaffected
                                                          * by this fix — dirty (control) */
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    /* SUB-BUCKET tick, at 5999ms — still inside shell_coarsen_age_ms's
     * one "now" bucket (s < 60u), same as S26_AC2_banner_age_same_bucket_
     * ticks_are_clean's base-face proof. This is the review round-1
     * finding: restoring the RAW (uncoarsened) `v->banner.age_ms` through
     * the launcher mask instead of the already-coarsened `key->banner`
     * would tick the age every millisecond and dirty the key here too —
     * a proxy that would have made the expiry assertion below pass for
     * the wrong reason (the key dirties on EVERY tick, not just the
     * genuine expiry). Asserting FALSE here is what actually pins the
     * mask restoring the coarsened field, not just any field named
     * `banner`. */
    advance(5999u);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a sub-bucket banner-age tick dirtied the launcher's render key - the mask must "
                              "restore the COARSENED key->banner, not the raw v->banner, or every millisecond "
                              "of a banner's life repaints the launcher");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_view(&H.shell)->banner.active, "expired one tick early");

    /* Exactly 6000ms after the push — the banner's own expiry, and also
     * the coarsening bucket boundary — with NO other state change at all
     * (no new message, no unread delta, no face switch): the ONLY thing
     * that changed is banner.active true->false. This is the exact
     * transition the pre-fix mask swallowed. */
    advance(1u);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the banner's own expiry did not dirty the launcher's render key - an expired "
                             "banner would sit stale on the glass (the P0 bug this PR fixes)");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
}

/* A second, DIFFERENT sender's banner taking over the (now-empty) banner
 * slot on the launcher must also dirty the key. `ff_notify` is a FIFO
 * (ff_notify.h): a fresh push from a sender with no live entry does not
 * pre-empt whoever is currently head, so DANA's banner has to actually
 * be gone (its own natural expiry, proven by the test above) before
 * KEV's arrival can become the new head — at which point it is a
 * genuinely new sender/name/text in `key->banner`, not a coalesce
 * (S26_coalesce_within_2s_updates_head_in_place's case, a different
 * sender so not even coalesce-eligible) and not a repeat of the same
 * content. */
static void S26_launcher_AC_second_sender_banner_dirties_key(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(DANA, "DANA", U_EVENING);
    inject_node(KEV_ID, "KEV", U_EVENING);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle the roster setup before reading the view back */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* stay ON the launcher */

    inject_text(DANA, "first");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_STRING("first", ff_shell_view(&H.shell)->banner.text);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_view(&H.shell)->banner.node_id);

    /* DANA's banner runs out its own 6s life (proven dirty by the test
     * above) — this test only needs the slot empty afterward. */
    advance(6000u);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);

    /* KEV's arrival, a DIFFERENT sender, becomes the new head. */
    advance(1000u);
    inject_text(KEV_ID, "second");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a second sender's banner did not dirty the launcher's render key");
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_STRING("second", ff_shell_view(&H.shell)->banner.text);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->banner.node_id);
}

/* Negative control, pinning the launcher mask's PRE-EXISTING anti-clobber
 * property — this PR only ADDS `key->banner` back to what the mask
 * restores; it must not weaken the mask's original job of keeping the
 * radar/feed churn that ticks constantly underneath the launcher (base
 * stays RADAR-worthy state the whole time the launcher is up) from
 * rebuilding the launcher's tree out from under a finger every frame.
 * Proxy-check discipline (AGENTS.md / docs/review/code-review.md item
 * 6): a position update that dirties NOTHING would make the "does not
 * dirty" assertion below vacuous, so this first proves the SAME update
 * genuinely dirties the key on an ordinary face (RADAR), then returns to
 * the launcher and shows the identical update does not. */
static void S26_launcher_radar_position_update_does_not_dirty_key(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.0});
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);
    inject_position(DANA, U_EVENING, 39.01, -82.01);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle the roster setup above before reading the view back */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* the boot default */

    /* Positive control: leave the launcher for the RADAR face and prove
     * this exact kind of position update IS live, rendered content there
     * — otherwise the negative control below would be measuring nothing. */
    ff_intent_t const to_radar = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    ff_shell_intent(&H.shell, &to_radar);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* face switch: settle */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face);
    advance(1000u);
    inject_position(DANA, U_EVENING + 1u, 39.05, -82.05);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "control failed: a moved crew position did not dirty the RADAR face at all");

    /* Back to the launcher, settled, then the SAME kind of update. */
    ff_intent_t const home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&H.shell, &home);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* face switch: settle */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    advance(1000u);
    inject_position(DANA, U_EVENING + 2u, 39.10, -82.10);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a radar position update dirtied the launcher's render key - the tree would "
                              "rebuild under a finger on every position tick (the anti-clobber property "
                              "this mask exists for)");
}

/* Review round 2 — the flare TAKEOVER's own NATURAL expiry (ff_flare_tick's
 * tick-driven timeout, never a FF_INTENT_TAKEOVER_DISMISS) with base ==
 * LAUNCHER underneath. Distinct from S16_AC13_active_face_is_never_flare_
 * even_during_a_takeover (which walks the takeover's whole life on the
 * launcher already) in one respect: that test never asserts the dirty
 * BIT at the precise expiry tick, only that active_face stays sane across
 * many 5s ticks — a takeover that stopped dirtying the key at expiry (the
 * exact clobber the flare-takeover render-key comment documents guarding
 * against, applied to the RECOVERY edge instead of the takeover's own
 * appearance) would pass that test silently. This one pins the expiry
 * tick's return value directly, and that the launcher genuinely renders
 * again afterward — not just that active_face reads LAUNCHER (which,
 * per S16 AC13, was already true THROUGHOUT the takeover and so is not
 * by itself proof the tree came back). */
static void S26_launcher_flare_takeover_natural_expiry_dirties_key_and_renders_launcher(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_EVENING);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle the roster setup before reading the view back */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* the boot default,
                                                                                        * base underneath */

    inject_flare(DANA, 2u); /* short (2s) takeover — receive-side dur_s IS the takeover duration
                              * (ff_flare_on_flare_rx: takeover_expiry_ms = now_ms + dur_s*1000),
                              * no default substitution on the receive path */
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t)); /* takeover raised: dirty (control) */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    /* S16 AC13: active_face is NEVER FLARE, takeover or not. */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    advance(1000u); /* t = push + 1000ms: mid-life, well before the 2000ms expiry */
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "a stable mid-life takeover repainted");

    /* Exactly at the takeover's own NATURAL expiry (2000ms after the
     * push — inclusive boundary, ff_flare.h's documented convention) —
     * driven purely by ff_flare_tick inside ff_shell_tick, NOT
     * FF_INTENT_TAKEOVER_DISMISS (which this test never sends). */
    advance(1000u); /* t = push + 2000ms */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the takeover's own natural expiry did not dirty the render key - the launcher "
                             "would stay stuck showing the takeover's last frame");
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    /* The launcher genuinely renders again, not just active_face reading
     * LAUNCHER (true throughout, S16 AC13) — a SECOND, independent dirty
     * source (the unread badge, the launcher's one live scalar per this
     * mask) still reaches the key, proving the tree is back to the
     * launcher's normal masked behavior rather than wedged on whatever
     * the takeover branch last produced. */
    inject_text(DANA, "you there?"); /* a fresh unread + a banner arrival: both launcher-live facts */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the launcher did not resume normal live rendering after the takeover cleared");
}

/* ---------------------------------------------------------------------
 * S25 slice c — ff_shell_set_batt_mv (docs/specs/S25-power-latch.md
 * "(c) Battery gauge")
 * ------------------------------------------------------------------- */

/* Mutation (c) target: revert ff_shell_tick's radar.batt_pct assignment
 * to the old hardcoded -1 and every assertion below that expects a real
 * percent fails. Also exercises the render-key mask fix alongside the
 * banner/unread ones already pinned above: `radar.batt_pct` is the
 * THIRD field this PR adds to what `shell_render_key`'s LAUNCHER branch
 * restores after its blanket memset (see that function's own doc
 * comment) — before this fix a battery reading that changed while the
 * launcher was showing (the common case; the launcher is home, S26
 * slice e) left the render key byte-identical and the status row sat
 * stale on the glass, the exact clobber class the banner exception
 * (#157) already fixed for a different field.
 *
 * PR #180 review round: `ff_shell_set_batt_mv` now takes `now_ms`
 * explicitly (that function's own doc comment) — every call below
 * passes `H.clk.t`, the harness's own fake-clock reading, the same
 * value `ff_shell_tick` is driven with, so pushes and ticks share one
 * consistent timeline (as any real caller must). The filter's own
 * moving-average + Schmitt-hysteresis math (mean-of-4 in the mV domain,
 * asymmetric low-band exit margin) is covered in isolation by
 * `core/tests/test_batt.c`; this test only pins that the shell wires a
 * real caller-supplied clock through faithfully and that the launcher
 * render-key mask reacts to the resulting displayed changes.
 */
static void S25c_batt_mv_reflects_filtered_value_and_dirties_launcher_key(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle boot */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face); /* the boot default */

    /* Before any push: honestly unknown, same "-1 until the first
     * sensor push" contract heading_deg/my_pos_ok already have. */
    TEST_ASSERT_EQUAL_INT8(-1, ff_shell_view(&H.shell)->radar.batt_pct);

    /* mv=3700 is an exact OCV-table point (ff_batt.h) -> 30%. First-
     * ever reading: shows immediately (ff_batt.h's "unknown-until-
     * read, then real", no warm-up), and this unknown -> known
     * transition must dirty the launcher's render key. */
    ff_shell_set_batt_mv(&H.shell, 3700, H.clk.t);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "the first battery reading did not dirty the launcher's render key - "
                             "radar.batt_pct is being masked out again (mutation (c)-adjacent regression)");
    TEST_ASSERT_EQUAL_INT8(30, ff_shell_view(&H.shell)->radar.batt_pct);

    /* mv=3707 -> 51% in isolation, but the pre-filled window (see
     * ff_batt.h's FF_BATT_FILTER_WINDOW doc comment) averages it with
     * the three settled 3700s first: (3700*3+3707)/4 rounds to 3702 mV,
     * still 30% — absorbed entirely, comfortably inside
     * FF_BATT_HYSTERESIS_PCT and nowhere near FF_BATT_LOW_PCT. The
     * displayed value — and therefore the launcher's render key — must
     * stay exactly as it was. Dropping the core hysteresis (mutation
     * (b)) is exactly what would flip this assertion. */
    advance(1000u);
    ff_shell_set_batt_mv(&H.shell, 3707, H.clk.t);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "a battery reading wobble dirtied the launcher's render key");
    TEST_ASSERT_EQUAL_INT8(30, ff_shell_view(&H.shell)->radar.batt_pct);

    /* A sustained, genuine move: mv=3900 (table-exact 65%) replaces one
     * more of the window's four slots. Mean = (3700*2+3707+3900)/4
     * rounds to 3752 mV -> 40% (ff_batt_pct_from_mv, not re-derived
     * here) — a 10-point move that clears the ordinary hysteresis, a
     * real KNOWN-to-KNOWN change (not merely the unknown->known edge
     * above), and it must dirty the key too. */
    advance(1000u);
    ff_shell_set_batt_mv(&H.shell, 3900, H.clk.t);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a genuine, sustained battery reading change did not dirty the launcher's "
                             "render key");
    TEST_ASSERT_EQUAL_INT8(40, ff_shell_view(&H.shell)->radar.batt_pct);
}

/* PR #180 review, should-fix #4: `ff_shell_set_batt_mv` used to read
 * `sh->now_ms` (the shell's own last-tick clock), which defaults to 0
 * before the shell's first `ff_shell_tick` — a battery read that
 * happens before that first tick (plausible at boot) would push
 * against a fake "now = 0" and get recorded (internally, in the OLD
 * buggy version) as having happened at time 0. Once the first REAL
 * tick then runs and updates `sh->now_ms` to the true boot-time clock
 * reading, a SECOND battery push shortly after that tick would (under
 * the old bug) compute its gap against that stale "time 0" instead of
 * the first push's own real moment — looking like a many-second gap
 * and spuriously tripping `FF_BATT_FILTER_STALE_GAP_MS`'s window
 * reset. This test drives exactly that shape — push, THEN a tick (the
 * moment `sh->now_ms` would jump, in the buggy version), THEN a second
 * push shortly after — with the fix in place: `now_ms` is now the
 * caller's own explicit argument on every call, never `sh->now_ms`, so
 * inserting a tick in between changes nothing about the filter's own
 * timeline. No spurious reset happens: the second push is diluted
 * (partially blended with the first), not treated as a lone post-gap
 * sample.
 */
static void S25c_set_batt_mv_before_first_tick_does_not_spuriously_reset(void)
{
    harness_init(100000u, false); /* H.clk.t starts at 100000, matching a real boot-time clock reading */

    /* Pushed BEFORE any ff_shell_tick call at all — sh->now_ms (were it
     * still read internally) would be 0 here, not 100000, and the OLD
     * buggy code would have recorded the filter's last-push time as 0. */
    ff_shell_set_batt_mv(&H.shell, 3700, H.clk.t);

    /* The FIRST real tick — this is the exact moment sh->now_ms jumps
     * from 0 to a real value (100000) in the old buggy code, which is
     * what makes the next push's gap look enormous under that bug. */
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_EQUAL_INT8(30, ff_shell_view(&H.shell)->radar.batt_pct);

    /* A second push 1s after that tick, comfortably under
     * FF_BATT_FILTER_STALE_GAP_MS as measured against the FIRST push's
     * own real time. The old bug would instead measure this gap
     * against the stale "time 0" the first push was mis-recorded at,
     * making it look like a ~100000ms gap — well past the 30000ms
     * threshold — and spuriously reset the window so this mv=4000
     * sample would dominate undiluted (80%). With the fix, this is an
     * ordinary short gap: mean = (3700*3+4000)/4 rounds to 3775 mV ->
     * 45% (ff_batt_pct_from_mv), the diluted (not undiluted) answer. */
    advance(1000u);
    ff_shell_set_batt_mv(&H.shell, 4000, H.clk.t);

    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    TEST_ASSERT_EQUAL_INT8_MESSAGE(45, ff_shell_view(&H.shell)->radar.batt_pct,
        "a battery push shortly after the first tick was spuriously treated as a stale-gap "
        "reset - ff_shell_set_batt_mv must use its own now_ms argument on every call, never "
        "sh->now_ms");
}

/* AC2 — a banner from a DIFFERENT paired conversation than the one
 * currently open MUST dirty the render key: it is a genuinely NEW top-
 * of-glass surface, not part of the open thread's own content (unlike an
 * inbox-only churn source, e.g. a STATUS elsewhere, which the S24c_AC8
 * opacity tests correctly keep invisible). The full shape: dirty once on
 * arrival, clean through the coarsened-age sub-minute bucket, dirty once
 * more (only) at the banner's own 6s expiry — proving the open thread's
 * OWN content never sees KEV's item while the banner overlay correctly
 * does. */
static void S26_AC2_banner_from_other_paired_conv_dirties_open_thread_key(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    s24c_swipe_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = DANA;
    ff_shell_intent(&H.shell, &open);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));  /* thread opened: dirty */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */

    uint8_t const msg_count_before = ff_shell_view(&H.shell)->inbox.thread.msg_count;

    /* KEV, not DANA: a banner from the OTHER paired conversation while
     * DANA's thread is open. */
    inject_text(KEV_ID, "you around?");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a banner from another paired conversation must dirty the open thread's key "
                             "- it is a new top-of-glass surface (S26(d))");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(msg_count_before, ff_shell_view(&H.shell)->inbox.thread.msg_count,
                                    "KEV's item leaked into DANA's own open thread content");
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, ff_shell_view(&H.shell)->banner.node_id);

    /* Coarsened age (spec: "so per-second churn does not rebuild"): the
     * banner's entire 6s life sits inside ff_fmt_age's one sub-minute
     * "now" bucket, so every one-second tick up to (but not including)
     * its own expiry must be clean — the tap target under a finger must
     * survive. */
    for (int s = 0; s < 5; s++) {
        advance(1000u);
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                                  "a sub-bucket banner-age tick rebuilt the open thread - the tap target "
                                  "under a finger would be destroyed");
    }

    /* Exactly one more dirty tick, at the banner's own 6000ms deadline
     * (inclusive boundary) — its expiry — and the banner is gone. */
    advance(1000u); /* t = push + 6000ms */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "the banner's own expiry did not repaint");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */
}

/* The banner's tap (FF_INTENT_BANNER_OPEN): routes to the sender's
 * thread, marks it read, and dismisses the head banner — S26(d)'s whole
 * navigation contract in one test. */
static void S26_banner_open_routes_marks_read_and_dismisses(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "you close?");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread);

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_INBOX, v->active_face);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, v->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(DANA, v->inbox.thread_node);
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(DANA)->unread); /* marked read */
    TEST_ASSERT_FALSE(v->banner.active);                  /* dismissed */
}

/* A stray BANNER_OPEN with no banner queued is a safe no-op (no route
 * change, no crash) — the screen only ever emits this from a rendered
 * banner, but the shell must not assume that. */
static void S26_banner_open_with_no_banner_is_noop(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle: a fresh shell's very first tick is always dirty */

    ff_app_face_t const before = ff_shell_view(&H.shell)->active_face;
    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(before, ff_shell_view(&H.shell)->active_face);
}

/* Routing rule 4 (the S16_AC3b pattern, same as the INBOX_* intents):
 * while a takeover owns the screen, BANNER_OPEN must not navigate,
 * mark-read or dismiss. */
static void S26_banner_open_is_inert_under_a_takeover(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "you close?");
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    inject_flare(DANA, 300); /* takeover up */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_EQUAL_UINT16(1, view_conv(DANA)->unread);         /* not marked read */
    TEST_ASSERT_NOT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
}

/* =================================================================== */
/* S26 banner-opens-conversation bugfix (2026-09-03,                    */
/* docs/specs/S26-device-lifecycle.md Amendments): "a banner opens the  */
/* conversation the message belongs to" — a GROUP/broadcast message's   */
/* banner must open the CREW thread, never the sender's private 1:1     */
/* thread. Maintainer repro: tapping the banner for a message that went */
/* to the GROUP chat opened the sender's 1:1 conversation instead.      */
/* =================================================================== */

/* The core bug, fixed: a group TEXT from KEV → banner → BANNER_OPEN →
 * the CREW thread is open — KEV's own direct thread is explicitly NOT
 * what opened (both the numeric thread key AND the "not KEV" negative
 * are asserted, the proxy-check discipline: a test that only checked
 * "some thread opened" could pass on the old, buggy routing too). */
static void S26_banner_open_group_message_opens_crew_thread_not_senders(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(KEV_ID, "KEV", U_EVENING);

    inject_text_broadcast(KEV_ID, "party at main stage");
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_banner_t const *b = &ff_shell_view(&H.shell)->banner;
    TEST_ASSERT_TRUE(b->active);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, b->node_id); /* the banner still names the real sender */
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(0)->unread); /* the CREW row, not KEV's, carries the unread */

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_INBOX, v->active_face);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, v->inbox.subview);
    /* The bug, precisely: thread_node used to be KEV_ID (the sender). */
    TEST_ASSERT_EQUAL_UINT32(0, v->inbox.thread_node);          /* CREW sentinel — the fix */
    TEST_ASSERT_NOT_EQUAL_UINT32(KEV_ID, v->inbox.thread_node); /* explicitly not the sender's own thread */
    TEST_ASSERT_FALSE(v->banner.active);                        /* dismissed */

    /* Mark-read hit the CREW conversation, not KEV's (which has no
     * traffic of its own here and so is honestly absent from the
     * conversation list per ff_inbox.h's paired-member rule — no
     * standalone DANA/KEV row exists to assert "still unread" against;
     * the CREW row's own unread clearing is the honest, available
     * proof). */
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(0)->unread);
}

/* Sibling of the group case: a DIRECT TEXT from KEV opens KEV's own 1:1
 * thread (unchanged by this bugfix — the existing DANA-flavored
 * S26_banner_open_routes_marks_read_and_dismisses test above already
 * covers this shape; this one exists so the group/direct pair sits
 * side by side with the SAME sender id, making the destination — not
 * the sender — the only variable between the two tests). */
static void S26_banner_open_direct_message_opens_senders_thread(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(KEV_ID, "KEV", U_EVENING);

    inject_text(KEV_ID, "you close?"); /* to == MY_ID: DIRECT */
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_UINT16(1, view_conv(KEV_ID)->unread);
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(0)->unread); /* CREW untouched by a direct message */

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, v->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, v->inbox.thread_node); /* the sender's own thread, not CREW */
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(KEV_ID)->unread); /* marked read */
    TEST_ASSERT_FALSE(v->banner.active);
}

/* A RALLY to the whole crew (MC_ADDR_BROADCAST) also opens the CREW
 * thread on tap — the OTHER banner-eligible kind (spec: "an incoming
 * MESSAGE or RALLY"), same routing rule. */
static void S26_banner_open_group_rally_opens_crew_thread(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(KEV_ID, "KEV", U_EVENING);

    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_rally(buf, sizeof(buf), (ff_latlon_t){39.0, -82.0}, "Main Stage");
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, KEV_ID, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_banner_t const *b = &ff_shell_view(&H.shell)->banner;
    TEST_ASSERT_TRUE(b->active);
    TEST_ASSERT_EQUAL_INT(FF_NOTIFY_RALLY, b->kind);
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, b->node_id);

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, v->inbox.subview);
    TEST_ASSERT_EQUAL_UINT32(0, v->inbox.thread_node); /* CREW, not KEV */
    TEST_ASSERT_EQUAL_UINT16(0, view_conv(0)->unread); /* marked read */
}

/* Proxy-check target for the mutation gate (PR body): a naive "route by
 * node_id only" revert would make this pass DIRECT (node_id happens to
 * equal the right thread there) but fail GROUP — exactly the asymmetry
 * S26_banner_open_group_message_opens_crew_thread_not_senders and
 * S26_banner_open_direct_message_opens_senders_thread together pin down.
 * A same-sender, two-conversations regression: KEV sends to the group
 * first, then DIRECT — two separate banners (ff_notify's conv-aware
 * coalescing — see test_notify.c), and opening the OLDEST (the group
 * one, still head-of-queue) must land on CREW, not KEV, even though a
 * KEV-addressed banner is also queued right behind it. */
static void S26_banner_open_picks_the_head_banners_own_conversation(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    inject_node(KEV_ID, "KEV", U_EVENING);

    inject_text_broadcast(KEV_ID, "party at main stage"); /* queued first (head) */
    advance(2500u); /* past the 2s coalesce window, so the next push is a second, distinct entry
                     * (ff_notify's conv-aware coalescing — same sender, different conv, never
                     * merges regardless of timing, but advancing past the window keeps this test's
                     * intent legible without leaning on that separately-tested guarantee). */
    inject_text(KEV_ID, "you coming?");                   /* queued second, DIRECT */
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_intent_t open = {.kind = FF_INTENT_BANNER_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    (void)ff_shell_tick(&H.shell, H.clk.t);

    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_EQUAL_UINT32(0, v->inbox.thread_node); /* the HEAD banner's conv (CREW), not the second (DIRECT) */
    TEST_ASSERT_TRUE(v->banner.active);                /* the second (direct) banner is still queued */
    TEST_ASSERT_EQUAL_UINT32(KEV_ID, v->banner.node_id);
}

/* =================================================================== */
/* S24 slice d — action popup (AC5) + Rally screen (AC6) + opacity (AC8) */
/*  + the demo-loopback send seam.                                       */
/* =================================================================== */

/* A pack with landmarks: one short-named ("Main Stage") and one whose
 * name (26 chars, no spaces) is too long to survive the WHEN suffix
 * intact — the proxy-killer for the truncate rule. Venue at (39,-82) so
 * the landmarks project/unproject at festival scale. */
static char const PACK_JSON_RALLY[] =
    "{\"festpack\":\"0.1\",\"utc_offset_min\":0,"
    "\"festival\":{\"name\":\"Rally Fest\",\"year\":2026,"
    "\"start\":\"2026-09-18\",\"end\":\"2026-09-20\","
    "\"venue\":{\"lat\":39.0,\"lon\":-82.0}},"
    "\"stages\":[{\"id\":\"a\",\"name\":\"A Stage\",\"color\":\"#00ff00\"}],"
    "\"schedule\":[{\"artist\":\"Solo\",\"stage\":\"a\",\"day\":\"2026-09-18\","
    "\"start\":\"20:00\",\"end\":\"21:00\"}],"
    "\"map\":{\"landmarks\":["
    "{\"id\":\"ms\",\"name\":\"Main Stage\",\"lat\":39.001,\"lon\":-82.001},"
    "{\"id\":\"bg\",\"name\":\"RallyPointAtTheBigOpenField\",\"lat\":39.002,\"lon\":-82.0}"
    "]}}";

/* Navigate to Signals (via the BOOT-button launcher, S26 slice e) so the
 * Signals sub-views are the visible face (the popup/rally are then
 * really rendered, and mark-read runs). */
static void s24d_to_inbox(void)
{
    ff_intent_t home = {.kind = FF_INTENT_HOME, .u = {0}};
    ff_shell_intent(&H.shell, &home); /* RADAR -> LAUNCHER */
    ff_intent_t select = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    select.u.launcher_idx = 2u; /* Signals — index 2 as of the amended 5-circle order */
    ff_shell_intent(&H.shell, &select); /* LAUNCHER -> SIGNALS */
}

/* Open a thread on `conv_node` (0 = crew) then its FAB -> the action
 * popup scoped to that thread. */
static void s24d_open_popup(uint32_t conv_node)
{
    s24d_to_inbox();
    ff_intent_t open = {.kind = FF_INTENT_INBOX_OPEN_THREAD, .u = {0}};
    open.u.node_id = conv_node;
    ff_shell_intent(&H.shell, &open);
    ff_intent_t fab = {.kind = FF_INTENT_INBOX_NEW, .u = {0}};
    ff_shell_intent(&H.shell, &fab);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_POPUP, ff_shell_view(&H.shell)->inbox.subview);
}

static void s24d_open_rally(void)
{
    ff_intent_t pr = {.kind = FF_INTENT_INBOX_POPUP_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &pr);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_RALLY, ff_shell_view(&H.shell)->inbox.subview);
}

/* The popup's third row is a FLARE, not a pulse (2026-09-02: PULSE is
 * retired end to end — this row used to also be exercised as
 * FF_INTENT_INBOX_POPUP_PULSE by a test of this exact shape; removed
 * along with the intent itself, see ff_intent.h's header note):
 * FF_INTENT_INBOX_POPUP_FLARE
 * sends a FLARE to the scope (decoded TYPE + dest NODE, not "a send
 * happened"), pushes the OUT FEED_FLARE item, and pops back to the thread. */
static void S24_popup_flare_sends_flare_to_scope_and_closes(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    s24d_open_popup(DANA);

    size_t const tx_before = P.tx_len;
    ff_intent_t pf = {.kind = FF_INTENT_INBOX_POPUP_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &pf);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));

    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
    ff_feed_item_t const *it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_FLARE, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_UINT32(DANA, it->to_node);
}

/* AC5 — the popup Compose row opens the composer with TO = the scope and
 * drops the sub-view to THREAD (under the modal). */
static void S24_AC5_popup_compose_opens_composer_at_scope(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    s24d_open_popup(DANA);

    ff_intent_t pc = {.kind = FF_INTENT_INBOX_POPUP_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &pc);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_EQUAL_UINT32(DANA, ff_shell_compose_to_node(&H.shell));
    /* Opening the composer LEAVES the Signals face, which resets the
     * sub-view to INBOX (the standing "a fresh entry to Signals lands on
     * the inbox" rule — the same behavior SIG_COMPOSE has): backing out of
     * the composer returns to the inbox, not a stale thread. */
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_INBOX, ff_shell_view(&H.shell)->inbox.subview);
}

/* AC5 — the popup Rally row opens the Rally sub-view. */
static void S24_AC5_popup_rally_opens_rally_screen(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    s24d_open_popup(DANA);
    s24d_open_rally();
}

/* AC6 — the WHERE list is the pack's landmarks (real names, in pack order),
 * On Me enabled iff my position is known. */
static void S24_AC6_rally_places_from_pack(void)
{
    s22_connect_shell();
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_RALLY, sizeof(PACK_JSON_RALLY) - 1u));
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.0});
    s24d_open_popup(0u); /* crew scope */
    s24d_open_rally();

    ff_app_rally_t const *r = &ff_shell_view(&H.shell)->inbox.rally;
    TEST_ASSERT_TRUE(r->on_me_ok);
    TEST_ASSERT_EQUAL_UINT8(2, r->place_count);
    TEST_ASSERT_EQUAL_STRING("Main Stage", r->place_names[0]);
}

/* AC6 — On Me disabled without a fix encodes NOTHING: with no pack and no
 * my_pos, the only "place" is a disabled On Me. Selecting it is refused and
 * Send transmits nothing (never a fabricated {0,0}). Member scope so a
 * first tap WOULD send if anything were resolvable. */
static void S24_AC6_on_me_disabled_encodes_nothing(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* No pack, no my_pos. */
    s24d_open_popup(DANA);
    s24d_open_rally();

    ff_app_rally_t const *r = &ff_shell_view(&H.shell)->inbox.rally;
    TEST_ASSERT_FALSE(r->on_me_ok);
    TEST_ASSERT_EQUAL_UINT8(0, r->place_count);
    TEST_ASSERT_FALSE(r->can_send);

    ff_intent_t selp = {.kind = FF_INTENT_RALLY_SELECT_PLACE, .u = {0}};
    selp.u.rally_idx = 0u; /* On Me */
    ff_shell_intent(&H.shell, &selp); /* rejected — row disabled */

    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len); /* nothing encoded */
}

/* AC6 — the WHEN rides in the rally NAME. A member rally to "Main Stage"
 * with WHEN +15m sends TYPE RALLY, addressed to the member, whose wire
 * name is exactly "Main Stage +15m" and whose position unprojects back to
 * the landmark's own lat/lon. */
static void S24_AC6_when_rides_in_the_name(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_RALLY, sizeof(PACK_JSON_RALLY) - 1u));
    s24d_open_popup(DANA);
    s24d_open_rally();

    ff_intent_t sp = {.kind = FF_INTENT_RALLY_SELECT_PLACE, .u = {0}};
    sp.u.rally_idx = 1u; /* first landmark = Main Stage */
    ff_shell_intent(&H.shell, &sp);
    ff_intent_t cw = {.kind = FF_INTENT_RALLY_CYCLE_WHEN, .u = {0}};
    ff_shell_intent(&H.shell, &cw); /* Now -> +15m */

    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_shell_intent(&H.shell, &send); /* member: first tap sends */
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(DANA, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    ff_proto_msg_t msg;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, &msg));
    TEST_ASSERT_EQUAL_STRING("Main Stage +15m", msg.body.rally.name);
    /* The landmark's position round-trips through unproject. */
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 39.001, msg.body.rally.pos.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -82.001, msg.body.rally.pos.lon);

    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
}

/* AC6 — the truncate rule: a place name too long to fit alongside the WHEN
 * suffix is truncated on the PLACE, never the suffix. The proxy this kills:
 * a naive truncate of the whole "<place> +30m" string to 24 bytes would
 * clip the "+30m" tag; here the sent name must be <= 24 bytes, END with
 * " +30m" intact, and begin with the (truncated) landmark prefix. */
static void S24_AC6_when_truncates_place_never_suffix(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_RALLY, sizeof(PACK_JSON_RALLY) - 1u));
    s24d_open_popup(DANA);
    s24d_open_rally();

    ff_intent_t sp = {.kind = FF_INTENT_RALLY_SELECT_PLACE, .u = {0}};
    sp.u.rally_idx = 2u; /* the long-named landmark */
    ff_shell_intent(&H.shell, &sp);
    ff_intent_t cw = {.kind = FF_INTENT_RALLY_CYCLE_WHEN, .u = {0}};
    ff_shell_intent(&H.shell, &cw); /* Now -> +15m */
    ff_shell_intent(&H.shell, &cw); /* +15m -> +30m */

    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    ff_proto_msg_t msg;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, &msg));
    size_t const nl = strlen(msg.body.rally.name);
    TEST_ASSERT_LESS_OR_EQUAL_UINT((unsigned)FF_PROTO_RALLY_NAME_MAX, (unsigned)nl); /* fits the wire */
    TEST_ASSERT_EQUAL_STRING(" +30m", msg.body.rally.name + (nl - 5u));             /* suffix intact */
    TEST_ASSERT_EQUAL_INT(0, strncmp(msg.body.rally.name, "RallyPoint", 10));       /* place, truncated */
}

/* Reviewer finding (PR #175 review): pin the by-index dispatch itself,
 * separate from the truncation-policy assertions above — selecting the
 * SECOND landmark (rally_idx = 2) must resolve to the SECOND landmark's
 * own position, not silently fall back to the first one (or to On Me). A
 * dispatch bug that always resolved index 1 regardless of the selected
 * index would still pass the truncation test above by accident if it
 * happened to return a name starting with "RallyPoint"; this test isolates
 * the position, which only the correctly-indexed landmark can produce. */
static void S24_AC6_selecting_second_landmark_picks_the_right_one(void)
{
    s22_connect_shell();
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_EQUAL_INT(0, ff_shell_load_pack(&H.shell, PACK_JSON_RALLY, sizeof(PACK_JSON_RALLY) - 1u));
    s24d_open_popup(DANA);
    s24d_open_rally();

    ff_intent_t sp = {.kind = FF_INTENT_RALLY_SELECT_PLACE, .u = {0}};
    sp.u.rally_idx = 2u; /* second landmark: RallyPointAtTheBigOpenField */
    ff_shell_intent(&H.shell, &sp);
    /* WHEN left at NOW (no suffix) — isolates indexing from truncation. */

    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_shell_intent(&H.shell, &send);
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    ff_proto_msg_t msg;
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, &msg));
    /* Name: the SECOND landmark's name (truncated to fit — 27 bytes over a
     * 24-byte budget, no suffix), never "Main Stage". */
    TEST_ASSERT_EQUAL_STRING("RallyPointAtTheBigOpenFi", msg.body.rally.name);
    /* Position: the SECOND landmark's own lat/lon (39.002, -82.0), never
     * the first landmark's (39.001, -82.001) and never On Me. */
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 39.002, msg.body.rally.pos.lat);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, -82.0, msg.body.rally.pos.lon);
}

/* AC6 — a crew-wide rally ARMS on the first Send tap and sends on the
 * second (S22 AC4 armed-confirm precedent); the confirm is visible; the
 * send pops back to the thread. */
static void S24_AC6_crew_rally_arms_then_sends(void)
{
    s22_connect_shell();
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){39.0, -82.0});
    s24d_open_popup(0u); /* crew scope */
    s24d_open_rally();

    size_t const tx_before = P.tx_len;
    ff_intent_t send = {.kind = FF_INTENT_RALLY_SEND, .u = {0}};
    ff_shell_intent(&H.shell, &send); /* first tap: arms, no send */
    TEST_ASSERT_EQUAL_size_t(tx_before, P.tx_len);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->inbox.rally.confirm_armed);

    ff_shell_intent(&H.shell, &send); /* second tap within the window: sends */
    TEST_ASSERT_GREATER_THAN_size_t(tx_before, P.tx_len);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, decode_packet_to(P.tx + tx_before, P.tx_len - tx_before));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_RALLY, decode_packet_private(P.tx + tx_before, P.tx_len - tx_before, NULL));
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_INBOX_SUB_THREAD, ff_shell_view(&H.shell)->inbox.subview);
}

/* AC8 — the popup and Rally screen are OPAQUE in the render key to
 * ordinary INBOX/THREAD churn beneath them (a fresh STATUS elsewhere does
 * NOT dirty the frame — see S26_AC2_banner_from_other_paired_conv_dirties_
 * popup_overlay_exactly_once for the one exception this opacity does NOT
 * cover: a banner-eligible MESSAGE/RALLY from another paired conversation
 * IS a genuinely new top-of-glass surface, and correctly dirties). A
 * genuine popup/rally-own change (opening the Rally screen) still
 * dirties. */
static void S24_AC8_popup_and_rally_opaque_to_feed_churn(void)
{
    s22_connect_shell();
    H.ev = ff_shell_events(&H.shell); /* s22_connect_shell does not bind the inject seam */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true)); /* a paired sender so its traffic reaches the feed */
    s24d_open_popup(DANA);

    /* A settled popup: a no-op tick is clean. */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t));
    /* A fresh inbound TEXT in ANOTHER conversation beneath — a genuine
     * inbox change — must not dirty the OPAQUE popup's OWN content (the
     * popup renders nothing about the inbox beneath it: subview stays
     * POPUP, scope stays DANA), but the paired KEV sender's text is ALSO
     * a banner-eligible kind, and a banner is a genuinely NEW top-of-
     * glass surface — NOT masked by the popup/rally opacity (see
     * shell_render_key's own comment on that mask). So this DOES dirty —
     * once, for the banner's arrival — while the popup's own scope is
     * provably unaffected underneath it. */
    ff_inbox_subview_t const subview_before = ff_shell_view(&H.shell)->inbox.subview;
    uint32_t const thread_node_before = ff_shell_view(&H.shell)->inbox.thread_node;
    inject_text(KEV_ID, "beneath");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a banner from another paired conversation must dirty the key - it is a new "
                             "top-of-glass surface (S26(d))");
    TEST_ASSERT_EQUAL_INT_MESSAGE(subview_before, ff_shell_view(&H.shell)->inbox.subview,
                                  "the banner's arrival must not itself change the popup's own subview");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(thread_node_before, ff_shell_view(&H.shell)->inbox.thread_node,
                                     "the banner's arrival must not itself change the popup's own scope");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "settled: no further churn from the banner");

    /* Opening the Rally screen is a real change (dirties). */
    ff_intent_t pr = {.kind = FF_INTENT_INBOX_POPUP_RALLY, .u = {0}};
    ff_shell_intent(&H.shell, &pr);
    TEST_ASSERT_TRUE(ff_shell_tick(&H.shell, H.clk.t));
    /* The Rally screen is opaque to the same churn (the SAME precise
     * shape: one dirty tick for the banner's arrival, then clean — the
     * Rally screen's own selection/echo fields are untouched). */
    inject_text(KEV_ID, "again");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a banner from another paired conversation must dirty the key over the Rally "
                             "screen too - it is a new top-of-glass surface (S26(d))");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_INBOX_SUB_RALLY, ff_shell_view(&H.shell)->inbox.subview,
                                  "the banner's arrival must not itself change the Rally screen's own subview");
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "settled: no further churn from the banner");
}

/* AC2, the popup-overlay leg — same full shape as the thread test above,
 * over the action popup (S24_AC8_popup_and_rally_opaque_to_feed_churn
 * proves the single dirty+settle for both popup AND rally; this proves
 * the fuller "exactly once, then clean, then exactly once more at
 * expiry" sequence, so a double-repaint on arrival or a missed/duplicated
 * expiry repaint over an overlay can't hide behind the shorter test). */
static void S26_AC2_banner_from_other_paired_conv_dirties_popup_overlay_exactly_once(void)
{
    s22_connect_shell();
    H.ev = ff_shell_events(&H.shell); /* s22_connect_shell does not bind the inject seam */
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));
    s24d_open_popup(DANA);
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled popup */

    ff_inbox_subview_t const subview_before = ff_shell_view(&H.shell)->inbox.subview;

    inject_text(KEV_ID, "you around?");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                             "a banner from another paired conversation must dirty the popup overlay's key "
                             "- it is a new top-of-glass surface (S26(d))");
    TEST_ASSERT_EQUAL_INT(subview_before, ff_shell_view(&H.shell)->inbox.subview);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->banner.active);

    for (int s = 0; s < 5; s++) {
        advance(1000u);
        TEST_ASSERT_FALSE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                                  "a sub-bucket banner-age tick rebuilt the popup overlay - the tap target "
                                  "under a finger would be destroyed");
    }

    advance(1000u); /* t = push + 6000ms */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t), "the banner's own expiry did not repaint");
    TEST_ASSERT_FALSE(ff_shell_view(&H.shell)->banner.active);
    TEST_ASSERT_EQUAL_INT(subview_before, ff_shell_view(&H.shell)->inbox.subview); /* the popup itself never moved */
    TEST_ASSERT_FALSE(ff_shell_tick(&H.shell, H.clk.t)); /* settled */
}


/* The demo-loopback SEND SEAM (the mechanism the device demo build wires):
 * with no accepting sender a broadcast flare is refused and no OUT item
 * appears; installing an accept-every-send sender via ff_shell_set_sender
 * makes the OUT item appear — exactly what the CONFIG_FF_DEMO_MODE loopback
 * does on device. (2026-09-02: this used to send a PULSE — retired, see
 * ff_intent.h's header note — FLARE is the only outbound quick signal
 * left.) */
static int s24d_loop_send_text(void *c, uint32_t d, char const *u)
{
    (void)c;
    (void)d;
    (void)u;
    return 0;
}
static int s24d_loop_send_private(void *c, uint32_t d, uint8_t const *p, size_t n)
{
    (void)c;
    (void)d;
    (void)p;
    (void)n;
    return 0;
}
static void S24_demo_loopback_seam_makes_out_items_appear(void)
{
    harness_init(100000u, false); /* NO transport: the default sender refuses */
    inject_my_info(MY_ID);

    ff_intent_t flare = {.kind = FF_INTENT_INBOX_FLARE, .u = {0}};
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell))); /* refused -> no OUT item */

    ff_wiring_sender_t loop = {s24d_loop_send_text, s24d_loop_send_private, NULL};
    ff_shell_set_sender(&H.shell, loop);
    ff_shell_intent(&H.shell, &flare);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell))); /* accepted -> OUT item appears */
    ff_feed_item_t const *it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_FLARE, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
}

/* ---------------------------------------------------------------------
 * S26 slice (c) — ff_shell_keep_awake, docs/specs/S26-device-lifecycle.md
 * "(c) Inactivity -> dim -> screen off", AC1: "no transition while an
 * FSM-declared keep awake holds (flare takeover pending, power menu
 * open, calibration running)". A pure function of an ff_app_state_t
 * (no shell instance needed) plus the one out-of-view fact
 * (touch_cal_running) — see ff_shell.h's doc comment.
 * ------------------------------------------------------------------- */

static void S26c_AC1_keep_awake_false_when_nothing_holds(void)
{
    ff_app_state_t view;
    memset(&view, 0, sizeof(view));
    view.active_face = FF_APP_FACE_RADAR;

    TEST_ASSERT_FALSE(ff_shell_keep_awake(&view, false));
}

static void S26c_AC1_keep_awake_true_while_flare_takeover_pending(void)
{
    ff_app_state_t view;
    memset(&view, 0, sizeof(view));
    view.active_face = FF_APP_FACE_RADAR;
    view.flare.takeover_active = true;

    TEST_ASSERT_TRUE(ff_shell_keep_awake(&view, false));
}

static void S26c_AC1_keep_awake_true_while_power_menu_open(void)
{
    ff_app_state_t view;
    memset(&view, 0, sizeof(view));
    view.active_face = FF_APP_FACE_POWER_MENU;

    TEST_ASSERT_TRUE(ff_shell_keep_awake(&view, false));
}

static void S26c_AC1_keep_awake_true_while_touch_cal_running(void)
{
    ff_app_state_t view;
    memset(&view, 0, sizeof(view));
    view.active_face = FF_APP_FACE_RADAR;

    TEST_ASSERT_TRUE(ff_shell_keep_awake(&view, true));
}

/* NULL view, e.g. a caller that has not ticked the shell yet — no
 * source to hold awake for, but touch_cal_running (the one fact the
 * view can never carry) still short-circuits true. */
static void S26c_AC1_keep_awake_null_view_is_safe(void)
{
    TEST_ASSERT_FALSE(ff_shell_keep_awake(NULL, false));
    TEST_ASSERT_TRUE(ff_shell_keep_awake(NULL, true));
}

static void S26c_AC1_keep_awake_true_while_quick_flare_pending(void)
{
    ff_app_state_t view;
    memset(&view, 0, sizeof(view));
    view.active_face = FF_APP_FACE_LAUNCHER;
    view.quick_flare_pending = true;

    TEST_ASSERT_TRUE(ff_shell_keep_awake(&view, false));
}

/* ------------------------------------------------------------------- */
/* S10 quick flare (docs/specs/S10-flare.md's Amendments, 2026-09-03):  */
/* "press HOME 5 times quickly to flare to the crew, no screen needed." */
/* ------------------------------------------------------------------- */

/* Drives `ff_shell_home_press` directly at `deliver=true` (the "screen
 * already ACTIVE" case) rather than through the sim's synthetic BOOT
 * button — the wake-only-touch interaction with a genuinely DIM/OFF
 * screen is targets/sim/tests/test_ctl_quick_flare.c's job (a real ctl
 * session, real ff_idle_touch_gate); this file exercises the shell-level
 * multitap-counting/flare-action wiring in isolation, the same split
 * test_wakeonly_touch.c (device-adjacent) and this file (shell-only)
 * already draw for every other input path.
 *
 * Keeps `H.clk.t` in lockstep with the timestamp passed to
 * `ff_shell_home_press`: `FF_INTENT_QUICK_FLARE`'s handler (ff_shell.c)
 * reads "now" from the shell's own injected clock (`shell_now`), not
 * from this parameter — same as every other button intent dispatched
 * through `ff_shell_intent` in this file (FLARE_START, POWER_MENU_OPEN,
 * ...), so a test that let the two drift would be timing its multitap
 * gaps against one clock while the eventual flare-expiry math ran
 * against a different one. */
static void press_home_at(uint32_t t_ms)
{
    H.clk.t = t_ms;
    ff_shell_home_press(&H.shell, t_ms, true);
}

static void S10_quick_flare_5_home_presses_start_sending(void)
{
    harness_init(100000u, false);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);

    press_home_at(100000u); /* 1 */
    press_home_at(100300u); /* 2 */
    press_home_at(100600u); /* 3 */
    press_home_at(100900u); /* 4 */
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_flare(&H.shell)->sending, "fired on the 4th press, not the 5th");
    press_home_at(101200u); /* 5 */

    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "did not fire on the 5th press");

    /* The projected view — what the sender overlay (scr_nav.c /
     * scr_launcher.c) actually renders from — agrees. */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_shell_view(&H.shell)->flare.sending);
}

static void S10_quick_flare_4_home_presses_do_not_start_sending(void)
{
    harness_init(100000u, false);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);

    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);
}

/* Taps 1-4 keep their ordinary HOME behaviour (spec: "harmless and
 * keeps the gesture idempotent") — pinned with a genuine nav change
 * (leave the launcher for Radar first, so a HOME dispatch is not
 * already a no-op), confirming the FIRST tap of a quick-flare run still
 * navigates home exactly as a bare FF_INTENT_HOME would. */
static void S10_quick_flare_taps_1_to_4_still_navigate_home(void)
{
    harness_init(100000u, false);
    ff_intent_t sel = {.kind = FF_INTENT_LAUNCHER_SELECT, .u = {0}};
    sel.u.launcher_idx = 0u; /* Radar — index 0 in the amended circle order */
    ff_shell_intent(&H.shell, &sel);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_RADAR, ff_shell_view(&H.shell)->active_face);

    press_home_at(100000u); /* tap 1 of a would-be run: HOME still dispatches, Radar -> Launcher */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending); /* only 1 of 5 — no flare yet */
}

/* "If already flaring, a 5-tap does nothing" (spec): a second,
 * independent 5-tap run (the first already fired and reset the
 * multitap FSM to idle) while STILL sending must not restart the send
 * — no second SEND_FLARE, no restarted timer. Pinned by the send
 * expiry staying byte-identical across the second run, not just
 * `sending` staying true (which a broken guard that restarts the timer
 * would still satisfy — the proxy AGENTS.md item 6 warns about). */
static void S10_quick_flare_already_flaring_a_second_run_does_not_resend(void)
{
    harness_init(100000u, false);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    press_home_at(101200u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
    uint32_t const expiry_after_first_run = ff_shell_flare(&H.shell)->send_expiry_ms;

    press_home_at(105000u);
    press_home_at(105300u);
    press_home_at(105600u);
    press_home_at(105900u);
    press_home_at(106200u);

    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expiry_after_first_run, ff_shell_flare(&H.shell)->send_expiry_ms,
                                      "a second 5-tap run while already sending restarted the send timer");
}

/* keep_awake mid-sequence through a REAL shell (not a bare fabricated
 * view, unlike the S26c_AC1 test above): 2 of 5 taps in — a run
 * genuinely in progress, not yet fired — the PROJECTED view must report
 * `quick_flare_pending`, and `ff_shell_keep_awake` must honor it. Also
 * checks the honest reverse: once the gap bound elapses with no further
 * press, the run has genuinely trailed off and must stop reading as
 * pending (nothing should hold the puck awake for an abandoned
 * gesture). */
static void S10_quick_flare_keep_awake_true_mid_sequence(void)
{
    harness_init(100000u, false);

    press_home_at(100000u); /* 1 */
    press_home_at(100300u); /* 2 */
    ff_shell_tick(&H.shell, H.clk.t);
    ff_app_state_t const *v = ff_shell_view(&H.shell);
    TEST_ASSERT_TRUE_MESSAGE(v->quick_flare_pending, "a 2-of-5 run in progress did not project as pending");
    TEST_ASSERT_TRUE(ff_shell_keep_awake(v, false));

    H.clk.t = 100300u + FF_MULTITAP_MAX_GAP_MS + 1u; /* past the gap bound, no further press */
    ff_shell_tick(&H.shell, H.clk.t);
    v = ff_shell_view(&H.shell);
    TEST_ASSERT_FALSE_MESSAGE(v->quick_flare_pending, "an expired run still projected as pending");
    TEST_ASSERT_FALSE(ff_shell_keep_awake(v, false));
}

/* ------------------------------------------------------------------- */
/* S10 quick flare, review round 2 — visible feedback in every reachable */
/* state (COMPOSE/POWER_MENU modals pop; a TAKEOVER is deliberately left */
/* alone). docs/specs/S10-flare.md's Amendments table.                   */
/* ------------------------------------------------------------------- */

/* COMPOSE is up (no takeover): the 5th press POPS the modal so the
 * flare starts on a face that actually shows it — neither
 * ff_scr_compose_build nor ff_scr_power_menu_build composites the
 * sender overlay (ff_face_dispatch_build's own dispatch: they are their
 * own fixed builders, neither takes ff_app_flare_t at all). Taps 1-4
 * must NOT pop it (ff_route_home is a documented no-op under a live
 * modal) — only the 5th, together with QUICK_FLARE, does. */
static void S10_quick_flare_pops_compose_modal_and_starts_sending(void)
{
    harness_init(100000u, false);
    ff_intent_t const open = {.kind = FF_INTENT_OPEN_COMPOSE, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face);

    press_home_at(100000u); /* 1 */
    press_home_at(100300u); /* 2 */
    press_home_at(100600u); /* 3 */
    press_home_at(100900u); /* 4 */
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face,
                                   "taps 1-4 popped COMPOSE — ff_route_home must no-op under a live modal");
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);

    press_home_at(101200u); /* 5th: pops the modal AND starts sending */

    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "quick flare did not start while COMPOSE was open");
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(FF_APP_FACE_COMPOSE, ff_shell_view(&H.shell)->active_face,
                                   "COMPOSE modal was not popped — the flare started invisibly behind it");
}

/* Same shape, POWER_MENU. */
static void S10_quick_flare_pops_power_menu_modal_and_starts_sending(void)
{
    harness_init(100000u, false);
    ff_intent_t const open = {.kind = FF_INTENT_POWER_MENU_OPEN, .u = {0}};
    ff_shell_intent(&H.shell, &open);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_POWER_MENU, ff_shell_view(&H.shell)->active_face);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FF_APP_FACE_POWER_MENU, ff_shell_view(&H.shell)->active_face,
                                   "taps 1-4 popped POWER_MENU — ff_route_home must no-op under a live modal");

    press_home_at(101200u); /* 5th */

    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "quick flare did not start while POWER_MENU was open");
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(FF_APP_FACE_POWER_MENU, ff_shell_view(&H.shell)->active_face,
                                   "POWER_MENU modal was not popped — the flare started invisibly behind it");
}

/* A TAKEOVER (a crew member's own flare) is up: deliberately NOT
 * touched — no modal to pop here anyway (a takeover is not routed,
 * ff_route.h's own "the takeover is not routed, it overrides" note),
 * but the point pinned here is behavioral, not just structural: the
 * flare STARTS (sending flips true, not silently dropped) while the
 * takeover stays fully intact and visible, and once the takeover
 * clears the base face (the launcher, boot default) renders the
 * sender overlay normally with no further action needed. */
static void S10_quick_flare_starts_under_a_takeover_but_leaves_it_alone(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_flare(DANA, 300u);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    press_home_at(101200u); /* 5th */

    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "quick flare did not start under a takeover");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->takeover_active,
                              "quick flare disturbed a crew member's own pending takeover");

    /* Once the takeover clears (a DISMISS here; natural expiry is the
     * same fact via ff_flare_tick, already covered by the launcher-mask
     * auto-end test below), the sender overlay is what's left showing —
     * both facts are simultaneously true, sending was never dropped. */
    ff_intent_t const dismiss = {.kind = FF_INTENT_TAKEOVER_DISMISS, .u = {0}};
    ff_shell_intent(&H.shell, &dismiss);
    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->takeover_active);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "dismissing the takeover cancelled my own send");
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);
}

/* Review round 2 — the SEND-side flare's own natural auto-end
 * (ff_flare_tick's tick-driven FF_FLARE_INTENT_SEND_FLARE_END at
 * send_expiry_ms, never a FLARE_END/CANCEL intent) with base ==
 * LAUNCHER underneath — the launcher render-key mask's fourth
 * exception (`sending`/`send_expires_in_ms`, ff_shell.c's
 * shell_render_key) must keep the countdown genuinely LIVE in the key,
 * not just restore the initial flip: unlike `takeover_active` (which
 * this mask deliberately keeps masked, since nothing renders it on the
 * launcher), a live send's countdown IS rendered there, so there is no
 * "stable mid-send" negative control the way the sibling takeover test
 * above has one — a live send is EXPECTED to dirty the key as its
 * countdown advances. What this test pins is that the auto-end tick
 * specifically both dirties the key and clears `sending`, proving the
 * mask does not silently freeze the overlay at whatever it looked like
 * partway through. */
static void S10_quick_flare_launcher_mask_auto_end_dirties_key(void)
{
    harness_init(100000u, false);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle the always-dirty first tick */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    press_home_at(101200u); /* 5th: starts sending */
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "starting a send did not dirty the launcher key — see the mask's fourth exception");
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    /* Exactly at the send's own natural auto-end: FF_FLARE_DEFAULT_DUR_S
     * (300s) after the 5th press, driven purely by ff_flare_tick inside
     * ff_shell_tick — never a CANCEL/FLARE_END intent. */
    H.clk.t = 101200u + (uint32_t)FF_FLARE_DEFAULT_DUR_S * 1000u;
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "the send's own natural auto-end did not dirty the render key while on the "
                              "launcher — the FLARING overlay would stay stuck on glass forever");
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);
}

/* ======================================================================
 * S10 Amendment (2026-09-03, "Wire honesty") — fix/flare-wire-send.
 *
 * The P0 bug: FF_INTENT_FLARE_START/QUICK_FLARE called ff_flare_send_begin
 * and discarded its FF_FLARE_INTENT_SEND_FLARE result, `(void)`'d — so
 * ff_flare's own SEND intent was consumed nowhere and NOTHING was ever
 * broadcast, while the sender overlay confidently said "you are flaring".
 * These tests assert the actual bytes reach the wire (decoded via
 * ff_proto_decode, not just "a send happened" — the proxy trap AGENTS.md
 * item 6 warns about), and the honest WAITING/SENT view distinction when
 * the link is down.
 *
 * A small injected sender spy stands in for `ff_wiring_sender_t` — the
 * same seam `ff_shell_set_sender`/S24_demo_loopback_seam_makes_out_items_
 * appear already uses — with an `accept` toggle so a test can simulate
 * "no mesh" (refused) then "link returned" (accepted) without standing up
 * the full P-pipe mc_client_t harness.
 * ==================================================================== */

typedef struct {
    uint8_t  buf[FF_PROTO_MAX_PAYLOAD];
    size_t   len;
    uint32_t dest;
    int      n_sends; /* every ATTEMPT, accepted or refused */
    bool     accept;  /* false: send_private refuses (rc != 0) — "no mesh" */
} flare_wire_spy_t;

static flare_wire_spy_t S;

static int flare_wire_spy_send_text(void *ctx, uint32_t dest, char const *utf8)
{
    (void)ctx;
    (void)dest;
    (void)utf8;
    return 0; /* unused by these tests; present only because the vtable requires it */
}

static int flare_wire_spy_send_private(void *ctx, uint32_t dest, uint8_t const *payload, size_t len)
{
    flare_wire_spy_t *s = (flare_wire_spy_t *)ctx;
    s->n_sends++;
    if (!s->accept) return -1;
    s->dest = dest;
    s->len = (len > sizeof(s->buf)) ? sizeof(s->buf) : len;
    memcpy(s->buf, payload, s->len);
    return 0;
}

static void flare_wire_spy_install(bool accept)
{
    memset(&S, 0, sizeof(S));
    S.accept = accept;
    ff_wiring_sender_t const sender = {flare_wire_spy_send_text, flare_wire_spy_send_private, &S};
    ff_shell_set_sender(&H.shell, sender);
}

/* FLARE_START with an accepting sender: exactly one FLARE frame with
 * dur_s 300 reaches the wire, the OUT feed item appears once, and the
 * view reads SENT. */
static void S10_wire_flare_start_accepted_sends_one_flare_frame_dur_300(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);

    TEST_ASSERT_EQUAL_INT(1, S.n_sends);
    TEST_ASSERT_EQUAL_UINT32(MC_ADDR_BROADCAST, S.dest);

    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    int const type = ff_proto_decode(S.buf, S.len, &msg);
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, type);
    TEST_ASSERT_EQUAL_UINT16(300u, msg.body.flare.dur_s);

    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
    ff_feed_item_t const *it = ff_feed_at(ff_shell_feed(&H.shell), 0);
    TEST_ASSERT_EQUAL(FEED_FLARE, it->kind);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);

    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ff_shell_view(&H.shell)->flare.wire_state);
}

/* QUICK_FLARE (5x HOME) mirrors FLARE_START exactly: same one frame. */
static void S10_wire_quick_flare_accepted_sends_one_flare_frame(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true);

    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    press_home_at(101200u); /* 5th */

    TEST_ASSERT_EQUAL_INT(1, S.n_sends);
    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, ff_proto_decode(S.buf, S.len, &msg));
    TEST_ASSERT_EQUAL_UINT16(300u, msg.body.flare.dur_s);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
}

/* Link down (refused sender): FLARE_START still starts sending locally
 * (the user's intent is real), but nothing reaches the wire and the view
 * honestly reads WAITING, not SENT. */
static void S10_wire_flare_start_refused_stays_waiting_no_feed_item(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(false);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);

    TEST_ASSERT_EQUAL_INT(1, S.n_sends); /* attempted */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending); /* the intent is still real */
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(ff_shell_feed(&H.shell))); /* refused: no OUT item fabricated */

    ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_EQUAL_MESSAGE(FF_FLARE_WIRE_WAITING, ff_shell_view(&H.shell)->flare.wire_state,
                               "a refused send must not read as SENT");
}

/* The link returns 7s later: the next retry tick (due at
 * FF_FLARE_RESEND_MS after the failed attempt) sends exactly one more
 * frame and flips the view to SENT; the feed item appears on THAT
 * success, not before. */
static void S10_wire_flare_start_link_returns_after_7s_retries_and_sends(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(false);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start); /* attempt #1, refused, wire_last_attempt_ms = 100000 */
    TEST_ASSERT_EQUAL_INT(1, S.n_sends);

    advance(7000u); /* link comes back at 107000 */
    S.accept = true;
    ff_shell_tick(&H.shell, H.clk.t); /* retry due at 100000 + FF_FLARE_RESEND_MS(5000) = 105000, already past */

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, S.n_sends, "the retry did not attempt a second send");
    ff_proto_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE, ff_proto_decode(S.buf, S.len, &msg));
    TEST_ASSERT_EQUAL_UINT16(300u, msg.body.flare.dur_s);

    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ff_shell_view(&H.shell)->flare.wire_state);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(ff_shell_feed(&H.shell)));
}

/* CANCEL after the send reached SENT: exactly one FLARE_END frame. */
static void S10_wire_cancel_after_sent_sends_one_flare_end_frame(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_EQUAL_INT(1, S.n_sends);

    ff_intent_t const cancel = {.kind = FF_INTENT_FLARE_END, .u = {0}};
    ff_shell_intent(&H.shell, &cancel);

    TEST_ASSERT_EQUAL_INT(2, S.n_sends);
    {
        ff_proto_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE_END, ff_proto_decode(S.buf, S.len, &msg));
    }
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);
}

/* CANCEL while WAITING (never confirmed): ends locally, no frame at all —
 * "never send END for a flare that never went out". */
static void S10_wire_cancel_while_waiting_sends_no_frame(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(false);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_EQUAL_INT(1, S.n_sends); /* the one refused attempt */

    ff_intent_t const cancel = {.kind = FF_INTENT_FLARE_END, .u = {0}};
    ff_shell_intent(&H.shell, &cancel);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, S.n_sends, "CANCEL while WAITING must not put a FLARE_END on the wire");
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);
}

/* Auto-end after the send reached SENT: exactly one FLARE_END frame, at
 * the send's own natural 300s expiry. */
static void S10_wire_auto_end_after_sent_sends_one_flare_end_frame(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_EQUAL_INT(1, S.n_sends);

    advance((uint32_t)FF_FLARE_DEFAULT_DUR_S * 1000u);
    ff_shell_tick(&H.shell, H.clk.t);

    TEST_ASSERT_EQUAL_INT(2, S.n_sends);
    {
        ff_proto_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        TEST_ASSERT_EQUAL_INT(FF_PROTO_TYPE_FLARE_END, ff_proto_decode(S.buf, S.len, &msg));
    }
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);
}

/* ---------------------------------------------------------------------
 * Review round 2 (2026-09-03) — should-fixes on the wire-honesty PR
 * before merge.
 * ------------------------------------------------------------------- */

/* (1a) Link down the whole time: FLARE_SENT must never fire — the chime
 * now marks the WAITING->SENT wire CONFIRMATION (shell_flare_wire), not
 * the bare send intent, so a send that stays refused must stay silent
 * even though `sending` is genuinely true and the overlay is up. */
static void S10_wire_flare_sent_chime_does_not_fire_while_waiting(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(false);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_WAITING, ff_shell_flare(&H.shell)->wire_state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sound_count(FF_SOUND_FLARE_SENT),
                                  "a refused (never-confirmed) send must never chime FLARE_SENT");

    /* Several retry attempts, all still refused: still silent. */
    for (int i = 0; i < 3; i++) {
        advance(FF_FLARE_RESEND_MS);
        ff_shell_tick(&H.shell, H.clk.t);
    }
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_WAITING, ff_shell_flare(&H.shell)->wire_state);
    TEST_ASSERT_EQUAL_INT(0, sound_count(FF_SOUND_FLARE_SENT));
}

/* (1b) Link returns partway through: exactly ONE chime, fired at the
 * retry that actually succeeds — not at the original (refused) send
 * intent, and not more than once for the retries that follow once SENT. */
static void S10_wire_flare_sent_chime_fires_once_when_retry_succeeds(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(false);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start); /* refused: no chime */
    TEST_ASSERT_EQUAL_INT(0, sound_count(FF_SOUND_FLARE_SENT));

    advance(7000u); /* link returns */
    S.accept = true;
    ff_shell_tick(&H.shell, H.clk.t); /* the retry due at +5000ms fires and succeeds */

    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ff_shell_flare(&H.shell)->wire_state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sound_count(FF_SOUND_FLARE_SENT),
                                  "the chime must fire exactly once, at the retry that actually confirmed");
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);

    /* Further ticks (still SENT, no more retries) must not chime again. */
    for (int i = 0; i < 5; i++) {
        advance(5000u);
        ff_shell_tick(&H.shell, H.clk.t);
    }
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_SENT));
}

/* (2) Pin: once SENT, the shell must never resend across many further
 * ticks — a "resend every tick" (or "forgot the wire_state==SENT gate on
 * retry") mutation would still pass every OTHER S10_wire_* test (they
 * only check the frame count shortly after the transition) but is caught
 * here by ticking well past several retry intervals and asserting the
 * frame count never grows past 1. */
static void S10_wire_no_resend_across_many_ticks_while_sent(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true);

    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_EQUAL_INT(1, S.n_sends);
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ff_shell_flare(&H.shell)->wire_state);

    /* 20 ticks, each FF_FLARE_RESEND_MS apart (100s total, well inside
     * the 300s send window) — every one of them is a moment a live "keep
     * retrying" bug would fire again. */
    for (int i = 0; i < 20; i++) {
        advance(FF_FLARE_RESEND_MS);
        ff_shell_tick(&H.shell, H.clk.t);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, S.n_sends,
                                  "SENT must never resend — a retry loop with no SENT gate would fire repeatedly here");
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_flare(&H.shell)->sending, "still well within the 300s send window");
}

/* (3) The launcher render-key mask must dirty on a WAITING->SENT
 * transition while the launcher is the base face — mirrors the mask's
 * pre-existing fourth exception (`sending`/`send_expires_in_ms`);
 * `wire_state` needed the identical treatment (fifth exception, this PR)
 * since the overlay now ALSO renders off that field. Isolated from every
 * OTHER field a real link-return also touches: the natural retry path
 * always sits FF_FLARE_RESEND_MS (5000ms) later, which by itself moves
 * the send_expires_in_ms bucket and would dirty the key regardless of
 * whether wire_state is masked correctly — exactly the proxy that would
 * let the mask-restore mutation escape. A SECOND FLARE_START intent at
 * the SAME clock reading instead restarts the send
 * (ff_flare_send_begin's own "start or restart" contract) with an
 * IDENTICAL send_expiry_ms (same now_ms, same default dur_s — asserted
 * below), so wire_state is the ONLY field that changes between the two
 * compared ticks. */
static void S10_wire_launcher_mask_waiting_to_sent_dirties_key(void)
{
    harness_init(100000u, false);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* settle the always-dirty first tick */
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);

    flare_wire_spy_install(false); /* link down: the first attempt is refused */
    ff_intent_t const start = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &start);
    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "starting a (refused) send did not dirty the launcher key");
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_WAITING, ff_shell_flare(&H.shell)->wire_state);
    uint32_t const expiry_before = ff_shell_flare(&H.shell)->send_expiry_ms;

    S.accept = true; /* link "returns" */
    ff_shell_intent(&H.shell, &start); /* same H.clk.t: restarts the send, not a tick-driven retry */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expiry_before, ff_shell_flare(&H.shell)->send_expiry_ms,
                                      "test setup invalid: send_expiry_ms moved — no longer isolating wire_state");
    TEST_ASSERT_EQUAL(FF_FLARE_WIRE_SENT, ff_shell_flare(&H.shell)->wire_state);

    TEST_ASSERT_TRUE_MESSAGE(ff_shell_tick(&H.shell, H.clk.t),
                              "the WAITING->SENT transition did not dirty the launcher key — with every other "
                              "field held constant, this can only mean the mask failed to restore wire_state");
    TEST_ASSERT_EQUAL_INT(FF_APP_FACE_LAUNCHER, ff_shell_view(&H.shell)->active_face);
}

/* ---------------------------------------------------------------------
 * S27 sounds — see docs/specs/S27-sounds.md. Concurrent PR #184, merged
 * into this branch alongside the S10 wire-honesty work above. Review
 * round 2 (2026-09-03) moved the FLARE_SENT chime from the bare send
 * INTENT into `shell_flare_wire`'s WAITING->SENT CONFIRMATION (see that
 * function's own doc comment) — so several tests below now install an
 * accepting sender (`flare_wire_spy_install(true)`) to actually reach
 * SENT; each says so where it matters.
 * ------------------------------------------------------------------- */

static void send_setting(ff_setting_id_t id, int32_t i)
{
    ff_intent_t in = {.kind = FF_INTENT_SETTING_SET, .u = {0}};
    in.u.setting.id = id;
    in.u.setting.v.i = i;
    ff_shell_intent(&H.shell, &in);
}

static void send_flare_start(void)
{
    ff_intent_t in = {.kind = FF_INTENT_FLARE_START, .u = {0}};
    ff_shell_intent(&H.shell, &in);
}

static void inject_rally(uint32_t from, char const *place)
{
    uint8_t buf[FF_PROTO_MAX_PAYLOAD];
    int n = ff_proto_encode_rally(buf, sizeof(buf), (ff_latlon_t){39.0, -82.0}, place);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    H.ev.on_private(H.ev.user, from, MC_ADDR_BROADCAST, FF_PORTNUM, buf, (size_t)n);
}

static void S27_flare_start_fires_flare_sent_exactly_once(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    /* S10 Amendment (2026-09-03, "Wire honesty", review round 2): FLARE_SENT
     * now fires on the WAITING->SENT wire transition, not on the bare send
     * intent (shell_flare_wire, ff_shell.c) — so this test needs a sender
     * that actually ACCEPTS the send (harness_init's default no-transport
     * sender always refuses) for the chime to ever fire at all. */
    flare_wire_spy_install(true);

    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
    send_flare_start();
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_SENT));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count); /* exactly one sound event total */
}

/* Gated on the visible face like the intent handler itself: a second
 * FLARE_START while a takeover is up must not also sound (the intent is
 * rejected before shell_sound is ever reached). */
static void S27_flare_start_during_a_takeover_fires_nothing(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_flare(DANA, 300); /* raises a takeover; also fires FLARE_INCOMING */
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->takeover_active);
    int const before = H.sound.count;

    send_flare_start();
    TEST_ASSERT_EQUAL_INT(before, H.sound.count);
}

/* The OTHER flare-send trigger: the 5x-HOME quick-flare gesture
 * (FF_INTENT_QUICK_FLARE, concurrent PR #183) has its own separate
 * ff_flare_send_begin call site (ff_shell.c) — this proves it also
 * fires FLARE_SENT, not just the Radar-face button. */
static void S27_quick_flare_gesture_fires_flare_sent_exactly_once(void)
{
    harness_init(100000u, false);
    flare_wire_spy_install(true); /* see S27_flare_start_fires_flare_sent_exactly_once's own comment */

    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
    press_home_at(100000u);
    press_home_at(100300u);
    press_home_at(100600u);
    press_home_at(100900u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, H.sound.count, "taps 1-4 must not sound FLARE_SENT");
    press_home_at(101200u); /* 5th: starts sending */

    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_SENT));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

static void S27_inbound_flare_from_paired_sender_fires_flare_incoming_exactly_once(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
    inject_flare(DANA, 300);
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_INCOMING));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

/* An unpaired sender's flare is ignored entirely by ff_flare_on_flare_rx's
 * own trust gate (should_alert stays false) — same proxy-check discipline
 * S16_AC11_unpaired_flare_neither_alerts_nor_takes_over already applies to
 * the haptic; mirrored here for the sound. */
static void S27_inbound_flare_from_unpaired_sender_fires_no_sound(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    inject_flare(DANA, 300); /* DANA never paired */
    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
}

static void S27_paired_message_fires_message_sound_exactly_once(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_text(DANA, "you close?");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

static void S27_unpaired_message_fires_no_sound(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    inject_text(DANA, "hi, who are you"); /* DANA never paired: S22 stranger rule */
    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
}

/* Should-fix from PR #184 review: two rapid messages from the SAME
 * sender within the notify queue's 2s coalesce window (ff_notify.h)
 * must produce exactly ONE MESSAGE sound, not two —
 * shell_notify_push_banner sounds only on FF_NOTIFY_PUSH_NEW, never on
 * a coalesced refresh of the already-queued banner. A second, DIFFERENT
 * sender still gets its own, independent sound. */
static void S27_coalesced_message_from_same_sender_sounds_once_different_sender_sounds_again(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, KEV_ID, true));

    inject_text(DANA, "you close?");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));

    /* Same sender, 500ms later: well within FF_NOTIFY_COALESCE_MS (2000ms)
     * -> coalesces into the same banner entry, no second sound. */
    advance(500u);
    inject_text(DANA, "still there?");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sound_count(FF_SOUND_MESSAGE),
                                  "a coalesced banner update from the same sender must not sound a second MESSAGE");
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);

    /* A DIFFERENT sender: a genuinely new banner, its own sound. */
    inject_text(KEV_ID, "yo");
    TEST_ASSERT_EQUAL_INT(2, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(2, H.sound.count);
}

static void S27_paired_rally_fires_rally_sound_exactly_once(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_rally(DANA, "Main Stage");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_RALLY));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

static void S27_sounds_off_silences_every_call_site(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* An ACCEPTING sender: FLARE_SENT now gates on the WAITING->SENT wire
     * transition (shell_flare_wire), so this test must reach SENT for
     * "sounds off silences it" to be a genuine measurement rather than a
     * pass-by-construction on a send that never confirmed anyway. */
    flare_wire_spy_install(true);

    send_setting(FF_SETTING_SOUNDS_ON, 0);
    TEST_ASSERT_FALSE(ff_shell_settings(&H.shell)->sounds_on);

    send_flare_start();
    inject_flare(DANA, 300); /* FLARE_INCOMING, even the "loudest thing", is silenced too */
    inject_text(DANA, "hi");
    inject_rally(DANA, "Main Stage");

    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
}

/* Positive control for the mutation above: with sounds back on, the
 * exact same sequence produces the expected four events (one each). */
static void S27_sounds_on_positive_control_for_the_off_test(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    TEST_ASSERT_TRUE(ff_shell_settings(&H.shell)->sounds_on); /* the default */
    flare_wire_spy_install(true); /* FLARE_SENT needs a confirmed wire send now — see the sibling test's comment */

    send_flare_start();
    inject_flare(DANA, 300);
    inject_text(DANA, "hi");
    inject_rally(DANA, "Main Stage");

    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_SENT));
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_INCOMING));
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_RALLY));
    TEST_ASSERT_EQUAL_INT(4, H.sound.count);
}

/* The mutation target (task's mutation (a), exercised end to end through
 * the real shell wiring — test_sound.c already covers the core policy in
 * isolation; this is the "the shell actually calls through it correctly"
 * half): quiet hours exempts ONLY the two FLARE events. */
static void S27_quiet_hours_only_the_two_flare_events_play(void)
{
    harness_seed_settings(0); /* UTC, so U_QUIET's 05:00Z is 05:00 local */
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    /* Latch the wall clock into quiet hours (S16_AC11's own technique). */
    inject_node(DANA, "DANA", U_QUIET);
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_EQUAL_INT(FF_WALL_MESH, w.src);
    TEST_ASSERT_TRUE(ff_quiet_now(ff_shell_settings(&H.shell), w.now_min));
    flare_wire_spy_install(true); /* FLARE_SENT needs a confirmed wire send now — see the sibling test's comment */

    send_flare_start();
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_SENT));

    inject_flare(DANA, 300);
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_FLARE_INCOMING));

    int const before = H.sound.count;
    inject_text(DANA, "you close?");
    inject_rally(DANA, "Main Stage");
    TEST_ASSERT_EQUAL_INT_MESSAGE(before, H.sound.count,
                                  "MESSAGE/RALLY must stay silent in quiet hours - only FLARE_* is exempt");
}

/* Positive control: OUTSIDE quiet hours, the same MESSAGE/RALLY pair DO
 * sound — proves the quiet-hours test above is measuring the exemption,
 * not a call site that never fires at all. */
static void S27_awake_hours_message_and_rally_do_play(void)
{
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    inject_node(DANA, "DANA", U_AWAKE);
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_FALSE(ff_quiet_now(ff_shell_settings(&H.shell), w.now_min));

    inject_text(DANA, "you close?");
    inject_rally(DANA, "Main Stage");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_RALLY));
}

/* BATT_LOW: fires once on a crossing into the low band, never again
 * while it stays low, and again on a genuinely NEW crossing after
 * recovering. Drives the REAL battery path (#180's `ff_shell_set_batt_mv`
 * + `ff_batt_filter_t`, merged concurrently with this slice — the S27
 * test/dev seam this test used before that landed is retired) rather
 * than poking `view.radar.batt_pct` directly: pushes a high pack
 * voltage once (the filter's own "first-ever reading shows immediately,
 * no hysteresis" rule, `ff_batt.h`), then four consecutive LOW pushes
 * (enough to fully cycle the filter's 4-sample averaging window past
 * the boundary regardless of the exact intermediate values — the
 * "crosses down into low promotes immediately" rule means the actual
 * crossing happens partway through those four pushes; this test does
 * not need to know exactly which one, only that `ff_shell_tick` after
 * them observes the FINAL state and reports the crossing exactly once),
 * then a matching four-push recovery + four-push re-crossing. */
static void S27_batt_low_fires_once_per_crossing_not_every_tick(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    enum { MV_HIGH = 4200u, MV_LOW = 3300u }; /* ff_batt.h's OCV table: 4200->100%, 3300->0% */

    ff_shell_set_batt_mv(&H.shell, MV_HIGH, H.clk.t); /* first-ever reading: shows 100% immediately */
    (void)ff_shell_tick(&H.shell, H.clk.t); /* shell_project runs every tick regardless of the dirty bit */
    TEST_ASSERT_EQUAL_INT8(100, ff_shell_view(&H.shell)->radar.batt_pct);
    TEST_ASSERT_EQUAL_INT(0, sound_count(FF_SOUND_BATT_LOW));

    for (int i = 0; i < 4; i++) {
        advance(1000u);
        ff_shell_set_batt_mv(&H.shell, MV_LOW, H.clk.t);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE_MESSAGE(ff_radar_batt_is_low(ff_shell_view(&H.shell)->radar.batt_pct),
                             "four low pushes did not settle the filter into the low band");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_BATT_LOW));

    /* The mutation target (task's mutation (b)): still low on every
     * subsequent tick (no new pushes at all) must NOT fire again. */
    for (int i = 0; i < 20; i++) {
        advance(1000u);
        (void)ff_shell_tick(&H.shell, H.clk.t);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sound_count(FF_SOUND_BATT_LOW),
                                  "BATT_LOW must fire once on the crossing, not on every tick it reads low");

    /* Recover above the threshold (four pushes: the filter's own exit
     * hysteresis needs the filtered value to clear FF_BATT_LOW_PCT +
     * FF_BATT_HYSTERESIS_PCT by a full margin, ff_batt.h). */
    for (int i = 0; i < 4; i++) {
        advance(1000u);
        ff_shell_set_batt_mv(&H.shell, MV_HIGH, H.clk.t);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_FALSE(ff_radar_batt_is_low(ff_shell_view(&H.shell)->radar.batt_pct));
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_BATT_LOW));

    /* ...then a SECOND, genuinely new crossing fires again — this is not
     * a stuck one-shot latch. */
    for (int i = 0; i < 4; i++) {
        advance(1000u);
        ff_shell_set_batt_mv(&H.shell, MV_LOW, H.clk.t);
    }
    (void)ff_shell_tick(&H.shell, H.clk.t);
    TEST_ASSERT_TRUE(ff_radar_batt_is_low(ff_shell_view(&H.shell)->radar.batt_pct));
    TEST_ASSERT_EQUAL_INT(2, sound_count(FF_SOUND_BATT_LOW));
}

/* An unknown reading (-1, the honest default — no ff_shell_set_batt_mv
 * call at all, same as neither target having a battery ADC wired yet)
 * is never "low" — never fires BATT_LOW, even the very first tick. */
static void S27_batt_unknown_never_fires_batt_low(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    for (int i = 0; i < 5; i++) {
        advance(1000u);
        (void)ff_shell_tick(&H.shell, H.clk.t);
    }
    TEST_ASSERT_EQUAL_INT(0, sound_count(FF_SOUND_BATT_LOW));
}

/* ---------------------------------------------------------------------
 * TAP — ff_shell_should_tap_sound's composed policy (sounds_on AND
 * ui_ticks AND not quiet), and ff_shell_sound_sink's end-to-end wiring.
 * ------------------------------------------------------------------- */

static void S27_tap_sound_gated_by_ui_ticks_and_sounds_on(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);

    /* Defaults: sounds_on=true, ui_ticks=false -> no tap sound (spec:
     * "default OFF"). */
    TEST_ASSERT_TRUE(ff_shell_settings(&H.shell)->sounds_on);
    TEST_ASSERT_FALSE(ff_shell_settings(&H.shell)->ui_ticks);
    TEST_ASSERT_FALSE(ff_shell_should_tap_sound(&H.shell));

    send_setting(FF_SETTING_UI_TICKS, 1);
    TEST_ASSERT_TRUE(ff_shell_should_tap_sound(&H.shell));

    send_setting(FF_SETTING_SOUNDS_ON, 0);
    TEST_ASSERT_FALSE_MESSAGE(ff_shell_should_tap_sound(&H.shell), "sounds_on must still gate TAP even with ui_ticks on");

    send_setting(FF_SETTING_SOUNDS_ON, 1);
    TEST_ASSERT_TRUE(ff_shell_should_tap_sound(&H.shell));

    TEST_ASSERT_FALSE(ff_shell_should_tap_sound(NULL));
}

static void S27_tap_sound_silenced_during_quiet_hours_even_with_ui_ticks_on(void)
{
    harness_seed_settings(0);
    harness_init(100000u, true);
    inject_my_info(MY_ID);
    send_setting(FF_SETTING_UI_TICKS, 1);
    TEST_ASSERT_TRUE(ff_shell_settings(&H.shell)->ui_ticks);

    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    inject_node(DANA, "DANA", U_QUIET);
    ff_wall_t const w = ff_shell_wall(&H.shell);
    TEST_ASSERT_TRUE(ff_quiet_now(ff_shell_settings(&H.shell), w.now_min));

    TEST_ASSERT_FALSE(ff_shell_should_tap_sound(&H.shell));
}

static void S27_shell_sound_sink_plays_tap_when_gated_on(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    send_setting(FF_SETTING_UI_TICKS, 1);

    ff_shell_sound_sink(&H.shell, FF_SOUND_TAP);
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_TAP));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);

    /* Defensive: this seam only ever carries TAP (ff_sound_emit.h's top
     * comment) — any other event through it is a no-op, never forwarded
     * to play_sound. */
    ff_shell_sound_sink(&H.shell, FF_SOUND_MESSAGE);
    TEST_ASSERT_EQUAL_INT(0, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count); /* unchanged */

    ff_shell_sound_sink(NULL, FF_SOUND_TAP); /* NULL-safe */
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

static void S27_shell_sound_sink_silent_when_ui_ticks_off(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    /* ui_ticks defaults false — the sink must NOT play. */
    ff_shell_sound_sink(&H.shell, FF_SOUND_TAP);
    TEST_ASSERT_EQUAL_INT(0, H.sound.count);
}

/* ===================================================================== */
/* fix/audio-init-order-seed-silence — "seeded/replayed history never    */
/* chimes" (docs/specs/S27-sounds.md Amendments). The real end-to-end    */
/* demo-seed path (ff_demo_seed against the actual embedded festpack) is */
/* covered in targets/sim/tests/test_ctl_flare_sequence.c's              */
/* S27_demo_seed_through_ctl_loop_fires_no_sound (test_shell can't link  */
/* ff-demo/a festpack fixture); these tests pin the shell-level MECHANISM*/
/* both of ff_demo_seed's mute and the mesh handshake/settle mute share: */
/* ff_shell_set_sound_muted_for_seed and shell_ev_state/ff_shell_tick's  */
/* own handshake-burst pair. See shell_t's `sound_muted_for_seed` field  */
/* doc comment (ff_shell.c) for who sets/clears it and why.              */
/* ===================================================================== */

/** ff_demo_seed's own bracketing pattern (mute, push seeded rally +
 *  message, unmute) reproduced directly against the public setter — the
 *  exact shape app/ff_demo.c uses around demo_seed_feed. Zero sounds
 *  while muted; the first live event once unmuted sounds exactly once. */
static void S27_sound_muted_for_seed_silences_seeded_rally_and_message(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    ff_shell_set_sound_muted_for_seed(&H.shell, true);
    inject_rally(DANA, "The Firefly Tower");
    inject_text(DANA, "lineup is stacked tonight");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, H.sound.count, "seeded rally + message must land silently");
    ff_shell_set_sound_muted_for_seed(&H.shell, false);

    /* The first LIVE event after seeding sounds exactly once — the mute
     * is a bounded window, not a stuck flag. Advance past the notify
     * coalesce window (FF_NOTIFY_COALESCE_MS, 2s) so this reads as a
     * genuinely new banner rather than a refresh of the muted one. */
    advance(3000u);
    inject_text(DANA, "on my way!");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

/** NULL-safe, matching every other ff_shell_set_* setter's convention. */
static void S27_sound_muted_for_seed_null_shell_is_a_safe_noop(void)
{
    ff_shell_set_sound_muted_for_seed(NULL, true); /* must not crash */
}

/** The cold-boot want_config replay/settle side of the same mechanism:
 *  shell_ev_state mutes on MC_STATE_HANDSHAKE, ff_shell_tick clears it at
 *  the not-ready->ready edge (the same edge shell_settle_replay itself
 *  keys off, S18 slice b). Positions never reach shell_sound either way
 *  (they go through shell_replay_buffer/shell_settle_replay, which never
 *  calls it) — MESSAGE is the observable per this fix's own PR-body
 *  note ("positions don't chime anyway — pick MESSAGE"). */
static void S27_handshake_burst_fires_no_sound_live_message_after_settle_sounds(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    H.ev.on_state(H.ev.user, MC_STATE_HANDSHAKE);
    inject_text(DANA, "cached while you were gone");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, H.sound.count, "mid-handshake traffic is a replay, not a live arrival");

    H.ev.on_state(H.ev.user, MC_STATE_READY);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* the not-ready->ready edge: shell_settle_replay + unmute */

    advance(3000u); /* clear of the notify coalesce window, same reasoning as the seed test above */
    inject_text(DANA, "here now");
    TEST_ASSERT_EQUAL_INT(1, sound_count(FF_SOUND_MESSAGE));
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);
}

/** A reconnect's SECOND handshake burst mutes again — not a one-shot
 *  "only cold boot" flag. Mirrors S16_AC9_want_config_replay_does_not_
 *  refresh_position_age's own drop/reconnect shape. */
static void S27_reconnect_handshake_burst_mutes_again(void)
{
    harness_init(100000u, false);
    inject_my_info(MY_ID);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));

    H.ev.on_state(H.ev.user, MC_STATE_HANDSHAKE);
    H.ev.on_state(H.ev.user, MC_STATE_READY);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    advance(3000u);
    inject_text(DANA, "live one");
    TEST_ASSERT_EQUAL_INT(1, H.sound.count);

    /* The drop, then the reconnect's own replay burst. */
    H.ev.on_state(H.ev.user, MC_STATE_DISCONNECTED);
    (void)ff_shell_tick(&H.shell, H.clk.t); /* was_ready -> false while link != CONNECTED */
    H.ev.on_state(H.ev.user, MC_STATE_HANDSHAKE);
    advance(3000u);
    inject_text(DANA, "replayed on reconnect");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, H.sound.count, "the reconnect's own handshake burst must mute too");

    H.ev.on_state(H.ev.user, MC_STATE_READY);
    (void)ff_shell_tick(&H.shell, H.clk.t);
    advance(3000u);
    inject_text(DANA, "live again");
    TEST_ASSERT_EQUAL_INT(2, H.sound.count);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S16_AC5a_unknown_sender_no_feed_no_crew_slot_one_heard_entry);
    RUN_TEST(S16_AC5b_known_unpaired_sender_no_feed_and_no_new_heard_entry);
    RUN_TEST(S16_AC5c_position_from_non_roster_node_is_dropped_and_noted);

    RUN_TEST(S06_shell_projects_batt_pct_unknown_and_batt_low_agrees);

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

    RUN_TEST(flare_second_takeover_dismisses);
    RUN_TEST(flare_rearm_after_dismiss_is_dirty);
    RUN_TEST(flare_takeover_is_opaque_to_underlying_churn);
    RUN_TEST(flare_takeover_bearing_keys_the_rendered_octant);

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

    RUN_TEST(S26_ff_shell_load_pack_twice_same_shell_same_toks_buffer);
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
    RUN_TEST(S22b_inbox_target_survives_rebuild_and_is_gated);
    RUN_TEST(S24_flare_chip_addresses_member_vs_whole_crew_as_flare);
    RUN_TEST(S22_AC4_send_to_just_unpaired_member_is_refused);
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

    RUN_TEST(S24_AC1_sig_flare_send_pushes_outgoing_item);
    RUN_TEST(S24_AC1_rally_send_pushes_outgoing_item_with_place_name);
    RUN_TEST(S24_AC1_refused_rally_pushes_no_outgoing_item);
    RUN_TEST(S24_AC1_shell_classifies_inbound_text_direction_via_my_info);
    RUN_TEST(S24_AC1_reply_context_skips_my_own_outgoing_item);
    RUN_TEST(S24_AC1_composer_send_pushes_outgoing_item);
    RUN_TEST(S24_AC1_composer_send_refused_pushes_no_item);

    RUN_TEST(S24_AC4_open_thread_marks_only_that_thread_read);
    RUN_TEST(S24_AC3_fab_pick_and_back_navigate_subviews);
    RUN_TEST(S24_AC3_leaving_inbox_face_resets_subview_to_inbox);
    RUN_TEST(S24_AC8_inbox_key_same_bucket_age_tick_is_clean);
    RUN_TEST(S24_AC8_presence_age_keys_rendered_bucket_only);
    RUN_TEST(S24_AC3_inbox_intents_are_inert_under_a_takeover);
    /* S24 slice d — popup / rally / opacity / demo-loopback seam. */
    RUN_TEST(S24_popup_flare_sends_flare_to_scope_and_closes);
    RUN_TEST(S24_AC5_popup_compose_opens_composer_at_scope);
    RUN_TEST(S24_AC5_popup_rally_opens_rally_screen);
    RUN_TEST(S24_AC6_rally_places_from_pack);
    RUN_TEST(S24_AC6_on_me_disabled_encodes_nothing);
    RUN_TEST(S24_AC6_when_rides_in_the_name);
    RUN_TEST(S24_AC6_when_truncates_place_never_suffix);
    RUN_TEST(S24_AC6_selecting_second_landmark_picks_the_right_one);
    RUN_TEST(S24_AC6_crew_rally_arms_then_sends);
    RUN_TEST(S24_AC8_popup_and_rally_opaque_to_feed_churn);
    RUN_TEST(S24_demo_loopback_seam_makes_out_items_appear);

    RUN_TEST(S24c_AC4_thread_projection_builds_messages_both_ways);
    RUN_TEST(S24c_AC4_live_arrival_into_open_thread_marks_read_only_when_visible);
    RUN_TEST(S24c_AC4_quick_chip_canned_reply_targets_thread_scope);
    RUN_TEST(S24c_AC4_flare_chip_sends_to_thread_scope_and_keeps_scope);
    RUN_TEST(S24c_AC8_thread_key_opaque_to_other_conversations_churn);
    RUN_TEST(S24c_AC8_thread_header_presence_keys_rendered_bucket);
    RUN_TEST(S24_inbox_1to1_projects_the_members_own_color);

    RUN_TEST(S26c_AC1_keep_awake_false_when_nothing_holds);
    RUN_TEST(S26c_AC1_keep_awake_true_while_flare_takeover_pending);
    RUN_TEST(S26c_AC1_keep_awake_true_while_power_menu_open);
    RUN_TEST(S26c_AC1_keep_awake_true_while_touch_cal_running);
    RUN_TEST(S26c_AC1_keep_awake_null_view_is_safe);
    RUN_TEST(S26c_AC1_keep_awake_true_while_quick_flare_pending);

    /* S10 quick flare — 5x HOME flares to the crew. */
    RUN_TEST(S10_quick_flare_5_home_presses_start_sending);
    RUN_TEST(S10_quick_flare_4_home_presses_do_not_start_sending);
    RUN_TEST(S10_quick_flare_taps_1_to_4_still_navigate_home);
    RUN_TEST(S10_quick_flare_already_flaring_a_second_run_does_not_resend);
    RUN_TEST(S10_quick_flare_keep_awake_true_mid_sequence);
    RUN_TEST(S10_quick_flare_pops_compose_modal_and_starts_sending);
    RUN_TEST(S10_quick_flare_pops_power_menu_modal_and_starts_sending);
    RUN_TEST(S10_quick_flare_starts_under_a_takeover_but_leaves_it_alone);
    RUN_TEST(S10_quick_flare_launcher_mask_auto_end_dirties_key);

    RUN_TEST(S10_wire_flare_start_accepted_sends_one_flare_frame_dur_300);
    RUN_TEST(S10_wire_quick_flare_accepted_sends_one_flare_frame);
    RUN_TEST(S10_wire_flare_start_refused_stays_waiting_no_feed_item);
    RUN_TEST(S10_wire_flare_start_link_returns_after_7s_retries_and_sends);
    RUN_TEST(S10_wire_cancel_after_sent_sends_one_flare_end_frame);
    RUN_TEST(S10_wire_cancel_while_waiting_sends_no_frame);
    RUN_TEST(S10_wire_auto_end_after_sent_sends_one_flare_end_frame);
    RUN_TEST(S10_wire_flare_sent_chime_does_not_fire_while_waiting);
    RUN_TEST(S10_wire_flare_sent_chime_fires_once_when_retry_succeeds);
    RUN_TEST(S10_wire_no_resend_across_many_ticks_while_sent);
    RUN_TEST(S10_wire_launcher_mask_waiting_to_sent_dirties_key);

    /* S26 slice d — ff_notify + message banner. */
    RUN_TEST(S26_AC3_paired_message_pushes_banner);
    RUN_TEST(S26_wake_pulse_true_once_after_banner_then_clear);
    RUN_TEST(S26_wake_pulse_not_raised_by_unpaired_sender_or_null);
    RUN_TEST(S26_AC3_unpaired_message_pushes_no_banner);
    RUN_TEST(S26_AC3_paired_rally_pushes_banner);
    RUN_TEST(S26_AC3_unpaired_rally_pushes_no_banner);
    RUN_TEST(S26_flare_and_reserved01_do_not_push_banners);
    RUN_TEST(S26_coalesce_within_2s_updates_head_in_place);
    RUN_TEST(S26_AC1_banner_auto_expires_after_6s);
    RUN_TEST(S26_AC2_banner_age_same_bucket_ticks_are_clean);
    RUN_TEST(S26_launcher_AC_banner_expiry_dirties_key);
    RUN_TEST(S26_launcher_AC_second_sender_banner_dirties_key);
    RUN_TEST(S26_launcher_radar_position_update_does_not_dirty_key);
    RUN_TEST(S26_launcher_flare_takeover_natural_expiry_dirties_key_and_renders_launcher);

    RUN_TEST(S25c_batt_mv_reflects_filtered_value_and_dirties_launcher_key);
    RUN_TEST(S25c_set_batt_mv_before_first_tick_does_not_spuriously_reset);

    RUN_TEST(S26_AC2_banner_from_other_paired_conv_dirties_open_thread_key);
    RUN_TEST(S26_AC2_banner_from_other_paired_conv_dirties_popup_overlay_exactly_once);
    RUN_TEST(S26_banner_open_routes_marks_read_and_dismisses);
    RUN_TEST(S26_banner_open_with_no_banner_is_noop);
    RUN_TEST(S26_banner_open_is_inert_under_a_takeover);
    RUN_TEST(S26_banner_open_group_message_opens_crew_thread_not_senders);
    RUN_TEST(S26_banner_open_direct_message_opens_senders_thread);
    RUN_TEST(S26_banner_open_group_rally_opens_crew_thread);
    RUN_TEST(S26_banner_open_picks_the_head_banners_own_conversation);

    /* S27 sounds. */
    RUN_TEST(S27_flare_start_fires_flare_sent_exactly_once);
    RUN_TEST(S27_flare_start_during_a_takeover_fires_nothing);
    RUN_TEST(S27_quick_flare_gesture_fires_flare_sent_exactly_once);
    RUN_TEST(S27_inbound_flare_from_paired_sender_fires_flare_incoming_exactly_once);
    RUN_TEST(S27_inbound_flare_from_unpaired_sender_fires_no_sound);
    RUN_TEST(S27_paired_message_fires_message_sound_exactly_once);
    RUN_TEST(S27_unpaired_message_fires_no_sound);
    RUN_TEST(S27_coalesced_message_from_same_sender_sounds_once_different_sender_sounds_again);
    RUN_TEST(S27_paired_rally_fires_rally_sound_exactly_once);
    RUN_TEST(S27_sounds_off_silences_every_call_site);
    RUN_TEST(S27_sounds_on_positive_control_for_the_off_test);
    RUN_TEST(S27_quiet_hours_only_the_two_flare_events_play);
    RUN_TEST(S27_awake_hours_message_and_rally_do_play);
    RUN_TEST(S27_batt_low_fires_once_per_crossing_not_every_tick);
    RUN_TEST(S27_batt_unknown_never_fires_batt_low);
    RUN_TEST(S27_tap_sound_gated_by_ui_ticks_and_sounds_on);
    RUN_TEST(S27_tap_sound_silenced_during_quiet_hours_even_with_ui_ticks_on);
    RUN_TEST(S27_shell_sound_sink_plays_tap_when_gated_on);
    RUN_TEST(S27_shell_sound_sink_silent_when_ui_ticks_off);
    RUN_TEST(S27_sound_muted_for_seed_silences_seeded_rally_and_message);
    RUN_TEST(S27_sound_muted_for_seed_null_shell_is_a_safe_noop);
    RUN_TEST(S27_handshake_burst_fires_no_sound_live_message_after_settle_sounds);
    RUN_TEST(S27_reconnect_handshake_burst_mutes_again);

    return UNITY_END();
}
