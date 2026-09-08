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
 * `[ver:1][type:1][body...]`, little-endian. FF_PROTO_MAX_PAYLOAD (200)
 * bytes total is the encode/transport-level maximum (a Meshtastic packet
 * size bound, not a per-message body allowance). `ver` is always
 * FF_PROTO_VERSION (1) on encode.
 *
 * Decode is **strict**: an unrecognized `ver` or `type`, or a body that
 * isn't *exactly* the defined length for its type (fixed 0 for
 * RESERVED_01/FLARE_END/RALLY_CLEAR, exactly 2/4 for FLARE/ACK_PING, exactly
 * `9 + name_len` for RALLY, exactly `1 + status_len` for STATUS) — all
 * mean "ignore silently" (forward compat with newer/older pucks at the
 * same festival is handled by the `ver` byte, not by tolerating unexplained
 * trailing bytes within `ver=1`; see docs/specs/S04-firefly-protocol.md's
 * Amendments). Decode returns 0 and never reads past `buf[0..n)`. See
 * S04-firefly-protocol.md's Amendments (PR #10) for the ruling and
 * rationale — untrusted RF input silently swallowing trailing garbage
 * could mask a framing/concatenation bug elsewhere.
 *
 * ## RESERVED_01 (0x01) — retired PULSE, 2026-09-02
 * The device dropped the notion of a "pulse" (docs/specs/S04's
 * Amendments): there is no encoder for 0x01 any more and nothing in this
 * codebase ever produces it again. The TYPE VALUE stays permanently
 * reserved rather than freed for reuse — an old puck already in the field
 * may still transmit it, and a future type silently inheriting 0x01 would
 * make that old puck's traffic misparse as something it never meant.
 * `ff_proto_decode` still recognizes it (same empty-body strict shape
 * PULSE always had) and returns it as a normal, SUCCESSFUL decode — type
 * `FF_PROTO_TYPE_RESERVED_01`, not 0/failure — because a well-formed frame
 * from an old build is not malformed input; it just carries no content any
 * more. Callers see a real, positive type and can choose to do nothing
 * with it (see app/ff_wiring.c) without that decision reading as a decode
 * error or an unrecognized-type rejection.
 *
 * ## Deviations from the spec's interface sketch (see PR for detail)
 *  - The spec's `## Interface` code block only shows encoders for
 *    PULSE/FLARE/RALLY/STATUS, but the type table also defines FLARE_END
 *    and RALLY_CLEAR as empty-body messages every bit as real as PULSE
 *    used to be — `ff_proto_encode_flare_end` / `ff_proto_encode_rally_clear`
 *    are added here, following the same empty-body shape RESERVED_01's
 *    (now-removed) encoder once did.
 *  - ACK_PING (type 0x07) is "reserved for delivery UX (v1.5)": its wire
 *    shape (`[nonce:4]`) is defined here and `ff_proto_decode` understands
 *    it (so a v1 puck that somehow receives one doesn't misparse it as
 *    unknown-type), but no `ff_proto_encode_ack_ping` is provided — nothing
 *    sends it yet, per spec.
 *
 * ## PING / PONG (0x08 / 0x09) — S29 FIND mode (docs/specs/S29-radio-only.md)
 * Two new types, deliberately NOT a reuse of the existing reserved 0x07
 * ACK_PING — that type's documented shape/semantics ("delivery UX v1.5")
 * is a different feature; reusing it here would retroactively redefine a
 * wire value S04 already reserved for something else. PING is "how do
 * you hear me" — a direct-addressed, unicast probe, `want_ack = false`
 * (silence after several missed pings is itself the answer, not a
 * routing failure to retry). PONG is the receiver's reply, carrying the
 * RSSI/SNR *the replier's own radio* measured on the PING that prompted
 * it — "they hear us at -xx dBm," the one fact FIND cannot get any other
 * way (our own reading of THEM already flows through the ordinary
 * `mc_rx_meta_t`/`ff_crew_on_rssi` path for any direct packet, PONG
 * included — no new plumbing needed for that half).
 */
#ifndef FF_PROTO_H
#define FF_PROTO_H

