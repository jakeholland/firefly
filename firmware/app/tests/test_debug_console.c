/**
 * test_debug_console.c — bench/debug console DISPATCH tests
 * (CONFIG_FF_DEBUG_CONSOLE). Drives `ff_dbgconsole_handle_line` against
 * a REAL `ff_shell_t` (no transport — synthetic `mc_events_t` injection,
 * the same seam test_shell.c uses), asserting on the reply lines AND on
 * the shell's own state afterward (a sent message landed in the feed, a
 * flare actually started) — not just "some string came back".
 *
 * The core PARSER (ff_dbgcmd_parse) has its own exhaustive coverage in
 * firmware/core/tests/test_dbgcmd.c; this file is about the SEAM —
 * every command reaching the right shell getter/intent/debug-send call
 * and nothing else.
 */
#include <string.h>

#include "unity.h"

#include "ff_debug_console.h"
#include "ff_shell.h"

#include "ff_crew.h"
#include "ff_feed.h"
#include "ff_flare.h"
#include "ff_heard.h"

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------- */
/* fixture — minimal transport-free shell, mirrors test_shell.c's       */
/* harness_init but with no store/haptic/sound (irrelevant here)        */
/* ------------------------------------------------------------------- */

typedef struct {
    uint32_t t;
} fake_clock_t;

static uint32_t fake_now(void *user)
{
    return ((fake_clock_t *)user)->t;
}

typedef struct {
    int calls;
    uint32_t last_dest;
    char last_text[256];
    int rc;

    /* NAME in Settings — `name`/`name <text>` mesh-push capture. */
    int      owner_calls;
    uint32_t owner_last_dest;
    char     owner_last_long[64];
    char     owner_last_short[16];
    int      owner_rc;
    uint32_t owner_packet_id; /* confirmation-fix follow-up — handed back on success */

    /* Confirmation-fix follow-up — the get_owner_request follow-up. */
    int      owner_req_calls;
    uint32_t owner_req_last_dest;
    int      owner_req_rc;
} sender_spy_t;

static int spy_send_admin_set_owner(void *ctx, uint32_t dest, char const *long_name, char const *short_name,
                                     uint32_t *out_packet_id)
{
    sender_spy_t *s = (sender_spy_t *)ctx;
    s->owner_calls++;
    s->owner_last_dest = dest;
    snprintf(s->owner_last_long, sizeof(s->owner_last_long), "%s", (long_name != NULL) ? long_name : "");
    snprintf(s->owner_last_short, sizeof(s->owner_last_short), "%s", (short_name != NULL) ? short_name : "");
    if (s->owner_rc == 0 && out_packet_id != NULL) {
        *out_packet_id = s->owner_packet_id;
    }
    return s->owner_rc;
}

static int spy_send_get_owner_request(void *ctx, uint32_t dest)
{
    sender_spy_t *s = (sender_spy_t *)ctx;
    s->owner_req_calls++;
    s->owner_req_last_dest = dest;
    return s->owner_req_rc;
}

static int spy_send_text(void *ctx, uint32_t dest, char const *utf8)
{
    sender_spy_t *s = (sender_spy_t *)ctx;
    s->calls++;
    s->last_dest = dest;
    snprintf(s->last_text, sizeof(s->last_text), "%s", utf8);
    return s->rc;
}

typedef struct {
    fake_clock_t clk;
    ff_clock_t clock;
    fp_pack_t pack;
    jsmntok_t toks[FP_MAX_TOKENS];
    ff_shell_t shell;
    mc_events_t ev;
    sender_spy_t sender;
} harness_t;

static harness_t H;

/* i2c/compass hooks under test — module-scoped so `dispatch()` (used by
 * every test) can forward them without every call site needing its own
 * parameter; NULL by default (harness_init resets both), matching the
 * sim target's own "no I2C bus" reality unless a test opts in by
 * pointing one at a fake below. */
static ff_dbgconsole_i2c_scan_fn s_i2c_hook;
static ff_dbgconsole_compass_status_fn s_compass_hook;

#define MY_ID 0x00001000u
#define DANA 0x0000DA1Au
#define STRANGER 0x0000AAAAu

