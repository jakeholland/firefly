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

/* Explicit "destination not known" sentinel for the `to` parameter of the
 * addressed inbound events (on_text/on_private). 0 is never a valid
 * Meshtastic node id (the wire protocol reserves it as "unset" — the same
 * reason rx-meta dispatch skips `from == 0`), so a producer that
 * genuinely lacks a packet's destination passes this rather than
 * guessing. It is neither MC_ADDR_BROADCAST nor any real node id, so
 * downstream direction classification reads it as unknown — never as
 * broadcast, never as addressed-to-me (issue #123's honesty rule). The
 * live decode path never needs it: a decoded MeshPacket always carries
 * `to`. */
#define MC_ADDR_UNKNOWN 0u

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

/**
 * Byte transport vtable — UART, TCP, later BLE.
 *
 * write() backpressure contract (settled here so every transport, present
 * and future, agrees on it):
 *   - A return of 0..len means that many bytes were ACCEPTED. len itself
 *     means the call fully succeeded.
 *   - A return of 0 means "accepted nothing right now, try again later" —
 *     this is NOT an error. A transport backed by a bounded buffer (a
 *     UART TX ring, for instance) is expected to return 0 when that
 *     buffer is momentarily full rather than block or fail; the caller
 *     (mc_write_bytes() in mc_client.c) retries a bounded number of
 *     times before giving up.
 *   - A negative return is a hard transport failure (dead fd, broken
 *     link) and is escalated immediately — no retry.
 *
 * mc_transport_tcp satisfies this today by retrying EAGAIN/EWOULDBLOCK
 * internally (see mc_tcp_write_cb) — it only ever returns a full/partial
 * byte count or -1, never 0, so nothing about its behavior changes here.
 * The contract exists for transports that DO naturally produce 0, most
 * notably the UART transport landing with S15: a full TX ring is normal
 * back-pressure, not a link failure, and treating it as one would fire a
 * reconnect storm every time the device fell behind draining its own
 * output.
 *
 * read() stays as documented inline: nonblocking, 0 = nothing available
 * (not an error either — see mc_tcp_read_cb's own comment on why a
 * repeating -1 here would be actively harmful to the reconnect backoff).
 */
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

/**
 * How a reported position was obtained — the *provenance* of the fix, not
 * its age. Translated at this boundary from Meshtastic's
 * `Position.LocSource`; the protobuf enum's numeric values deliberately do
 * not escape into core/ (see the note in issue #33 and ARCHITECTURE.md's
 * "core is pure" principle). An unrecognized wire value maps to
 * MC_LOC_UNKNOWN rather than being passed through, so a future firmware
 * adding LOC_* members can never make us assert provenance we don't
 * understand.
 *
 * The distinction that matters: MEASURED vs ASSERTED.
 *  - INTERNAL/EXTERNAL are *measurements* — a GPS actually fixed this
 *    location at some point, so "how recently" is a meaningful question.
 *  - MANUAL is an *assertion* — an installer typed it in. It has no
 *    measurement behind it at any age, so freshness is a category error
 *    for it (a fixed-position landmark re-broadcasts the same asserted
 *    point forever; see issue #33).
 *  - UNKNOWN is neither: the node did not tell us.
 *
 * Note on UNKNOWN: `location_source` is a proto3 implicit-presence enum,
 * so "field absent" and "explicitly LOC_UNSET" are literally the same
 * bytes on the wire. Both mean "the sender did not state provenance", so
 * a single UNKNOWN member loses no information and there is deliberately
 * no separate `has_loc_source` flag — UNKNOWN *is* the explicit unknown,
 * a distinct enum member rather than a sentinel smuggled into a value
 * that could also be a real reading.
 *
 * Consumers must not treat UNKNOWN as measured. Most stock firmware does
 * populate this field, but "didn't say" is not evidence of a GPS fix.
 */
typedef enum {
    MC_LOC_UNKNOWN = 0,  /* sender stated no provenance */
    MC_LOC_MANUAL = 1,   /* asserted by a human/config — no measurement */
    MC_LOC_INTERNAL = 2, /* measured by the node's own GPS */
    MC_LOC_EXTERNAL = 3, /* measured by an attached/EUD GPS */
} mc_loc_source_t;

