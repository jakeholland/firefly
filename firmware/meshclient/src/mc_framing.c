#include "mc_framing.h"

#include <string.h>

void mc_framer_init(mc_framer_t *f)
{
    memset(f, 0, sizeof(*f));
    f->state = MC_FRAMER_START1;
}

bool mc_framer_feed(mc_framer_t *f, uint8_t byte, uint8_t const **out, uint16_t *out_len)
{
    switch (f->state) {
    case MC_FRAMER_START1:
        if (byte == MC_FRAME_MAGIC1) {
            f->state = MC_FRAMER_START2;
        }
        /* else: garbage before the first magic byte — not a resync, just
         * normal seeking. Stay in START1. */
        return false;

    case MC_FRAMER_START2:
        if (byte == MC_FRAME_MAGIC2) {
            f->state = MC_FRAMER_LEN_HI;
        } else if (byte == MC_FRAME_MAGIC1) {
            /* stay in START2: this byte could be the real START1 of a
             * frame that follows a stray 0x94. */
        } else {
            f->state = MC_FRAMER_START1;
            f->resync_count++;
        }
        return false;

    case MC_FRAMER_LEN_HI:
        f->expected = (uint16_t)(byte << 8);
        f->state = MC_FRAMER_LEN_LO;
        return false;

    case MC_FRAMER_LEN_LO:
        f->expected = (uint16_t)(f->expected | byte);
        if (f->expected > MC_MAX_FRAME) {
            /* Oversize declared length: never enter PAYLOAD (which would
             * need to write more than the buffer holds). Resync. */
            f->state = MC_FRAMER_START1;
            f->resync_count++;
            return false;
        }
        f->filled = 0;
        if (f->expected == 0) {
            /* Degenerate zero-length frame: complete immediately. */
            f->state = MC_FRAMER_START1;
            if (out) {
                *out = f->buf;
            }
            if (out_len) {
                *out_len = 0;
            }
            return true;
        }
        f->state = MC_FRAMER_PAYLOAD;
        return false;

    case MC_FRAMER_PAYLOAD:
        f->buf[f->filled++] = byte;
        if (f->filled == f->expected) {
            f->state = MC_FRAMER_START1;
            if (out) {
                *out = f->buf;
            }
            if (out_len) {
                *out_len = f->expected;
            }
            return true;
        }
        return false;

    default:
        f->state = MC_FRAMER_START1;
        return false;
    }
}

uint16_t mc_frame_encode(uint8_t *out, size_t out_cap, uint8_t const *payload, uint16_t payload_len)
{
    if (out == NULL || (payload == NULL && payload_len > 0)) {
        return 0;
    }
    if (payload_len > MC_MAX_FRAME) {
        return 0;
    }
    size_t total = (size_t)payload_len + 4u;
    if (total > out_cap) {
        return 0;
    }

    out[0] = MC_FRAME_MAGIC1;
    out[1] = MC_FRAME_MAGIC2;
    out[2] = (uint8_t)((payload_len >> 8) & 0xFFu);
    out[3] = (uint8_t)(payload_len & 0xFFu);
    if (payload_len > 0) {
        memcpy(out + 4, payload, payload_len);
    }
    return (uint16_t)total;
}