static void harness_init(uint32_t t0_ms)
{
    memset(&H, 0, sizeof(H));
    s_i2c_hook = NULL;
    s_compass_hook = NULL;
    H.clk.t = t0_ms;
    H.clock.now_ms = fake_now;
    H.clock.user = &H.clk;

    ff_shell_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.clock = &H.clock;
    cfg.pack = &H.pack;
    cfg.toks = H.toks;
    cfg.ntoks = FP_MAX_TOKENS;

    TEST_ASSERT_EQUAL_INT(0, ff_shell_init(&H.shell, &cfg));
    H.ev = ff_shell_events(&H.shell);
}

static void harness_wire_sender(int rc)
{
    H.sender.rc = rc;
    ff_wiring_sender_t sender;
    memset(&sender, 0, sizeof(sender));
    sender.send_text = spy_send_text;
    sender.ctx = &H.sender;
    sender.send_admin_set_owner = spy_send_admin_set_owner;
    sender.send_get_owner_request = spy_send_get_owner_request;
    ff_shell_set_sender(&H.shell, sender);
}

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

static void inject_node(uint32_t node, char const *short_name, uint32_t last_heard)
{
    mc_nodeinfo_t n = nodeinfo(node, short_name, last_heard);
    H.ev.on_node(H.ev.user, &n);
}

/* NAME in Settings — a self NodeInfo carrying a long_name (the mesh's
 * own reported owner name), the `name` console command's confirmation
 * source. */
static void inject_self_long_name(uint32_t node, char const *long_name)
{
    mc_nodeinfo_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = node;
    n.has_long_name = true;
    strncpy(n.long_name, long_name, sizeof(n.long_name) - 1);
    H.ev.on_node(H.ev.user, &n);
}

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

/* ------------------------------------------------------------------- */
/* reply capture                                                        */
/* ------------------------------------------------------------------- */

typedef struct {
    char lines[40][220];
    int n;
} capture_t;

static void capture_reset(capture_t *c)
{
    memset(c, 0, sizeof(*c));
}

static void capture_reply(void *user, char const *line)
{
    capture_t *c = (capture_t *)user;
    int const cap = (int)(sizeof(c->lines) / sizeof(c->lines[0]));
    if (c->n < cap) {
        snprintf(c->lines[c->n], sizeof(c->lines[0]), "%s", line);
    }
    c->n++;
}

static bool capture_has_line_containing(capture_t const *c, char const *needle)
{
    int const cap = (int)(sizeof(c->lines) / sizeof(c->lines[0]));
    int const n = (c->n < cap) ? c->n : cap;
    for (int i = 0; i < n; i++) {
        if (strstr(c->lines[i], needle) != NULL) return true;
    }
    return false;
}

static void dispatch(char const *line, capture_t *out)
{
    capture_reset(out);
    ff_dbgconsole_handle_line(&H.shell, line, strlen(line), ff_shell_now_ms(&H.shell), capture_reply, out,
                               s_i2c_hook, s_compass_hook);
}

/* ------------------------------------------------------------------- */
/* tests                                                                 */
/* ------------------------------------------------------------------- */

static void dbgconsole_help_lists_commands(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("help", &cap);

    TEST_ASSERT_TRUE(cap.n > 1);
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: help"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dm <node_hex>"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: i2c"));
}

static void dbgconsole_unknown_command_gets_try_help(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("frobnicate", &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: ? try help", cap.lines[0]);
}

static void dbgconsole_extra_args_also_gets_try_help(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("roster now", &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: ? try help", cap.lines[0]);
}

static void dbgconsole_empty_line_produces_no_reply(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("   \r\n", &cap);

    TEST_ASSERT_EQUAL_INT(0, cap.n);
}

static void dbgconsole_me_reports_defaults_when_nothing_known(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("me", &cap);

    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: me node=!00000000 link=NONE"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: me pos ok=0"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: me wall latched=0"));
}

static void dbgconsole_me_reports_node_pos_and_wall_after_events(void)
{
    harness_init(1000);
    inject_my_info(MY_ID);
    ff_shell_set_my_pos(&H.shell, (ff_latlon_t){12.5, -71.25});
    /* A plausible mesh timestamp bootstraps the wall latch — any inbound
     * NodeInfo does this (shell_observe_wall_nodeinfo runs unconditionally,
     * before any pairing/trust check). */
    inject_node(STRANGER, "Strngr", (uint32_t)1789768800u);

    capture_t cap;
    dispatch("me", &cap);

    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: me node=!00001000"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "pos ok=1"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "lat=12.500000"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: me wall latched=1 trust=BOOTSTRAP"));
}

