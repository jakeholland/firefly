/**
 * test_ctl_server.c — S13c: ctl_server.h/.c tests.
 *
 * Group 1 exercises ff_ctl_feed_byte directly (byte arrays, no sockets) —
 * this is where "bound every read, reject oversized lines" is pinned,
 * including the exact boundary (FF_CTL_MAX_LINE-1 succeeds, one byte more
 * doesn't) per the task brief's "exact boundary tests" requirement.
 *
 * Group 2 exercises ff_ctl_process_line directly (a line string in, a
 * handler vtable, a response string out) — every command's success path
 * and every guard path (missing/invalid field, unsupported callback,
 * malformed JSON, non-object root, unknown cmd) gets its own test, per
 * the task brief's "every guard path tested" requirement. Two mutations
 * are spot-checked in the comments next to the tests they'd break.
 *
 * Group 3 is a small end-to-end test over a real loopback socket, proving
 * ff_ctl_open/ff_ctl_poll/ff_ctl_close actually move bytes correctly
 * (framing + dispatch wired together), and that the socket only accepts
 * localhost connections.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"

#include "ctl_server.h"

void setUp(void) {}
void tearDown(void) {}

/* ----------------------------------------------------------------------
 * Group 1: ff_ctl_feed_byte
 * -------------------------------------------------------------------- */

static void feed_string(ff_ctl_linebuf_t *lb, char const *s, ff_ctl_feed_result_t *last_result)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        *last_result = ff_ctl_feed_byte(lb, s[i]);
    }
}

static void S13c_ctl_feed_byte_simple_line(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;

    feed_string(&lb, "{\"cmd\":\"quit\"}", &r);
    TEST_ASSERT_EQUAL(FF_CTL_FEED_NEED_MORE, r);

    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_STRING("{\"cmd\":\"quit\"}", lb.buf);
}

static void S13c_ctl_feed_byte_strips_crlf(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;
    feed_string(&lb, "abc\r", &r);
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_STRING("abc", lb.buf);
}

static void S13c_ctl_feed_byte_two_lines_in_sequence(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;
    feed_string(&lb, "one", &r);
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_STRING("one", lb.buf);

    feed_string(&lb, "two", &r);
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_STRING("two", lb.buf);
}

/* Exact boundary: a line whose content is FF_CTL_MAX_LINE-1 bytes (then a
 * '\n') must succeed. Mutation check: if the off-by-one in
 * ff_ctl_feed_byte's `>=` bound were ever loosened to `>`, this line would
 * start failing to fit the NUL at lb.buf[len] one byte later than
 * documented — this test pins the exact boundary, not an approximation
 * of it. */
static void S13c_ctl_feed_byte_exactly_max_line_minus_1_succeeds(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;

    for (size_t i = 0; i < FF_CTL_MAX_LINE - 1; i++) {
        r = ff_ctl_feed_byte(&lb, 'a');
        TEST_ASSERT_EQUAL(FF_CTL_FEED_NEED_MORE, r);
    }
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_UINT32(FF_CTL_MAX_LINE - 1, (uint32_t)strlen(lb.buf));
}

/* One byte more (content length FF_CTL_MAX_LINE, no newline yet) must be
 * rejected — the other half of the same boundary. Mutation check: if the
 * bound were loosened, this would silently succeed and lb.buf could be
 * written one byte past its declared capacity on some other input shape;
 * this test would stop seeing FF_CTL_FEED_TOO_LONG and fail. */
static void S13c_ctl_feed_byte_exactly_max_line_content_rejected(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;

    for (size_t i = 0; i < FF_CTL_MAX_LINE - 1; i++) {
        r = ff_ctl_feed_byte(&lb, 'a');
    }
    TEST_ASSERT_EQUAL(FF_CTL_FEED_NEED_MORE, r);
    r = ff_ctl_feed_byte(&lb, 'a'); /* the FF_CTL_MAX_LINE-th content byte */
    TEST_ASSERT_EQUAL(FF_CTL_FEED_TOO_LONG, r);
}

