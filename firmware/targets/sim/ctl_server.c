/**
 * ctl_server.c — S13 slice c: ffsim's control socket. See ctl_server.h.
 */
#include "ctl_server.h"

#define JSMN_STATIC
#include "jsmn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* -----------------------------------------------------------------------
 * Layer 1a: bounded line framing.
 * --------------------------------------------------------------------- */

ff_ctl_feed_result_t ff_ctl_feed_byte(ff_ctl_linebuf_t *lb, char byte)
{
    if (lb->resyncing) {
        if (byte == '\n') lb->resyncing = false;
        return FF_CTL_FEED_NEED_MORE;
    }

    if (byte == '\n') {
        size_t n = lb->len;
        if (n > 0 && lb->buf[n - 1] == '\r') n--; /* tolerate CRLF clients */
        lb->buf[n] = '\0';
        lb->len = 0; /* reset for the next line; caller reads lb->buf
                        (via strlen) before feeding another byte */
        return FF_CTL_FEED_LINE;
    }

    /* `- 1` reserves room for the NUL this function (or the '\n' branch
     * above) always writes at lb->buf[lb->len] — see ctl_server.h's
     * FF_CTL_MAX_LINE doc comment for the exact boundary this enforces:
     * a line whose content is FF_CTL_MAX_LINE-1 bytes (plus the '\n')
     * fits; one byte more does not. */
    if (lb->len >= FF_CTL_MAX_LINE - 1) {
        lb->len = 0;
        lb->resyncing = true;
        return FF_CTL_FEED_TOO_LONG;
    }

    lb->buf[lb->len++] = byte;
    return FF_CTL_FEED_NEED_MORE;
}

/* -----------------------------------------------------------------------
 * Layer 1b: minimal jsmn-based command parsing.
 *
 * Deliberately duplicated rather than shared with targets/sim/fixture.c's
 * fx_* helpers — same "duplicated here, not shared" call fixture.c's own
 * header comment documents (this module has a stricter, smaller job:
 * flat top-level command fields only, and it must stay free of any
 * fixture.h/ff_app_state.h dependency — see ctl_server.h's design note).
 * --------------------------------------------------------------------- */

#define CTL_MAX_TOKENS 32

typedef struct {
    char const *js;
    jsmntok_t const *toks;
    int ntoks;
} ctl_ctx_t;

static int ctl_skip(ctl_ctx_t const *c, int i)
{
    if (i < 0 || i >= c->ntoks) return c->ntoks;
    jsmntok_t const *t = &c->toks[i];
    int next = i + 1;
    if (t->type == JSMN_OBJECT) {
        for (int k = 0; k < t->size; k++) {
            next = ctl_skip(c, next); /* key */
            next = ctl_skip(c, next); /* value */
        }
    } else if (t->type == JSMN_ARRAY) {
        for (int k = 0; k < t->size; k++) {
            next = ctl_skip(c, next);
        }
    }
    return next;
}

static bool ctl_obj_get(ctl_ctx_t const *c, int obj_i, char const *key, int *val_i)
{
    if (obj_i < 0 || obj_i >= c->ntoks) return false;
    jsmntok_t const *t = &c->toks[obj_i];
    if (t->type != JSMN_OBJECT) return false;
    size_t keylen = strlen(key);
    int i = obj_i + 1;
    for (int k = 0; k < t->size; k++) {
        int key_i = i;
        if (key_i < 0 || key_i >= c->ntoks) return false;
        jsmntok_t const *kt = &c->toks[key_i];
        int val_index = key_i + 1;
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == keylen &&
            memcmp(c->js + kt->start, key, keylen) == 0) {
            *val_i = val_index;
            return true;
        }
        i = ctl_skip(c, val_index);
    }
    return false;
}

/* Copies a JSMN_STRING token's raw bytes into dst (NUL-terminated,
 * truncated to fit). Returns the token's true byte length (which may
 * exceed dst_sz-1 if truncation occurred) so callers that must not
 * silently truncate (screenshot's `path`) can detect and reject that. */
