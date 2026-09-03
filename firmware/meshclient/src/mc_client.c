#include "mc_client.h"

#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"

#include "meshtastic/mesh.pb.h"

/* -------------------------------------------------------------------- */
/* Small helpers                                                        */
/* -------------------------------------------------------------------- */

static uint32_t mc_now(mc_client_t const *c)
{
    return (c->clock.now_ms != NULL) ? c->clock.now_ms(c->clock.user) : 0u;
}

static void mc_set_state(mc_client_t *c, mc_state_t s)
{
    if (c->state == s) {
        return;
    }
    c->state = s;
    if (c->events.on_state != NULL) {
        c->events.on_state(c->events.user, s);
    }
}

static uint32_t mc_rand_next(mc_client_t *c)
{
    /* xorshift32 — not cryptographic, just enough entropy that two
     * clients started at different times don't collide on want_config_id. */
    uint32_t x = c->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->rng_state = x;
    return x;
}

static void mc_copy_name(char *dst, char const *src)
{
    /* src is nanopb's static char[MC_NAME_MAX] field (see
     * mc_nanopb.options) — already null-terminated within budget. */
    size_t n = strlen(src);
    if (n >= MC_NAME_MAX) {
        n = MC_NAME_MAX - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Translate Meshtastic's LocSource into the firefly-side enum. Deliberately
 * an explicit switch rather than a cast: the protobuf's numeric values must
 * not leak past this boundary (issue #33), and an unrecognized value from a
 * future firmware falls through to UNKNOWN instead of being asserted as
 * provenance we don't understand. */
static mc_loc_source_t mc_loc_source_from_pb(meshtastic_Position_LocSource src)
{
    switch (src) {
    case meshtastic_Position_LocSource_LOC_MANUAL:
        return MC_LOC_MANUAL;
    case meshtastic_Position_LocSource_LOC_INTERNAL:
        return MC_LOC_INTERNAL;
    case meshtastic_Position_LocSource_LOC_EXTERNAL:
        return MC_LOC_EXTERNAL;
    case meshtastic_Position_LocSource_LOC_UNSET:
    default:
        /* LOC_UNSET and "field absent" are the same bytes on the wire
         * (proto3 implicit presence), and both mean "sender stated no
         * provenance". No information is lost by folding them. */
        return MC_LOC_UNKNOWN;
    }
}

static void mc_position_from_pb(meshtastic_Position const *pb, bool has_rx_time, uint32_t rx_time,
                                 mc_position_t *out)
{
    memset(out, 0, sizeof(*out));
    out->lat = (double)pb->latitude_i * 1e-7;
    out->lon = (double)pb->longitude_i * 1e-7;
    out->has_altitude = pb->has_altitude;
    out->altitude_m = pb->altitude;
    out->time = pb->time;
    out->has_rx_time = has_rx_time;
    out->rx_time = rx_time;
    out->loc_source = mc_loc_source_from_pb(pb->location_source);

    /* precision_bits has implicit presence: a wire value of 0 and an absent
     * field are the same bytes, and >32 bits of a 32-bit coordinate is
     * untrusted-RF garbage, not a measurement — reported absent, never
     * clamped (a clamped 32 would assert "full precision" for a malformed
     * packet). See the field's doc comment in mc_client.h.
     *
     * Wire varints above UINT32_MAX never reach this gate: vendored nanopb
     * fails the whole Position decode ("integer too large") — a nonstandard
     * strictness (mainline protobuf C++ truncates, which would turn 2^32+32
     * into a present 32) that S03_AC11_precision_overflow_varint_yields_no_
     * position pins; if a decoder change breaks that test, add a pre-decode
     * guard here rather than deleting the test. */
    out->has_precision_bits = (pb->precision_bits >= 1u) && (pb->precision_bits <= 32u);
    if (out->has_precision_bits) {
        out->precision_bits = pb->precision_bits;
    } /* else: left zeroed by the memset above, benign for flag-ignoring callers */
}

/**
 * Decide whether our radio heard `pkt`'s sender directly.
 *
 * `has_decoded_bitfield` says whether the packet's decoded Data carried the
 * explicit-presence `bitfield` member. That is the vendored protobuf's own
 * documented tell for "this sender runs firmware new enough to populate
 * hop_start" (mesh.pb.h, hop_start comment: firmware before 2.3.0 never set
 * hop_start, so hop_start == 0 must be read as unknown rather than direct
 * until the sender's bitfield — added in 2.5.0 — proves otherwise).
 *
 * Everything that isn't positively established lands on UNKNOWN. That
 * asymmetry is deliberate: a false DIRECT silently mis-attributes a relay's
 * signal strength to a distant friend, which is precisely the kind of
 * confident-but-wrong reading this project refuses to render.
 */
static mc_rx_path_t mc_rx_path_from_pkt(meshtastic_MeshPacket const *pkt, bool has_decoded_bitfield)
{
    if (pkt->via_mqtt) {
        /* Arrived over the internet. Whatever our radio did or didn't
         * measure, it was not this sender's transmission. */
        return MC_RX_PATH_INDIRECT;
    }

    if (pkt->hop_start > 0u) {
        if (pkt->hop_limit > pkt->hop_start) {
            return MC_RX_PATH_UNKNOWN; /* malformed — hops travelled is negative */
        }
        return (pkt->hop_limit == pkt->hop_start) ? MC_RX_PATH_DIRECT : MC_RX_PATH_INDIRECT;
    }

    /* hop_start == 0: genuinely a zero-hop-limit packet (which cannot have
     * been relayed, hence direct), or an old sender that never populated
     * the field. Only the sender's bitfield distinguishes them. */
    if (has_decoded_bitfield && pkt->hop_limit == 0u) {
        return MC_RX_PATH_DIRECT;
    }
    return MC_RX_PATH_UNKNOWN;
}

static void mc_emit_rx_meta(mc_client_t *c, meshtastic_MeshPacket const *pkt,
                             bool has_decoded_bitfield)
{
    if (c->events.on_rx_meta == NULL || pkt->from == 0u) {
        return;
    }

    mc_rx_meta_t m;
    memset(&m, 0, sizeof(m));

    /* Both readings below are decoded from unvalidated wire bytes, so both
     * get the same treatment: a value that is not physically a radio
     * reading is not a measurement, and is reported ABSENT rather than
     * passed on — or squeezed into range — as though we had measured it.
     * The bounds are deliberately far wider than any real radio (see
     * MC_RSSI_MIN_DBM / MC_SNR_MIN_DB in mc_client.h): the job is
     * excluding garbage, not second-guessing the radio.
     *
     * Reporting absent rather than saturating matters concretely for the
     * consumer this exists to serve: ff_crew's close-range predicate is
     * `rssi_dbm > -60dBm`, so a clamped INT16_MAX — like a truncated 0 —
     * would sail straight through it and fabricate a CLOSE lock out of a
     * malformed packet. `has_rssi == false` cannot. */
    if (pkt->has_rx_rssi && pkt->rx_rssi >= MC_RSSI_MIN_DBM && pkt->rx_rssi <= MC_RSSI_MAX_DBM) {
        m.has_rssi = true;
        m.rssi_dbm = (int16_t)pkt->rx_rssi; /* in-range by the test above */
    }

    /* rx_snr has implicit presence: exactly 0.0 is indistinguishable from
     * absent, so we report it as unknown. See the mc_rx_meta_t doc comment
     * — under-claim rather than fabricate.
     *
     * NaN needs its own test and is the dangerous case: it compares
     * unequal to *everything*, including 0.0f, so a bare `!= 0.0f` check
     * admits it and the library ends up asserting "this is a measurement"
     * for a non-number — untrusted RF input, silently poisoning every
     * downstream comparison (all false against NaN) and any running mean
     * over the trend window. `x == x` is false only for NaN and needs no
     * <math.h>. The range test then excludes infinities and absurd
     * magnitudes. NOTE: neither test survives -ffast-math/-Ofast, which
     * this project does not use and must not adopt without revisiting
     * this. */
    m.has_snr = (pkt->rx_snr == pkt->rx_snr) && (pkt->rx_snr != 0.0f) &&
                 (pkt->rx_snr >= MC_SNR_MIN_DB) && (pkt->rx_snr <= MC_SNR_MAX_DB);
    /* Left zeroed when absent, so a caller that ignores the flag reads 0
     * rather than NaN — the same way memset leaves rssi_dbm. */
    m.snr_db = m.has_snr ? pkt->rx_snr : 0.0f;

    m.rx_path = mc_rx_path_from_pkt(pkt, has_decoded_bitfield);

    c->events.on_rx_meta(c->events.user, pkt->from, &m);
}

/* Retry budget for a write() that returns 0 ("accepted nothing, try again
 * later" — see the mc_transport_t.write() contract in mc_client.h), NOT
 * an error. Measured in *calls*, not wall-clock time: mc_write_bytes()
 * has no clock to measure against — mc_client_t carries a ff_clock_t, but
 * it is read only at the mc_tick()/mc_begin_handshake()/mc_send_heartbeat()
 * call sites that already have a `now_ms` in hand, not down here, and this
 * loop has no yield point where wall time would meaningfully advance
 * between attempts anyway (no sleep, no tick boundary) — a millisecond-
 * resolution clock could read the same value for the whole loop, so a
 * wall-clock deadline would not actually bound anything. A call-count
 * budget bounds it unconditionally, matching the pattern
 * mc_transport_tcp.c's own write() already uses for its EAGAIN retries.
 *
 * 64 is chosen to comfortably outlast a momentary stall on a bounded
 * buffer (a UART TX ring draining at its own pace, S15) without turning a
 * single frame's worth of back-pressure into a spurious reconnect: at the
 * spec's ~50 Hz mc_tick() cadence, a caller retrying a handshake/heartbeat
 * frame gets called again in ~20 ms regardless, so this budget only needs
 * to cover one transport's worth of "still draining, not stuck" — not
 * span multiple ticks. A permanently full/broken buffer still fails in a
 * small, bounded number of calls instead of spinning forever. */
#define MC_WRITE_ZERO_RETRY_BUDGET 64u

static bool mc_write_bytes(mc_client_t *c, uint8_t const *buf, size_t len)
{
    size_t sent = 0;
    uint32_t zero_retries = 0;
    while (sent < len) {
        int n = c->transport.write(c->transport.io, buf + sent, len - sent);
        if (n < 0) {
            return false; /* hard transport failure — escalate immediately */
        }
        if (n == 0) {
            /* "Try again later", not an error (see mc_transport_t's
             * write() contract) — but bounded, so a permanently stuck
             * transport still fails instead of spinning forever. */
            zero_retries++;
            if (zero_retries >= MC_WRITE_ZERO_RETRY_BUDGET) {
                return false;
            }
            continue;
        }
        zero_retries = 0; /* progress made; reset the budget for any later stall */
        sent += (size_t)n;
    }
    return true;
}

static bool mc_send_frame(mc_client_t *c, uint8_t const *payload, uint16_t len)
{
    uint8_t framed[MC_MAX_FRAME + 4u];
    uint16_t total = mc_frame_encode(framed, sizeof(framed), payload, len);
    if (total == 0) {
        return false;
    }
    return mc_write_bytes(c, framed, total);
}

static void mc_fail_and_schedule_reconnect(mc_client_t *c, uint32_t now_ms)
{
    mc_set_state(c, MC_STATE_DISCONNECTED);
    c->reconnect_pending = true;
    c->reconnect_at_ms = now_ms + 2000u;
}

static void mc_begin_handshake(mc_client_t *c, uint32_t now_ms)
{
    mc_framer_init(&c->framer);
    c->want_config_id = mc_rand_next(c);
    if (c->want_config_id == 0u) {
        c->want_config_id = 1u; /* avoid an all-zero nonce */
    }
    c->last_rx_ms = now_ms;
    c->last_heartbeat_ms = now_ms;
    c->reconnect_pending = false;

    mc_set_state(c, MC_STATE_HANDSHAKE);

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    tr.payload_variant.want_config_id = c->want_config_id;

    uint8_t buf[32];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &tr) ||
        !mc_send_frame(c, buf, (uint16_t)os.bytes_written)) {
        mc_fail_and_schedule_reconnect(c, now_ms);
    }
}

static void mc_send_heartbeat(mc_client_t *c, uint32_t now_ms)
{
    c->last_heartbeat_ms = now_ms;

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;

    uint8_t buf[16];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &tr) ||
        !mc_send_frame(c, buf, (uint16_t)os.bytes_written)) {
        mc_fail_and_schedule_reconnect(c, now_ms);
    }
}

