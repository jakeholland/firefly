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
} sender_spy_t;

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

#define MY_ID 0x00001000u
#define DANA 0x0000DA1Au
#define STRANGER 0x0000AAAAu

static void harness_init(uint32_t t0_ms)
{
    memset(&H, 0, sizeof(H));
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
    ff_dbgconsole_handle_line(&H.shell, line, strlen(line), ff_shell_now_ms(&H.shell), capture_reply, out);
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

    return UNITY_END();
}
