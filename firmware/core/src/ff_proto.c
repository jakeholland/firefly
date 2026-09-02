#include "ff_proto.h"

#include <string.h>

/* -------------------------------------------------------------------- */
/* Little-endian byte packing helpers. Explicit byte-at-a-time (not a
 * struct cast or memcpy of a native int) so this is correct regardless of
 * host endianness/alignment — matters less on the sim host (x86/arm64 are
 * both LE) than it will on the wire between differently-built pucks, and
 * costs nothing. */
/* -------------------------------------------------------------------- */

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t get_u16le(uint8_t const *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(uint8_t const *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_i32le(uint8_t *p, int32_t v)
{
    put_u32le(p, (uint32_t)v);
}

static int32_t get_i32le(uint8_t const *p)
{
    return (int32_t)get_u32le(p);
}

/* RALLY lat/lon: same fixed-point convention as Meshtastic's own
 * meshtastic_Position.latitude_i/longitude_i (degrees * 1e7, round to
 * nearest, ties away from zero) — see mc_client.c's mc_send_position /
 * decode_position, spec S04 AC3 ("shared fixture with S03"). This module
 * cannot depend on meshclient (no meshclient dependency, see ff_proto.h),
 * so the formula is reproduced here rather than shared. */
static int32_t deg_to_i1e7(double deg)
{
    double scaled = deg * 1e7;
    scaled += (scaled >= 0.0) ? 0.5 : -0.5;
    return (int32_t)scaled;
}

static double i1e7_to_deg(int32_t v)
{
    return (double)v * 1e-7;
}

/* -------------------------------------------------------------------- */
/* Encoders                                                              */
/* -------------------------------------------------------------------- */

/* Writes the [ver:1][type:1] envelope. Caller has already verified `n` is
 * large enough for the envelope plus whatever body it's about to write. */
static void put_envelope(uint8_t *buf, uint8_t type)
{
    buf[0] = (uint8_t)FF_PROTO_VERSION;
    buf[1] = type;
}

/* Shared body for the empty-body types (FLARE_END/RALLY_CLEAR — RESERVED_01
 * used to be a third, but its encoder is retired, see ff_proto.h): just the
 * envelope, nothing else. */
static int encode_empty(uint8_t *buf, size_t n, uint8_t type)
{
    size_t total = FF_PROTO_ENVELOPE_LEN;
    if (buf == NULL || n < total) {
        return -1;
    }
    put_envelope(buf, type);
    return (int)total;
}

int ff_proto_encode_flare_end(uint8_t *buf, size_t n)
{
    return encode_empty(buf, n, (uint8_t)FF_PROTO_TYPE_FLARE_END);
}

int ff_proto_encode_rally_clear(uint8_t *buf, size_t n)
{
    return encode_empty(buf, n, (uint8_t)FF_PROTO_TYPE_RALLY_CLEAR);
}

int ff_proto_encode_flare(uint8_t *buf, size_t n, uint16_t dur_s)
{
    size_t total = FF_PROTO_ENVELOPE_LEN + 2u;
    if (buf == NULL || n < total) {
        return -1;
    }
    put_envelope(buf, (uint8_t)FF_PROTO_TYPE_FLARE);
    put_u16le(buf + 2, dur_s);
    return (int)total;
}

int ff_proto_encode_rally(uint8_t *buf, size_t n, ff_latlon_t p, char const *name)
{
    size_t name_len = (name != NULL) ? strlen(name) : 0u;
    if (name_len > FF_PROTO_RALLY_NAME_MAX) {
        return -1; /* name too long to represent */
    }

    size_t body_len = 4u + 4u + 1u + name_len;
    size_t total = FF_PROTO_ENVELOPE_LEN + body_len;
    if (buf == NULL || n < total) {
        return -1;
    }

    put_envelope(buf, (uint8_t)FF_PROTO_TYPE_RALLY);
    put_i32le(buf + 2, deg_to_i1e7(p.lat));
    put_i32le(buf + 6, deg_to_i1e7(p.lon));
    buf[10] = (uint8_t)name_len;
    if (name_len > 0u) {
        memcpy(buf + 11, name, name_len);
    }
    return (int)total;
}

int ff_proto_encode_status(uint8_t *buf, size_t n, char const *status)
{
    size_t len = (status != NULL) ? strlen(status) : 0u;
    if (len > FF_PROTO_STATUS_MAX) {
        return -1; /* status too long to represent */
    }

    size_t body_len = 1u + len;
    size_t total = FF_PROTO_ENVELOPE_LEN + body_len;
    if (buf == NULL || n < total) {
        return -1;
    }

    put_envelope(buf, (uint8_t)FF_PROTO_TYPE_STATUS);
    buf[2] = (uint8_t)len;
    if (len > 0u) {
        memcpy(buf + 3, status, len);
    }
    return (int)total;
}

/* -------------------------------------------------------------------- */
/* Decoder                                                               */
/* -------------------------------------------------------------------- */

int ff_proto_decode(uint8_t const *buf, size_t n, ff_proto_msg_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    if (buf == NULL || out == NULL) {
        return 0;
    }
    if (n < FF_PROTO_ENVELOPE_LEN || n > FF_PROTO_MAX_PAYLOAD) {
        return 0;
    }
    if (buf[0] != (uint8_t)FF_PROTO_VERSION) {
        return 0;
    }

    uint8_t type = buf[1];
    uint8_t const *body = buf + FF_PROTO_ENVELOPE_LEN;
    size_t body_len = n - FF_PROTO_ENVELOPE_LEN;

    switch (type) {
    case FF_PROTO_TYPE_RESERVED_01: /* was PULSE — still a real, successful
                                      * decode, see ff_proto.h's RESERVED_01
                                      * section; just no encoder any more. */
    case FF_PROTO_TYPE_FLARE_END:
    case FF_PROTO_TYPE_RALLY_CLEAR:
        /* Strict: these types define an empty body, so any body at all
         * (trailing bytes) is rejected — see ff_proto.h / spec Amendments. */
        if (body_len != 0u) {
            return 0;
        }
        out->type = type;
        return type;

    case FF_PROTO_TYPE_FLARE:
        if (body_len != 2u) {
            return 0;
        }
        out->type = type;
        out->body.flare.dur_s = get_u16le(body);
        return type;

    case FF_PROTO_TYPE_RALLY: {
        if (body_len < 9u) { /* lat(4) + lon(4) + name_len(1) */
            return 0;
        }
        uint8_t name_len = body[8];
        if (name_len > FF_PROTO_RALLY_NAME_MAX || body_len != 9u + (size_t)name_len) {
            return 0;
        }
        out->type = type;
        out->body.rally.pos.lat = i1e7_to_deg(get_i32le(body));
        out->body.rally.pos.lon = i1e7_to_deg(get_i32le(body + 4));
        if (name_len > 0u) {
            memcpy(out->body.rally.name, body + 9, name_len);
        }
        out->body.rally.name[name_len] = '\0';
        return type;
    }

    case FF_PROTO_TYPE_STATUS: {
        if (body_len < 1u) {
            return 0;
        }
        uint8_t status_len = body[0];
        if (status_len > FF_PROTO_STATUS_MAX || body_len != 1u + (size_t)status_len) {
            return 0;
        }
        out->type = type;
        if (status_len > 0u) {
            memcpy(out->body.status.text, body + 1, status_len);
        }
        out->body.status.text[status_len] = '\0';
        return type;
    }

    case FF_PROTO_TYPE_ACK_PING:
        if (body_len != 4u) {
            return 0;
        }
        out->type = type;
        out->body.ack_ping.nonce = get_u32le(body);
        return type;

    default:
        return 0; /* unknown type: forward-compat ignore */
    }
}