/**
 * Whether our own radio heard the sender itself, or only via a relay.
 *
 * This is the qualifier that makes `mc_rx_meta_t.rssi_dbm` meaningful.
 * RSSI/SNR are measured by *our* radio against the signal that actually
 * arrived — which, for a relayed packet, is the **relay's** transmission,
 * not the originator's. Attributing that number to `from` would report a
 * loud neighbouring relay as if the distant friend it forwarded for were
 * standing next to you. Only MC_RX_PATH_DIRECT licenses attributing
 * rssi/snr to `from`.
 *
 * UNKNOWN is not a soft DIRECT — it means we could not establish the hop
 * count, and per the vendored protobuf's own guidance (mesh.pb.h, the
 * `hop_start` comment) an unestablished hop count must be treated as
 * unknown rather than optimistically as direct.
 */
typedef enum {
    MC_RX_PATH_UNKNOWN = 0,  /* hop count not establishable — assume nothing */
    MC_RX_PATH_DIRECT = 1,   /* our radio heard `from` itself, 0 hops, over LoRa */
    MC_RX_PATH_INDIRECT = 2, /* relayed by another node, or arrived via MQTT */
} mc_rx_path_t;

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

    /* Provenance of the fix — measured, asserted, or unstated. Carries no
     * age information; `time`/`rx_time` remain the only freshness inputs.
     * See mc_loc_source_t. */
    mc_loc_source_t loc_source;

    /* Coordinate precision as stated by the sender. The vendored protobuf
     * says only: "Indicates the bits of precision set by the sending node"
     * (mesh.pb.h, Position.precision_bits, tag 23) — concretely, how many
     * high-order bits of latitude_i/longitude_i survived the sending
     * channel's positionPrecision truncation. This matters because the
     * truncation is invisible in the coordinates themselves: the default
     * public channel keeps 13 bits (~5.8 km grid), and the resulting
     * multi-km error arrives as two perfectly ordinary-looking doubles
     * with a fresh timestamp (issue #47 — 2673 m measured on hardware).
     * This field is the only wire-level tell.
     *
     * When present, the value is 1..32. Approximate cell edge in metres:
     * (2^32 >> bits) * 1e-7 deg * ~111,320 m/deg of latitude — ~5836 m at
     * 13 bits, ~730 m at 16, ~2.9 m at 24; 32 is untruncated. (Longitude
     * cells shrink by cos(latitude); treat this as a scale, not a radius.)
     *
     * has_precision_bits == false folds together three wire situations
     * this library cannot tell apart — stated honestly rather than
     * inventing a distinction the wire can't carry (proto3 implicit
     * presence; the same reasoning probe_node.py's _proto3_num documents):
     *  - the field was never set (sender predates it, or a replay dropped
     *    it — see the path caveat below);
     *  - a wire value of exactly 0, byte-identical to absent. No real fix
     *    is lost here: in Meshtastic's channel config, precision 0 means
     *    "position disabled on this channel", so 0 never legitimately
     *    accompanies actual coordinates;
     *  - a value > 32, which is not a precision of a 32-bit coordinate —
     *    untrusted RF garbage, reported absent rather than clamped (same
     *    policy as the RSSI/SNR range gates in mc_rx_meta_t). Wire varints
     *    above UINT32_MAX never reach the gate at all: the vendored
     *    decoder rejects the whole Position ("integer too large") — a
     *    load-bearing, nonstandard strictness pinned by its own test; see
     *    the note at the gate in mc_client.c.
     *
     * Absent must NOT be read as "full precision". Absent means the
     * sender did not say; the coordinates may still be truncated.
     *
     * Path caveat, hardware-verified (two Heltec V3s, firmware 2.7.26):
     * both decode paths (live POSITION_APP packets and the want_config
     * NodeInfo replay) fill this field identically, and the wire format
     * allows it on both — but presence is NOT uniform in practice.
     *  - Live packets stamp it affirmatively, even at full precision: a
     *    fix sent on a positionPrecision:32 channel arrived with
     *    precisionBits: 32 on the wire (nonzero, so proto3 keeps it), so
     *    on modern firmware a full-precision live fix is distinguishable
     *    from an unstamped one.
     *  - The same node's row in the receiving board's node DB, seconds
     *    later, had no precision field at all — on both a truncated
     *    (13-bit) and a full-precision channel. Stock firmware does not
     *    preserve it across the nodeDB (its PositionLite → Position
     *    conversion never copies precision_bits; firmware
     *    TypeConversions.cpp, checked at master 2026-08), exactly as the
     *    replay carries no rx_time.
     * So a consumer that assumes uniform presence will mis-handle the
     * replay — and the replay is the cold-boot path. Note the affirmative
     * stamping is observed on one firmware version; what an older sender
     * puts here is not established, which is why absent stays "unknown,
     * not full" rather than being assumed impossible. */
    bool has_precision_bits;
    uint32_t precision_bits;
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

    /* Whether the nodeDB believes this node is a direct neighbour. Derived
     * from NodeInfo's explicit-presence `hops_away` (plus `via_mqtt`), so
     * "the field wasn't populated" is reported as MC_RX_PATH_UNKNOWN and
     * never silently as DIRECT.
     *
     * This is a nodeDB *summary*, not a per-packet fact: it describes how
     * the node was last heard, with no timestamp attached, and it can go
     * stale exactly like any other cached NodeInfo field. Use it to answer
     * "could this node plausibly ever give us usable RSSI?", not "is this
     * RSSI sample attributable?" — that question is per-packet and is
     * answered by mc_rx_meta_t.rx_path. */
    mc_rx_path_t rx_path;
} mc_nodeinfo_t;

