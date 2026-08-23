/**
 * mc_transport_tcp.h — POSIX TCP transport (sim target: connects to
 * meshtasticd). See docs/specs/S03-meshclient.md slice e.
 *
 * The UART transport (esp32s3, comms brain) lands with S15; this is the
 * dev/sim stand-in, same role, different wire.
 */
#ifndef MC_TRANSPORT_TCP_H
#define MC_TRANSPORT_TCP_H

#include "mc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd; /* -1 = not connected */
} mc_tcp_t;

/**
 * Open a TCP connection to host:port (blocking connect — this call
 * returns once connected or once it fails; the socket itself is left
 * nonblocking for subsequent read()/write() calls). Returns 0 on success,
 * negative on failure (bad host, connect refused, etc).
 */
int mc_tcp_open(mc_tcp_t *t, char const *host, uint16_t port);

/** Close the socket, if open. Safe to call on an already-closed mc_tcp_t. */
void mc_tcp_close(mc_tcp_t *t);

/**
 * Build an mc_transport_t backed by `t`. `t` must outlive the transport
 * (and must already be open — call mc_tcp_open() first).
 */
mc_transport_t mc_tcp_transport(mc_tcp_t *t);

#ifdef __cplusplus
}
#endif

#endif /* MC_TRANSPORT_TCP_H */