/* -------------------------------------------------------------------- */
/* Decode dispatch                                                      */
/* -------------------------------------------------------------------- */

static void mc_process_mesh_packet(mc_client_t *c, meshtastic_MeshPacket const *pkt)
{
    if (pkt->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        /* Encrypted — we have no keys, and decode scope v1 doesn't cover
         * it anyway. Counted, not an error. The radio still measured the
         * signal that carried it, though, so the reception metadata is
         * both valid and useful (see mc_events_t.on_rx_meta): an encrypted
         * packet is a perfectly good RSSI sample even when its contents
         * are not. Without the decoded Data we cannot inspect `bitfield`,
         * so hop_start == 0 stays MC_RX_PATH_UNKNOWN here. */
        mc_emit_rx_meta(c, pkt, false);
        c->stats.decode_skipped++;
        return;
    }

    meshtastic_Data const *d = &pkt->payload_variant.decoded;
    uint32_t portnum = (uint32_t)d->portnum;

    /* Fires before any payload event, per the on_rx_meta ordering
     * guarantee, and regardless of whether the portnum is in decode scope. */
    mc_emit_rx_meta(c, pkt, d->has_bitfield);

    if (portnum == (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP) {
        if (c->events.on_text != NULL) {
            char text[MC_TEXT_MAX + 1u];
            size_t n = d->payload.size;
            if (n > MC_TEXT_MAX) {
                n = MC_TEXT_MAX;
            }
            memcpy(text, d->payload.bytes, n);
            text[n] = '\0';
            c->events.on_text(c->events.user, pkt->from, pkt->to, text, n);
        }
    } else if (portnum == (uint32_t)meshtastic_PortNum_POSITION_APP) {
        meshtastic_Position pos = meshtastic_Position_init_zero;
        pb_istream_t is = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
        if (!pb_decode(&is, meshtastic_Position_fields, &pos)) {
            /* Genuine protobuf corruption: the only case decode_errors
             * counts. Mirrors the NodeInfo replay path (below), which
             * likewise counts nothing for a well-formed message that
             * simply lacks coordinates. */
            c->stats.decode_errors++;
        } else if (pos.has_latitude_i && pos.has_longitude_i) {
            mc_position_t out;
            mc_position_from_pb(&pos, pkt->has_rx_time, pkt->rx_time, &out);
            if (c->events.on_position != NULL) {
                c->events.on_position(c->events.user, pkt->from, &out);
            }
        }
        /* else: well-formed Position with no fix yet (both fields are
         * explicit-presence and proto3 implicit-presence means "absent"
         * and "0" are the same bytes) — a legitimate "no GPS fix yet"
         * broadcast, not corruption. Nothing decoded wrong here, so
         * nothing is counted: neither decode_errors (that would mislabel
         * an honest no-fix broadcast as malformed protobuf) nor
         * decode_skipped (the portnum *is* in decode scope; we understood
         * the message perfectly, it just had nothing to report). Silently
         * dropping the event is exactly what the NodeInfo path already
         * does for the same condition (`ni->has_position &&
         * ni->position.has_latitude_i && ni->position.has_longitude_i`,
         * with no else branch at all). */
    } else if (portnum >= MC_PORTNUM_PRIVATE_MIN && portnum <= MC_PORTNUM_PRIVATE_MAX) {
        if (c->events.on_private != NULL) {
            c->events.on_private(c->events.user, pkt->from, pkt->to, portnum, d->payload.bytes,
                                  d->payload.size);
        }
    } else {
        c->stats.decode_skipped++;
    }
}

static void mc_process_from_radio(mc_client_t *c, meshtastic_FromRadio const *fr)
{
    switch (fr->which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        c->my_node_id = fr->payload_variant.my_info.my_node_num;
        c->has_my_node_id = true;
        if (c->events.on_my_info != NULL) {
            c->events.on_my_info(c->events.user, c->my_node_id);
        }
        break;

    case meshtastic_FromRadio_node_info_tag: {
        /* Note: if this NodeInfo's User.long_name/short_name exceeds
         * MC_NAME_MAX, we never get here at all — nanopb already failed
         * pb_decode() on the whole FromRadio in mc_tick(), and that node's
         * num/position/battery are silently lost with it (counted only in
         * decode_errors). See the MC_NAME_MAX doc comment in mc_client.h. */
        meshtastic_NodeInfo const *ni = &fr->payload_variant.node_info;
        mc_nodeinfo_t out;
        memset(&out, 0, sizeof(out));
        out.node_num = ni->num;

        if (ni->has_user) {
            if (ni->user.long_name[0] != '\0') {
                out.has_long_name = true;
                mc_copy_name(out.long_name, ni->user.long_name);
            }
            if (ni->user.short_name[0] != '\0') {
                out.has_short_name = true;
                mc_copy_name(out.short_name, ni->user.short_name);
            }
            out.hw_model = (uint32_t)ni->user.hw_model;
        }

        if (ni->has_position && ni->position.has_latitude_i && ni->position.has_longitude_i) {
            out.has_position = true;
            mc_position_from_pb(&ni->position, false, 0, &out.position);
        }

        if (ni->has_device_metrics && ni->device_metrics.has_battery_level) {
            out.has_battery_level = true;
            out.battery_level = ni->device_metrics.battery_level;
        }

        out.last_heard = ni->last_heard;

        /* NodeInfo carries its own hop summary with *explicit* presence
         * (has_hops_away), so unlike MeshPacket.hop_start there is no
         * old-firmware ambiguity to resolve — absent simply means the
         * nodeDB never recorded one, which is UNKNOWN, not DIRECT.
         *
         * Deliberately NOT surfaced from this path: NodeInfo.snr. It is a
         * cached "SNR of the last message we heard from this node" with
         * implicit presence and no rx timestamp of its own — and this
         * path already hardcodes has_rx_time = false precisely because a
         * want_config replay carries no reception time. A signal reading
         * that cannot be timestamped cannot feed a 5-second trend window,
         * and exposing it would invite exactly the mistake this PR is
         * about: treating replayed history as a live measurement. Live
         * SNR arrives per-packet via on_rx_meta instead. */
        if (ni->via_mqtt) {
            out.rx_path = MC_RX_PATH_INDIRECT;
        } else if (ni->has_hops_away) {
            out.rx_path = (ni->hops_away == 0u) ? MC_RX_PATH_DIRECT : MC_RX_PATH_INDIRECT;
        } else {
            out.rx_path = MC_RX_PATH_UNKNOWN;
        }

        if (c->events.on_node != NULL) {
            c->events.on_node(c->events.user, &out);
        }
        break;
    }

    case meshtastic_FromRadio_config_complete_id_tag:
        if (c->state == MC_STATE_HANDSHAKE &&
            fr->payload_variant.config_complete_id == c->want_config_id) {
            mc_set_state(c, MC_STATE_READY);
        } else {
            c->stats.decode_skipped++;
        }
        break;

    case meshtastic_FromRadio_packet_tag:
        mc_process_mesh_packet(c, &fr->payload_variant.packet);
        break;

    default:
        c->stats.decode_skipped++;
        break;
    }
}

/* -------------------------------------------------------------------- */
/* Public API                                                            */
/* -------------------------------------------------------------------- */

void mc_init(mc_client_t *c, mc_transport_t t, mc_events_t ev, ff_clock_t const *clock)
{
    memset(c, 0, sizeof(*c));
    c->transport = t;
    c->events = ev;
    if (clock != NULL) {
        c->clock = *clock;
    }
    c->state = MC_STATE_DISCONNECTED;
    mc_framer_init(&c->framer);
    c->next_packet_id = 1u;

    uint32_t now = mc_now(c);
    uint32_t seed = now ^ (uint32_t)(uintptr_t)c ^ 0x9E3779B9u;
    c->rng_state = (seed != 0u) ? seed : 1u;
    c->last_rx_ms = now;
}

void mc_connect(mc_client_t *c)
{
    mc_begin_handshake(c, mc_now(c));
}

void mc_tick(mc_client_t *c, uint32_t now_ms)
{
    /* Bounded drain (MC_TICK_MAX_FRAMES, see its doc comment in
     * mc_client.h): stop once this call has decoded that many complete
     * frames, leaving anything still buffered in the transport for the
     * next call(s). Read one byte at a time — checked against the cap
     * *before* each read — so a byte is never pulled out of the transport
     * unless it is immediately fed to the framer; nothing is ever read
     * and then discarded, so nothing is lost between calls, and frame
     * order is naturally preserved (the framer/transport are simply
     * resumed where this call left off). */
    uint32_t frames_this_tick = 0;

    for (;;) {
        if (frames_this_tick >= MC_TICK_MAX_FRAMES) {
            break;
        }

        uint8_t byte;
        int n = c->transport.read(c->transport.io, &byte, 1);
        if (n < 0) {
            mc_fail_and_schedule_reconnect(c, now_ms);
            break;
        }
        if (n == 0) {
            break;
        }
        c->last_rx_ms = now_ms;

        uint8_t const *frame_buf = NULL;
        uint16_t frame_len = 0;
        if (mc_framer_feed(&c->framer, byte, &frame_buf, &frame_len)) {
            c->stats.frames_ok++;
            frames_this_tick++;

            meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
            pb_istream_t is = pb_istream_from_buffer(frame_buf, frame_len);
            if (pb_decode(&is, meshtastic_FromRadio_fields, &fr)) {
                mc_process_from_radio(c, &fr);
            } else {
                c->stats.decode_errors++;
            }
        }
    }

    if (c->state != MC_STATE_DISCONNECTED && (now_ms - c->last_rx_ms) >= 30000u) {
        mc_fail_and_schedule_reconnect(c, now_ms);
    }

    if (c->state == MC_STATE_DISCONNECTED && c->reconnect_pending &&
        (int32_t)(now_ms - c->reconnect_at_ms) >= 0) {
        c->stats.reconnects++;
        mc_begin_handshake(c, now_ms);
    }

    if (c->state != MC_STATE_DISCONNECTED && (now_ms - c->last_heartbeat_ms) >= 15000u) {
        mc_send_heartbeat(c, now_ms);
    }
}

static int mc_send_data_packet(mc_client_t *c, uint32_t dest, uint32_t portnum, uint8_t const *payload,
                                size_t len, bool want_ack)
{
    if (c->state != MC_STATE_READY) {
        return -1;
    }
    if (len > MC_TEXT_MAX) {
        return -1;
    }

    meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
    tr.which_payload_variant = meshtastic_ToRadio_packet_tag;

    meshtastic_MeshPacket *pkt = &tr.payload_variant.packet;
    pkt->id = c->next_packet_id++;
    pkt->to = dest;
    pkt->want_ack = want_ack;
    if (c->has_my_node_id) {
        pkt->from = c->my_node_id; /* proto3 implicit presence: 0 ⇒ omitted,
                                     * matching what the real device does
                                     * when the client hasn't told it yet. */
    }
    pkt->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    pkt->payload_variant.decoded.portnum = (meshtastic_PortNum)portnum;
    pkt->payload_variant.decoded.payload.size = (pb_size_t)len;
    if (len > 0) {
        memcpy(pkt->payload_variant.decoded.payload.bytes, payload, len);
    }

    uint8_t buf[MC_MAX_FRAME];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &tr)) {
        return -1;
    }
    if (!mc_send_frame(c, buf, (uint16_t)os.bytes_written)) {
        mc_fail_and_schedule_reconnect(c, mc_now(c));
        return -1;
    }
    return 0;
}