/**
 * Per-packet reception metadata, measured by our local radio.
 *
 * Delivered via `mc_events_t.on_rx_meta` for every inbound MeshPacket that
 * names a sender — including packets whose payload is out of decode scope
 * or encrypted, since the radio still measured the signal that carried
 * them. That breadth is the point: position broadcasts alone arrive on a
 * multi-minute interval, far too slow to feed a 5-second RSSI trend
 * window, whereas telemetry/nodeinfo/text/routing traffic is frequent.
 *
 * Every field is explicitly presence-flagged rather than sentinel-coded.
 * The library never invents a reading, and callers must check the flags:
 * a zeroed mc_rx_meta_t means "we know nothing", not "0 dBm, direct".
 */
/* Plausibility bounds for the two radio readings below. Anything outside
 * these is not a measurement — it is malformed or hostile wire data — and
 * is reported as absent rather than clamped into range (clamping would
 * hand the caller a fabricated reading that passes threshold tests).
 *
 * Deliberately far wider than any real radio: genuine RSSI lives near
 * [-150, +20] dBm and LoRa SNR near [-30, +15] dB, so these can only ever
 * reject garbage, never a real sample. They are not a statement about what
 * a radio *should* report. */
#define MC_RSSI_MIN_DBM (-512)
#define MC_RSSI_MAX_DBM (512)
#define MC_SNR_MIN_DB (-128.0f)
#define MC_SNR_MAX_DB (128.0f)

