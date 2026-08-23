/**
 * ff_proto.h — S04: firefly protocol, app-layer packets riding Meshtastic
 * as opaque payloads on a private portnum.
 *
 * Spec: docs/specs/S04-firefly-protocol.md
 *
 * Pure encode/decode only — no I/O, no meshclient dependency (this module
 * must stay includable by anything, extraction-grade like meshclient
 * itself; the two just happen to be strangers to each other). Sending is
 * `mc_send_private(c, dest, FF_PORTNUM, buf, encoded_len, want_ack)` wired
 * up in the app layer; reactions (haptics, takeover-screen, etc.) live in
 * S06/S10.
 *
 * ## Wire format
 * `[ver:1][type:1][body...]`, little-endian, max FF_PROTO_MAX_PAYLOAD (200)
 * bytes total. `ver` is always FF_PROTO_VERSION (1) on encode. On decode,
 * an unrecognized `ver` or `type`, a body too short for its type, or a
 * total length outside [2, FF_PROTO_MAX_PAYLOAD] all mean "ignore silently"
 * (forward compat with newer/older pucks at the same festival) — decode
 * returns 0 and never reads past `buf[0..n)`.
 *
 * Fixed-body types (PULSE/FLARE_END/RALLY_CLEAR) and any trailing bytes
 * beyond what a variable-body type declares (RALLY's name, STATUS's text)
 * are ignored rather than rejected — this is what lets a minor-version
 * receiver decode a message a future minor version padded with extra
 * fields it doesn't understand yet.
 *
 * ## Deviations from the spec's interface sketch (see PR for detail)
 *  - The spec's `## Interface` code block only shows encoders for
 *    PULSE/FLARE/RALLY/STATUS, but the type table also defines FLARE_END
 *    and RALLY_CLEAR as empty-body messages every bit as real as PULSE —
 *    `ff_proto_encode_flare_end` / `ff_proto_encode_rally_clear` are added
 *    here, following the exact same shape as `ff_proto_encode_pulse`.
 *  - ACK_PING (type 0x07) is "reserved for delivery UX (v1.5)": its wire
 *    shape (`[nonce:4]`) is defined here and `ff_proto_decode` understands
 *    it (so a v1 puck that somehow receives one doesn't misparse it as
 *    unknown-type), but no `ff_proto_encode_ack_ping` is provided — nothing
 *    sends it yet, per spec.
 */
#ifndef FF_PROTO_H
#define FF_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "ff_latlon.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Meshtastic portnum this protocol rides on (private/experimental range,
 * 256-511 — see mc_client.h's MC_PORTNUM_PRIVATE_MIN/MAX). */
#define FF_PORTNUM 269u

/** Envelope version this build encodes and understands as "current". */
#define FF_PROTO_VERSION 1u

/** [ver:1][type:1] envelope size, before any body. */
#define FF_PROTO_ENVELOPE_LEN 2u

/** Whole-packet cap, per spec ("Payload: ... max 200 B"). Applies to both
 * what encode will ever produce and what decode will ever accept. */
#define FF_PROTO_MAX_PAYLOAD 200u

/** RALLY name: length-prefixed, at most this many bytes (spec: "name...≤24"). */
#define FF_PROTO_RALLY_NAME_MAX 24u

/** STATUS text: length-prefixed, at most this many bytes (spec: "status...≤20"). */
#define FF_PROTO_STATUS_MAX 20u

typedef enum {
    FF_PROTO_TYPE_PULSE = 0x01,       /* "thinking of you"; no body */
    FF_PROTO_TYPE_FLARE = 0x02,       /* come-find-me; [dur_s:2] */
    FF_PROTO_TYPE_FLARE_END = 0x03,   /* sender cancelled; no body */
    FF_PROTO_TYPE_RALLY = 0x04,       /* [lat:i32][lon:i32][name_len:1][name] */
    FF_PROTO_TYPE_RALLY_CLEAR = 0x05, /* no body */
    FF_PROTO_TYPE_STATUS = 0x06,      /* [status_len:1][status] */
    FF_PROTO_TYPE_ACK_PING = 0x07,    /* reserved, v1.5: [nonce:4] */
} ff_proto_type_t;

/** FLARE body: come-find-me duration. */
typedef struct {
    uint16_t dur_s;
} ff_proto_flare_t;

/** RALLY body: a crew rally point. `name` is always NUL-terminated on
 * decode, at most FF_PROTO_RALLY_NAME_MAX bytes plus the terminator. */
typedef struct {
    ff_latlon_t pos;
    char name[FF_PROTO_RALLY_NAME_MAX + 1];
} ff_proto_rally_t;

/** STATUS body: free-text status. `text` is always NUL-terminated on
 * decode, at most FF_PROTO_STATUS_MAX bytes plus the terminator. */
typedef struct {
    char text[FF_PROTO_STATUS_MAX + 1];
} ff_proto_status_t;

/** ACK_PING body (reserved, v1.5). */
typedef struct {
    uint32_t nonce;
} ff_proto_ack_ping_t;

/** Decoded message: `type` is one of ff_proto_type_t (mirrors
 * ff_proto_decode's return value); `body` is valid per `type` for
 * FLARE/RALLY/STATUS/ACK_PING and unused (zeroed) for the empty-body
 * types (PULSE/FLARE_END/RALLY_CLEAR). */
typedef struct {
    uint8_t type;
    union {
        ff_proto_flare_t flare;
        ff_proto_rally_t rally;
        ff_proto_status_t status;
        ff_proto_ack_ping_t ack_ping;
    } body;
} ff_proto_msg_t;

/**
 * Encoders. All write `[ver:1][type:1][body...]` into `buf` (capacity `n`)
 * and return the number of bytes written (>0) on success.
 *
 * Return a negative value and write nothing to `buf` if:
 *  - `buf` is NULL or `n` is too small for the encoded message, or
 *  - (rally/status only) the given string is longer than its max
 *    (FF_PROTO_RALLY_NAME_MAX / FF_PROTO_STATUS_MAX bytes).
 *
 * The size check always happens before any byte is written — a
 * too-small-buffer call never partially writes.
 */
int ff_proto_encode_pulse(uint8_t *buf, size_t n);
int ff_proto_encode_flare(uint8_t *buf, size_t n, uint16_t dur_s);
int ff_proto_encode_flare_end(uint8_t *buf, size_t n);
int ff_proto_encode_rally(uint8_t *buf, size_t n, ff_latlon_t p, char const *name);
int ff_proto_encode_rally_clear(uint8_t *buf, size_t n);
int ff_proto_encode_status(uint8_t *buf, size_t n, char const *status);

/**
 * ff_proto_decode — parse `[ver:1][type:1][body...]` from `buf` (`n` bytes).
 *
 * Returns the message type (a positive ff_proto_type_t value) and fills
 * `*out` on success. Returns 0 and leaves `*out` zeroed (never reads
 * outside `buf[0..n)`) if:
 *  - `buf` or `out` is NULL,
 *  - `n` is outside [FF_PROTO_ENVELOPE_LEN, FF_PROTO_MAX_PAYLOAD],
 *  - the version byte isn't FF_PROTO_VERSION,
 *  - the type byte is unrecognized, or
 *  - the body is too short for its type (including a declared
 *    name_len/status_len that would run past the end of `buf`).
 *
 * Trailing bytes beyond what a type's body needs are ignored, not
 * rejected (forward-compat padding room for future minor versions).
 */
int ff_proto_decode(uint8_t const *buf, size_t n, ff_proto_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FF_PROTO_H */