int mc_send_text(mc_client_t *c, uint32_t dest, char const *utf8)
{
    if (utf8 == NULL) {
        return -1;
    }
    size_t len = strlen(utf8);
    bool want_ack = (dest != MC_ADDR_BROADCAST);
    return mc_send_data_packet(c, dest, (uint32_t)meshtastic_PortNum_TEXT_MESSAGE_APP,
                                (uint8_t const *)utf8, len, want_ack);
}

int mc_send_private(mc_client_t *c, uint32_t dest, uint32_t portnum, uint8_t const *payload,
                     size_t len, bool want_ack)
{
    if (payload == NULL && len > 0) {
        return -1;
    }
    return mc_send_data_packet(c, dest, portnum, payload, len, want_ack);
}

int mc_send_position(mc_client_t *c, ff_latlon_t p)
{
    if (c->state != MC_STATE_READY) {
        return -1;
    }

    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = true;
    pos.latitude_i = (int32_t)(p.lat * 1e7 + (p.lat >= 0.0 ? 0.5 : -0.5));
    pos.has_longitude_i = true;
    pos.longitude_i = (int32_t)(p.lon * 1e7 + (p.lon >= 0.0 ? 0.5 : -0.5));

    uint8_t payload[32];
    pb_ostream_t os = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&os, meshtastic_Position_fields, &pos)) {
        return -1;
    }

    return mc_send_data_packet(c, MC_ADDR_BROADCAST, (uint32_t)meshtastic_PortNum_POSITION_APP, payload,
                                os.bytes_written, false);
}

mc_state_t mc_state(mc_client_t const *c)
{
    return c->state;
}

mc_stats_t mc_get_stats(mc_client_t const *c)
{
    mc_stats_t s = c->stats;
    s.frames_resynced = c->framer.resync_count;
    return s;
}