typedef struct {
    /* RSSI in dBm as measured by our radio. Presence-flagged because 0 is
     * a legitimate reading on some radios (SX126x reports exactly 0 dBm;
     * SX127x's formula can go positive), so no in-band sentinel could be
     * safely reserved — see the `rx_rssi` comment in the vendored
     * meshtastic/mesh.pb.h. Meaningful for `from` only when
     * `rx_path == MC_RX_PATH_DIRECT`. A wire value outside
     * [MC_RSSI_MIN_DBM, MC_RSSI_MAX_DBM] reads absent. */
    bool has_rssi;
    int16_t rssi_dbm;

    /* SNR in dB as measured by our radio.
     *
     * Honest-data caveat, deliberate and load-bearing: Meshtastic's
     * `MeshPacket.rx_snr` is a proto3 *implicit-presence* float, so an
     * absent field and a genuine 0.0 dB reading serialize to identical
     * bytes. There is no way to distinguish them, so this library reports
     * exactly 0.0 as `has_snr == false` — under-claiming (calling a real
     * 0.0 dB reading "unknown") rather than over-claiming (inventing a
     * reading for a field the sender never set). Callers therefore never
     * see a fabricated SNR; they occasionally lose a real one sitting
     * precisely on zero. Unlike RSSI, this could not be fixed by a
     * presence flag on our side — the information is already gone by the
     * time the bytes reach us.
     *
     * NaN, infinities, and values outside [MC_SNR_MIN_DB, MC_SNR_MAX_DB]
     * also read absent. NaN especially: it is a non-number arriving from
     * untrusted RF, and it would otherwise pass an `!= 0.0f` presence test
     * while silently failing every downstream comparison. When has_snr is
     * false, snr_db is zeroed — never NaN — so a caller that ignores the
     * flag gets the same benign 0 that rssi_dbm gives it. */
    bool has_snr;
    float snr_db;

    /* Whether rssi/snr may be attributed to `from` at all. See
     * mc_rx_path_t — this is the qualifier, not a nicety. */
    mc_rx_path_t rx_path;
} mc_rx_meta_t;

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

    /**
     * Firefly protocol rides here (private/experimental portnums 256-511).
     *
     * `to` is the MeshPacket's destination address, delivered exactly as
     * on_text already delivers it (issue #123): MC_ADDR_BROADCAST for a
     * broadcast, a specific node id for a directed packet, or
     * MC_ADDR_UNKNOWN from a producer that genuinely lacks the address.
     * The client itself always has it (`pkt->to`), so 1:1 private
     * traffic (a flare sent to one member) is classifiable downstream
     * as addressed-to-me instead of collapsing to unknown.
     */
    void (*on_private)(void *u, uint32_t from, uint32_t to, uint32_t portnum,
                        uint8_t const *payload, size_t len);
    void (*on_my_info)(void *u, uint32_t my_node_id);

    /**
     * Per-packet radio metadata (RSSI/SNR/hop path) for any inbound
     * MeshPacket carrying a nonzero `from`, including encrypted packets
     * and portnums outside decode scope.
     *
     * Ordering guarantee: for a packet that also produces a payload event
     * (on_position/on_text/on_private), on_rx_meta fires *first*, so a
     * consumer can correlate the two by `from` within one dispatch
     * without buffering. Packets with `from == 0` (sender unknown) never
     * fire this — there would be nobody to attribute the reading to.
     *
     * Self-packets are NOT filtered: if the radio echoes back a packet
     * this node originated, it arrives with `from == my_node_id` and will
     * fire this callback like any other. Nothing is fabricated by that
     * (an echo carries no rx_rssi, so `has_rssi` is false), but a caller
     * maintaining a per-peer roster wants to skip its own id rather than
     * create a slot for itself.
     *
     * Firing does not imply any field is present: check `m->has_rssi` /
     * `m->has_snr`, and check `m->rx_path == MC_RX_PATH_DIRECT` before
     * attributing either reading to `from`.
     */
    void (*on_rx_meta)(void *u, uint32_t from, mc_rx_meta_t const *m);

    /**
     * NAME in Settings, confirmation-fix follow-up — fires when an
     * `AdminMessage.get_owner_response` arrives (ADMIN_APP, portnum 6):
     * the direct, on-demand answer to `mc_send_get_owner_request`, as
     * opposed to `on_node`'s NodeInfo replay (which the comms brain only
     * re-sends on its own schedule — the next want_config handshake, or
     * an hours-scale periodic broadcast — never right after a
     * `set_owner` push). `long_name`/`short_name` are each "" when the
     * response's `User` left that field unset (proto3 implicit
     * presence — absent and empty are the same bytes), never NULL, so a
     * caller may `strcmp` them directly.
     *
     * This library sends `get_owner_request` only to `dest == self` (see
     * `mc_send_get_owner_request`'s doc comment), so any response this
     * fires for is definitionally this node's own current owner — no
     * `from`/self check is needed downstream, unlike `on_node`.
     */
    void (*on_owner)(void *u, char const *long_name, char const *short_name);

    /**
     * NAME in Settings, confirmation-fix follow-up — fires for a
     * ROUTING_APP reply that reports the outcome of an earlier
     * `want_ack` send (e.g. `mc_send_set_owner`'s admin write):
     * `request_id` is the original outgoing `MeshPacket.id` (matches the
     * `out_packet_id` that send call handed back), `ok` is true only for
     * `Routing.error_reason == NONE` — every other reason (including a
     * malformed/absent `error_reason`, which decodes to NONE=0 on the
     * wire and is therefore indistinguishable from success; see
     * mc_process_mesh_packet's own comment) is a NAK. A caller that
     * cannot find a matching in-flight `request_id` should ignore the
     * event rather than guess which send it belonged to.
     */
    void (*on_routing_ack)(void *u, uint32_t request_id, bool ok);

    void *user;
} mc_events_t;

