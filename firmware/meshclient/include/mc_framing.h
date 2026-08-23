/**
 * mc_framing.h — Meshtastic stream framing: 0x94 0xC3 [len_hi] [len_lo]
 * <protobuf>, byte-at-a-time, with silent resync on garbage or an oversize
 * declared length. Internal to meshclient; mc_client.c drives it, and the
 * S03_AC1 unit tests exercise it directly (in-tree header, not installed).
 *
 * No allocation: mc_framer_t is a plain struct the caller owns.
 */
#ifndef MC_FRAMING_H
#define MC_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max protobuf frame payload, per docs/specs/S03-meshclient.md ("max frame
 * 512 B"). A declared length greater than this is treated as garbage: the
 * framer resyncs instead of ever writing past the buffer. */
#define MC_MAX_FRAME 512u

#define MC_FRAME_MAGIC1 0x94u
#define MC_FRAME_MAGIC2 0xC3u

typedef enum {
    MC_FRAMER_START1 = 0, /* scanning for 0x94 */
    MC_FRAMER_START2,     /* have 0x94, scanning for 0xC3 */
    MC_FRAMER_LEN_HI,     /* have 0x94 0xC3, want length high byte */
    MC_FRAMER_LEN_LO,     /* have length high byte, want low byte */
    MC_FRAMER_PAYLOAD,    /* accumulating `expected` payload bytes */
} mc_framer_state_t;

typedef struct {
    mc_framer_state_t state;
    uint16_t expected;               /* declared payload length */
    uint16_t filled;                 /* bytes accumulated so far in PAYLOAD */
    uint8_t buf[MC_MAX_FRAME];       /* payload accumulator / last frame */
    uint32_t resync_count;           /* garbage-prefix or oversize-len events */
} mc_framer_t;

void mc_framer_init(mc_framer_t *f);

/**
 * Feed one byte into the framer.
 *
 * Returns true when a complete frame payload is available: *out points at
 * f->buf (valid until the next mc_framer_feed call) and *out_len is its
 * length (may be 0 for a degenerate zero-length frame). Returns false
 * otherwise, including while silently resyncing past garbage or an
 * oversize declared length (> MC_MAX_FRAME) — mc_framer_t.resync_count is
 * incremented whenever that happens, for mc_stats_t reporting.
 */
bool mc_framer_feed(mc_framer_t *f, uint8_t byte, uint8_t const **out, uint16_t *out_len);

/**
 * Build a framed buffer (0x94 0xC3 len_hi len_lo <payload>) into `out`,
 * which must have room for at least `payload_len + 4` bytes and
 * `payload_len <= MC_MAX_FRAME`. Returns the total framed length, or 0 on
 * failure (payload too large, or NULL args).
 */
uint16_t mc_frame_encode(uint8_t *out, size_t out_cap, uint8_t const *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* MC_FRAMING_H */
