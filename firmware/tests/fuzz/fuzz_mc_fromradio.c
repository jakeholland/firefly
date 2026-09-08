/**
 * fuzz_mc_fromradio.c — end-to-end fuzz of the byte stream a comms-brain
 * link actually produces: framer -> nanopb FromRadio decode ->
 * mc_process_from_radio() -> (for FromRadio.packet) mc_process_mesh_packet()
 * -> the per-portnum Position/Telemetry/AdminMessage/Routing/text decodes.
 * This is the one harness that reaches mc_client.c's *static*
 * mc_process_from_radio()/mc_process_mesh_packet() at all, since neither is
 * exported — going through the real public mc_tick() entry point is not a
 * workaround, it's the only way a caller ever reaches that code, on device
 * or here.
 *
 * Input encoding: byte 0 is a garbage-prefix length (mod 9, so 0-8 bytes)
 * fed to the framer BEFORE a real frame header — this is the "boot-garbage
 * window" docs/hardware/comms-brain.md describes (the ROM boot banner's
 * bytes landing on the same UART ahead of the first real Meshtastic
 * frame). Everything after the garbage span becomes the frame's payload
 * verbatim (capped to MC_MAX_FRAME) — i.e. the fuzzer's mutations land
 * directly on the FromRadio protobuf bytes, which is what actually lets
 * random mutation reach deep into the Position/Telemetry/Admin/Routing/
 * NodeInfo decode branches instead of needing to independently rediscover
 * a valid magic+length prefix by chance.
 *
 * The client is never told a want_config handshake ever completed for
 * most of the corpus (mc_connect() below starts the handshake but nothing
 * here forges a matching config_complete_id), so this also exercises
 * mc_process_from_radio() while c->state == MC_STATE_HANDSHAKE — real
 * scrambled/reordered wire traffic could just as easily arrive before
 * config_complete as after it.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mc_client.h"
#include "mc_framing.h"

typedef struct {
    uint8_t const *data;
    size_t size;
    size_t pos;
} fuzz_cursor_t;

static int fuzz_read(void *io, uint8_t *buf, size_t maxlen)
{
    fuzz_cursor_t *cur = (fuzz_cursor_t *)io;
    size_t remaining = cur->size - cur->pos;
    size_t n = (remaining < maxlen) ? remaining : maxlen;
    if (n > 0) {
        memcpy(buf, cur->data + cur->pos, n);
        cur->pos += n;
    }
    return (int)n;
}

static int fuzz_write(void *io, uint8_t const *buf, size_t len)
{
    (void)io;
    (void)buf;
    /* Pretend every outgoing byte (handshake/heartbeat frames this
     * library sends) was accepted immediately — this harness cares about
     * INBOUND decode robustness, not the write-backpressure retry loop
     * (that's mc_transport_uart_accept.h's own host-run test). */
    return (int)len;
}

static uint32_t s_now_ms;

static uint32_t fuzz_now_ms(void *user)
{
    (void)user;
    return s_now_ms;
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    if (size < 1) {
        return 0;
    }

    uint8_t garbage_n = (uint8_t)(data[0] % 9u); /* 0..8 */
    size_t pos = 1;
    if (pos + garbage_n > size) {
        garbage_n = (uint8_t)(size - pos);
    }
    uint8_t const *garbage = data + pos;
    pos += garbage_n;

    size_t payload_len = size - pos;
    if (payload_len > MC_MAX_FRAME) {
        payload_len = MC_MAX_FRAME;
    }
    uint8_t const *payload = data + pos;

    static uint8_t stream[8u + MC_MAX_FRAME];
    size_t slen = 0;
    memcpy(stream + slen, garbage, garbage_n);
    slen += garbage_n;
    stream[slen++] = MC_FRAME_MAGIC1;
    stream[slen++] = MC_FRAME_MAGIC2;
    stream[slen++] = (uint8_t)((payload_len >> 8) & 0xFFu);
    stream[slen++] = (uint8_t)(payload_len & 0xFFu);
    memcpy(stream + slen, payload, payload_len);
    slen += payload_len;

    fuzz_cursor_t cur = {.data = stream, .size = slen, .pos = 0};

    mc_events_t ev;
    memset(&ev, 0, sizeof(ev)); /* every on_* callback NULL-safe, see mc_client.c */

    ff_clock_t clock = {.now_ms = fuzz_now_ms, .user = NULL};
    mc_transport_t transport = {.write = fuzz_write, .read = fuzz_read, .io = &cur};

    mc_client_t c;
    s_now_ms = 0;
    mc_init(&c, transport, ev, &clock);
    mc_connect(&c);

    /* MC_TICK_MAX_FRAMES (32) frames dispatched per mc_tick() call, one
     * frame's worth of input here at most — a handful of ticks is more
     * than enough to fully drain `stream` (including any tick_carry_buf
     * leftover) regardless of where MC_TICK_READ_CHUNK boundaries fall. */
    for (int i = 0; i < 8; i++) {
        s_now_ms += 50u;
        mc_tick(&c, s_now_ms);
    }

    return 0;
}