/* Byte-chunk size mc_tick() reads from the transport at a time. Ordinary
 * chunked I/O (not one byte per transport.read() call) — the cap below is
 * what bounds per-call work, not the read granularity, so there is no
 * reason to pay one transport call per byte. 64 matches the pre-existing
 * chunk size this library has always used. */
#define MC_TICK_READ_CHUNK 64u

/* Cap on complete FromRadio frames DISPATCHED within one mc_tick() call
 * (i.e. delivered to mc_process_from_radio() — decode_errors/decode_skipped
 * still get bumped by whatever fires within a dispatch, this just bounds
 * how many frames are pulled through per call).
 *
 * Without a cap, mc_tick()'s read loop drains the transport completely in
 * one call — fine for ordinary traffic, but meshtasticd's want_config
 * response can burst a NodeInfo dump of the whole mesh's node database in
 * one delivery (a busy public channel can carry on the order of 100
 * nodes), and mc_tick() shares its calling tick with UI rendering
 * (ff_shell.c's ff_shell_tick). Decoding a large dump's worth of
 * protobufs back-to-back in a single call would stall that shared tick.
 *
 * 32 caps the worst case at a small, constant amount of decode work per
 * call: even a 100-node dump finishes in ~4 ticks (~80 ms at the spec's
 * 50 Hz cadence, docs/specs/S03-meshclient.md), invisible to a human, while
 * any single mc_tick() call never does more than 32 frames' worth of
 * pb_decode() + dispatch.
 *
 * Frames beyond the cap are simply not consumed yet: mc_tick() still
 * reads the transport in MC_TICK_READ_CHUNK-sized chunks (cheap — one
 * transport.read() call per chunk, not per byte), but if the cap lands
 * mid-chunk, the unconsumed remainder of that chunk is copied into
 * mc_client_t's own tick_carry_buf rather than fed to the framer or
 * discarded. mc_framer_t.buf cannot hold it — that accumulator only ever
 * holds bytes belonging to the ONE frame currently in progress, not raw
 * leftover bytes spanning a chunk boundary. The next mc_tick() call feeds
 * tick_carry_buf to the framer first, before reading the transport again,
 * so nothing is lost and frame order is preserved across calls. */
#define MC_TICK_MAX_FRAMES 32u

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

    /* Bytes read from the transport in a MC_TICK_MAX_FRAMES-capped
     * mc_tick() call that couldn't be fed to the framer this call because
     * the cap was already hit mid-chunk. Fed to the framer FIRST on the
     * next mc_tick() call, before any further transport.read() — see
     * MC_TICK_MAX_FRAMES's doc comment. tick_carry_len == 0 means empty;
     * tick_carry_pos is how much of tick_carry_buf[0..tick_carry_len) has
     * already been consumed. */
    uint8_t tick_carry_buf[MC_TICK_READ_CHUNK];
    uint16_t tick_carry_len;
    uint16_t tick_carry_pos;
} mc_client_t;

/** Initialize a freshly-allocated client. Does not touch the transport or
 * start the handshake — call mc_connect() for that. Safe to call again to
 * fully reset a client (equivalent to a fresh mc_client_t).
 *
 * mc_init() itself always leaves the outgoing packet-id counter
 * (`next_packet_id`) at the legacy default of 1 — see mc_seed_packet_ids()
 * below for why a real caller should override that before the first
 * send. */
void mc_init(mc_client_t *c, mc_transport_t t, mc_events_t ev, ff_clock_t const *clock);

