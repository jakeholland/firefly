# S04 · firefly protocol — app-layer packets

## Purpose
Firefly-specific messages between pucks, riding Meshtastic as opaque payloads on a private portnum. Versioned from day one; pucks of different firmware ages must coexist.

## Wire format
- **Portnum: 269** (private app range 256–511; chosen constant `FF_PORTNUM`).
- Payload: `[ver:1][type:1][body…]`, little-endian, max 200 B. `ver = 1`. Unknown ver or type ⇒ ignore silently (forward compat). Decode is strict: ver-1 bodies must be exactly their defined length; trailing bytes ⇒ ignore message. The 200 B cap is an encode/transport bound.
- Types:

| type | name | body | semantics |
|---|---|---|---|
| 0x01 | PULSE | — | "thinking of you"; haptic on paired receivers |
| 0x02 | FLARE | `[dur_s:2]` | come-find-me: receivers takeover-screen + lock arrow on sender for dur (default 300) |
| 0x03 | FLARE_END | — | sender cancelled |
| 0x04 | RALLY | `[lat:i32 1e-7][lon:i32 1e-7][name_len:1][name…≤24]` | set/replace crew rally point |
| 0x05 | RALLY_CLEAR | — | |
| 0x06 | STATUS | `[status_len:1][status…≤20]` | free-text status ("RAGING") |
| 0x07 | ACK_PING | `[nonce:4]` | reserved for delivery UX (v1.5) |

- Addressing: PULSE/FLARE to broadcast with **crew filtering receiver-side** (only react if sender is paired) — keeps airtime simple; RALLY/STATUS broadcast likewise. `want_ack` true for FLARE only.
- **Pairing v1 = channel membership + explicit crew list** (node ids marked paired in S02, persisted via S11). Spark-by-touch ceremony is v2; v1 pairs via Crew screen "add from heard nodes".

## Interface (`core/include/ff_proto.h`)
```c
int  ff_proto_encode_pulse(uint8_t *buf, size_t n);
int  ff_proto_encode_flare(uint8_t *buf, size_t n, uint16_t dur_s);
int  ff_proto_encode_rally(uint8_t *buf, size_t n, ff_latlon_t p, char const *name);
int  ff_proto_encode_status(uint8_t *buf, size_t n, char const *status);
// Decode: returns type or 0 on ignore; fills union out.
int  ff_proto_decode(uint8_t const *buf, size_t n, ff_proto_msg_t *out);
```
Pure encode/decode only — sending is `mc_send_private(…, FF_PORTNUM, …)` wired in app; reactions (haptics, takeover) live in S06/S10.

## Acceptance criteria
1. Round-trip every type: encode→decode equals input (table-driven, includes max-length names/status).
2. Truncated body, bad ver (0, 2), unknown type (0x7F) all return 0, never read OOB (ASan clean).
3. RALLY lat/lon 1e-7 encoding matches Meshtastic's own position convention (shared fixture with S03).
4. Encode into too-small buffer returns negative, writes nothing.
5. Golden bytes fixture committed (`tests/fixtures/ffproto_v1.bin`) so future versions must keep decoding v1.

## Slices
Single PR.

## Amendments
- **2026-08-22, PR #10:** Independent review flagged that `ff_proto_decode` accepted and silently discarded arbitrary trailing bytes past a message's declared body on every type (confirmed concretely: a 24-byte RALLY padded with 176 bytes of `0xEF` garbage to the 200 B cap still decoded successfully). Ruling (siding with the reviewer): decode is strict — each ver-1 type requires its exact body length; any trailing bytes ⇒ return 0 (reject). Rationale: the `ver` byte is the extensibility mechanism, not open-ended per-message padding within `ver=1`; on untrusted RF input, silently accepting unexplained trailing data can mask framing/concatenation bugs elsewhere rather than surface them. The 200 B cap stays as the encode/transport-level maximum only (a Meshtastic packet size bound), not a decode-side "trailing bytes are fine up to here" allowance.
