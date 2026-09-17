/**
 * fuzz_mc_framing.c — fuzzes mc_framer_feed() directly: the raw byte-at-a-
 * time UART framer state machine (0x94 0xC3 [len_hi] [len_lo] <payload>),
 * including the "boot-garbage window" scenario docs/hardware/comms-brain.md
 * describes (the ROM boot banner's bytes landing on the same UART before
 * any real Meshtastic frame) — every input byte here is unconstrained, so
 * a run naturally spends most of its time in exactly that "garbage before
 * the first magic byte" state.
 *
 * Invariants checked on every completed frame (mc_framer_feed() returning
 * true), beyond "doesn't crash under ASan/UBSan":
 *   - out_len never exceeds MC_MAX_FRAME (the buffer mc_framer_t.buf's
 *     actual capacity) — a violation here would mean mc_framer_feed()
 *     wrote past its own accumulator.
 *   - the framer always returns to MC_FRAMER_START1 after completing (or
 *     giving up on) a frame, i.e. it never gets stuck.
 *   - resync_count and timeout_discards only ever increase, never
 *     wrap/underflow within one run (both are uint32_t event counters,
 *     not ring indices). `now_ms` is advanced by the harness on every
 *     byte (mostly 1ms, occasionally a jump past
 *     MC_FRAMER_RESYNC_TIMEOUT_MS) so the resync-on-stall path
 *     (mc_framing.h) gets exercised alongside the garbage/oversize-len
 *     paths.
 */
#include <stdint.h>
#include <stddef.h>

#include "mc_framing.h"

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    mc_framer_t f;
    mc_framer_init(&f);

    uint32_t last_resync = f.resync_count;
    uint32_t last_timeout_discards = f.timeout_discards;
    uint32_t now_ms = 0u;

    for (size_t i = 0; i < size; i++) {
        uint8_t const *out = NULL;
        uint16_t out_len = 0xFFFFu; /* poison, so a missed write is visible */

        /* Mostly advance by 1ms (well under MC_FRAMER_RESYNC_TIMEOUT_MS,
         * so most of the run still exercises ordinary framing), but
         * occasionally jump by well OVER the timeout so the resync-on-
         * stall path (mc_framing.h) gets fuzzed too, not just the
         * garbage/oversize-len paths the rest of this harness already
         * covered. */
        now_ms += ((data[i] % 8u) == 0u) ? (MC_FRAMER_RESYNC_TIMEOUT_MS + 25u) : 1u;

        bool got = mc_framer_feed(&f, data[i], now_ms, &out, &out_len);

        if (got) {
            if (out_len > MC_MAX_FRAME) {
                __builtin_trap();
            }
            if (out_len > 0 && out == NULL) {
                __builtin_trap();
            }
            if (f.state != MC_FRAMER_START1) {
                __builtin_trap();
            }
        }

        if (f.resync_count < last_resync) {
            __builtin_trap(); /* counter must never go backwards */
        }
        last_resync = f.resync_count;

        if (f.timeout_discards < last_timeout_discards) {
            __builtin_trap(); /* counter must never go backwards */
        }
        last_timeout_discards = f.timeout_discards;
    }

    /* Also exercise mc_frame_encode() with fuzzer-derived sizes — its own
     * bounds checks (payload_len > MC_MAX_FRAME, out_cap too small) are
     * pure arithmetic on attacker/caller-influenced lengths and cost
     * nothing extra to fuzz alongside the decode side. */
    if (size >= 4) {
        uint16_t declared_len = (uint16_t)((data[0] << 8) | data[1]);
        size_t cap_offset = (size_t)data[2] % 8u; /* under/over-size the cap a little */
        uint8_t out_buf[MC_MAX_FRAME + 8u];
        size_t out_cap = (cap_offset < sizeof(out_buf)) ? sizeof(out_buf) - cap_offset : 0u;
        uint16_t written = mc_frame_encode(out_buf, out_cap, data + 3, (uint16_t)(size - 3));
        (void)declared_len;
        if (written > 0 && (size_t)written > out_cap) {
            __builtin_trap();
        }
    }

    return 0;
}