static void S13c_ctl_feed_byte_resyncs_after_too_long_line(void)
{
    ff_ctl_linebuf_t lb = {0};
    ff_ctl_feed_result_t r = FF_CTL_FEED_NEED_MORE;

    for (size_t i = 0; i < FF_CTL_MAX_LINE; i++) {
        r = ff_ctl_feed_byte(&lb, 'x');
    }
    TEST_ASSERT_EQUAL(FF_CTL_FEED_TOO_LONG, r);

    /* Still resyncing: more garbage (even a lone '\n' isn't enough — wait,
     * the FIRST '\n' after the overflow ends resync) — feed a few more
     * junk bytes, then the newline that ends the oversized line. */
    r = ff_ctl_feed_byte(&lb, 'y');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_NEED_MORE, r); /* resyncing: swallowed silently */
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_NEED_MORE, r); /* this newline ends resync, is not itself a line */

    /* Normal operation resumes cleanly. */
    feed_string(&lb, "ok", &r);
    r = ff_ctl_feed_byte(&lb, '\n');
    TEST_ASSERT_EQUAL(FF_CTL_FEED_LINE, r);
    TEST_ASSERT_EQUAL_STRING("ok", lb.buf);
}

/* ----------------------------------------------------------------------
 * Group 2: ff_ctl_process_line
 * -------------------------------------------------------------------- */

typedef struct {
    int  tap_calls;
    double tap_x, tap_y;

    int  swipe_calls;
    char swipe_dir[8];

    int      hold_calls;
    double   hold_x, hold_y;
    uint32_t hold_ms;

    int      clock_calls;
    uint32_t clock_advance_ms;
    bool     clock_advance_ok;   /* what clock_advance() should return */
    char const *clock_err;

    int  state_calls;
    char const *state_json_value; /* NULL = "unavailable" (-1 return) */

    int  screenshot_calls;
    char screenshot_path[64];
    bool screenshot_ok;
    char const *screenshot_err;

    int quit_calls;
} spy_t;

static void spy_tap(void *u, double x, double y)
{
    spy_t *s = (spy_t *)u;
    s->tap_calls++;
    s->tap_x = x;
    s->tap_y = y;
}

static void spy_swipe(void *u, char const *dir)
{
    spy_t *s = (spy_t *)u;
    s->swipe_calls++;
    (void)snprintf(s->swipe_dir, sizeof(s->swipe_dir), "%s", dir);
}

static void spy_hold(void *u, double x, double y, uint32_t ms)
{
    spy_t *s = (spy_t *)u;
    s->hold_calls++;
    s->hold_x = x;
    s->hold_y = y;
    s->hold_ms = ms;
}

static bool spy_clock_advance(void *u, uint32_t advance_ms, char const **err)
{
    spy_t *s = (spy_t *)u;
    s->clock_calls++;
    s->clock_advance_ms = advance_ms;
    if (!s->clock_advance_ok) {
        *err = s->clock_err;
        return false;
    }
    return true;
}

static int spy_state_json(void *u, char *buf, size_t buf_sz)
{
    spy_t *s = (spy_t *)u;
    s->state_calls++;
    if (s->state_json_value == NULL) return -1;
    int n = snprintf(buf, buf_sz, "%s", s->state_json_value);
    if (n < 0 || (size_t)n >= buf_sz) return -1;
    return n;
}

static bool spy_screenshot(void *u, char const *path, char const **err)
{
    spy_t *s = (spy_t *)u;
    s->screenshot_calls++;
    (void)snprintf(s->screenshot_path, sizeof(s->screenshot_path), "%s", path);
    if (!s->screenshot_ok) {
        *err = s->screenshot_err;
        return false;
    }
    return true;
}

static void spy_quit(void *u)
{
    spy_t *s = (spy_t *)u;
    s->quit_calls++;
}

