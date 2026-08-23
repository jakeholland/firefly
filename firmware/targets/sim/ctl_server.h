/**
 * ctl_server.h — S13 slice c: ffsim's line-oriented JSON control socket.
 *
 * `ffsim --ctl PORT` opens this: a localhost-only (127.0.0.1), POSIX-only,
 * newline-delimited JSON command/response socket the Python e2e harness
 * (firmware/tests/e2e/) and firmware/tools/dev/crew_sim.py drive. Wire
 * protocol reference: firmware/tools/dev/CTL.md.
 *
 * Design: this header splits into two layers on purpose.
 *   1. A pure, socket-free core (`ff_ctl_feed_byte` / `ff_ctl_process_line`)
 *      that does all the actual parsing/dispatch/bounds-checking and is
 *      unit-testable with plain byte arrays — no lv_init(), no fd, no
 *      threads. This is where "bound every read, reject oversized lines,
 *      handle malformed JSON" is proven (see tests/test_ctl_server.c).
 *   2. A thin POSIX socket driver (`ff_ctl_open`/`ff_ctl_poll`/
 *      `ff_ctl_close`) that just moves bytes through layer 1. main.c's
 *      event loop calls `ff_ctl_poll()` once per frame; it never blocks.
 *
 * Dependency-free per spec ("Keep it dependency-free (POSIX sockets)"):
 * only libc + POSIX sockets + the already-vendored jsmn parser (same one
 * fixture.c uses) — no new third-party dependency added for this.
 */
#ifndef FF_CTL_SERVER_H
#define FF_CTL_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard bound on one input line, INCLUDING the trailing '\n' — "bound
 * every read, reject oversized lines" (task brief). Every real command
 * (tap/swipe/clock/state/screenshot/quit) fits in a small fraction of
 * this; 4 KiB leaves generous headroom for a long --screenshot path
 * while still bounding worst-case per-connection memory to a fixed,
 * small, stack/struct-sized amount (ff_ctl_linebuf_t.buf below). */
#define FF_CTL_MAX_LINE 4096u

/* Response buffer budget. The largest response is `{"cmd":"state"}`'s
 * dump (see fixture.h's FF_FIXTURE_DUMP_MAX) plus a small JSON wrapper
 * (`{"ok":true,"state":...}`) — sized with headroom above that. */
#define FF_CTL_MAX_RESP (10u * 1024u)

/** Handler vtable: what a "tap"/"swipe"/"clock"/"state"/"screenshot"/
 * "quit" command actually *does*, injected by the caller (main.c) so this
 * module has zero LVGL/PNG/state-struct dependencies of its own — same
 * "interfaces first" convention as mc_transport_t / ff_clock_t
 * (docs/ARCHITECTURE.md principle 2). Every callback receives `user`
 * (opaque, set by the caller) as its first argument. */
typedef struct {
    void *user;

    /** Inject a tap/click at (x, y) (screen pixels; not range-checked
     * here — LVGL's own indev handling is the authority on out-of-bounds
     * coordinates). */
    void (*tap)(void *user, double x, double y);

    /** Inject a swipe gesture. `dir` is exactly "left" or "right"
     * (ff_ctl_process_line has already validated this before calling). */
    void (*swipe)(void *user, char const *dir);

    /** Advance the mock clock by `advance_ms`. Returns false (and sets
     * `*err` to a short static reason string) if clock control isn't
     * available right now (e.g. ffsim wasn't started with --mock-clock) —
     * a guard path, not a crash: the ctl connection stays open and the
     * caller sees a normal `{"ok":false,...}` response. */
    bool (*clock_advance)(void *user, uint32_t advance_ms, char const **err);

    /** Write the current app state as fixture-schema JSON (a single JSON
     * object, no trailing newline) into `buf` (capacity `buf_sz`,
     * NUL-terminated on success). Returns the byte length written
     * (excluding the NUL) on success, or a negative value if it doesn't
     * fit / isn't available. */
    int (*state_json)(void *user, char *buf, size_t buf_sz);

    /** Render a screenshot to `path`. Returns true on success; on
     * failure sets `*err` to a short static reason string. */
    bool (*screenshot)(void *user, char const *path, char const **err);

    /** Called once, after "quit" is validated and just before the
     * `{"ok":true}` response is sent. May be NULL if the caller has
     * nothing to do here (ff_ctl_poll's caller learns about quit from its
     * own return value regardless). */
    void (*quit)(void *user);
} ff_ctl_handlers_t;

/* ------------------------------------------------------------------- */
/* Layer 1: pure line framing + command dispatch (no sockets).         */
/* ------------------------------------------------------------------- */

/** Per-connection line-framing state. Zero-initialize (or assign `= {0}`)
 * before first use; safe on the stack or embedded in a larger struct. */
