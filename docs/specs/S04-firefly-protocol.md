# S04 · firefly protocol — app-layer packets

## Purpose
Firefly-specific messages between pucks, riding Meshtastic as opaque payloads on a private portnum. Versioned from day one; pucks of different firmware ages must coexist.

## Wire format
- **Portnum: 269** (private app range 256–511; chosen constant `FF_PORTNUM`).
- Payload: `[ver:1][type:1][body…]`, little-endian, max 200 B. `ver = 1`. Unknown ver or type ⇒ ignore silently (forward compat). Decode is strict: ver-1 bodies must be exactly their defined length; trailing bytes ⇒ ignore message. The 200 B cap is an encode/transport bound.
- Types:

| type | name | body | semantics |
|---|---|---|---|
| 0x01 | *(RESERVED)* | — | **retired 2026-09-02.** Was PULSE ("thinking of you"; haptic on paired receivers) — the device dropped the notion of a "pulse" (maintainer decision, see Amendments). No encoder exists for this value any more, and it must never be reassigned to a new type: an old puck in the field may still transmit it, and reuse would make that traffic misparse as something it never meant. `ff_proto_decode` still recognizes the empty-body shape and reports it as `FF_PROTO_TYPE_RESERVED_01` — a real, successful decode, not an error — so a receiver can honestly do nothing with it instead of treating a known-old frame as malformed input. |
| 0x02 | FLARE | `[dur_s:2]` | come-find-me: receivers takeover-screen + lock arrow on sender for dur (default 300) |
| 0x03 | FLARE_END | — | sender cancelled |
| 0x04 | RALLY | `[lat:i32 1e-7][lon:i32 1e-7][name_len:1][name…≤24]` | set/replace crew rally point |
| 0x05 | RALLY_CLEAR | — | |
| 0x06 | STATUS | `[status_len:1][status…≤20]` | free-text status ("RAGING") |
| 0x07 | ACK_PING | `[nonce:4]` | reserved for delivery UX (v1.5) |

- Addressing: FLARE to broadcast with **crew filtering receiver-side** (only react if sender is paired) — keeps airtime simple; RALLY/STATUS broadcast likewise. `want_ack` true for FLARE only.
- **Pairing v1 = channel membership + explicit crew list** (node ids marked paired in S02, persisted via S11). Spark-by-touch ceremony is v2; v1 pairs via Crew screen "add from heard nodes".

## Interface (`core/include/ff_proto.h`)
```c
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
- **2026-09-02, maintainer decision — PULSE retired end to end:** the device drops the notion of a "pulse" entirely, not just the outbound send path PR #129 already retired. Type 0x01 is now permanently RESERVED (`FF_PROTO_TYPE_RESERVED_01` in `ff_proto.h`) rather than freed for reuse, so an old puck's traffic can never misparse as a future type. `ff_proto_encode_pulse` is removed (nothing encodes 0x01 again); `ff_proto_decode` still recognizes 0x01's empty-body shape and returns it as a normal successful decode, not 0/failure — a well-formed frame from an old build is not malformed input. `app/ff_wiring.c`'s inbound handling drops a RESERVED_01 frame silently: no feed item, no S26 notify/banner (see S26's Notifications section) — it is honestly nothing, the same treatment FLARE_END/RALLY_CLEAR/ACK_PING already got. The sender's aliveness is still recorded: `mc_events_t.on_rx_meta` fires for every inbound MeshPacket regardless of portnum or payload (see `ff_shell.c`'s `shell_ev_rx_meta`), so presence/heard tracking already sees a retired frame's radio activity independent of what the decode path does with its content — no separate "poke" was needed. `ff_wiring_ctx_t` gains a bench-visible `retired_frame_count` (the `ff_wall_trust_rejected_count` convention: a counter tests/tooling can read, not a log call) so a dropped retired frame is observable without instrumentation. `ff_feed_kind_t`'s `FEED_PULSE` is removed outright (not reserved — feed items are never persisted to NVS and fixtures encode kinds by name, not number, so renumbering is safe); `ff_demofeed`'s generator (S23) never draws it, S26 notify/the inbox thread renderer drop the "pulsed you"/"pulsed the crew" wording and the amber pulse mark, and the two outbound-pulse intents `ff_shell.c` kept as a dead-but-tested programmatic seam since PR #129 (`FF_INTENT_INBOX_PULSE`, `FF_INTENT_INBOX_POPUP_PULSE`, `FF_WIRING_REPLY_PULSE`) are deleted along with their handlers — the outbound quick signal has been FLARE since #129 and nothing exercises the pulse seam any more.
