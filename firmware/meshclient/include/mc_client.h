/**
 * mc_client.h — embedded Meshtastic client library.
 *
 * Makes this device a Meshtastic *client* (same role as the phone apps)
 * over any byte transport: stream framing, the want_config handshake,
 * node/position/message events, and sending. Extraction-grade: no
 * includes from firefly's core/ or app/ (the shared types, ff_clock_t and
 * ff_latlon_t, live in platform/, see docs/ARCHITECTURE.md).
 *
 * See docs/specs/S03-meshclient.md for the full behavioral contract.
 *
 * No dynamic allocation: mc_client_t is a plain struct the caller owns
 * (stack, static, whatever) and passes by pointer to every call.
 */
#ifndef MC_CLIENT_H
#define MC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff_clock.h"
#include "ff_latlon.h"

#include "mc_framing.h" /* MC_MAX_FRAME */

#ifdef __cplusplus
extern "C" {
#endif

/* Broadcast destination address, per the Meshtastic wire protocol. */
#define MC_ADDR_BROADCAST 0xFFFFFFFFu

/* Meshtastic "private/experimental" portnum range (spec S04 rides here). */
#define MC_PORTNUM_PRIVATE_MIN 256u
#define MC_PORTNUM_PRIVATE_MAX 511u

/* Static field sizes, per docs/specs/S03-meshclient.md ("callback field
 * sizes per spec: names 40 B, text 237 B (Meshtastic max)"). Must match
 * meshclient/tools/mc_nanopb.options.
 *
 * nanopb behavior note: these are hard budgets, not truncation limits. A
 * User.long_name/short_name longer than MC_NAME_MAX (or a Data.payload
 * longer than MC_TEXT_MAX) makes nanopb's pb_decode() fail the *entire*
 * enclosing FromRadio message — not just drop or truncate that one field.
 * Concretely: one NodeInfo dump entry with an oversized name silently
 * loses its node_num/position/battery too (counted only in the aggregate
 * mc_stats_t.decode_errors, since mc_client.c never sees a partially
 * decoded message to inspect). This is memory-safe (verified against
 * pb_decode.c's pb_dec_string) and matches "skipped silently but
 * counted" in spirit, but it's coarser-grained than per-field truncation
 * and worth knowing before debugging a "node just didn't show up" report. */
#define MC_NAME_MAX 40u
#define MC_TEXT_MAX 237u

/* -------------------------------------------------------------------- */
/* Transport + clock seams                                              */
/* -------------------------------------------------------------------- */

/** Byte transport vtable — UART, TCP, later BLE. */
typedef struct {
    int (*write)(void *io, uint8_t const *buf, size_t len);
    int (*read)(void *io, uint8_t *buf, size_t maxlen); /* nonblocking, 0 = nothing */
    void *io;
} mc_transport_t;

/* -------------------------------------------------------------------- */
/* Decoded value types (decode scope v1)                                */
/* -------------------------------------------------------------------- */

typedef enum {
    MC_STATE_DISCONNECTED = 0,
    MC_STATE_HANDSHAKE = 1,
    MC_STATE_READY = 2,
} mc_state_t;

/** A position fix. Unknown fields are explicitly flagged, never faked
 * (see CLAUDE.md "Honest data over pretty data"). */
typedef struct {
    double lat; /* degrees */
    double lon; /* degrees */
    bool has_altitude;
    int32_t altitude_m;
    uint32_t time;    /* GPS fix time, unix seconds (0 = unknown/not sent) */
    bool has_rx_time;
    uint32_t rx_time; /* when the local radio received this, unix seconds */
} mc_position_t;

typedef struct {
    uint32_t node_num;

    bool has_long_name;
    char long_name[MC_NAME_MAX];
    bool has_short_name;
    char short_name[MC_NAME_MAX];

    uint32_t hw_model; /* meshtastic_HardwareModel, kept as a plain int here
                         * so this header never needs to see the protobuf
                         * enum type */

    bool has_position;
    mc_position_t position;

    bool has_battery_level;
    uint32_t battery_level; /* 0-100, >100 conventionally means "powered" */

    uint32_t last_heard; /* unix seconds, 0 = unknown */
} mc_nodeinfo_t;

/** Counts of frames/packets we saw but didn't fully decode, per spec
 * ("Everything else skipped silently but counted"). */
typedef struct {
    uint32_t frames_ok;         /* frames the framer completed */
    uint32_t frames_resynced;   /* garbage-prefix or oversize-len events */
    uint32_t decode_errors;     /* frame parsed as protobuf but malformed */
    uint32_t decode_skipped;    /* out-of-decode-scope FromRadio/portnum */
    uint32_t reconnects;        /* auto-reconnect attempts started */
} mc_stats_t;

/* -------------------------------------------------------------------- */
/* Events                                                                */
/* -------------------------------------------------------------------- */

typedef struct {
    void (*on_state)(void *u, mc_state_t s); /* DISCONNECTED/HANDSHAKE/READY */
    void (*on_node)(void *u, mc_nodeinfo_t const *n); /* id, names, hw, battery */
    void (*on_position)(void *u, uint32_t node, mc_position_t const *p);
    void (*on_text)(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len);
    void (*on_private)(void *u, uint32_t from, uint32_t portnum, uint8_t const *payload,
                        size_t len); /* firefly protocol rides here */
    void (*on_my_info)(void *u, uint32_t my_node_id);
    void *user;
} mc_events_t;

/* -------------------------------------------------------------------- */
/* Client                                                                */
/* -------------------------------------------------------------------- */

/**
 * Opaque-in-spirit but plain-struct-in-practice (no dynamic allocation
 * allowed, so the caller must be able to allocate this itself). Treat
 * every field as private; only mc_state()/mc_get_stats() are supported
 * ways to read it. Layout may change between versions.
 */
typedef struct mc_client {
    mc_transport_t transport;
    mc_events_t events;
    ff_clock_t clock;

    mc_state_t state;

    mc_framer_t framer;

    uint32_t my_node_id;
    bool has_my_node_id;

    uint32_t want_config_id;
    uint32_t rng_state;
    uint32_t next_packet_id;

    uint32_t last_rx_ms;
    uint32_t last_heartbeat_ms;
    uint32_t reconnect_at_ms;
    bool reconnect_pending;

    mc_stats_t stats;
} mc_client_t;

/** Initialize a freshly-allocated client. Does not touch the transport or
 * start the handshake — call mc_connect() for that. Safe to call again to
 * fully reset a client (equivalent to a fresh mc_client_t). */
void mc_init(mc_client_t *c, mc_transport_t t, mc_events_t ev, ff_clock_t const *clock);

/** Pump read/parse/heartbeat/reconnect. Call at ~50 Hz (every ~20ms). */
void mc_tick(mc_client_t *c, uint32_t now_ms);

/** Start (or restart) the want_config handshake. */
void mc_connect(mc_client_t *c);

/** Broadcast or direct-message plain UTF-8 text. dest = MC_ADDR_BROADCAST
 * for the primary channel. Returns 0 on success, negative on failure
 * (not READY, utf8 too long for MC_TEXT_MAX, encode/write failure). */
int mc_send_text(mc_client_t *c, uint32_t dest, char const *utf8);

/** Send arbitrary bytes on a private/experimental portnum (the firefly
 * protocol, spec S04, rides here). Returns 0 on success, negative on
 * failure (not READY, len too long for MC_TEXT_MAX, encode/write failure). */
int mc_send_private(mc_client_t *c, uint32_t dest, uint32_t portnum, uint8_t const *payload,
                     size_t len, bool want_ack);

/** Broadcast a position update. Rarely needed — the comms brain (stock
 * Meshtastic node with its own GPS) normally owns and sends position.
 * Returns 0 on success, negative on failure. */
int mc_send_position(mc_client_t *c, ff_latlon_t p);

mc_state_t mc_state(mc_client_t const *c);
mc_stats_t mc_get_stats(mc_client_t const *c);

#ifdef __cplusplus
}
#endif

#endif /* MC_CLIENT_H */