static size_t ctl_copy_str(ctl_ctx_t const *c, int i, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    if (i < 0 || i >= c->ntoks || dst_sz == 0) return 0;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_STRING) return 0;
    size_t n = (size_t)(t->end - t->start);
    size_t copy_n = (n >= dst_sz) ? dst_sz - 1 : n;
    memcpy(dst, c->js + t->start, copy_n);
    dst[copy_n] = '\0';
    return n;
}

static double ctl_num(ctl_ctx_t const *c, int i, double dflt)
{
    if (i < 0 || i >= c->ntoks) return dflt;
    jsmntok_t const *t = &c->toks[i];
    if (t->type != JSMN_PRIMITIVE) return dflt;
    int n = t->end - t->start;
    char buf[32];
    if (n <= 0 || (size_t)n >= sizeof(buf)) return dflt;
    memcpy(buf, c->js + t->start, (size_t)n);
    buf[n] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return dflt;
    return v;
}

static void ctl_ok(char *resp, size_t resp_sz)
{
    (void)snprintf(resp, resp_sz, "{\"ok\":true}");
}

static void ctl_err(char *resp, size_t resp_sz, char const *msg)
{
    /* Every message passed here is a static string literal we control
     * (never client input), so no JSON escaping is needed — PROVIDED
     * `msg` itself never contains a literal '"' or '\\'. This has bitten
     * this file once already (a "left"/"right" error message with
     * embedded quotes corrupted its own JSON response; found and fixed
     * alongside PR #19's review-fixup round, along with the same bug in
     * ctl_out_path.c). Every `ctl_err(...)` call site's literal must
     * stay quote-free — there is deliberately no runtime escaping here,
     * matching this whole module's "every callback pointer/string is
     * either developer-authored or explicitly validated, never both
     * assumed and unescaped" discipline. */
    (void)snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"%s\"}", msg);
}

