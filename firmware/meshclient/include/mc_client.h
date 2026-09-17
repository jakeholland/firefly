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

    /**
     * [api] Diagnostics (S03 amendment) — satellite count as stated by
     * the sender (`Position.sats_in_view`, mesh.pb.h tag 19). Same
     * proto3 implicit-presence shape as `precision_bits` above (a plain
     * SINGULAR uint32, no wire-level has-flag), and the same folding
     * rule: a wire value of exactly 0 is byte-identical to "field never
     * set", and a position fix genuinely arriving with zero satellites
     * in view is implausible (a GPS needs several in view to compute any
     * fix at all) — so 0 reads absent here too, per this field's own
     * documented precedent. Unlike precision_bits there is no
     * upper-bound garbage check: any nonzero uint32 is a plausible (if
     * unusually large) satellite count, so nothing above 0 is rejected.
     * Absent must NOT be read as "no satellites in view" — it means the
     * sender did not say. */
    bool has_sats_in_view;
    uint32_t sats_in_view;
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
 * mc_user_reply_t — `[api]` bench finding 2026-09-14: the names carried
 * by a LIVE `NODEINFO_APP` `MeshPacket` (`mc_events_t.on_nodeinfo_reply`
 * — see that event's own doc comment, and `mc_send_nodeinfo_request`).
 *
 * Deliberately NOT `mc_nodeinfo_t` reused with the rest force-zeroed: a
 * live NodeInfo packet's payload is only a `meshtastic_User` — no
 * position, no battery, no `last_heard`/hops summary. Those belong to a
 * DIFFERENT message (the want_config replay's `FromRadio.node_info`
 * wrapper, decoded separately into `mc_nodeinfo_t` via `on_node`) that
 * this one is not. A shared struct with unpopulated fields would invite
 * a caller to read a fact this decode never actually carried; a
 * dedicated, narrower type cannot be misread that way. */
typedef struct {
    bool has_long_name;
    char long_name[MC_NAME_MAX];
    bool has_short_name;
    char short_name[MC_NAME_MAX];
} mc_user_reply_t;

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

    /* ---------------------------------------------------------------
     * [api] A02 slice D (docs/specs/S02-core-crew.md's 2026-09-13
     * amendment §B) — the three facts the crew-admission rule needs and
     * this struct did not carry. Additive; every existing caller is
     * unaffected.
     * ------------------------------------------------------------- */

    /* The channel-table index the radio reports for this packet
     * (`MeshPacket.channel`).
     *
     * PRESENCE-FLAGGED, and this flag is load-bearing in a way the
     * others here are not: the crew channel is normally index 0 (the
     * primary), so "we don't know which channel" silently reading as 0
     * is *exactly* how a stranger on the public channel gets admitted to
     * somebody's crew. Absent must never read as 0.
     *
     * Present iff the field genuinely is a channel-table index. Two
     * cases where it is NOT, both from the vendored protobuf's own
     * comments (mesh.pb.h):
     *  - an ENCRYPTED-variant packet, where "deep inside the device
     *    Router code, this field instead contains the 'channel hash'".
     *    A hash that happened to be 0 would name the crew slot;
     *  - a PKI-encrypted DM (`pki_encrypted`), which proves possession
     *    of a key PAIR, not of the crew channel's key — the whole basis
     *    of membership — and carries no meaningful channel.
     *
     * Note what absence does NOT mean: proto3 gives this field implicit
     * presence, so a packet genuinely received on the primary channel
     * serializes `channel` as nothing at all. That is not "unknown" —
     * mesh.proto states it outright: "If unset, packet was on the
     * primary channel." So a decoded, non-PKI packet reports
     * has_channel_index == true with channel_index == 0, which is the
     * honest reading, not a fabricated default. */
    bool     has_channel_index;
    uint32_t channel_index;

    /* `MeshPacket.via_mqtt` — this packet reached our radio over the
     * internet rather than over the air.
     *
     * Not presence-flagged: it is a proto3 bool whose absent state and
     * false state are both "not via MQTT", which is a single meaning,
     * unlike the index above. Note that `rx_path` ALREADY folds this in
     * (via_mqtt forces MC_RX_PATH_INDIRECT), but folded is not good
     * enough for the admission rule: plenty of ordinary relayed LoRa
     * packets are INDIRECT too, and a crew is people who are here. This
     * field is the un-folded fact. */
    bool via_mqtt;

    /* The decoded payload's portnum (`Data.portnum`).
     *
     * PRESENCE-FLAGGED because an encrypted-variant packet has no
     * decoded payload to read one from — which is also, conveniently,
     * exactly the case the admission rule's clause 1 rejects.
     *
     * [api] NOTE, flagged for review rather than slipped in: the
     * amendment's own `[api]` list names only the two fields above. The
     * rule it specifies has a sixth clause about the portnum, and it
     * puts admission on `on_rx_meta` ("it falls out of routing admission
     * through `shell_ev_rx_meta` rather than `shell_ev_node`") — so
     * without this the shell would have to correlate two callbacks by
     * `from`, and live NODEINFO_APP packets (which produce no payload
     * event in this library at all) could never admit anyone. Carrying
     * the portnum here is per-packet reception metadata like everything
     * else in this struct, and it keeps the rule evaluable in one place. */
    bool     has_portnum;
    uint32_t portnum;
} mc_rx_meta_t;