static void dbgconsole_roster_lists_only_paired_members_with_position(void)
{
    harness_init(1000);
    TEST_ASSERT_TRUE(ff_shell_pair(&H.shell, DANA, true));
    /* A plausible last_heard latches the wall clock — position adoption
     * needs it latched to translate a rx unix time into a monotonic ms
     * (shell_rx_ms_from_unix); an unlatched clock drops the position
     * rather than fabricate an age (CLAUDE.md: honest data). */
    inject_node(DANA, "Dana", (uint32_t)1789768800u);
    inject_position(DANA, (uint32_t)1789768900u, 40.0, -74.0);
    /* A merely-heard stranger must NOT appear in roster. */
    inject_node(STRANGER, "Strngr", (uint32_t)1789768800u);

    capture_t cap;
    dispatch("roster", &cap);

    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: roster n=1"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "id=!0000da1a name=Dana"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "has_pos=1 lat=40.000000 lon=-74.000000"));
    TEST_ASSERT_FALSE(capture_has_line_containing(&cap, "aaaa"));
}

static void dbgconsole_heard_lists_unpaired_sender(void)
{
    harness_init(1000);
    inject_node(STRANGER, "Strngr", 2500);

    capture_t cap;
    dispatch("heard", &cap);

    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: heard n=1"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "id=!0000aaaa"));
}

static void dbgconsole_send_calls_sender_broadcast_and_updates_feed(void)
{
    harness_init(1000);
    harness_wire_sender(0);

    capture_t cap;
    dispatch("send hello crew", &cap);

    TEST_ASSERT_EQUAL_STRING("dbg: send ok dest=broadcast", cap.lines[0]);
    TEST_ASSERT_EQUAL_INT(1, H.sender.calls);
    TEST_ASSERT_EQUAL_STRING("hello crew", H.sender.last_text);

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(1, ff_feed_count(feed));
    ff_feed_item_t const *it = ff_feed_at(feed, 0);
    TEST_ASSERT_NOT_NULL(it);
    TEST_ASSERT_EQUAL(FEED_DIR_OUT, it->dir);
    TEST_ASSERT_EQUAL_STRING("hello crew", it->text);
}

static void dbgconsole_send_with_no_sender_reports_failed(void)
{
    harness_init(1000); /* no sender wired */
    capture_t cap;
    dispatch("send hi", &cap);

    TEST_ASSERT_EQUAL_STRING("dbg: send failed dest=broadcast", cap.lines[0]);
    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(feed)); /* a refused send fabricates no feed item */
}

static void dbgconsole_dm_calls_sender_with_parsed_dest(void)
{
    harness_init(1000);
    harness_wire_sender(0);

    capture_t cap;
    dispatch("dm a1b2c3d4 omw", &cap);

    TEST_ASSERT_EQUAL_STRING("dbg: dm ok dest=!a1b2c3d4", cap.lines[0]);
    TEST_ASSERT_EQUAL_INT(1, H.sender.calls);
    TEST_ASSERT_EQUAL_UINT32(0xa1b2c3d4u, H.sender.last_dest);
    TEST_ASSERT_EQUAL_STRING("omw", H.sender.last_text);
}