bool ff_ctl_process_line(char const *line, ff_ctl_handlers_t const *h, char *resp, size_t resp_sz)
{
    if (resp == NULL || resp_sz == 0) return false;
    resp[0] = '\0';

    if (h == NULL) {
        ctl_err(resp, resp_sz, "no handlers");
        return false;
    }
    if (line == NULL || line[0] == '\0') {
        ctl_err(resp, resp_sz, "empty line");
        return false;
    }

    size_t len = strlen(line);
    /* Defense in depth: ff_ctl_feed_byte already guarantees any line
     * reaching here is < FF_CTL_MAX_LINE, but this function is also
     * called directly by tests/other callers, so it re-checks its own
     * documented bound rather than trusting the caller. */
    if (len >= FF_CTL_MAX_LINE) {
        ctl_err(resp, resp_sz, "line too long");
        return false;
    }

    jsmntok_t toks[CTL_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, line, len, toks, CTL_MAX_TOKENS);
    if (r <= 0 || toks[0].type != JSMN_OBJECT) {
        ctl_err(resp, resp_sz, "malformed json");
        return false;
    }
    ctl_ctx_t ctx = {line, toks, r};

    int cmd_i;
    if (!ctl_obj_get(&ctx, 0, "cmd", &cmd_i) || toks[cmd_i].type != JSMN_STRING) {
        ctl_err(resp, resp_sz, "missing cmd");
        return false;
    }
    char cmd[32];
    ctl_copy_str(&ctx, cmd_i, cmd, sizeof(cmd));

    if (strcmp(cmd, "tap") == 0) {
        int xi, yi;
        if (!ctl_obj_get(&ctx, 0, "x", &xi) || !ctl_obj_get(&ctx, 0, "y", &yi) ||
            toks[xi].type != JSMN_PRIMITIVE || toks[yi].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "tap requires numeric x,y");
            return false;
        }
        /* NAN as ctl_num()'s "missing/unparseable" sentinel would itself
         * fail isfinite() below and get rejected the same as any other
         * out-of-range value — no separate "couldn't parse the number"
         * case needed. */
        double x = ctl_num(&ctx, xi, NAN);
        double y = ctl_num(&ctx, yi, NAN);
        /* Review fix (PR #19 finding #1): a bare (lv_coord_t)x cast of an
         * out-of-range or non-finite double is undefined behavior per
         * C11 6.3.1.4 — reproduced with {"cmd":"tap","x":1e300,"y":0}
         * under -fsanitize=undefined. Validate BEFORE this ever reaches
         * a narrowing cast in the handler (main.c's ff_loop_tap), not
         * after — same "fail loud instead of silently misbehaving"
         * discipline screenshot's path-length check already uses. */
        if (!isfinite(x) || !isfinite(y) || x < FF_CTL_TAP_COORD_MIN || x > FF_CTL_TAP_COORD_MAX ||
            y < FF_CTL_TAP_COORD_MIN || y > FF_CTL_TAP_COORD_MAX) {
            ctl_err(resp, resp_sz, "tap x,y must be finite and within [-32768, 32767]");
            return false;
        }
        if (h->tap == NULL) {
            ctl_err(resp, resp_sz, "tap unsupported");
            return false;
        }
        h->tap(h->user, x, y);
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "swipe") == 0) {
        int di;
        if (!ctl_obj_get(&ctx, 0, "dir", &di) || toks[di].type != JSMN_STRING) {
            ctl_err(resp, resp_sz, "swipe requires dir");
            return false;
        }
        char dir[8];
        ctl_copy_str(&ctx, di, dir, sizeof(dir));
        if (strcmp(dir, "left") != 0 && strcmp(dir, "right") != 0) {
            /* No embedded '"' — see ctl_err()'s doc comment: it does not
             * escape the messages it's given, so a literal quote here
             * would corrupt the JSON response (found and fixed alongside
             * PR #19's review-fixup round; ctl_out_path.c's error
             * strings had the same bug). */
            ctl_err(resp, resp_sz, "swipe dir must be left or right");
            return false;
        }
        if (h->swipe == NULL) {
            ctl_err(resp, resp_sz, "swipe unsupported");
            return false;
        }
        h->swipe(h->user, dir);
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "hold") == 0) {
        int xi, yi;
        if (!ctl_obj_get(&ctx, 0, "x", &xi) || !ctl_obj_get(&ctx, 0, "y", &yi) ||
            toks[xi].type != JSMN_PRIMITIVE || toks[yi].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "hold requires numeric x,y");
            return false;
        }
        /* Same NAN-as-sentinel / finite-range discipline as "tap" above —
         * see that handler's comment for the full reasoning. */
        double x = ctl_num(&ctx, xi, NAN);
        double y = ctl_num(&ctx, yi, NAN);
        if (!isfinite(x) || !isfinite(y) || x < FF_CTL_TAP_COORD_MIN || x > FF_CTL_TAP_COORD_MAX ||
            y < FF_CTL_TAP_COORD_MIN || y > FF_CTL_TAP_COORD_MAX) {
            ctl_err(resp, resp_sz, "hold x,y must be finite and within [-32768, 32767]");
            return false;
        }

        double ms = (double)FF_CTL_HOLD_DEFAULT_MS;
        int mi;
        if (ctl_obj_get(&ctx, 0, "ms", &mi)) {
            if (toks[mi].type != JSMN_PRIMITIVE) {
                ctl_err(resp, resp_sz, "hold ms must be numeric");
                return false;
            }
            ms = ctl_num(&ctx, mi, NAN);
            if (!isfinite(ms) || ms < 0.0 || ms > (double)FF_CTL_HOLD_MS_MAX) {
                ctl_err(resp, resp_sz, "hold ms must be within [0, 65535]");
                return false;
            }
        }

        if (h->hold == NULL) {
            ctl_err(resp, resp_sz, "hold unsupported");
            return false;
        }
        h->hold(h->user, x, y, (uint32_t)ms);
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "clock") == 0) {
        int ai;
        if (!ctl_obj_get(&ctx, 0, "advance_ms", &ai) || toks[ai].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "clock requires numeric advance_ms");
            return false;
        }
        double ms = ctl_num(&ctx, ai, -1.0);
        if (ms < 0.0) {
            ctl_err(resp, resp_sz, "advance_ms must be >= 0");
            return false;
        }
        if (h->clock_advance == NULL) {
            ctl_err(resp, resp_sz, "clock unsupported");
            return false;
        }
        char const *err = NULL;
        if (!h->clock_advance(h->user, (uint32_t)ms, &err)) {
            ctl_err(resp, resp_sz, (err != NULL) ? err : "clock advance failed");
            return false;
        }
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "state") == 0) {
        if (h->state_json == NULL) {
            ctl_err(resp, resp_sz, "state unsupported");
            return false;
        }
        /* Reserve room for the `{"ok":true,"state":...}` wrapper below. */
        char state_buf[FF_CTL_MAX_RESP > 128 ? FF_CTL_MAX_RESP - 64 : 64];
        int n = h->state_json(h->user, state_buf, sizeof(state_buf));
        if (n < 0) {
            ctl_err(resp, resp_sz, "state unavailable");
            return false;
        }
        int written = snprintf(resp, resp_sz, "{\"ok\":true,\"state\":%s}", state_buf);
        if (written < 0 || (size_t)written >= resp_sz) {
            ctl_err(resp, resp_sz, "state too large for response buffer");
        }
        return false;
    }

    if (strcmp(cmd, "screenshot") == 0) {
        int pi;
        if (!ctl_obj_get(&ctx, 0, "path", &pi) || toks[pi].type != JSMN_STRING) {
            ctl_err(resp, resp_sz, "screenshot requires path");
            return false;
        }
        char path[4000];
        size_t true_len = ctl_copy_str(&ctx, pi, path, sizeof(path));
        if (true_len == 0) {
            ctl_err(resp, resp_sz, "screenshot path must be non-empty");
            return false;
        }
        if (true_len >= sizeof(path)) {
            /* Never silently write to a truncated path — fail loud
             * instead (CLAUDE.md: honest data over pretty data). */
            ctl_err(resp, resp_sz, "screenshot path too long");
            return false;
        }
        if (h->screenshot == NULL) {
            ctl_err(resp, resp_sz, "screenshot unsupported");
            return false;
        }
        char const *err = NULL;
        if (!h->screenshot(h->user, path, &err)) {
            ctl_err(resp, resp_sz, (err != NULL) ? err : "screenshot failed");
            return false;
        }
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "flare") == 0) {
        int fi;
        if (!ctl_obj_get(&ctx, 0, "from", &fi) || toks[fi].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "flare requires numeric from");
            return false;
        }
        /* NAN as ctl_num()'s "missing/unparseable" sentinel fails the
         * range check below the same way any other bad value does — see
         * the "tap" handler's identical convention above. */
        double from = ctl_num(&ctx, fi, NAN);
        if (!isfinite(from) || from < 0.0 || from > 4294967295.0 /* UINT32_MAX */) {
            ctl_err(resp, resp_sz, "flare from must be a valid node id (0..4294967295)");
            return false;
        }

        double dur_s = 300.0; /* FF_FLARE_DEFAULT_DUR_S — this module deliberately has no
                                * core dependency (see this header's design note), so the
                                * default is restated as a literal rather than #included. */
        int di;
        if (ctl_obj_get(&ctx, 0, "dur_s", &di)) {
            if (toks[di].type != JSMN_PRIMITIVE) {
                ctl_err(resp, resp_sz, "flare dur_s must be numeric");
                return false;
            }
            dur_s = ctl_num(&ctx, di, NAN);
            if (!isfinite(dur_s) || dur_s < 0.0 || dur_s > 65535.0 /* UINT16_MAX */) {
                ctl_err(resp, resp_sz, "flare dur_s must be within [0, 65535]");
                return false;
            }
        }

        if (h->flare == NULL) {
            ctl_err(resp, resp_sz, "flare unsupported");
            return false;
        }
        char const *err = NULL;
        if (!h->flare(h->user, (uint32_t)from, (uint16_t)dur_s, &err)) {
            ctl_err(resp, resp_sz, (err != NULL) ? err : "flare failed");
            return false;
        }
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "wall") == 0) {
        int ui;
        if (!ctl_obj_get(&ctx, 0, "unix_s", &ui) || toks[ui].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "wall requires numeric unix_s");
            return false;
        }
        double unix_s = ctl_num(&ctx, ui, NAN);
        /* Bound to the same era ff_wall's own plausibility window can ever
         * accept (a doubled margin around it, so the gate itself — not this
         * parse — is what rejects an implausible-but-representable time and
         * the reply can say so). 4e9 ~ year 2096. */
        if (!isfinite(unix_s) || unix_s < 0.0 || unix_s > 4.0e9) {
            ctl_err(resp, resp_sz, "wall unix_s must be within [0, 4e9]");
            return false;
        }
        if (h->wall == NULL) {
            ctl_err(resp, resp_sz, "wall unsupported");
            return false;
        }
        char const *err = NULL;
        if (!h->wall(h->user, (int64_t)unix_s, &err)) {
            ctl_err(resp, resp_sz, (err != NULL) ? err : "wall failed");
            return false;
        }
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "batt_mv") == 0) {
        int mi;
        if (!ctl_obj_get(&ctx, 0, "mv", &mi) || toks[mi].type != JSMN_PRIMITIVE) {
            ctl_err(resp, resp_sz, "batt_mv requires numeric mv");
            return false;
        }
        /* Same NAN-as-sentinel / finite-range discipline as "tap"/"hold"
         * above. Upper bound is UINT16_MAX (pack_mv's wire type,
         * ff_shell_set_batt_mv) — this layer only bounds what's safe to
         * narrow; ff_batt.h's own plausibility window (2500..4600) is
         * what decides whether a value in range is an honest reading. */
        double mv = ctl_num(&ctx, mi, NAN);
        if (!isfinite(mv) || mv < 0.0 || mv > 65535.0 /* UINT16_MAX */) {
            ctl_err(resp, resp_sz, "batt_mv mv must be within [0, 65535]");
            return false;
        }
        if (h->batt_mv == NULL) {
            ctl_err(resp, resp_sz, "batt_mv unsupported");
            return false;
        }
        h->batt_mv(h->user, (uint16_t)mv);
        ctl_ok(resp, resp_sz);
        return false;
    }

    if (strcmp(cmd, "quit") == 0) {
        if (h->quit != NULL) h->quit(h->user);
        ctl_ok(resp, resp_sz);
        return true;
    }

    ctl_err(resp, resp_sz, "unknown cmd");
    return false;
}

