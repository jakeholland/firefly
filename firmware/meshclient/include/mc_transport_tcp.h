/**
 * mc_transport_tcp.h — POSIX TCP transport (sim target: connects to
 * meshtasticd). See docs/specs/S03-meshclient.md slice e.
 *
 * The UART transport (esp32s3, comms brain) lands with S15; this is the
 * dev/sim stand-in, same role, different wire.
 *
 * ---------------------------------------------------------------------
 * RE-DIAL (S16 slice e — docs/specs/S16-app-shell.md, "Two defects this
 * closes" is silent on this one; recorded in the slice e brief instead,
 * flagged in PR #56's review)
 * ---------------------------------------------------------------------
 * `mc_client_t` auto-reconnects: on any transport error it schedules a
 * retry (`mc_fail_and_schedule_reconnect`) and, once the backoff elapses,
 * tries the handshake again (`mc_begin_handshake`), which is just another
 * `transport.write()` call. Before this slice, that write went to the
 * SAME fd the drop broke — `mc_tcp_t` remembered nothing about how it got
 * connected, so there was no way to open a fresh socket, and a force-
 * closed connection could never be re-established: `mc_client`'s own
 * retry loop ran forever against a dead file descriptor.
 *
 * Fixed here, in the transport, not in `mc_client.c`: `mc_tcp_open` now
 * remembers `host`/`port`, and a broken connection (`read()` seeing EOF
 * or an error, or `write()` failing) drops the dead fd and marks the
 * target "needs a fresh dial" rather than reporting an error on every
 * subsequent call forever. `write()` is where the actual re-dial happens
 * — it is only ever called by `mc_client` at the moments it is genuinely
 * trying to (re)establish something (`mc_begin_handshake` on its own 2 s
 * backoff, `mc_send_heartbeat`, or an app-level send that already checked
 * `MC_STATE_READY`), so redial attempts are naturally rate-limited by
 * `mc_client`'s own schedule rather than needing a second timer here.
 *
 * `read()`, once the fd is down, reports "nothing available" (0) rather
 * than repeating the error (-1). That distinction is load-bearing: before
 * this slice, `read()` returning -1 on EVERY tick after a drop (the fd
 * never became un-broken on its own) reset `mc_client`'s
 * `reconnect_at_ms` back out another 2 s on every single call — so the
 * backoff window could never actually elapse and `mc_begin_handshake`
 * was never reached at all. Reporting the error exactly once, then
 * "nothing available", lets the schedule run.
 */
#ifndef MC_TRANSPORT_TCP_H
#define MC_TRANSPORT_TCP_H

#include <stdbool.h>
#include <stdint.h>

#include "mc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Matches live_setup.c's own host buffer budget (`char host[256]`) — the
 * longest identifier this transport is ever asked to redial against. */
#define MC_TCP_HOST_MAX 256u

typedef struct {
    int fd; /* -1 = not connected */

    /* Re-dial target, captured by mc_tcp_open (S16 slice e). `have_target`
     * is false until an mc_tcp_open() call succeeds, and is cleared again
     * by mc_tcp_close() — an explicit close is the caller taking the
     * transport away deliberately, not a drop to recover from, so a
     * later stray read()/write() must not silently reopen a socket the
     * caller believes it closed. */
    char host[MC_TCP_HOST_MAX];
    uint16_t port;
    bool have_target;
} mc_tcp_t;

/**
 * Open a TCP connection to host:port (blocking connect — this call
 * returns once connected or once it fails; the socket itself is left
 * nonblocking for subsequent read()/write() calls). Returns 0 on success,
 * negative on failure (bad host, connect refused, etc).
 *
 * Also records `host`/`port` (truncated to MC_TCP_HOST_MAX - 1 bytes) so
 * a later drop can be redialed — see this header's top comment. `host`
 * itself need not outlive the call; it is copied.
 */
int mc_tcp_open(mc_tcp_t *t, char const *host, uint16_t port);

/** Close the socket, if open, and forget the re-dial target — a later
 *  read()/write() will not silently reopen it. Safe to call on an
 *  already-closed mc_tcp_t. */
void mc_tcp_close(mc_tcp_t *t);

/**
 * Build an mc_transport_t backed by `t`. `t` must outlive the transport
 * (and must already be open — call mc_tcp_open() first).
 *
 * The returned `write()` transparently re-dials `t`'s recorded host/port
 * if the connection has dropped (S16 slice e); `read()` reports the drop
 * exactly once, then "nothing available" until a redial (via `write()`)
 * succeeds — never a repeating error. See this header's top comment.
 */
mc_transport_t mc_tcp_transport(mc_tcp_t *t);

#ifdef __cplusplus
}
#endif

#endif /* MC_TRANSPORT_TCP_H */