static void dbgconsole_dm_zero_dest_rejected_without_sending(void)
{
    /* `dm 0 ...` / `dm 00000000 ...` both parse to dest==0 (parse_node_hex
     * accepts any 1-8 hex digits, all-zero included) — `ff_shell_debug_
     * send_text` would silently collapse that to a broadcast. `dm` must
     * refuse it instead of ever reaching the sender: proves the seam
     * itself never fires, not just that the reply text looks right. */
    harness_init(1000);
    harness_wire_sender(0);

    capture_t cap;
    dispatch("dm 0 hi", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: ? dm needs a non-zero node id", cap.lines[0]);
    TEST_ASSERT_EQUAL_INT(0, H.sender.calls);

    dispatch("dm 00000000 hi", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: ? dm needs a non-zero node id", cap.lines[0]);
    TEST_ASSERT_EQUAL_INT(0, H.sender.calls);

    ff_feed_t const *feed = ff_shell_feed(&H.shell);
    TEST_ASSERT_EQUAL_UINT8(0, ff_feed_count(feed)); /* a rejected dm fabricates no feed item */
}

static void dbgconsole_flare_start_already_sending_then_cancel(void)
{
    harness_init(1000);
    harness_wire_sender(0);

    capture_t cap;
    dispatch("flare", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: flare started dur_s=300", cap.lines[0]);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);

    dispatch("flare", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: flare already sending", cap.lines[0]);
    TEST_ASSERT_TRUE(ff_shell_flare(&H.shell)->sending);

    dispatch("flare cancel", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: flare cancelled", cap.lines[0]);
    TEST_ASSERT_FALSE(ff_shell_flare(&H.shell)->sending);

    dispatch("flare cancel", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: flare not sending", cap.lines[0]);
}

static void dbgconsole_wall_reports_unlatched_then_latched_with_trust_and_source(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("wall", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: wall latched=0 rejected=0", cap.lines[0]);

    inject_node(STRANGER, "Strngr", (uint32_t)1789768800u);
    dispatch("wall", &cap);
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "latched=1"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "trust=BOOTSTRAP"));
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "last_src=!0000aaaa"));
    /* No pack loaded and no settings offset set: the UTC offset itself
     * is unknown (offset_min=?), so "assumed" — whether that offset was
     * a stated value or a guess — has nothing to be assumed ABOUT.
     * Printing "assumed=0" here would read as "a definite, non-assumed
     * offset", which is false; the field must read as unknown too. */
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "offset_min=? assumed=?"));
}

static void dbgconsole_wall_assumed_is_numeric_once_offset_is_known(void)
{
    /* Once an offset actually resolves (here: a settings offset the user
     * set), "assumed" is a real, meaningful flag again and must print
     * as 0/1, not "?" — the "?" fallback added for the unknown-offset
     * case above must not swallow the known case too. */
    harness_init(1000);
    inject_node(STRANGER, "Strngr", (uint32_t)1789768800u); /* latches the wall clock */

    ff_intent_t const set = {.kind = FF_INTENT_SETTING_SET,
                              .u = {.setting = {.id = FF_SETTING_UTC_OFFSET_MIN, .v = {.i = -240}}}};
    ff_shell_intent(&H.shell, &set);

    capture_t cap;
    dispatch("wall", &cap);
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "offset_min=-240 assumed=0"));
}

/* ------------------------------------------------------------------- */
/* i2c — platform-hook forwarding (fakes here; the real scan/compass    */
/* implementations live in app_main.c and are exercised only on device) */
/* ------------------------------------------------------------------- */

static int fake_i2c_scan_ok(void *user, char *out, size_t cap)
{
    (void)user;
    snprintf(out, cap, "0x20 io-expander, 0x53 touch");
    return 0;
}

static int fake_i2c_scan_fail(void *user, char *out, size_t cap)
{
    (void)user;
    (void)out;
    (void)cap;
    return -1;
}

static int fake_compass_status_ok(void *user, char *out, size_t cap)
{
    (void)user;
    snprintf(out, cap, "mag=absent imu=found heading=? cal=identity");
    return 0;
}