/* -----------------------------------------------------------------------
 * Layer 2: the POSIX TCP driver.
 * --------------------------------------------------------------------- */

/* Monotonic milliseconds — self-contained (no injected ff_clock_t; this
 * is the socket-facing layer, not the pure core, and every other timing
 * source in this file, e.g. ctl_send_line's select() timeout, already
 * reads real time directly). Mirrors main.c's own ff_wall_clock_ms(). */
static uint32_t ctl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000));
}

int ff_ctl_open(ff_ctl_server_t *srv, uint16_t port, uint32_t idle_timeout_ms)
{
    if (srv == NULL) return -1;
    srv->listen_fd = -1;
    srv->client_fd = -1;
    memset(&srv->lb, 0, sizeof(srv->lb));
    srv->idle_timeout_ms = idle_timeout_ms;
    srv->client_last_activity_ms = 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only, per spec */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }

    srv->listen_fd = fd;
    return 0;
}

void ff_ctl_close(ff_ctl_server_t *srv)
{
    if (srv == NULL) return;
    if (srv->client_fd >= 0) {
        close(srv->client_fd);
        srv->client_fd = -1;
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}

static void ctl_accept_if_needed(ff_ctl_server_t *srv)
{
    if (srv->client_fd >= 0) return;

    int fd = accept(srv->listen_fd, NULL, NULL);
    if (fd < 0) return; /* EAGAIN, or a transient accept() error: try again next poll */

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return;
    }

    /* Single-client dev tool: a second simultaneous connection attempt
     * would queue in the listen backlog (size 1) and get refused by the
     * kernel once that fills, rather than being served — documented
     * behavior, not a bug (see ctl_server.h's ff_ctl_poll doc comment). */
    srv->client_fd = fd;
    memset(&srv->lb, 0, sizeof(srv->lb));
    srv->client_last_activity_ms = ctl_now_ms();
}