typedef struct {
    char   buf[FF_CTL_MAX_LINE];
    size_t len;
    bool   resyncing; /* true: discarding bytes after an oversized line,
                          up to and including the next '\n', before
                          resuming normal line accumulation */
} ff_ctl_linebuf_t;

typedef enum {
    FF_CTL_FEED_NEED_MORE, /* no complete line yet; keep feeding bytes */
    FF_CTL_FEED_LINE,      /* lb->buf[0..lb->len) is one complete,
                               NUL-terminated, in-bound line (trailing
                               '\r'/'\n' already stripped); lb is reset
                               and ready for the next line */
    FF_CTL_FEED_TOO_LONG,  /* this byte completed (or extended) a line
                               that exceeds FF_CTL_MAX_LINE-1 bytes without
                               a newline; lb is now resyncing (see above) —
                               the caller should send an error response
                               once per FF_CTL_FEED_TOO_LONG and otherwise
                               just keep feeding bytes normally */
} ff_ctl_feed_result_t;

/** Feed one byte into `lb`. Pure and allocation-free — the whole "bound
 * every read, reject oversized lines" contract lives here, independent of
 * where the bytes came from (a real socket in ff_ctl_poll, or a test's
 * plain byte array). */
ff_ctl_feed_result_t ff_ctl_feed_byte(ff_ctl_linebuf_t *lb, char byte);

/**
 * ff_ctl_process_line — parse one JSON command line (`line`, NUL-
 * terminated, already known to be within FF_CTL_MAX_LINE) and dispatch to
 * `h`, writing exactly one JSON response (NUL-terminated, no trailing
 * newline) into `resp` (capacity `resp_sz`).
 *
 * `line` is treated as untrusted socket input, not developer-authored
 * fixture data: malformed JSON, a non-object root, a missing/unknown
 * `cmd`, or a command missing/mistyping its required fields all produce a
 * `{"ok":false,"error":"..."}` response — never a crash, and `line` is
 * never read past its NUL terminator. Individual callback pointers in `h`
 * may be NULL (answers `{"ok":false,"error":"... unsupported"}` for just
 * that command); NULL `h` itself always answers
 * `{"ok":false,"error":"no handlers"}` without touching it.
 *
 * Returns true iff `line` was a successfully-validated `"quit"` command
 * (h->quit was called and the response is the ok reply) — the caller
 * (ff_ctl_poll, or a test driving this directly) uses this to know when
 * to stop. Every other case, success or failure, returns false.
 */
bool ff_ctl_process_line(char const *line, ff_ctl_handlers_t const *h, char *resp, size_t resp_sz);

/* ------------------------------------------------------------------- */
/* Layer 2: the actual POSIX TCP server driving layer 1.               */
/* ------------------------------------------------------------------- */

/** Treat every field as private (same convention as mc_client_t) — only
 * ff_ctl_open/ff_ctl_poll/ff_ctl_close touch these. */
typedef struct {
    int listen_fd; /* -1 = not open */
    int client_fd; /* -1 = no client currently connected */
    ff_ctl_linebuf_t lb;
} ff_ctl_server_t;

/**
 * ff_ctl_open — opens a listening socket bound to 127.0.0.1:`port` (IPv4
 * loopback only — "localhost only" per spec: never binds INADDR_ANY),
 * nonblocking, `SO_REUSEADDR`. Returns 0 on success, negative on failure
 * (socket/bind/listen error). `*srv` is fully initialized either way
 * (listen_fd == -1 on failure).
 */
int ff_ctl_open(ff_ctl_server_t *srv, uint16_t port);

/** Closes the listening socket and any connected client. Safe to call on
 * an already-closed/never-opened `*srv`. */
void ff_ctl_close(ff_ctl_server_t *srv);

/**
 * ff_ctl_poll — nonblocking, safe to call every frame from ffsim's main
 * loop (never sleeps/blocks on I/O). Accepts one new client if none is
 * currently connected (dropping any additional simultaneous connection
 * attempts — this is a single-client dev tool, not a multiplexed
 * server); if a client is connected, drains whatever bytes are currently
 * available and services every complete command line found in them.
 *
 * A client that disconnects (EOF/reset) is dropped silently; ff_ctl_poll
 * goes back to accepting a new one on the next call — no special
 * handling needed by the caller.
 *
 * Returns true the moment a `"quit"` command has been fully processed
 * (response already sent) — the caller should stop calling ff_ctl_poll
 * and shut down. Returns false otherwise, including "no client connected
 * right now".
 */
bool ff_ctl_poll(ff_ctl_server_t *srv, ff_ctl_handlers_t const *h);

#ifdef __cplusplus
}
#endif

#endif /* FF_CTL_SERVER_H */