static void dbgconsole_i2c_unavailable_without_a_scan_hook(void)
{
    /* No scan hook (the sim target's own reality: no I2C bus at all) —
     * exactly one honest reply, no fabricated compass line either, even
     * with a compass hook wired up: nothing to scan means nothing to
     * follow up on. */
    harness_init(1000);
    s_compass_hook = fake_compass_status_ok;

    capture_t cap;
    dispatch("i2c", &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: i2c unavailable on this target", cap.lines[0]);
}

static void dbgconsole_i2c_reports_scan_and_compass_status_verbatim(void)
{
    /* Both hooks present: the scan line and the compass line are the
     * hook's own text, forwarded byte-for-byte behind this file's
     * "dbg: i2c "/"dbg: compass " prefixes — proves the text is
     * forwarded, not reformatted or reinterpreted along the way. */
    harness_init(1000);
    s_i2c_hook = fake_i2c_scan_ok;
    s_compass_hook = fake_compass_status_ok;

    capture_t cap;
    dispatch("i2c", &cap);

    TEST_ASSERT_EQUAL_INT(2, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: i2c 0x20 io-expander, 0x53 touch", cap.lines[0]);
    TEST_ASSERT_EQUAL_STRING("dbg: compass mag=absent imu=found heading=? cal=identity", cap.lines[1]);
}

static void dbgconsole_i2c_scan_failure_still_reports_compass(void)
{
    /* A scan hook that reports it could not run (e.g. the bus was never
     * brought up) gets the honest "scan failed" line — never whatever
     * partial/stale text might be sitting in its own output buffer —
     * but the compass line still follows: it comes from the compass
     * driver's own state, independent of this particular bus sweep. */
    harness_init(1000);
    s_i2c_hook = fake_i2c_scan_fail;
    s_compass_hook = fake_compass_status_ok;

    capture_t cap;
    dispatch("i2c", &cap);

    TEST_ASSERT_EQUAL_INT(2, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: i2c scan failed", cap.lines[0]);
    TEST_ASSERT_EQUAL_STRING("dbg: compass mag=absent imu=found heading=? cal=identity", cap.lines[1]);
}

static void dbgconsole_i2c_omits_compass_line_without_a_compass_hook(void)
{
    /* Scan hook present, compass hook NULL (e.g. CONFIG_FF_COMPASS=n on
     * a build that still has CONFIG_FF_DEBUG_CONSOLE=y): the scan line
     * still prints; the compass line is omitted entirely, never printed
     * with fields it cannot honestly answer. */
    harness_init(1000);
    s_i2c_hook = fake_i2c_scan_ok;

    capture_t cap;
    dispatch("i2c", &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: i2c 0x20 io-expander, 0x53 touch", cap.lines[0]);
}

/* ------------------------------------------------------------------- */
/* S12 step 3 — "cal" and its four sub-verbs                             */
/* ------------------------------------------------------------------- */

/* Same 14-sample, all-8-octants fixture test_geo.c's own
 * S01_AC5_calibration_recovers_hard_offset_and_improves_heading and
 * test_intent.c's S12step3_* tests already use — reused for the same
 * "provably the same claim" reason those files' own comments give. */
static const ff_vec3_t s_cal_full_coverage_samples[] = {
    {0.050000f, -0.480000f, 0.020000f},   {-0.296410f, 0.229808f, -0.297543f},
    {-0.296410f, -0.289808f, -0.297543f}, {0.396410f, 0.229808f, -0.297543f},
    {0.396410f, -0.289808f, -0.297543f},  {0.050000f, -0.030000f, 0.570000f},
    {-0.296410f, -0.289808f, 0.337543f},  {0.396410f, -0.289808f, 0.337543f},
    {-0.550000f, -0.030000f, 0.020000f},  {0.650000f, -0.030000f, 0.020000f},
    {-0.296410f, 0.229808f, 0.337543f},   {0.396410f, 0.229808f, 0.337543f},
    {0.050000f, 0.420000f, 0.020000f},    {0.050000f, -0.030000f, -0.530000f},
};
#define CAL_FULL_N (sizeof(s_cal_full_coverage_samples) / sizeof(s_cal_full_coverage_samples[0]))

static void dbgconsole_cal_status_reports_identity_when_uncalibrated_and_inactive(void)
{
    harness_init(1000);

    capture_t cap;
    dispatch("cal", &cap);

    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("dbg: cal active=0 cal=identity", cap.lines[0]);
}

static void dbgconsole_cal_start_then_status_reports_live_progress(void)
{
    harness_init(1000);

    capture_t cap;
    dispatch("cal start", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal started", cap.lines[0]);

    for (size_t i = 0; i < CAL_FULL_N; i++) {
        ff_shell_compass_cal_sample(&H.shell, s_cal_full_coverage_samples[i]);
    }

    dispatch("cal", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal active=1 progress_pct=100 samples=14 can_finish=1 cal=identity",
                             cap.lines[0]);

    dispatch("cal start", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal already active", cap.lines[0]);
}

static void dbgconsole_cal_finish_below_threshold_reports_failure_and_stays_active(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("cal start", &cap);

    ff_vec3_t const clustered[] = {{0.40f, 0.05f, 0.10f}, {0.42f, 0.06f, 0.11f}, {0.38f, 0.04f, 0.09f}};
    for (size_t i = 0; i < sizeof(clustered) / sizeof(clustered[0]); i++) {
        ff_shell_compass_cal_sample(&H.shell, clustered[i]);
    }

    dispatch("cal finish", &cap);
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: cal finish failed"));
    TEST_ASSERT_TRUE(ff_shell_compass_cal_status(&H.shell).active);
    TEST_ASSERT_FALSE(ff_shell_compass_cal_status(&H.shell).cal_valid);
}

static void dbgconsole_cal_finish_at_full_coverage_reports_ok_and_persists(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("cal start", &cap);

    for (size_t i = 0; i < CAL_FULL_N; i++) {
        ff_shell_compass_cal_sample(&H.shell, s_cal_full_coverage_samples[i]);
    }

    dispatch("cal finish", &cap);
    TEST_ASSERT_TRUE(capture_has_line_containing(&cap, "dbg: cal finished ok"));
    TEST_ASSERT_FALSE(ff_shell_compass_cal_status(&H.shell).active);
    TEST_ASSERT_TRUE(ff_shell_compass_cal_status(&H.shell).cal_valid);

    dispatch("cal", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal active=0 cal=custom", cap.lines[0]);
}

static void dbgconsole_cal_finish_with_no_session_reports_not_active(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("cal finish", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal not active", cap.lines[0]);
}

static void dbgconsole_cal_cancel_reports_cancelled_then_not_active(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("cal start", &cap);

    dispatch("cal cancel", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal cancelled", cap.lines[0]);
    TEST_ASSERT_FALSE(ff_shell_compass_cal_status(&H.shell).active);

    dispatch("cal cancel", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal not active", cap.lines[0]);
}

static void dbgconsole_cal_clear_drops_a_calibrated_puck_to_identity(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("cal start", &cap);
    for (size_t i = 0; i < CAL_FULL_N; i++) {
        ff_shell_compass_cal_sample(&H.shell, s_cal_full_coverage_samples[i]);
    }
    dispatch("cal finish", &cap);
    TEST_ASSERT_TRUE(ff_shell_compass_cal_status(&H.shell).cal_valid);

    dispatch("cal clear", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal cleared", cap.lines[0]);
    TEST_ASSERT_FALSE(ff_shell_compass_cal_status(&H.shell).cal_valid);

    dispatch("cal clear", &cap);
    TEST_ASSERT_EQUAL_STRING("dbg: cal already uncalibrated", cap.lines[0]);
}

/* ------------------------------------------------------------------- */
/* NAME in Settings                                                     */
/* ------------------------------------------------------------------- */

static void dbgconsole_name_bare_reports_unset_and_unknown(void)
{
    harness_init(1000);
    capture_t cap;
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=(unset) mesh=unknown confirmed=0 seq=0 pushed=none ack=none reply=none mismatch=0 link=NONE",
        cap.lines[0]);
}

static void dbgconsole_name_set_commits_and_reports_confirmed_false(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);

    TEST_ASSERT_EQUAL_STRING("Jake", ff_shell_settings(&H.shell)->my_name);
    TEST_ASSERT_EQUAL_INT(1, H.sender.owner_calls);
    TEST_ASSERT_EQUAL_UINT32(MY_ID, H.sender.owner_last_dest);
    TEST_ASSERT_EQUAL_STRING("Jake", H.sender.owner_last_long);
    TEST_ASSERT_EQUAL_STRING("JAKE", H.sender.owner_last_short);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=unknown confirmed=0 seq=1 pushed=Jake/JAKE ack=none reply=none mismatch=0 link=NONE",
        cap.lines[0]);
}

static void dbgconsole_name_reports_confirmed_once_self_nodeinfo_matches(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);
    TEST_ASSERT_TRUE(strstr(cap.lines[0], "confirmed=0") != NULL);

    inject_self_long_name(MY_ID, "Jake"); /* the mesh caught up */
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=Jake confirmed=1 seq=1 pushed=Jake/JAKE ack=none reply=none mismatch=0 link=NONE",
        cap.lines[0]);
}

/* Confirmation-fix follow-up — the trailing pushed=/ack=/reply= fields
 * through a full get_owner_response round trip and a routing NAK, both
 * via the bench console (the coordinator's own bench-test surface). */
static void dbgconsole_name_reports_reply_once_get_owner_response_arrives(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, H.sender.owner_req_calls,
                                  "a successful push must follow up with its own get_owner_request");

    H.ev.on_owner(H.ev.user, "Jake", "JAKE");
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=Jake confirmed=1 seq=1 pushed=Jake/JAKE ack=none reply=Jake/JAKE mismatch=0 link=NONE",
        cap.lines[0]);
}

static void dbgconsole_name_reports_nak_as_push_failed_not_pending(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    H.sender.owner_packet_id = 0x77u;
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);

    H.ev.on_routing_ack(H.ev.user, 0x77u, false);
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=unknown confirmed=0 seq=1 pushed=Jake/JAKE ack=nak reply=none mismatch=0 link=NONE",
        cap.lines[0]);
}

/* ====================================================================
 * Confirmation fix round 2 (bench finding, 2026-09-06, AFTER commit
 * 51e4ae1, real puck + Meshtastic 2.7.26 comms brain): `name Jake H` ->
 * `pushed=ok reply=none` forever (fixed at the meshclient layer, see
 * mc_send_get_owner_request's own doc comment), and `name Jake` when the
 * node's name is ALREADY Jake reported `confirmed=1` INSTANTLY with
 * `ack=none reply=none` — a false positive from stale equality, fixed
 * here by the push-generation (`seq=`) gate.
 * ==================================================================== */

/**
 * THE false-positive reproduction, at the console layer, matching the
 * exact bench transcript: push "Jake", let a matching reply confirm it
 * for real, then re-issue the EXACT SAME `name Jake` command (this
 * feature's own retry mechanism for a push that may have silently
 * failed). Before any NEW reply arrives for this second push, the console
 * must read pending (`confirmed=0`), never re-use the first push's own
 * confirmation. `seq=` visibly ticks from 1 to 2 across the two pushes.
 */
static void dbgconsole_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);
    H.ev.on_owner(H.ev.user, "Jake", "JAKE"); /* first push, genuinely confirmed */
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=Jake confirmed=1 seq=1 pushed=Jake/JAKE ack=none reply=Jake/JAKE mismatch=0 link=NONE",
        cap.lines[0]);

    dispatch("name Jake", &cap); /* re-commit the SAME text — a fresh push generation */
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "dbg: name stored=Jake mesh=Jake confirmed=0 seq=2 pushed=Jake/JAKE ack=none reply=none mismatch=0 link=NONE",
        cap.lines[0],
        "stale pre-push equality (mesh=Jake from the FIRST push) must never read as THIS push's confirmation");
}