static ff_ctl_handlers_t spy_handlers(spy_t *s)
{
    ff_ctl_handlers_t h = {0};
    h.user = s;
    h.tap = spy_tap;
    h.swipe = spy_swipe;
    h.hold = spy_hold;
    h.clock_advance = spy_clock_advance;
    h.state_json = spy_state_json;
    h.screenshot = spy_screenshot;
    h.quit = spy_quit;
    return h;
}

static bool resp_ok(char const *resp)
{
    return strstr(resp, "\"ok\":true") != NULL;
}

static void S13c_ctl_process_line_tap(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    bool quit = ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":12,\"y\":34.5}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(quit);
    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.tap_calls);
    TEST_ASSERT_EQUAL_DOUBLE(12.0, s.tap_x);
    TEST_ASSERT_EQUAL_DOUBLE(34.5, s.tap_y);
}

static void S13c_ctl_process_line_tap_missing_y_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":12}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls); /* guard path: handler must not fire */
}

/* --- Review fix (PR #19 finding #1): tap coordinate bounds. ---
 * Reproduces (as a real unit test, not just a manual `nc` session) the
 * UBSan-confirmed finding: {"cmd":"tap","x":1e300,"y":0} used to reach a
 * bare (lv_coord_t)x cast in main.c's ff_loop_tap, undefined behavior per
 * C11 6.3.1.4. Exact boundary per the task brief's "exact boundary
 * tests": FF_CTL_TAP_COORD_MAX (32767) accepted, one past it rejected —
 * same shape as the line-length boundary tests above. */
static void S13c_ctl_process_line_tap_at_coord_max_accepted(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":32767,\"y\":-32768}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.tap_calls);
    TEST_ASSERT_EQUAL_DOUBLE(32767.0, s.tap_x);
    TEST_ASSERT_EQUAL_DOUBLE(-32768.0, s.tap_y);
}

static void S13c_ctl_process_line_tap_one_past_coord_max_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":32768,\"y\":0}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls); /* rejected before the handler ever runs */
}

static void S13c_ctl_process_line_tap_one_past_coord_min_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":0,\"y\":-32769}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls);
}

/* The exact finding from the review: 1e300 used to fire
 * "runtime error: 1e+300 is outside the range of representable values of
 * type 'int'" under -fsanitize=undefined. Pinned here so a regression
 * (someone loosening the bounds check) is caught by ctest, not by a
 * human re-running the sim under UBSan by hand. */
static void S13c_ctl_process_line_tap_huge_value_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":1e300,\"y\":0}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls);
}

static void S13c_ctl_process_line_tap_infinity_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":1e999,\"y\":0}", &h, resp, sizeof(resp)); /* strtod overflow -> +inf */

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls);
}

/* JSON itself has no NaN literal, so this exercises the OTHER path to a
 * non-finite value: ctl_num()'s "couldn't parse this token" sentinel
 * (NAN, since S13 review fixup) rather than a client-supplied literal —
 * e.g. a numeric field jsmn tokenized as a primitive but strtod can't
 * parse (here: the primitive "null", handled distinctly elsewhere, but
 * an unparseable bare-word primitive like "NaN" — not valid JSON, so
 * this reaches ctl_num() as a JSMN_PRIMITIVE token strtod() still can't
 * consume in this implementation's minimal parser). */
static void S13c_ctl_process_line_tap_unparseable_number_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":truthy,\"y\":0}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.tap_calls);
}

static void S13c_ctl_process_line_swipe_left_and_right(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"swipe\",\"dir\":\"left\"}", &h, resp, sizeof(resp));
    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_STRING("left", s.swipe_dir);

    ff_ctl_process_line("{\"cmd\":\"swipe\",\"dir\":\"right\"}", &h, resp, sizeof(resp));
    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_STRING("right", s.swipe_dir);
    TEST_ASSERT_EQUAL_INT(2, s.swipe_calls);
}

