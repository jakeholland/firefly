#include "mc_transport_tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int mc_tcp_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Bound on how long ONE candidate address is given to complete its
 * TCP handshake (review round 1, PR #61, D1). `mc_tcp_dial` is called
 * from `mc_tcp_redial`, which `mc_tcp_write_cb` calls from inside
 * `mc_tick()`'s synchronous tick loop, roughly every 2 s while
 * disconnected (`mc_client`'s own reconnect backoff) — a connect() that
 * blocks for the OS's own timeout (60-75 s, measured >8 s before this
 * fix against an unreachable-but-not-refused host) freezes the render
 * loop on every single retry. A refused connection (ECONNREFUSED) still
 * fails near-instantly, exactly as before; this bound only matters for
 * the "nobody answers at all" case — a comms brain mid-reboot or out of
 * range, which is precisely the case reconnect logic exists to survive.
 * Small relative to the 2 s retry cadence so it doesn't itself become a
 * visible stall, generous relative to a real handshake (loopback/LAN
 * completes in low single-digit ms). */
#define MC_TCP_CONNECT_TIMEOUT_MS 300

/** The actual dial: resolve + connect + set nonblocking. Returns an open
 *  fd, or -1 on any failure. Shared by mc_tcp_open (the first connect)
 *  and mc_tcp_redial (S16 slice e's recovery from a drop) so there is
 *  exactly one place that knows how to make a socket.
 *
 *  Nonblocking is set BEFORE connect(), not after — connect() itself is
 *  the call that can hang, so setting nonblocking only once it returns
 *  bounds nothing (review round 1, PR #61, D1). A connect() that can't
 *  complete synchronously (EINPROGRESS, the normal case for anything not
 *  on the same host) is given MC_TCP_CONNECT_TIMEOUT_MS via select() to
 *  finish, then its outcome is read from SO_ERROR — writability alone
 *  doesn't distinguish "connected" from "failed", both make the fd
 *  writable. Timing out closes the candidate and moves on/fails, exactly
 *  as a refused connection already did. */
static int mc_tcp_dial(char const *host, uint16_t port)
{
    if (host == NULL) {
        return -1;
    }

    char port_str[6];
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        if (mc_tcp_set_nonblocking(candidate) < 0) {
            close(candidate);
            continue;
        }

        int rc = connect(candidate, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fd = candidate; /* completed synchronously (e.g. loopback) */
            break;
        }
        if (errno != EINPROGRESS) {
            /* Refused, no route, etc. — fails fast, no wait needed. */
            close(candidate);
            continue;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(candidate, &wfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = MC_TCP_CONNECT_TIMEOUT_MS * 1000};
        int sel = select(candidate + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            /* Timed out (the blackhole case this bound exists for), or
             * select() itself failed: give up on this candidate rather
             * than wait any longer. */
            close(candidate);
            continue;
        }

        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) != 0 || soerr != 0) {
            close(candidate); /* writable because it failed, not because it connected */
            continue;
        }

        fd = candidate;
        break;
    }
    freeaddrinfo(res);

    return fd; /* already nonblocking on success; -1 if every candidate failed */
}

int mc_tcp_open(mc_tcp_t *t, char const *host, uint16_t port)
{
    if (t == NULL || host == NULL) {
        return -1;
    }
    t->fd = -1;
    t->have_target = false;

    int fd = mc_tcp_dial(host, port);
    if (fd < 0) {
        return -1;
    }

    t->fd = fd;

    size_t n = strlen(host);
    if (n >= sizeof(t->host)) {
        n = sizeof(t->host) - 1u;
    }
    memcpy(t->host, host, n);
    t->host[n] = '\0';
    t->port = port;
    t->have_target = true;

    return 0;
}

/** Redial the last-opened target (S16 slice e). Returns 0 and updates
 *  t->fd on success; leaves t->fd untouched (still -1) on failure — the
 *  caller (mc_tcp_write_cb) reports the failure the same way a write to
 *  a live-but-broken socket always has. */