static void ctl_send_line(int fd, char const *resp)
{
    char buf[FF_CTL_MAX_RESP + 2];
    int n = snprintf(buf, sizeof(buf), "%s\n", resp);
    if (n < 0) return;
    size_t total = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

    size_t sent = 0;
    int retries_left = 200; /* mirrors mc_transport_tcp.c's bounded EAGAIN retry */
    while (sent < total) {
        ssize_t w = send(fd, buf + sent, total - sent, 0);
        if (w > 0) {
            sent += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (retries_left-- <= 0) return;
            struct timeval tv = {.tv_sec = 0, .tv_usec = 2000};
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            (void)select(fd + 1, NULL, &wfds, NULL, &tv);
            continue;
        }
        return; /* connection error — caller's next poll will notice via recv() */
    }
}

bool ff_ctl_poll(ff_ctl_server_t *srv, ff_ctl_handlers_t const *h)
{
    if (srv == NULL || srv->listen_fd < 0) return false;

    ctl_accept_if_needed(srv);
    if (srv->client_fd < 0) return false;

    /* Review fix (PR #19 finding #3): a connected-but-silent client
     * would otherwise hold the single client slot (listen backlog 1)
     * forever, starving any well-behaved client (the e2e/nightly
     * harness) that tries to connect next, with no recovery path. Drop
     * it and let the very next ff_ctl_poll call accept a replacement. */
    if (srv->idle_timeout_ms > 0) {
        uint32_t now = ctl_now_ms();
        if (now - srv->client_last_activity_ms > srv->idle_timeout_ms) {
            close(srv->client_fd);
            srv->client_fd = -1;
            return false;
        }
    }

    uint8_t chunk[512];
    ssize_t n = recv(srv->client_fd, chunk, sizeof(chunk), 0);
    if (n == 0) {
        close(srv->client_fd);
        srv->client_fd = -1;
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        close(srv->client_fd);
        srv->client_fd = -1;
        return false;
    }

    bool quit = false;
    for (ssize_t i = 0; i < n; i++) {
        ff_ctl_feed_result_t fr = ff_ctl_feed_byte(&srv->lb, (char)chunk[i]);
        if (fr == FF_CTL_FEED_TOO_LONG) {
            ctl_send_line(srv->client_fd, "{\"ok\":false,\"error\":\"line too long\"}");
            continue;
        }
        if (fr != FF_CTL_FEED_LINE) continue;

        /* Refresh the idle deadline only on a FULLY-PROCESSED command
         * line, not on every raw byte or an oversized-line event — a
         * client dribbling in bytes of an oversized line forever would
         * otherwise reset the clock indefinitely without ever completing
         * a real command. */
        srv->client_last_activity_ms = ctl_now_ms();

        char resp[FF_CTL_MAX_RESP];
        bool this_is_quit = ff_ctl_process_line(srv->lb.buf, h, resp, sizeof(resp));
        ctl_send_line(srv->client_fd, resp);
        if (this_is_quit) {
            quit = true;
            break;
        }
    }
    return quit;
}