static void S13c_ctl_process_line_swipe_invalid_dir_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"swipe\",\"dir\":\"up\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.swipe_calls);
    /* Regression pin: an earlier version of this error message had
     * literal embedded quotes ("swipe dir must be \"left\" or
     * \"right\"") that corrupted the JSON response, since ctl_err()
     * doesn't escape its message argument — exact-match the whole
     * response (not just "ok:false somewhere in there") so that class of
     * bug can't creep back in unnoticed. */
    TEST_ASSERT_EQUAL_STRING("{\"ok\":false,\"error\":\"swipe dir must be left or right\"}", resp);
}

/* --- issue #70: `hold` --- */

static void S70_ctl_process_line_hold_explicit_ms(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":120,\"y\":340,\"ms\":900}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.hold_calls);
    TEST_ASSERT_EQUAL_DOUBLE(120.0, s.hold_x);
    TEST_ASSERT_EQUAL_DOUBLE(340.0, s.hold_y);
    TEST_ASSERT_EQUAL_UINT32(900, s.hold_ms);
}

/* `ms` omitted entirely: FF_CTL_HOLD_DEFAULT_MS (600 — comfortably past
 * LVGL's 400ms long_press_time), not 0/unset. */
static void S70_ctl_process_line_hold_defaults_ms(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":1,\"y\":2}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_UINT32(FF_CTL_HOLD_DEFAULT_MS, s.hold_ms);
}

static void S70_ctl_process_line_hold_missing_y_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":12}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls); /* guard path: handler must not fire */
}

/* Same coordinate bounds as `tap` (FF_CTL_TAP_COORD_MIN/MAX, reused —
 * see ctl_server.c's hold handler). Exact boundary per the standing
 * "exact boundary tests" convention: max accepted, one past rejected. */
static void S70_ctl_process_line_hold_at_coord_max_accepted(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":32767,\"y\":-32768}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.hold_calls);
}

static void S70_ctl_process_line_hold_one_past_coord_max_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":32768,\"y\":0}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls);
}

static void S70_ctl_process_line_hold_non_finite_x_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":1e999,\"y\":0}", &h, resp, sizeof(resp)); /* strtod overflow -> +inf */

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls);
}

/* Exact boundary on `ms`: FF_CTL_HOLD_MS_MAX (65535) accepted, one past
 * it rejected — same shape as tap's coordinate boundary tests. */
static void S70_ctl_process_line_hold_ms_at_max_accepted(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":0,\"y\":0,\"ms\":65535}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_UINT32(65535, s.hold_ms);
}

static void S70_ctl_process_line_hold_ms_one_past_max_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":0,\"y\":0,\"ms\":65536}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls);
}

static void S70_ctl_process_line_hold_negative_ms_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":0,\"y\":0,\"ms\":-1}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls);
}

static void S70_ctl_process_line_hold_non_numeric_ms_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":0,\"y\":0,\"ms\":\"long\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.hold_calls);
}

static void S70_ctl_process_line_hold_unsupported_callback_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    h.hold = NULL; /* simulate a caller that didn't wire hold support */
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"hold\",\"x\":1,\"y\":2}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "unsupported"));
}

static void S13c_ctl_process_line_clock_advance(void)
{
    spy_t s = {0};
    s.clock_advance_ok = true;
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"clock\",\"advance_ms\":250}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_UINT32(250, s.clock_advance_ms);
}

static void S13c_ctl_process_line_clock_negative_advance_is_error(void)
{
    spy_t s = {0};
    s.clock_advance_ok = true;
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"clock\",\"advance_ms\":-5}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.clock_calls); /* rejected before the handler even runs */
}

static void S13c_ctl_process_line_clock_unavailable_without_mock_clock(void)
{
    spy_t s = {0};
    s.clock_advance_ok = false;
    s.clock_err = "clock control requires --mock-clock";
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"clock\",\"advance_ms\":10}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "--mock-clock"));
    TEST_ASSERT_EQUAL_INT(1, s.clock_calls); /* handler *did* run, and honestly declined */
}

static void S13c_ctl_process_line_state_embeds_json(void)
{
    spy_t s = {0};
    s.state_json_value = "{\"fixture\":\"x\"}";
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"state\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"state\":{\"fixture\":\"x\"}}", resp);
}