/** Meshtastic's own channel-table size (`MAX_NUM_CHANNELS`). Bounds the
 *  client's cached snapshot of the table — see
 *  `mc_client_get_channel_snapshot`. */
#define MC_CHANNEL_MAX 8u

/**
 * [api] A02 slice D — one row of the radio's channel table, delivered
 * during `want_config` (docs/specs/S02-core-crew.md's 2026-09-13
 * amendment §B).
 *
 * Why a client needs this at all: `MeshPacket.channel` is an INDEX, and
 * mesh.proto says the index is "inherently a local concept and
 * meaningless to send between nodes". A radio provisioned by CLI or by
 * the stock Meshtastic app may hold the crew channel anywhere, so the
 * only honest way to know which index is the crew's is to look it up in
 * this table by NAME and PSK — never to assume 0.
 *
 * Both halves of that match matter. The name alone is not enough (anyone
 * can name a channel `FIRE-4K9M7X` without holding the key), and the PSK
 * alone is not enough either (Meshtastic's on-air channel hash folds the
 * name, A02 §1.3, so two radios agreeing on a key but not a name never
 * hear each other).
 */
typedef struct {
    uint8_t  index;      /* the channel's slot in the table, 0 = primary */
    char     name[12];   /* Meshtastic's own limit: "Less than 12 bytes", NUL-terminated */
    uint8_t  psk[32];
    /* Length of `psk`, as the radio actually reported it — Meshtastic
     * itself only ever writes 0, 1, 16 or 32, but this is a reading, not
     * a promise: any length up to 32 is passed through as measured, and
     * a longer one is reported as 0 (no key) rather than truncated into
     * a plausible-looking wrong key. 0 means the channel stated no key. */
    uint8_t  psk_len;
    bool     is_primary; /* Channel.role == PRIMARY */

    /* [api] A02 slice D2 — `ChannelSettings.module_settings.
     * position_precision`, PRESENCE-FLAGGED.
     *
     * The flag is load-bearing in both directions, which is why an
     * "absent means 0" shortcut is not taken here:
     *  - on a READ, an absent `module_settings` submessage is a channel
     *    that never stated a precision, which is NOT the same claim as
     *    "this channel shares nothing" — and the pre-crew snapshot
     *    (ff_crewstart.h) has to be able to restore the distinction;
     *  - on a WRITE, Meshtastic reads an ABSENT submessage as the
     *    default (32), so the submessage is always emitted explicitly
     *    (A02 §1.5's own "saying it out loud is free"). A writer that
     *    left it absent to mean 0 would silently ship full precision.
     *
     * `position_precision` is meaningless when `has_position_precision`
     * is false, and is zeroed rather than left as stack garbage. */
    bool     has_position_precision;
    uint32_t position_precision;
} mc_channel_t;

/**
 * [api] Diagnostics — a node's own DEVICE metrics, decoded from a
 * `Telemetry` message carrying a `device_metrics` variant
 * (TELEMETRY_APP, portnum 67; `meshtastic_Telemetry.which_variant ==
 * meshtastic_Telemetry_device_metrics_tag`). Meshtastic's `Telemetry`
 * message is a oneof over several metric kinds (environment, power,
 * local stats, ...); this library decodes ONLY the device_metrics
 * variant — see `mc_events_t.on_telemetry`'s own doc comment for why
 * every other variant is silently not-an-error, mirroring the existing
 * "well-formed, nothing this library understands to report" precedent
 * ADMIN_APP already sets for a reply variant it doesn't interpret.
 *
 * Every field is explicit-presence-flagged, same discipline as
 * `mc_rx_meta_t`/`mc_position_t` above: `meshtastic_DeviceMetrics`'s own
 * fields are all real proto3 `optional` (explicit presence — nanopb
 * generates a genuine `has_*` per field, unlike `Position.sats_in_view`'s
 * implicit-presence uint32), so this struct's `has_*` flags are a
 * faithful passthrough of the wire's own presence bits, not a synthesized
 * heuristic.
 */
