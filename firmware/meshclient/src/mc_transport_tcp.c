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

int mc_tcp_open(mc_tcp_t *t, char const *host, uint16_t port)
{
    if (t == NULL || host == NULL) {
        return -1;
    }
    t->fd = -1;

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
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return -1;
    }
    if (mc_tcp_set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    t->fd = fd;
    return 0;
}

void mc_tcp_close(mc_tcp_t *t)
{
    if (t != NULL && t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

static int mc_tcp_read_cb(void *io, uint8_t *buf, size_t maxlen)
{
    mc_tcp_t *t = (mc_tcp_t *)io;
    if (t == NULL || t->fd < 0) {
        return -1;
    }

    ssize_t n = recv(t->fd, buf, maxlen, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        /* Peer closed the connection. */
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0; /* nothing available right now */
    }
    return -1;
}

static int mc_tcp_write_cb(void *io, uint8_t const *buf, size_t len)
{
    mc_tcp_t *t = (mc_tcp_t *)io;
    if (t == NULL || t->fd < 0) {
        return -1;
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
                return sent > 0 ? (int)sent : -1;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(t->fd, &wfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 5000};
            (void)select(t->fd + 1, NULL, &wfds, NULL, &tv);
            continue;
        }
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