#include <stdbool.h>
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
    FF_PROTO_TYPE_RESERVED_01 = 0x01, /* was PULSE ("thinking of you", no body),
                                        * retired 2026-09-02 — see this header's
                                        * "RESERVED_01" section. Never reassign
                                        * this wire value to a new type. */
    FF_PROTO_TYPE_FLARE = 0x02,       /* come-find-me; [dur_s:2] */
    FF_PROTO_TYPE_FLARE_END = 0x03,   /* sender cancelled; no body */
    FF_PROTO_TYPE_RALLY = 0x04,       /* [lat:i32][lon:i32][name_len:1][name] */
    FF_PROTO_TYPE_RALLY_CLEAR = 0x05, /* no body */
    FF_PROTO_TYPE_STATUS = 0x06,      /* [status_len:1][status] */
    FF_PROTO_TYPE_ACK_PING = 0x07,    /* reserved, v1.5: [nonce:4] */
    /* S29 FIND mode — see this header's "PING / PONG" section above. */
    FF_PROTO_TYPE_PING = 0x08,        /* [nonce:4] */
    FF_PROTO_TYPE_PONG = 0x09,        /* [nonce:4][rssi:2 i16][has_snr:1][snr_x10:2 i16] */
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

/** PING body (S29): the nonce a PONG echoes back, correlating a reply to
 * the probe that triggered it (a peer may see pings from more than one
 * sender in a session). */
typedef struct {
    uint32_t nonce;
} ff_proto_ping_t;

/** PONG body (S29): the echoed nonce, plus the RSSI/SNR the REPLIER's own
 * radio measured on the PING packet that prompted this reply — "they
 * hear us at -xx dBm". `snr_x10` is SNR*10 as a signed i16 (one decimal
 * place, matches `mc_rx_meta_t.snr_db`'s own float without shipping a
 * float on the wire); `has_snr` mirrors `mc_rx_meta_t.has_snr`'s own
 * presence rule (proto3 float 0.0 is indistinguishable from absent, so
 * this body carries its own explicit flag rather than re-deriving one —
 * see mc_client.h's `has_snr` doc comment for the full reasoning this
 * mirrors). `rssi_dbm` has no presence flag of its own: unlike SNR,
 * every PONG sender has SOME rssi reading for the PING it just decoded
 * (mc_rx_meta_t.has_rssi is false only for implausible/malformed radio
 * data — see mc_client.h's MC_RSSI_MIN/MAX_DBM plausibility gate — and a
 * replier that can't even trust its own RSSI reading for the packet it
 * is actively replying to has nothing honest to report; ff_shell.c's
 * PING auto-reply handler simply does not send a PONG in that case,
 * rather than encode a body claiming a reading that doesn't exist). */
typedef struct {
    uint32_t nonce;
    int16_t  rssi_dbm;
    bool     has_snr;
    int16_t  snr_x10;
} ff_proto_pong_t;

/** Decoded message: `type` is one of ff_proto_type_t (mirrors
 * ff_proto_decode's return value); `body` is valid per `type` for
 * FLARE/RALLY/STATUS/ACK_PING/PING/PONG and unused (zeroed) for the
 * empty-body types (RESERVED_01/FLARE_END/RALLY_CLEAR). */
typedef struct {
    uint8_t type;
    union {
        ff_proto_flare_t flare;
        ff_proto_rally_t rally;
        ff_proto_status_t status;
        ff_proto_ack_ping_t ack_ping;
        ff_proto_ping_t ping;   /* S29 */
        ff_proto_pong_t pong;   /* S29 */
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
int ff_proto_encode_flare(uint8_t *buf, size_t n, uint16_t dur_s);
int ff_proto_encode_flare_end(uint8_t *buf, size_t n);
int ff_proto_encode_rally(uint8_t *buf, size_t n, ff_latlon_t p, char const *name);
int ff_proto_encode_rally_clear(uint8_t *buf, size_t n);
int ff_proto_encode_status(uint8_t *buf, size_t n, char const *status);
/** S29 — see this header's "PING / PONG" section. */
int ff_proto_encode_ping(uint8_t *buf, size_t n, uint32_t nonce);
/** `has_snr` false writes `snr_x10` as 0 on the wire (never a fabricated
 * reading — the decoder honors `has_snr`, not the raw zero, exactly
 * like `mc_rx_meta_t`'s own has_snr/snr_db pair). */
int ff_proto_encode_pong(uint8_t *buf, size_t n, uint32_t nonce, int16_t rssi_dbm, bool has_snr, int16_t snr_x10);

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
 *  - the body length isn't *exactly* the defined length for its type
 *    (including a declared name_len/status_len that would run past the
 *    end of `buf`, or that leaves unexplained bytes after it).
 *
 * Strict: any trailing bytes beyond a type's exact defined body length are
 * rejected, not ignored — see ff_proto.h's Wire format section and
 * docs/specs/S04-firefly-protocol.md's Amendments (PR #10).
 */
int ff_proto_decode(uint8_t const *buf, size_t n, ff_proto_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FF_PROTO_H */