/**
 * Seed the outgoing packet-id generator (`next_packet_id`).
 *
 * Meshtastic's router keeps a short packet history keyed on (from, id)
 * and silently drops a repeat as "already seen recently". mc_init()
 * always starts `next_packet_id` at 1, so two client lifetimes that both
 * start there (e.g. this device rebooting) collide on every id until the
 * higher of the two sessions' send counts is exceeded — DMs and
 * broadcasts vanish with no error, only a log line on the *receiving*
 * node. Calling this once, any time after mc_init() and before the first
 * mc_send_text()/mc_send_private()/mc_send_position(), gives each
 * lifetime a distinct starting point so ids from a fresh boot don't
 * retread ids a previous boot already used within the router's history
 * window.
 *
 * meshclient stays pure C11 with no RNG dependency of its own — the seed
 * is the caller's platform's problem: mix time and pid for a desktop/sim
 * build (or pass a fixed value for deterministic tests — 1 reproduces
 * mc_init()'s own default, i.e. the pre-this-function sequence), or
 * `esp_random()` on the ESP32-S3 device build. A 32-bit random start
 * makes an id collision with a prior session's ids negligible next to
 * the router's ~10-minute history window; the device can't additionally
 * mix in its own node id at this point because `my_node_id` only arrives
 * later, via `on_my_info`, well after the first packet may need to send.
 *
 * ids assigned by mc_send_data_packet() start at `seed`, then increment
 * by 1 per outgoing packet (mc_send_text/mc_send_private/mc_send_position
 * all share the one counter). 0 is never a valid Meshtastic packet id
 * (same "unset" convention as MC_ADDR_UNKNOWN), so `seed == 0` is treated
 * as 1, and the counter skips 0 when it wraps past UINT32_MAX rather than
 * handing out 0 as a real id.
 *
 * Not calling this at all is equivalent to seeding with 1 — mc_init()'s
 * own default — which is what every existing test relies on. */
void mc_seed_packet_ids(mc_client_t *c, uint32_t seed);

/** Pump read/parse/heartbeat/reconnect. Call at ~50 Hz (every ~20ms).
 * Bounded: dispatches at most MC_TICK_MAX_FRAMES frames per call (see its
 * doc comment near mc_client_t, above) — a large burst drains over
 * several calls, never one, and nothing read from the transport is ever
 * lost when the cap lands mid-chunk (see mc_client_t.tick_carry_*). */
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

/**
 * mc_send_set_owner — send a Meshtastic AdminMessage.set_owner: sets this
 * node's `User{long_name, short_name}` — the mesh "owner" identity every
 * other node and phone app displays for it (`[api]`, NAME-in-Settings
 * feature).
 *
 * Rides ADMIN_APP (portnum 6), `want_ack` is always true (an admin write
 * worth calling this for is worth the mesh stack retrying, unlike a
 * best-effort broadcast text) and the packet id comes from the same
 * seeded generator every other send uses (`mc_seed_packet_ids`).
 *
 * `dest` is the destination node id — pass this node's OWN id
 * (`ff_shell_my_node_id` on the app side) to reach the "local admin, no
 * key needed" path: Meshtastic's PhoneAPI zeroes `MeshPacket.from` for
 * EVERY packet a locally-attached client submits ("We don't let clients
 * assign nodenums to their sent messages" — meshtastic/firmware
 * `src/mesh/MeshService.cpp:188`, `MeshService::handleToRadio`), and
 * `AdminModule::handleReceivedProtobuf` only requires a session passkey
 * when `mp.from != 0` (`src/modules/AdminModule.cpp`) — so a message this
 * device (acting as the comms brain's own local client, exactly like the
 * phone app) submits to itself is trusted with no key exchange at all.
 * Verified by reading meshtastic/firmware tag `v2.7.26` (commit
 * `54e0d8d0`) — the same firmware version this repo's own S03 spec
 * amendments hardware-verified other wire behavior against. A `dest`
 * that is NOT this node's own id would still encode and send, but would
 * land on `AdminModule`'s passkey-required path on a REMOTE node and be
 * rejected there; this library does not enforce `dest == self` itself
 * (the caller already knows its own id, or doesn't call this yet).
 *
 * `long_name`/`short_name` may each be NULL or "" to leave that field
 * unset on the wire — Meshtastic's `AdminModule::handleSetOwner` only
 * overwrites a field when the incoming `User`'s field is non-empty, so a
 * NULL/"" `short_name` (for instance) updates only the long name.
 * Neither is validated against the puck-name charset here (that is
 * `ff_meshname_sanitize`'s job, one layer up, core/include/ff_meshname.h)
 * — this function only bounds each to `MC_NAME_MAX - 1` bytes (truncated,
 * never rejected, matching this library's existing string-field
 * convention) before encoding.
 *
 * Returns 0 on success, negative on failure (not READY, encode/write
 * failure).
 *
 * `out_packet_id` — confirmation-fix follow-up (bench finding: the comms
 * brain never re-sends its own NodeInfo right after a `set_owner`, so
 * the OLD "wait for a self NodeInfo" confirmation path could hang
 * forever) — is OPTIONAL (NULL-safe) and, on a successful send (return
 * 0 only), receives the outgoing `MeshPacket.id` this call used, so the
 * caller can correlate a later `mc_events_t.on_routing_ack` reply
 * against THIS specific push rather than guessing. Left untouched on
 * failure (return negative) — there is no in-flight packet id to hand
 * back.
 */
