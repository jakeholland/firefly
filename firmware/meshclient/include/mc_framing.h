/**
 * mc_framing.h — Meshtastic stream framing: 0x94 0xC3 [len_hi] [len_lo]
 * <protobuf>, byte-at-a-time, with silent resync on garbage, an oversize
 * declared length, or a stalled mid-frame byte gap (see
 * MC_FRAMER_RESYNC_TIMEOUT_MS). Internal to meshclient; mc_client.c drives
 * it, and the S03_AC1 unit tests exercise it directly (in-tree header, not
 * installed).
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

/**
 * debt/link-churn-2026-09-16 — mid-frame resync timeout.
 *
 * Before this fix, a frame that had begun (at least the first magic byte
 * matched) but then went silent mid-header or mid-payload — e.g. bytes
 * lost to an ESP32-S3 light-sleep window, `docs/specs/S26-device-
 * lifecycle.md`'s own "Not UART-wake: the RX bytes ... are lost" — was
 * never abandoned: the framer just kept writing whatever arrived NEXT
 * (often the following frame's own magic/length header) onto the frame in
 * progress, then reported that spliced Frankenstein blob as one
 * "complete" frame once the byte count happened to match. That corrupted
 * blob fails `pb_decode()` in `mc_client.c`, and — this is the expensive
 * part — it also consumes the next real frame's header, so ONE byte gap
 * was costing TWO frames (the truncated one and the one after it), not
 * one.
 *
 * The fix: if no byte arrives for more than this many milliseconds while
 * the framer is mid-frame (state != MC_FRAMER_START1), the frame in
 * progress is discarded and the state machine resets to START1 — the
 * byte that broke the silence is then fed fresh from START1, so if it
 * happens to be the next frame's own 0x94 (the exact splice scenario
 * above), that next frame is recognized and parsed correctly instead of
 * being eaten. Net effect: a byte gap now costs exactly the ONE frame it
 * actually interrupted, and the parser resynchronizes on the very next
 * header instead of the one after that.
 *
 * This is a per-byte INACTIVITY timeout, not a total-frame deadline: the
 * clock (`mc_framer_t.last_byte_ms`) is pushed forward on every byte
 * accepted while mid-frame, so a legitimately slow-but-continuous sender
 * (bytes trickling in with gaps shorter than this timeout, however long
 * the frame takes overall) is never punished — only an actual GAP longer
 * than the timeout triggers a discard.
 *
 * Value (a judgement call, not a derived constant — stated here rather
 * than left implicit): 50 ms. At 115200 baud one byte takes ~86.8 us
 * (8N1, 10 bits/byte), so consecutive bytes of a frame actually being
 * streamed arrive with a gap of at most a few byte-times plus whatever
 * scheduling jitter `mc_tick()`'s own read-then-feed loop adds — single-
 * digit milliseconds at most on this hardware, nowhere near 50 ms. 50 ms
 * is also comfortably ABOVE that jitter margin (never trips on a healthy,
 * merely-momentarily-delayed stream) yet far BELOW every timescale that
 * actually causes a real gap here: the light-sleep fast-window wake
 * period is 300 ms-1500 ms (`FF_IDLE_LIGHT_SLEEP_FAST_TIMER_MS`/
 * `_SLOW_TIMER_MS`, `ff_idle.h`) and the silence watchdog is 30 s
 * (`mc_client.c`) — so a stalled frame is discarded and resynchronized 6x
 * to 30x before the NEXT scheduled sleep-wake cycle could even begin
 * delivering the colliding frame that used to get spliced in. Neither
 * bound is derived from first principles; both are chosen with real
 * margin on both sides, per the task's own instruction not to
 * over-derive this number.
 */
#define MC_FRAMER_RESYNC_TIMEOUT_MS 50u

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

    /* debt/link-churn-2026-09-16 — mid-frame resync timeout (see
     * MC_FRAMER_RESYNC_TIMEOUT_MS above). `last_byte_ms` is the caller's
     * `now_ms` at the most recent byte accepted while state != START1;
     * meaningless (and unread) while state == START1, since the timeout
     * only ever applies to a frame already in progress. `timeout_discards`
     * is a COUNTER DISTINCT FROM `resync_count`: `resync_count` means
     * "garbage byte(s) seen while scanning, or an oversize declared
     * length" (mc_framer_feed's own long-standing contract, unchanged by
     * this fix); `timeout_discards` means "a frame that legitimately
     * began was abandoned because it stalled too long" — a different
     * fact a bench operator needs told apart from ordinary line noise, so
     * it gets its own field rather than being folded into resync_count's
     * existing, already-documented meaning. Both are surfaced via
     * `mc_stats_t` (mc_client.h) — see `frames_resynced`/
     * `frames_timeout_discarded`. */
    uint32_t last_byte_ms;
    uint32_t timeout_discards;
} mc_framer_t;

void mc_framer_init(mc_framer_t *f);

/**
 * Feed one byte into the framer.
 *
 * `now_ms` is the caller's own time base — same convention as the rest of
 * `mc_client`/`ff_clock_t`: the framer never calls a clock itself, only
 * ever compares timestamps the caller hands it. Used solely for the
 * mid-frame resync timeout (MC_FRAMER_RESYNC_TIMEOUT_MS above); a caller
 * that never expects a stalled frame (e.g. a one-shot fixture replay) can
 * pass any monotonically-nondecreasing value, including a constant, as
 * long as no single call-to-call gap while mid-frame exceeds the timeout.
 *
 * Returns true when a complete frame payload is available: *out points at
 * f->buf (valid until the next mc_framer_feed call) and *out_len is its
 * length (may be 0 for a degenerate zero-length frame). Returns false
 * otherwise, including while silently resyncing past garbage, an oversize
 * declared length (> MC_MAX_FRAME), or a stalled mid-frame gap —
 * `mc_framer_t.resync_count`/`timeout_discards` are incremented
 * respectively whenever those happen, for `mc_stats_t` reporting.
 */
bool mc_framer_feed(mc_framer_t *f, uint8_t byte, uint32_t now_ms, uint8_t const **out, uint16_t *out_len);

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