static int mc_tcp_redial(mc_tcp_t *t)
{
    if (!t->have_target) {
        return -1; /* never opened, or explicitly closed — nothing to redial */
    }
    int fd = mc_tcp_dial(t->host, t->port);
    if (fd < 0) {
        return -1;
    }
    t->fd = fd;
    return 0;
}

void mc_tcp_close(mc_tcp_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    /* An explicit close is the caller taking the transport away on
     * purpose — forget the target so a stray read()/write() afterward
     * can't silently reopen a socket the caller believes is shut. */
    t->have_target = false;
}

static int mc_tcp_read_cb(void *io, uint8_t *buf, size_t maxlen)
{
    mc_tcp_t *t = (mc_tcp_t *)io;
    if (t == NULL) {
        return -1;
    }
    if (t->fd < 0) {
        /* Down, and (by construction) already reported once — see the
         * fall-through cases below, and this header's/mc_transport_tcp.h's
         * top comment for why this must be "nothing available" (0) and
         * not another error: mc_client's own reconnect backoff is what
         * eventually triggers a write()-driven redial, and a repeating
         * -1 here would reset that backoff's clock on every single tick,
         * so it could never elapse. */
        return 0;
    }

    ssize_t n = recv(t->fd, buf, maxlen, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        /* Peer closed the connection. Drop the dead fd now, so every
         * call after this one reports "nothing available" rather than
         * repeating the error — the fd itself doesn't heal on its own,
         * and mc_client must see the error exactly once to transition
         * state, not on every tick forever. */
        close(t->fd);
        t->fd = -1;
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0; /* nothing available right now */
    }
    /* A hard error (ECONNRESET etc.): same drop-and-report-once handling. */
    close(t->fd);
    t->fd = -1;
    return -1;
}

static int mc_tcp_write_cb(void *io, uint8_t const *buf, size_t len)
{
    mc_tcp_t *t = (mc_tcp_t *)io;
    if (t == NULL) {
        return -1;
    }

    if (t->fd < 0) {
        /* THE RE-DIAL (S16 slice e). write() is only ever called by
         * mc_client at moments it is actually trying to (re)establish
         * something — mc_begin_handshake on its own 2 s backoff,
         * mc_send_heartbeat, or a send already gated on MC_STATE_READY —
         * so a redial attempt here is naturally rate-limited by
         * mc_client's own schedule, not a second timer this transport
         * would otherwise need. A failed redial reports -1 exactly like
         * a write to a broken live socket always has; mc_client's own
         * mc_fail_and_schedule_reconnect reschedules the next attempt. */
        if (mc_tcp_redial(t) != 0) {
            return -1;
        }
    }

    /* The socket is nonblocking (so reads never stall mc_tick()), but
     * callers of mc_transport_t.write() expect either "fully written" or
     * "error" for a single frame (<= MC_MAX_FRAME + 4 bytes) — small
     * enough that the kernel send buffer essentially never backs up in
     * practice. Retry on EAGAIN with a short poll instead of surfacing a
     * transient full-buffer as a hard transport error. */
    size_t sent = 0;
    int retries_left = 200; /* ~200 * up to 5ms select = bounded, not infinite */
    while (sent < len) {
        ssize_t n = send(t->fd, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (retries_left-- <= 0) {
                close(t->fd);
                t->fd = -1;
                return sent > 0 ? (int)sent : -1;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(t->fd, &wfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 5000};
            (void)select(t->fd + 1, NULL, &wfds, NULL, &tv);
            continue;
        }
        /* A hard error (EPIPE, ECONNRESET, ...): drop the dead fd so the
         * NEXT write() redials instead of failing against it forever. */
        close(t->fd);
        t->fd = -1;
        return sent > 0 ? (int)sent : -1;
    }
    return (int)sent;
}

mc_transport_t mc_tcp_transport(mc_tcp_t *t)
{
    mc_transport_t transport;
    transport.write = mc_tcp_write_cb;
    transport.read = mc_tcp_read_cb;
    transport.io = t;
    return transport;
}