int mc_send_set_owner(mc_client_t *c, uint32_t dest, char const *long_name, char const *short_name,
                       uint32_t *out_packet_id);

/**
 * mc_send_get_owner_request — confirmation-fix follow-up: send a
 * Meshtastic `AdminMessage.get_owner_request`, asking `dest` to reply
 * with its current owner `User` (`AdminMessage.get_owner_response`,
 * delivered via `mc_events_t.on_owner`). Exists because the comms brain
 * does NOT proactively re-announce its own NodeInfo right after a
 * `set_owner` write lands — only the next want_config handshake or the
 * periodic (hours-scale) broadcast carries it — so a puck that just
 * pushed a new owner name has no other honest way to learn "did that
 * actually take" without either waiting arbitrarily long or asking
 * directly. This is the asking.
 *
 * Rides ADMIN_APP (portnum 6), same as `mc_send_set_owner`. `want_ack`
 * is false: the value of this call is the `get_owner_response` payload
 * itself (or its absence, honestly read as "no answer yet" — see
 * `ff_shell.c`'s retry/timeout handling), not the mesh-level delivery
 * receipt a `want_ack` NAK/ACK would add on top; a caller that wants
 * that too can watch `mc_send_set_owner`'s own `out_packet_id`/
 * `on_routing_ack` pairing instead. `dest` should be this node's own id
 * for the same "local admin, no key needed" reason `mc_send_set_owner`'s
 * doc comment explains in full — a request to a REMOTE node's admin
 * module needs a session passkey this library does not manage.
 *
 * `meshtastic_Data.want_response` IS set true on the encoded packet
 * (confirmation-fix round 2, bench finding 2026-09-06: a real puck +
 * Meshtastic 2.7.26 comms brain never replied at all — `reply=none`
 * after every retry — because this bit was never set). Verified against
 * `meshtastic/firmware` tag `v2.7.26.54e0d8d0`,
 * `src/modules/AdminModule.cpp`, `AdminModule::handleGetOwner`:
 * `myReply` (the `get_owner_response`) is only built and queued
 * `if (req.decoded.want_response)` — an admin read with the bit unset is
 * silently answered with nothing, which is exactly the CLI's own
 * `wantResponse=True` convention on every admin read. This is
 * independent of `want_ack` above: `want_response` asks the ADMIN MODULE
 * for its payload reply; `want_ack` (unused here) would ask the ROUTING
 * layer for a mesh-delivery receipt. See `mc_send_data_packet_ex`'s own
 * doc comment (`mc_client.c`) for the full citation with source.
 *
 * Returns 0 on success, negative on failure (not READY, encode/write
 * failure).
 */
int mc_send_get_owner_request(mc_client_t *c, uint32_t dest);

mc_state_t mc_state(mc_client_t const *c);
mc_stats_t mc_get_stats(mc_client_t const *c);

#ifdef __cplusplus
}
#endif

#endif /* MC_CLIENT_H */