/**
 * A fresh reply for the current push naming a DIFFERENT owner than was
 * pushed reports `mismatch=1`, never silently folded into either
 * `confirmed=1` or the plain `ack=nak` failure path (a routing NAK says
 * nothing about what name the admin module actually ended up with).
 */
static void dbgconsole_name_reports_mismatch_when_the_reply_names_someone_else(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    inject_my_info(MY_ID);

    capture_t cap;
    dispatch("name Jake", &cap);

    H.ev.on_owner(H.ev.user, "SomeoneElse", "SOME"); /* someone else re-set the owner in between */
    dispatch("name", &cap);
    TEST_ASSERT_EQUAL_STRING(
        "dbg: name stored=Jake mesh=SomeoneElse confirmed=0 seq=1 pushed=Jake/JAKE ack=none reply=SomeoneElse/SOME "
        "mismatch=1 link=NONE",
        cap.lines[0]);
}

static void dbgconsole_name_set_with_no_node_id_still_commits_locally(void)
{
    harness_init(1000);
    harness_wire_sender(0);
    /* deliberately no inject_my_info */

    capture_t cap;
    dispatch("name Jake", &cap);

    TEST_ASSERT_EQUAL_STRING("Jake", ff_shell_settings(&H.shell)->my_name);
    TEST_ASSERT_EQUAL_INT(0, H.sender.owner_calls); /* no self id known -> no push attempted */
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(dbgconsole_help_lists_commands);
    RUN_TEST(dbgconsole_unknown_command_gets_try_help);
    RUN_TEST(dbgconsole_extra_args_also_gets_try_help);
    RUN_TEST(dbgconsole_empty_line_produces_no_reply);

    RUN_TEST(dbgconsole_me_reports_defaults_when_nothing_known);
    RUN_TEST(dbgconsole_me_reports_node_pos_and_wall_after_events);

    RUN_TEST(dbgconsole_roster_lists_only_paired_members_with_position);
    RUN_TEST(dbgconsole_heard_lists_unpaired_sender);

    RUN_TEST(dbgconsole_send_calls_sender_broadcast_and_updates_feed);
    RUN_TEST(dbgconsole_send_with_no_sender_reports_failed);
    RUN_TEST(dbgconsole_dm_calls_sender_with_parsed_dest);
    RUN_TEST(dbgconsole_dm_zero_dest_rejected_without_sending);

    RUN_TEST(dbgconsole_flare_start_already_sending_then_cancel);
    RUN_TEST(dbgconsole_wall_reports_unlatched_then_latched_with_trust_and_source);
    RUN_TEST(dbgconsole_wall_assumed_is_numeric_once_offset_is_known);

    RUN_TEST(dbgconsole_i2c_unavailable_without_a_scan_hook);
    RUN_TEST(dbgconsole_i2c_reports_scan_and_compass_status_verbatim);
    RUN_TEST(dbgconsole_i2c_scan_failure_still_reports_compass);
    RUN_TEST(dbgconsole_i2c_omits_compass_line_without_a_compass_hook);

    RUN_TEST(dbgconsole_cal_status_reports_identity_when_uncalibrated_and_inactive);
    RUN_TEST(dbgconsole_cal_start_then_status_reports_live_progress);
    RUN_TEST(dbgconsole_cal_finish_below_threshold_reports_failure_and_stays_active);
    RUN_TEST(dbgconsole_cal_finish_at_full_coverage_reports_ok_and_persists);
    RUN_TEST(dbgconsole_cal_finish_with_no_session_reports_not_active);
    RUN_TEST(dbgconsole_cal_cancel_reports_cancelled_then_not_active);
    RUN_TEST(dbgconsole_cal_clear_drops_a_calibrated_puck_to_identity);

    RUN_TEST(dbgconsole_name_bare_reports_unset_and_unknown);
    RUN_TEST(dbgconsole_name_set_commits_and_reports_confirmed_false);
    RUN_TEST(dbgconsole_name_reports_confirmed_once_self_nodeinfo_matches);
    RUN_TEST(dbgconsole_name_reports_reply_once_get_owner_response_arrives);
    RUN_TEST(dbgconsole_name_reports_nak_as_push_failed_not_pending);
    RUN_TEST(dbgconsole_name_recommitting_the_same_name_does_not_falsely_confirm_from_stale_mesh_state);
    RUN_TEST(dbgconsole_name_reports_mismatch_when_the_reply_names_someone_else);
    RUN_TEST(dbgconsole_name_set_with_no_node_id_still_commits_locally);

    return UNITY_END();
}