typedef struct {
    bool has_battery_level;
    uint32_t battery_level; /* 0-100, >100 conventionally "powered" — same convention as mc_nodeinfo_t.battery_level */

    bool has_channel_utilization;
    float channel_utilization; /* percent, 0-100 (Meshtastic's own convention: airtime pct of the primary channel) */

    bool has_air_util_tx;
    float air_util_tx; /* percent, 0-100 (this node's own TX airtime pct) */

    bool has_uptime_seconds;
    uint32_t uptime_seconds; /* the REPORTING node's own uptime, not ours */
} mc_telemetry_t;

/** Counts of frames/packets we saw but didn't fully decode, per spec
 * ("Everything else skipped silently but counted"). */
typedef struct {
    uint32_t frames_ok;         /* frames the framer completed */
    uint32_t frames_resynced;   /* garbage-prefix or oversize-len events */
    uint32_t decode_errors;     /* frame parsed as protobuf but malformed */
    uint32_t decode_skipped;    /* out-of-decode-scope FromRadio/portnum */
    uint32_t reconnects;        /* auto-reconnect attempts started */

    /* [api] debt/S15c-handshake-stall (bench finding, 2026-09-13): how
     * many times a want_config request has been RE-SENT because the
     * handshake it belongs to stalled without a config_complete
     * (MC_HANDSHAKE_TIMEOUT_MS, below). Since boot, across every
     * handshake — not a per-handshake gauge (that counter is private and
     * resets each handshake; this one only ever grows, like every other
     * member here).
     *
     * Distinct from `reconnects`, and the distinction is the whole
     * diagnostic value: `reconnects` counts times the link was declared
     * DEAD and re-dialled from scratch; this counts times the link was
     * ALIVE (frames arriving, framer happy) but the session handshake on
     * top of it went unanswered. A bench session showing frames_ok
     * climbing, decode_errors 0, reconnects flat and THIS climbing is
     * precisely the "radio is fine, PhoneAPI session is not" shape that
     * used to be invisible. */
    uint32_t handshake_retries;

    /* [api] debt/link-churn-2026-09-16: `mc_framer_t.timeout_discards`
     * (mc_framing.h — see MC_FRAMER_RESYNC_TIMEOUT_MS's own doc comment
     * for the full mechanism), since boot. A frame that legitimately
     * began (first magic byte matched) but then went more than the
     * timeout without another byte — most plausibly a light-sleep byte
     * gap, per docs/specs/S26-device-lifecycle.md's own statement that
     * inbound UART bytes are lost during light sleep — was discarded
     * rather than spliced with whatever arrived next. Distinct from
     * `frames_resynced` (which counts garbage-prefix/oversize-len events,
     * a different failure shape — see that field's own comment): this
     * counts a frame that started well but stalled, not noise the framer
     * never mistook for a frame at all. Read against `decode_errors`: a
     * session where THIS climbs while `decode_errors` stays flat is the
     * fix working as intended — stalls caught and discarded cleanly
     * (costing exactly the one interrupted frame) instead of silently
     * corrupting a `pb_decode()` (which used to also cost the frame
     * AFTER it, the one whose header got spliced in). `decode_errors` can
     * of course still climb for reasons unrelated to a stall — a
     * genuinely malformed message is still a genuinely malformed message
     * (mc_client.h's own decode_errors doc comment lists the other
     * sources) — this field only rules IN the stall mechanism, it cannot
     * rule other causes out. */
    uint32_t frames_timeout_discarded;
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
     * [api] bench finding 2026-09-14 — fires for a LIVE `NODEINFO_APP`
     * `MeshPacket` (portnum 4), as opposed to `on_node`'s want_config
     * NodeInfo REPLAY (`FromRadio.node_info`, a synthesized nodeDB dump
     * with no reception time of its own — see `on_node`'s doc comment
     * and `docs/specs/S02-core-crew.md`'s 2026-09-13 amendment §B for
     * why the replay must never be treated as a live observation).
     *
     * Fires for EVERY live NodeInfo this radio decodes — a crew member's
     * own periodic re-announcement, or the reply this library's
     * `mc_send_nodeinfo_request` solicited; this event does not
     * distinguish the two, because the payload itself (a bare
     * `meshtastic_User`) carries nothing that would let it. `from` is
     * the reporting node; `user` carries only the two name fields a
     * live NodeInfo payload actually has (see `mc_user_reply_t`'s own
     * doc comment for why it is not `mc_nodeinfo_t` reused). Both
     * `has_long_name`/`has_short_name` may be false on a User that
     * genuinely left both unset — that is a legitimate (if unusual)
     * reply, not a decode failure, and this event still fires so a
     * caller's own "did I get an answer at all" bookkeeping (e.g. this
     * puck's NodeInfo-request throttle) sees it.
     *
     * `from == 0` never fires this (mirrors `on_rx_meta`'s own "nobody
     * to attribute the reading to" rule) — not a NodeInfo-specific
     * restriction, `mc_process_mesh_packet` never has a `from == 0` to
     * dispatch from in the first place.
     */
    void (*on_nodeinfo_reply)(void *u, uint32_t from, mc_user_reply_t const *user);

    /**
     * [api] Diagnostics — fires for a `Telemetry` message (TELEMETRY_APP,
     * portnum 67) carrying a `device_metrics` variant. `from` is the
     * reporting node — for THIS puck's comms brain reporting on itself,
     * `from == my_node_id` (the same self-attribution rule `on_position`/
     * `on_node` already give callers; no separate self-flag needed).
     * Fires for every node the mesh carries device telemetry for, not
     * only self — a caller building a "my node's own link/radio health"
     * view (the DIAGNOSTICS screen this event exists for) filters on
     * `from == my_node_id` itself, the same way `shell_ev_rx_meta`
     * already filters `on_rx_meta` for crew-specific purposes. Never
     * fires for a `Telemetry` carrying any OTHER variant (environment,
     * power, local stats, ...) — those decode fine but this library
     * currently has nothing to report about them, mirroring the
     * pre-existing "well-formed, not this library's concern yet"
     * precedent already documented on the ADMIN_APP dispatch branch in
     * mc_client.c.
     */
    void (*on_telemetry)(void *u, uint32_t from, mc_telemetry_t const *t);

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

    /**
     * [api] A02 slice D — one row of the radio's channel table, from the
     * `want_config` reply stream (`FromRadio.channel`, tag 10), which
     * this library used to drop on the floor.
     *
     * Fires once per configured channel, in the order the radio sends
     * them, between `on_my_info` and `config_complete`. A caller
     * resolving "which index is my crew channel" should clear whatever
     * it resolved when `on_state(MC_STATE_HANDSHAKE)` fires and rebuild
     * from these — the table is re-sent on every handshake, including
     * the one after a reboot or an admin write, which is exactly when a
     * cached index can have gone stale.
     *
     * `psk_len == 0` means the channel stated no key. That is not the
     * same as an all-zero key and must not be read as one: Meshtastic
     * uses a 1-byte PSK as shorthand for a well-known default key, and a
     * channel row whose key this library could not read is reported with
     * no key rather than with a plausible-looking wrong one.
     *
     * `ch` is valid only for the duration of the call.
     */
    void (*on_channel)(void *u, mc_channel_t const *ch);

    /**
     * [api] A02 slice D2 — the radio's LoRa REGION, from the
     * `want_config` reply stream (`FromRadio.config` carrying a
     * `lora` variant, `Config.LoRaConfig.region`).
     *
     * Exists for exactly one question, and deliberately carries nothing
     * else from `LoRaConfig`: **is the region UNSET (0)?** A02 §1.7
     * forbids Firefly from ever writing `lora_config` or guessing a
     * region from anywhere, so the only honest thing a crew-start flow
     * can do on an unprovisioned radio is stop and say so
     * ("Set the radio region on the phone first"). It cannot say that
     * without being able to see the region, and before this the client
     * dropped the whole `config` frame on the floor.
     *
     * `region` is the raw `RegionCode` enum value as the radio reported
     * it, NOT translated into a Firefly vocabulary: this library has no
     * business deciding which of the two dozen regions are "fine", and
     * a caller that only needs `!= 0` should not have to trust a
     * translation table to tell it that.
     *
     * Re-sent on every handshake, like the channel table — a caller
     * should clear what it cached when `on_state(MC_STATE_HANDSHAKE)`
     * fires rather than trusting a value from before a reboot.
     */
    void (*on_lora_region)(void *u, uint32_t region);

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

/* Handshake watchdog — debt/S15c-handshake-stall (bench finding,
 * 2026-09-13: the link sat in RECONNECTING for 20+ minutes with
 * frames_ok climbing, decode_errors 0 and reconnects stuck at 1; only a
 * reset recovered it).
 *
 * Why a SECOND watchdog was needed. mc_tick()'s only liveness check used
 * to be "no bytes read for 30 s" (`last_rx_ms`), and `last_rx_ms` is
 * refreshed by ANY inbound byte regardless of state. So a handshake that
 * never gets answered — the want_config request lost on the wire, or the
 * config_complete reply lost, while the comms brain keeps emitting
 * ordinary NodeInfo/packet traffic the client happily decodes and
 * dispatches — kept its own watchdog fed forever. Nothing re-sent
 * want_config, and nothing could: the client only ever issued one per
 * handshake. It is the exact same blind spot `FromRadio.rebooted`
 * handling was added for (see mc_tick()'s doc comment), but reached
 * without any reboot to announce, so no explicit tell exists to key off.
 *
 * The fix is a deadline on the handshake itself, measured from when the
 * want_config went out rather than from the last byte in. Bench numbers
 * for scale: a cold-boot handshake completes in ~20 ms and a deliberate
 * re-handshake in ~0.9 s, so 10 s is ~11x the SLOWEST answer actually
 * observed (and ~500x the fastest) — enough headroom that it can only
 * fire on a genuinely unanswered request, never on a slow one (even a
 * whole-mesh NodeInfo dump drains in a handful of ticks, see
 * MC_TICK_MAX_FRAMES).
 *
 * Review note (PR #296) — where that headroom is THINNEST, so a future
 * change to either constant knows what it is spending. The deadline is
 * wall-clock, but the drain is per-tick-capped (MC_TICK_MAX_FRAMES
 * frames per mc_tick), so the real budget is "frames the caller can
 * drain in 10 s", which depends on the caller's tick cadence. Awake, the
 * esp32s3 target ticks at its ~50 Hz frame cadence and the budget is
 * thousands of frames. Asleep it is not: S26f light sleep wakes on a
 * 1500 ms timer and runs ONE ff_shell_tick per wake, so the budget falls
 * to 32 frames/1.5 s ~= 213 frames in 10 s. A want_config replay is
 * roughly 30 frames of my_info/config/moduleConfig/channels plus one
 * NodeInfo per known node, so a reconnect that lands while the puck is
 * asleep still clears with ~1.6x margin against Meshtastic's default
 * 100-node ESP32 nodeDB — arithmetic from these constants, not a bench
 * measurement, and the tightest case found in review. If either the
 * nodeDB cap or the sleep wake period grows, re-do this sum before
 * assuming 10 s is still generous: a deadline that fires mid-dump would
 * restart the dump rather than rescue it. */
#define MC_HANDSHAKE_TIMEOUT_MS 10000u

/* Re-sends of want_config before a stalled handshake is escalated to a
 * full DISCONNECTED + backoff reconnect (which then starts a brand new
 * handshake, with a fresh nonce and this budget reset). Bounded, per the
 * house rule that every retry loop must be — but the ESCALATION is not,
 * so the link still never gives up while the radio is alive: the outer
 * reconnect loop is what "never gives up" means here (see
 * ff_shell.c's shell_ev_state comment), and this budget only bounds how
 * long one handshake attempt is given before that outer loop takes over. */
#define MC_HANDSHAKE_MAX_RETRIES 3u

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

    /* Our OWN owner names and hardware model, as most recently reported
     * by the radio itself (the want_config nodeDB entry for
     * `my_node_id`, refreshed by an `AdminMessage.get_owner_response`).
     * Radio-sourced, never invented: each stays "" / UNSET until the
     * radio says otherwise, and a later report that omits a field never
     * blanks a name already learned. Read by
     * `mc_send_nodeinfo_request`, whose payload IS a `meshtastic_User`
     * that the peer writes straight into its nodeDB — see that
     * function's doc comment for why an EMPTY one is not an option. */
    char     my_long_name[MC_NAME_MAX];
    char     my_short_name[MC_NAME_MAX];
    uint32_t my_hw_model;

    uint32_t want_config_id;
    uint32_t rng_state;
    uint32_t next_packet_id;

    uint32_t last_rx_ms;
    uint32_t last_heartbeat_ms;
    uint32_t reconnect_at_ms;
    bool reconnect_pending;

    /* Handshake watchdog state (see MC_HANDSHAKE_TIMEOUT_MS above).
     * `want_config_sent_ms` is when the CURRENT want_config frame went
     * onto the wire — deliberately not `last_rx_ms`, which unrelated
     * inbound traffic keeps refreshing and which is exactly why the
     * stall was invisible. `handshake_retry_count` is how many re-sends
     * this ONE handshake has spent out of MC_HANDSHAKE_MAX_RETRIES; it
     * resets on every fresh handshake, unlike the monotonic
     * mc_stats_t.handshake_retries a caller reads. */
    uint32_t want_config_sent_ms;
    uint32_t handshake_retry_count;

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

    /* [api] A02 slice D2 — the channel table as of the CURRENT
     * handshake, for `mc_client_get_channel_snapshot`. Cleared whenever
     * a fresh want_config starts, so a row here is always something
     * THIS session's radio said, never a memory of a previous one.
     * Bounded by MC_CHANNEL_MAX; a row with an index past that is
     * dispatched to `on_channel` like any other but is not cached
     * (there is no slot for it, and silently rewriting somebody else's
     * slot would be worse than not remembering). */
    mc_channel_t channels[MC_CHANNEL_MAX];
    bool         channel_seen[MC_CHANNEL_MAX];
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
 * lost when the cap lands mid-chunk (see mc_client_t.tick_carry_*).
 *
 * Reboot-session-loss handling (bench finding, 2026-09-06): a
 * `FromRadio.rebooted` frame from the comms brain (Meshtastic tells a
 * connected client explicitly when it just rebooted — e.g. a few seconds
 * after an admin write like `mc_send_set_owner`'s `set_owner`, per
 * Meshtastic's own `AdminModule::saveChanges`) is treated as an immediate
 * session loss: the client drops straight into a fresh want_config
 * handshake (`on_state(MC_STATE_HANDSHAKE)` fires, same as any other link
 * drop) rather than waiting for the 30s no-RX-bytes watchdog — which,
 * critically, does NOT reliably fire on its own here, because other
 * FromRadio traffic (queueStatus) keeps `last_rx_ms` advancing right
 * through the reboot even though the session on the other end is gone.
 * A caller that only watches `mc_state()`/`on_state` sees the ordinary
 * READY -> HANDSHAKE -> READY sequence around a reboot with no separate
 * event to handle.
 *
 * Handshake-stall handling (bench finding, 2026-09-13 — see
 * MC_HANDSHAKE_TIMEOUT_MS above for the full mechanism): the 30s
 * no-RX-bytes watchdog is likewise blind to a handshake that is never
 * ANSWERED, for the same reason — unrelated inbound traffic keeps
 * `last_rx_ms` advancing while no `config_complete` ever arrives, and
 * the link would sit in HANDSHAKE indefinitely. mc_tick() therefore also
 * enforces a deadline on the handshake itself, re-sending want_config
 * (same nonce) up to MC_HANDSHAKE_MAX_RETRIES times before escalating to
 * the ordinary reconnect path. Again nothing new to handle: a caller
 * watching `on_state` sees only the eventual HANDSHAKE -> READY, and
 * `mc_get_stats().handshake_retries` is there for a caller that wants to
 * log or display the re-asks. */
void mc_tick(mc_client_t *c, uint32_t now_ms);

/** Start (or restart) the want_config handshake. */
void mc_connect(mc_client_t *c);

/**
 * Broadcast or direct-message plain UTF-8 text. dest = MC_ADDR_BROADCAST
 * for the primary channel, which sends with `want_ack == false` (the
 * mesh gives a broadcast no delivery receipt at all); any other `dest`
 * sends with `want_ack == true` — a direct text is worth the mesh
 * stack's own retries and a routing ACK/NAK reply
 * (`mc_events_t.on_routing_ack`), unlike a best-effort broadcast.
 *
 * Returns 0 on success, negative on failure (not READY, utf8 too long
 * for MC_TEXT_MAX, encode/write failure).
 *
 * `out_packet_id` — outbox delivery status feature (2026-09-07 bench
 * finding: "sending when lost doesn't work" surfaced that a text send
 * had no way to correlate its own `on_routing_ack` reply, the same gap
 * `mc_send_set_owner`'s own `out_packet_id` closed for the NAME feature)
 * — is OPTIONAL (NULL-safe) and, on a successful send (return 0 only),
 * receives the outgoing `MeshPacket.id` this call used, so the caller
 * can match a later `on_routing_ack` to THIS specific text. Left
 * untouched on failure (return negative) — there is no in-flight packet
 * id to hand back. Set (harmlessly) even for a broadcast send, which
 * will never receive an ack for it to correlate against; callers that
 * don't need it may pass NULL. `[api]`: every implementer of
 * `ff_wiring_sender_t.send_text` (ff_wiring.h) in the tree was updated
 * in the same change to carry this parameter through.
 */
int mc_send_text(mc_client_t *c, uint32_t dest, char const *utf8, uint32_t *out_packet_id);

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

/**
 * mc_send_nodeinfo_request — `[api]` bench finding 2026-09-14: send a
 * `NODEINFO_APP` packet to `dest` carrying THIS node's own `User` with
 * `meshtastic_Data.want_response = true`, asking it to reply with its
 * own `User` right away instead of waiting for its next periodic
 * NodeInfo broadcast (Meshtastic's stock interval is hours-scale — see
 * `docs/specs/A02-crew-join.md` §4.4 / `docs/specs/S02-core-crew.md`'s
 * 2026-09-13 amendment §F, "a member admitted with no NodeInfo yet…
 * the name fills in when NodeInfo actually arrives").
 *
 * `dest` is the REMOTE node's id — unlike `mc_send_set_owner`/
 * `mc_send_get_owner_request`/`mc_client_set_channel`, this is not the
 * "local admin, no key needed" path (it doesn't ride `ADMIN_APP` at
 * all), so there is no `dest == self` requirement here.
 *
 * Verified against `meshtastic/firmware` tag `v2.7.26.54e0d8d`,
 * `src/modules/NodeInfoModule.cpp`: `handleReceivedProtobuf` records
 * every inbound NodeInfo it decodes into the local nodeDB regardless of
 * `want_response` (that part needs no request at all — it is what an
 * ordinary broadcast already does for every listener). The REQUEST half
 * is `MeshModule`'s own generic reply mechanism: returning `false` from
 * `handleReceivedProtobuf` while the incoming `Data.want_response` bit
 * is set causes the base class to call `NodeInfoModule::allocReply`,
 * which sends the local node's OWN `User` (`owner`) back to the
 * requester — subject to that node's own throttle (skips if it already
 * broadcast NodeInfo within the last ~10 minutes, or already answered
 * the SAME requester within the last 12 hours; both are the remote
 * node's concern, not this call's). This is the identical mechanism
 * `mc_send_get_owner_request` already documents in full for
 * `AdminModule::handleGetOwner` — same bit, same "an admin/info read
 * with the bit unset is silently answered with nothing" rule, a
 * different module.
 *
 * **The payload is our own `User`, and must never be an empty one.**
 * `NodeInfoModule::handleReceivedProtobuf` does not merely inspect the
 * request's payload for the `want_response` bit's sake — it decodes it
 * as a `meshtastic_User` and hands it to `NodeDB::updateUser`, which
 * overwrites the peer's stored record for our node with it (the one
 * escape is the PKI guard: a peer that already holds a 32-byte public
 * key for us drops any `User` that doesn't carry the matching key, and
 * replies anyway). So an "empty ask" is not payload-free — it is a wire
 * claim that this node has no name, and any peer without our key on
 * file believes it, blanking the very name this feature exists to
 * exchange. Sending our own names instead is also what Meshtastic's own
 * clients do for this exact request (`Meshtastic-Apple`'s
 * `exchangeUserInfo`, `NODEINFO_APP` + `wantResponse`, payload = the
 * local `User`).
 *
 * The names come from `mc_client_t.my_long_name`/`my_short_name` — what
 * the RADIO last said about its own owner (its want_config nodeDB entry
 * for `my_node_id`, refreshed by a `get_owner_response`), never
 * anything this library invented. Before the radio has said anything
 * they are empty, and an empty name is encoded as absent (proto3
 * implicit presence) rather than as a claim of emptiness.
 *
 * `is_licensed` is sent false, and `NodeInfoModule` discards the `User`
 * of a request whose `is_licensed` differs from its OWN owner's — so a
 * licensed (HAM) peer learns nothing from this payload. It still
 * REPLIES (the reply is `MeshModule::callModules`' job, run before that
 * module return value is even looked at), which is what this call is
 * for. Accepted, not worked around: this library has no honest way to
 * know the wearer's licence status, and inventing one to satisfy a
 * remote check would be a lie about a legal fact.
 *
 * Rides `NODEINFO_APP` (portnum 4). `want_ack` is false — the value of
 * this call is the reply's `User` payload
 * (`mc_events_t.on_node`/`.on_nodeinfo`, wherever the caller's decode
 * path lands it), not a routing-layer delivery receipt; a caller that
 * also wants that can watch `on_routing_ack` against `out_packet_id`.
 *
 * `out_packet_id` — mirrors every other send in this header (optional,
 * NULL-safe; receives the outgoing `MeshPacket.id` on success only) —
 * is included for parity even though no caller in this tree currently
 * correlates a NodeInfo reply back to a specific request id; a future
 * caller that wants to is not blocked on a signature change.
 *
 * Returns 0 on success, negative on failure (not READY, encode/write
 * failure).
 */
int mc_send_nodeinfo_request(mc_client_t *c, uint32_t dest, uint32_t *out_packet_id);

/**
 * mc_client_set_channel — [api] A02 slice D2: send a Meshtastic
 * `AdminMessage.set_channel`, writing one row of the radio's channel
 * table.
 *
 * This is the puck's half of "start a crew": the phone app has had
 * `applyChannelSet` since PR #274, and the puck — no camera, no phone —
 * needs the same power over its own comms brain. It rides the SAME
 * local-admin path `mc_send_set_owner` documents in full: ADMIN_APP
 * (portnum 6), `want_ack` true, `dest` must be this node's OWN id so
 * `AdminModule` takes the no-passkey-needed branch (`mp.from == 0` for
 * anything a locally-attached client submits — meshtastic/firmware
 * `v2.7.26`, `MeshService::handleToRadio` + `AdminModule::
 * handleReceivedProtobuf`). A `dest` that is not this node's own id
 * encodes and sends but lands on the passkey-required path on the
 * remote node and is rejected there; this library does not enforce it,
 * exactly as `mc_send_set_owner` does not.
 *
 * **What is written, and what is deliberately not.** Everything in
 * `*ch` and nothing else: index, name, psk, role, and a
 * `ModuleSettings` submessage that is ALWAYS emitted (A02 §1.5 — an
 * absent submessage reads as the default precision 32 on Meshtastic's
 * side, so a writer that left it out to mean 0 would silently ship full
 * precision). `uplink_enabled`/`downlink_enabled` are left false: a crew
 * is never bridged to MQTT. **`lora_config` — region, modem preset — is
 * not part of `mc_channel_t` and is never written by this call**
 * (A02 §1.7: locale is not location, a wrong region is a regulatory
 * violation, and the radio's region was set once at flash time).
 *
 * **Bounds, checked before anything is encoded.** `ch->index` must be
 * under `MC_CHANNEL_MAX`; `ch->psk_len` must be 0, 1, 16 or 32 (the only
 * lengths `ChannelSettings.psk` documents — a 7-byte key is not a
 * shorter key, it is a corrupt one); `ch->name` must be NUL-terminated
 * within its 12 bytes. Any violation returns negative and sends
 * nothing.
 *
 * **nanopb note.** `ChannelSettings.name` and `.psk` are unbounded in
 * channel.proto, so the generator emits them as `pb_callback_t` with no
 * callback installed — the same fact `mc_decode_channel_frame`'s comment
 * explains for the decode direction. This call installs ENCODE
 * callbacks over caller-owned buffers; unlike decode, a oneof's union is
 * not memset during encode, so the pointers survive to be used.
 *
 * Returns 0 on success, negative on failure (not READY, out-of-bounds
 * input, encode/write failure).
 *
 * `out_packet_id` is OPTIONAL (NULL-safe) and, on success only,
 * receives the outgoing `MeshPacket.id`, so a caller can correlate a
 * later `mc_events_t.on_routing_ack` against THIS write. Left untouched
 * on failure. Note that an ACK here means the admin frame was
 * delivered, NOT that the channel now holds what was asked for — the
 * only proof of that is a read-back (`mc_connect` a fresh handshake and
 * watch `on_channel`), which is exactly what `ff_crewstart`'s VERIFYING
 * state exists to do.
 */
int mc_client_set_channel(mc_client_t *c, uint32_t dest, mc_channel_t const *ch, uint32_t *out_packet_id);

/**
 * mc_client_get_channel_snapshot — [api] A02 slice D2: read back a row
 * of the channel table as the CURRENT handshake reported it.
 *
 * The client caches every `on_channel` row of the live handshake (up to
 * `MC_CHANNEL_MAX`) so a caller can ask "what is on index 0 right now"
 * at the moment it needs to know — which is immediately before
 * overwriting it. That is the pre-crew snapshot LEAVE restores
 * (`ff_crewstart.h`): without it, leaving a crew could only ever mean
 * "reset to the factory default", which is a different and usually
 * wrong thing to do to somebody's radio.
 *
 * The cache is cleared at the start of every want_config, so a row here
 * is always something THIS session's radio said. Returns false —
 * writing nothing — for a NULL argument, an index at or past
 * `MC_CHANNEL_MAX`, or an index the current handshake has not reported.
 * There is deliberately no "well, it's probably the default" fallback:
 * a snapshot nobody took is a snapshot that does not exist, and the
 * caller says so rather than restoring a guess.
 */
bool mc_client_get_channel_snapshot(mc_client_t const *c, uint8_t index, mc_channel_t *out);

mc_state_t mc_state(mc_client_t const *c);
mc_stats_t mc_get_stats(mc_client_t const *c);

#ifdef __cplusplus
}
#endif

#endif /* MC_CLIENT_H */