static void S13c_ctl_process_line_state_unavailable_is_error(void)
{
    spy_t s = {0};
    s.state_json_value = NULL; /* spy_state_json returns -1 */
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"state\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_state_response_buffer_too_small_is_error(void)
{
    spy_t s = {0};
    s.state_json_value = "{\"fixture\":\"a-fairly-long-value-to-not-fit\"}";
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[16]; /* deliberately far too small for the wrapped response */

    ff_ctl_process_line("{\"cmd\":\"state\"}", &h, resp, sizeof(resp));

    /* Must not silently truncate a JSON response into something that
     * parses as a *different*, smaller, wrong document — must fail loud
     * instead. */
    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_screenshot(void)
{
    spy_t s = {0};
    s.screenshot_ok = true;
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"screenshot\",\"path\":\"/tmp/out.png\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_STRING("/tmp/out.png", s.screenshot_path);
}

static void S13c_ctl_process_line_screenshot_missing_path_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"screenshot\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.screenshot_calls);
}

static void S13c_ctl_process_line_screenshot_handler_failure_reports_err(void)
{
    spy_t s = {0};
    s.screenshot_ok = false;
    s.screenshot_err = "no such directory";
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"screenshot\",\"path\":\"/no/such/dir/x.png\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "no such directory"));
}

/* Boundary: screenshot's internal path[] buffer is 4000 bytes. A path
 * token of exactly 3999 bytes (the max that fits with its NUL) must
 * succeed; 4000 must fail loud (never silently truncate a filesystem
 * path — see ctl_server.c's comment at this check). */
static void build_json_with_path_of_len(char *out, size_t out_sz, size_t path_len)
{
    size_t off = 0;
    off += (size_t)snprintf(out + off, out_sz - off, "{\"cmd\":\"screenshot\",\"path\":\"");
    for (size_t i = 0; i < path_len; i++) {
        out[off++] = 'p';
    }
    off += (size_t)snprintf(out + off, out_sz - off, "\"}");
    out[off] = '\0';
}

static void S13c_ctl_process_line_screenshot_path_at_buffer_limit_succeeds(void)
{
    spy_t s = {0};
    s.screenshot_ok = true;
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[256];

    static char line[FF_CTL_MAX_LINE];
    build_json_with_path_of_len(line, sizeof(line), 3999);

    ff_ctl_process_line(line, &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.screenshot_calls);
}

static void S13c_ctl_process_line_screenshot_path_over_buffer_limit_is_error(void)
{
    spy_t s = {0};
    s.screenshot_ok = true;
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[256];

    static char line[FF_CTL_MAX_LINE];
    build_json_with_path_of_len(line, sizeof(line), 4000);

    ff_ctl_process_line(line, &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(0, s.screenshot_calls); /* rejected before ever writing anything */
}

static void S13c_ctl_process_line_quit(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    bool quit = ff_ctl_process_line("{\"cmd\":\"quit\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_TRUE(quit);
    TEST_ASSERT_TRUE(resp_ok(resp));
    TEST_ASSERT_EQUAL_INT(1, s.quit_calls);
}

static void S13c_ctl_process_line_unknown_cmd_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    bool quit = ff_ctl_process_line("{\"cmd\":\"levitate\"}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(quit);
    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_missing_cmd_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("{\"x\":1}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_malformed_json_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("not json at all {{{", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_non_object_root_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("[1,2,3]", &h, resp, sizeof(resp));
    TEST_ASSERT_FALSE(resp_ok(resp));

    ff_ctl_process_line("\"just a string\"", &h, resp, sizeof(resp));
    TEST_ASSERT_FALSE(resp_ok(resp));

    ff_ctl_process_line("42", &h, resp, sizeof(resp));
    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_empty_line_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    char resp[512];

    ff_ctl_process_line("", &h, resp, sizeof(resp));
    TEST_ASSERT_FALSE(resp_ok(resp));
}

static void S13c_ctl_process_line_null_handlers_is_error(void)
{
    char resp[512];
    bool quit = ff_ctl_process_line("{\"cmd\":\"quit\"}", NULL, resp, sizeof(resp));
    TEST_ASSERT_FALSE(quit);
    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "no handlers"));
}

static void S13c_ctl_process_line_unsupported_callback_is_error(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);
    h.tap = NULL; /* simulate a caller that didn't wire tap support */
    char resp[512];

    ff_ctl_process_line("{\"cmd\":\"tap\",\"x\":1,\"y\":2}", &h, resp, sizeof(resp));

    TEST_ASSERT_FALSE(resp_ok(resp));
    TEST_ASSERT_NOT_NULL(strstr(resp, "unsupported"));
}

/* ----------------------------------------------------------------------
 * Group 3: real loopback socket, ff_ctl_open/poll/close.
 * -------------------------------------------------------------------- */

static uint16_t ctl_bound_port(ff_ctl_server_t const *srv)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(srv->listen_fd, (struct sockaddr *)&addr, &len));
    return ntohs(addr.sin_port);
}

static int connect_loopback(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, connect(fd, (struct sockaddr *)&addr, sizeof(addr)));
    return fd;
}

static void S13c_ctl_socket_binds_loopback_only(void)
{
    ff_ctl_server_t srv;
    TEST_ASSERT_EQUAL_INT(0, ff_ctl_open(&srv, 0, 0)); /* port 0: OS picks an ephemeral port; idle timeout disabled */

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(srv.listen_fd, (struct sockaddr *)&addr, &len));
    /* INADDR_LOOPBACK is already host byte order; addr.sin_addr.s_addr
     * (filled in by the kernel) is network byte order — ntohl() belongs
     * on exactly one side of this comparison, not both. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)INADDR_LOOPBACK, ntohl(addr.sin_addr.s_addr));

    ff_ctl_close(&srv);
}

static void S13c_ctl_socket_poll_end_to_end(void)
{
    spy_t s = {0};
    s.state_json_value = "{\"fixture\":\"e2e\"}";
    ff_ctl_handlers_t h = spy_handlers(&s);

    ff_ctl_server_t srv;
    TEST_ASSERT_EQUAL_INT(0, ff_ctl_open(&srv, 0, 0)); /* idle timeout disabled */
    uint16_t port = ctl_bound_port(&srv);

    int client = connect_loopback(port);

    char const *req = "{\"cmd\":\"state\"}\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(req), (int)send(client, req, strlen(req), 0));

    /* Give ff_ctl_poll a few passes to accept + service the request —
     * nonblocking, so a couple of retries covers scheduling jitter
     * without ever sleeping. */
    bool quit = false;
    char got[256] = {0};
    for (int i = 0; i < 200 && got[0] == '\0'; i++) {
        quit = ff_ctl_poll(&srv, &h);
        ssize_t n = recv(client, got, sizeof(got) - 1, MSG_DONTWAIT);
        if (n > 0) got[n] = '\0';
    }

    TEST_ASSERT_FALSE(quit);
    TEST_ASSERT_NOT_NULL(strstr(got, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(got, "\"fixture\":\"e2e\""));
    TEST_ASSERT_EQUAL_INT(1, s.state_calls);

    close(client);
    ff_ctl_close(&srv);
}

/* --- Review fix (PR #19 finding #3): idle-connection eviction. ---
 * A tiny idle_timeout_ms (real time, not mocked — this module has no
 * injected clock, see ctl_server.c's ctl_now_ms) keeps this test fast
 * (well under a second) while still exercising the real code path: a
 * connected-but-silent client is dropped once idle_timeout_ms elapses,
 * and the listener accepts a replacement right after. */
static void S13c_ctl_socket_idle_client_is_evicted_and_replaced(void)
{
    spy_t s = {0};
    s.state_json_value = "{\"fixture\":\"after-evict\"}";
    ff_ctl_handlers_t h = spy_handlers(&s);

    ff_ctl_server_t srv;
    TEST_ASSERT_EQUAL_INT(0, ff_ctl_open(&srv, 0, 100)); /* 100ms idle timeout */
    uint16_t port = ctl_bound_port(&srv);

    int silent_client = connect_loopback(port);
    /* Never sends anything. Poll for well past idle_timeout_ms, giving
     * ff_ctl_poll every chance to evict it. */
    for (int i = 0; i < 400; i++) {
        (void)ff_ctl_poll(&srv, &h);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }

    /* The evicted client's socket should observe EOF (the far end closed
     * it) rather than staying silently open forever. */
    char buf[8];
    ssize_t n = recv(silent_client, buf, sizeof(buf), MSG_DONTWAIT);
    TEST_ASSERT_EQUAL_INT(0, (int)n); /* 0 = orderly close (EOF), not "no data yet" (-1/EAGAIN) */
    close(silent_client);

    /* A second, well-behaved client must now be able to connect and get
     * served — proving the slot was actually freed, not just the old
     * connection torn down with the harness stuck. */
    int client2 = connect_loopback(port);
    char const *req = "{\"cmd\":\"state\"}\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(req), (int)send(client2, req, strlen(req), 0));

    char got[256] = {0};
    for (int i = 0; i < 200 && got[0] == '\0'; i++) {
        (void)ff_ctl_poll(&srv, &h);
        ssize_t r = recv(client2, got, sizeof(got) - 1, MSG_DONTWAIT);
        if (r > 0) got[r] = '\0';
    }
    TEST_ASSERT_NOT_NULL(strstr(got, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(got, "after-evict"));

    close(client2);
    ff_ctl_close(&srv);
}

/* idle_timeout_ms == 0 means "disabled" — a silent client must NOT be
 * evicted (this is what every other test in this file relies on, since
 * they all pass 0; pinned explicitly here as its own guard-path test). */
static void S13c_ctl_socket_idle_timeout_zero_disables_eviction(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);

    ff_ctl_server_t srv;
    TEST_ASSERT_EQUAL_INT(0, ff_ctl_open(&srv, 0, 0)); /* disabled */
    uint16_t port = ctl_bound_port(&srv);

    int silent_client = connect_loopback(port);
    for (int i = 0; i < 150; i++) { /* well past what would be a 100ms timeout */
        (void)ff_ctl_poll(&srv, &h);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }

    char buf[8];
    ssize_t n = recv(silent_client, buf, sizeof(buf), MSG_DONTWAIT);
    /* Still open: -1/EAGAIN ("no data, but not closed"), not 0 (EOF). */
    TEST_ASSERT_TRUE(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    close(silent_client);
    ff_ctl_close(&srv);
}

static void S13c_ctl_socket_poll_quit_returns_true(void)
{
    spy_t s = {0};
    ff_ctl_handlers_t h = spy_handlers(&s);

    ff_ctl_server_t srv;
    TEST_ASSERT_EQUAL_INT(0, ff_ctl_open(&srv, 0, 0)); /* idle timeout disabled */
    uint16_t port = ctl_bound_port(&srv);
    int client = connect_loopback(port);

    char const *req = "{\"cmd\":\"quit\"}\n";
    TEST_ASSERT_EQUAL_INT((int)strlen(req), (int)send(client, req, strlen(req), 0));

    bool quit = false;
    for (int i = 0; i < 200 && !quit; i++) {
        quit = ff_ctl_poll(&srv, &h);
    }

    TEST_ASSERT_TRUE(quit);
    TEST_ASSERT_EQUAL_INT(1, s.quit_calls);

    close(client);
    ff_ctl_close(&srv);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(S13c_ctl_feed_byte_simple_line);
    RUN_TEST(S13c_ctl_feed_byte_strips_crlf);
    RUN_TEST(S13c_ctl_feed_byte_two_lines_in_sequence);
    RUN_TEST(S13c_ctl_feed_byte_exactly_max_line_minus_1_succeeds);
    RUN_TEST(S13c_ctl_feed_byte_exactly_max_line_content_rejected);
    RUN_TEST(S13c_ctl_feed_byte_resyncs_after_too_long_line);

    RUN_TEST(S13c_ctl_process_line_tap);
    RUN_TEST(S13c_ctl_process_line_tap_missing_y_is_error);
    RUN_TEST(S13c_ctl_process_line_tap_at_coord_max_accepted);
    RUN_TEST(S13c_ctl_process_line_tap_one_past_coord_max_is_error);
    RUN_TEST(S13c_ctl_process_line_tap_one_past_coord_min_is_error);
    RUN_TEST(S13c_ctl_process_line_tap_huge_value_is_error);
    RUN_TEST(S13c_ctl_process_line_tap_infinity_is_error);
    RUN_TEST(S13c_ctl_process_line_tap_unparseable_number_is_error);
    RUN_TEST(S13c_ctl_process_line_swipe_left_and_right);
    RUN_TEST(S13c_ctl_process_line_swipe_invalid_dir_is_error);
    RUN_TEST(S70_ctl_process_line_hold_explicit_ms);
    RUN_TEST(S70_ctl_process_line_hold_defaults_ms);
    RUN_TEST(S70_ctl_process_line_hold_missing_y_is_error);
    RUN_TEST(S70_ctl_process_line_hold_at_coord_max_accepted);
    RUN_TEST(S70_ctl_process_line_hold_one_past_coord_max_is_error);
    RUN_TEST(S70_ctl_process_line_hold_non_finite_x_is_error);
    RUN_TEST(S70_ctl_process_line_hold_ms_at_max_accepted);
    RUN_TEST(S70_ctl_process_line_hold_ms_one_past_max_is_error);
    RUN_TEST(S70_ctl_process_line_hold_negative_ms_is_error);
    RUN_TEST(S70_ctl_process_line_hold_non_numeric_ms_is_error);
    RUN_TEST(S70_ctl_process_line_hold_unsupported_callback_is_error);
    RUN_TEST(S13c_ctl_process_line_clock_advance);
    RUN_TEST(S13c_ctl_process_line_clock_negative_advance_is_error);
    RUN_TEST(S13c_ctl_process_line_clock_unavailable_without_mock_clock);
    RUN_TEST(S13c_ctl_process_line_state_embeds_json);
    RUN_TEST(S13c_ctl_process_line_state_unavailable_is_error);
    RUN_TEST(S13c_ctl_process_line_state_response_buffer_too_small_is_error);
    RUN_TEST(S13c_ctl_process_line_screenshot);
    RUN_TEST(S13c_ctl_process_line_screenshot_missing_path_is_error);
    RUN_TEST(S13c_ctl_process_line_screenshot_handler_failure_reports_err);
    RUN_TEST(S13c_ctl_process_line_screenshot_path_at_buffer_limit_succeeds);
    RUN_TEST(S13c_ctl_process_line_screenshot_path_over_buffer_limit_is_error);
    RUN_TEST(S13c_ctl_process_line_quit);
    RUN_TEST(S13c_ctl_process_line_unknown_cmd_is_error);
    RUN_TEST(S13c_ctl_process_line_missing_cmd_is_error);
    RUN_TEST(S13c_ctl_process_line_malformed_json_is_error);
    RUN_TEST(S13c_ctl_process_line_non_object_root_is_error);
    RUN_TEST(S13c_ctl_process_line_empty_line_is_error);
    RUN_TEST(S13c_ctl_process_line_null_handlers_is_error);
    RUN_TEST(S13c_ctl_process_line_unsupported_callback_is_error);

    RUN_TEST(S13c_ctl_socket_binds_loopback_only);
    RUN_TEST(S13c_ctl_socket_poll_end_to_end);
    RUN_TEST(S13c_ctl_socket_idle_client_is_evicted_and_replaced);
    RUN_TEST(S13c_ctl_socket_idle_timeout_zero_disables_eviction);
    RUN_TEST(S13c_ctl_socket_poll_quit_returns_true);

    return UNITY_END();
}
