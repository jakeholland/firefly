# S03 · meshclient — embedded Meshtastic client library

## Purpose
A clean, reusable C library that makes this device a Meshtastic *client* — same role as the phone apps — over any byte transport. Wraps framing, the config handshake, node/position/message events, and sending. **Extraction-grade: no firefly includes.**

## Reference
Meshtastic client API docs + the archived Swift client in this repo's `archive/meshtastic-ios-app` branch (working handshake & framing to crib from). Protobufs: `meshtastic/protobufs` pinned submodule-free — vendored generated nanopb sources under `meshclient/proto/` with the generator script `meshclient/tools/gen_nanopb.sh`.

## Interface (`meshclient/include/mc_client.h`)
```c
typedef struct {           // byte transport vtable — UART, TCP, later BLE
  int  (*write)(void *io, uint8_t const *buf, size_t len);
  int  (*read) (void *io, uint8_t *buf, size_t maxlen);   // nonblocking, 0 = nothing
  void *io;
} mc_transport_t;

// Packet metadata Meshtastic already gives a client, translated at this
// boundary into firefly-side enums (issues #33, #35). Protobuf enum values
// never reach core/.
typedef enum { MC_LOC_UNKNOWN, MC_LOC_MANUAL,                 // asserted, not measured
               MC_LOC_INTERNAL, MC_LOC_EXTERNAL } mc_loc_source_t;
typedef enum { MC_RX_PATH_UNKNOWN, MC_RX_PATH_DIRECT,
               MC_RX_PATH_INDIRECT } mc_rx_path_t;

typedef struct {          // per-packet radio measurements, all presence-flagged
  bool has_rssi;  int16_t rssi_dbm;
  bool has_snr;   float   snr_db;
  mc_rx_path_t rx_path;   // rssi/snr attributable to `from` only when DIRECT
} mc_rx_meta_t;

typedef struct {
  void (*on_state)(void *u, mc_state_t s);                     // DISCONNECTED/HANDSHAKE/READY
  void (*on_node)(void *u, mc_nodeinfo_t const *n);            // id, names, hw, battery, rx_path
  void (*on_position)(void *u, uint32_t node, mc_position_t const *p);  // p.loc_source, p.precision_bits
  void (*on_text)(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len);
  void (*on_private)(void *u, uint32_t from, uint32_t to, uint32_t portnum,
                     uint8_t const *payload, size_t len);      // firefly protocol rides here
                     // `to` added by issue #123 ([api]): the MeshPacket's destination,
                     // delivered exactly as on_text delivers it (broadcast / node id /
                     // MC_ADDR_UNKNOWN), so 1:1 private traffic classifies DIRECT.
  void (*on_my_info)(void *u, uint32_t my_node_id);
  void (*on_rx_meta)(void *u, uint32_t from, mc_rx_meta_t const *m);  // every packet w/ a sender
  void *user;
} mc_events_t;

void mc_init(mc_client_t *c, mc_transport_t t, mc_events_t ev, ff_clock_t const *clock);
void mc_tick(mc_client_t *c, uint32_t now_ms);   // pump read/parse/heartbeat; call ~50 Hz
void mc_connect(mc_client_t *c);                  // starts want_config handshake
int  mc_send_text(mc_client_t *c, uint32_t dest, char const *utf8);      // dest=0xFFFFFFFF broadcast
int  mc_send_private(mc_client_t *c, uint32_t dest, uint32_t portnum,
                     uint8_t const *payload, size_t len, bool want_ack);
int  mc_send_position(mc_client_t *c, ff_latlon_t p);        // rarely needed; comms brain owns GPS
mc_state_t mc_state(mc_client_t const *c);
```

## Behavior
- **Framing:** stream protocol `0x94 0xC3 [len_hi] [len_lo] [protobuf FromRadio/ToRadio]`; resync on garbage by scanning for magic; max frame 512 B.
- **Handshake:** on connect send `ToRadio.want_config_id = random`; consume NodeInfo/Config/Channel dump; READY on `config_complete_id` match. Auto-reconnect with 2 s backoff if transport errors or 30 s silence (send heartbeat every 15 s).
- **Decode scope v1:** MyNodeInfo, NodeInfo, Position, MeshPacket(decoded: TEXT_MESSAGE_APP, POSITION_APP, PRIVATE range). Everything else skipped silently but counted (`mc_stats`).
- **nanopb** with static allocation; callback fields sized: names 40 B, text 237 B (Meshtastic max).
- **Packet metadata (added for #33/#35).** Meshtastic already hands a client more than the payload; this library surfaces it instead of dropping it, translated into firefly-side enums:
  - **Position provenance.** `Position.location_source` → `mc_position_t.loc_source`. MANUAL means the position was *asserted* (an installer typed it in), not measured — a fixed-position landmark re-broadcasts the same asserted point forever, so freshness is a category error for it (#33). `LOC_UNSET` and an absent field are the same bytes on the wire (proto3 implicit presence); both fold to `MC_LOC_UNKNOWN`, which is a distinct enum member, not a sentinel. Unrecognized future wire values also fold to UNKNOWN rather than leaking a raw number.
  - **Per-packet RSSI/SNR.** `MeshPacket.rx_rssi`/`rx_snr` → `on_rx_meta`, fired for **every** inbound packet naming a sender — including encrypted payloads and out-of-decode-scope portnums, since the radio measured the signal regardless. Breadth is deliberate: position broadcasts alone arrive on a multi-minute interval, far too slow for a 5 s RSSI trend window. Fires *before* the payload event for the same packet, so callers can correlate by `from` without buffering.
  - **Hop path is the qualifier, not a nicety.** RSSI is measured against whatever signal actually arrived — for a relayed packet that is the *relay's* transmission, not the originator's. `mc_rx_path_t` says whether the reading may be attributed to `from` at all. Derived from `hop_start`/`hop_limit`, plus the sender's `Data.bitfield` as the tell for firmware new enough to populate `hop_start` (pre-2.3.0 never did, so a bare `hop_start == 0` reads UNKNOWN, never DIRECT); `via_mqtt` forces INDIRECT. `NodeInfo` gets the same field from its explicit-presence `hops_away`.
  - **Unknown stays unknown.** Every added field is presence-flagged or has an explicit UNKNOWN member; none uses an in-band sentinel. One irreducible gap is documented rather than papered over: `rx_snr` has implicit presence, so a genuine 0.0 dB reading is byte-identical to an absent field and is therefore reported as `has_snr == false` — under-claiming rather than fabricating.
  - **Implausible is not a measurement.** Both readings are decoded from unvalidated wire bytes, so both are range-gated (`MC_RSSI_MIN_DBM`/`MC_SNR_MIN_DB` and friends, set far wider than any real radio) and reported **absent** when they fall outside — never clamped, never truncated. `rx_snr` additionally rejects NaN, which compares unequal to everything including `0.0f` and would otherwise pass a bare presence test while silently failing every downstream comparison. Saturating instead of absenting would be actively harmful here: `ff_crew`'s close-range predicate is `rssi_dbm > -60dBm`, which a clamped `INT16_MAX` or a truncated `0` both satisfy, fabricating a CLOSE lock out of a malformed packet.
  - **Coordinate precision (#47).** `Position.precision_bits` → `mc_position_t.has_precision_bits`/`precision_bits`, on both decode paths. Meshtastic channels truncate outgoing coordinates to the channel's `positionPrecision` high bits — 13 on the default public channel, a ~5.8 km grid that produced a measured 2.7 km error indistinguishable from a full fix in the coordinates alone; this field is the only wire-level tell. Present ⇒ 1..32. Implicit presence again: wire `0` is byte-identical to absent (and `0` means "position disabled" in channel config, so it never legitimately accompanies coordinates) — both read absent. A value `> 32` is untrusted-RF garbage and reads **absent, not clamped** (a clamp to 32 would assert full precision for a malformed packet). Absent is never "full precision" — it means the sender didn't say. Presence is *not* uniform across paths in practice (hardware-verified, 2× Heltec V3 @ 2.7.26): live packets stamp the field affirmatively even at full precision (`precisionBits: 32` observed on a `positionPrecision: 32` channel), while the same node's nodeDB row replays with the field absent on both truncated and full-precision channels — stock firmware's `PositionLite → Position` conversion never copies it. NodeInfo positions therefore typically read absent even for senders whose live packets state it, and the replay is the cold-boot path.
  - **Deliberately not surfaced:** `NodeInfo.snr`. It is a cached "SNR of the last message we heard" with implicit presence and no reception timestamp (the same path already hardcodes `has_rx_time = false` for want_config replays). A signal reading that cannot be timestamped cannot feed a trend window, and exposing it would invite treating replayed history as a live measurement.
- Transports shipped: `mc_transport_tcp` (POSIX, sim) and `mc_transport_uart` stub landing with S15.

## Acceptance criteria
1. Framing: byte-dribble (1 byte per read) and garbage-prefix fixtures both yield the framed protobuf exactly once; oversize len resyncs without overflow.
2. Handshake against recorded meshtasticd byte capture (`tests/fixtures/handshake.bin`) reaches READY and emits ≥1 on_node + on_my_info.
3. Position packet fixture → `on_position` with correct 1e-7 degree conversion and rx_time.
4. `send_text` produces a ToRadio frame that meshtasticd accepts (e2e, S14 nightly) and byte-golden matches fixture (unit).
5. `on_private` fires for portnum 256–511 with payload passed through untouched.
6. Silence 30 s → reconnect: state sequence READY→DISCONNECTED→HANDSHAKE observed with mock transport.
7. Library has zero includes from `core/` or `app/` except `ff_clock_t` (moves to a shared `platform/` header).
8. Fuzz smoke: 10k random-byte frames parse without crash/UBSan findings.
9. **Position provenance (#33).** Each `LocSource` value reaches `mc_position_t.loc_source` as its firefly-side counterpart, on both the live-packet and the NodeInfo-replay path. An **absent** field, an explicit `LOC_UNSET`, and an unrecognized future value all read `MC_LOC_UNKNOWN` — never `MC_LOC_INTERNAL`, i.e. "the sender said nothing" is never reported as a GPS measurement. `mc_nodeinfo_t.rx_path` follows `hops_away`'s explicit presence: absent reads UNKNOWN, not DIRECT.
10. **Per-packet radio metadata (#35).** `on_rx_meta` fires with `has_rssi`/`rssi_dbm` and `has_snr`/`snr_db` for decoded, out-of-scope, and encrypted packets alike, and before the payload event for the same packet; never for `from == 0`; and a client that leaves the callback NULL is unaffected. Absent RSSI reads `has_rssi == false` while a genuine **0 dBm** reading reads present-and-zero (the case an in-band sentinel would corrupt). Implausible readings read **absent, not clamped**: an RSSI outside `[MC_RSSI_MIN_DBM, MC_RSSI_MAX_DBM]` (notably `65536`, which truncates to the protected `0`), and an SNR that is NaN, ±infinity, or outside `[MC_SNR_MIN_DB, MC_SNR_MAX_DB]`; `snr_db` is zeroed whenever `has_snr` is false, so an ignored flag never yields NaN. Readings at the edge of the physically plausible (−150/+20 dBm, −30/+15 dB) must still read **present**, so the gate can only reject garbage. `rx_path` is DIRECT only when positively established — `hop_start == 0` without the sender's bitfield, and `hop_limit > hop_start`, both read UNKNOWN; `via_mqtt` reads INDIRECT.

11. **Coordinate precision (#47).** `precision_bits` reaches `mc_position_t` presence-flagged, on both the live-packet and the NodeInfo-replay path. Stated values 1–32 read present (1 and 32 inclusive — boundaries are statements, not garbage); wire `0` reads absent (byte-identical to an absent field, proto3 implicit presence); `33` and arbitrarily large values read **absent, not clamped and not reduced mod 32** (`0xFFFFFFFF` would otherwise read 31). When absent, `precision_bits` is zeroed so a flag-ignoring caller can never pass a `>= N` precision gate on leftover wire bytes.

## Slices
a) framing+resync · b) protobuf gen + decode path · c) handshake state machine · d) send path · e) TCP transport + fixture capture tool (Python script recording meshtasticd session) · f) packet metadata: position provenance + per-packet RSSI/SNR/hop path (AC9, AC10; issues #33, #35) · g) coordinate precision (AC11; issue #47).

**f and g make the data available; they do not consume it.** Wiring `on_rx_meta` into `ff_crew_on_rssi`, and giving `ff_crew` an asserted-position state off the live/stale/lost axis, both belong to S16 slice b1 (callback wiring) — see `docs/specs/S16-app-shell.md`. Until that lands, the Radar face's CLOSE mode still has no path from a radio to the screen. Likewise the *consumer* of precision — the Radar face refusing to render a confident metre-level distance from kilometre-level data — stays with issue #47, alongside the Firefly-channel provisioning that makes full precision available at all.

## Amendments

### `mc_transport_t.write()` backpressure contract (debt/meshclient-contracts)

The vtable's `write()` had no documented contract for a partial or zero
return: `mc_write_bytes`/`mc_send_frame` (`mc_client.c`) treated any
`write() <= 0` as a hard failure and escalated straight to reconnect. Only
`mc_transport_tcp` happened to satisfy that, by retrying `EAGAIN`/
`EWOULDBLOCK` internally up to 200 times so its own `write()` never
actually returns 0. The UART transport landing with S15 will naturally
return 0 for "TX ring full" — treating that as a hard failure would fire a
reconnect storm every time the device's own output momentarily outpaced
the ring.

**Contract, now stated in `mc_client.h`'s `mc_transport_t` doc comment:**
`write()` returns the number of bytes accepted, `0..len`. `0` means
"accepted nothing right now, try again later" — **not** an error. A
negative return is a hard transport failure and is escalated immediately,
no retry.

`mc_write_bytes()` now retries a `0` return up to `MC_WRITE_ZERO_RETRY_BUDGET`
(64, `mc_client.c`) times — a *call* budget, not a wall-clock one: the
retry loop has no clock of its own in scope, no yield point, and no sleep
between attempts, so a millisecond-resolution deadline could read the same
timestamp for the entire loop and fail to bound anything. 64 calls is
chosen to comfortably outlast one transport's momentary stall (a UART TX
ring draining at its own pace) without turning a single frame's
back-pressure into a spurious reconnect, while a permanently stuck
transport still fails in a small, bounded number of calls rather than
spinning forever. `mc_transport_tcp` is unchanged — it still never returns
0, so this budget is never exercised on the shipped dev transport.

Tests: `S03_debt_write_backpressure_below_budget_sends_frame_no_reconnect`,
`S03_debt_write_backpressure_budget_exhausted_triggers_reconnect`,
`S03_debt_write_negative_return_fails_immediately`
(`meshclient/tests/test_meshclient.c`).

### Honest `decode_errors` on the live Position path (debt/meshclient-contracts)

`mc_process_mesh_packet`'s `POSITION_APP` branch counted `decode_errors++`
whenever `pb_decode()` succeeded but the message lacked
`latitude_i`/`longitude_i` — indistinguishable, in the stats, from genuine
protobuf corruption. A Position broadcast with no coordinates is a
legitimate "no GPS fix yet" message (both fields are proto3 implicit
presence, so "absent" and "explicit zero" are the same wire bytes); it is
not malformed. The NodeInfo replay path already got this right — it
silently emits nothing for the same missing-coordinates condition, with no
counter touched at all.

**Fix:** `decode_errors` now increments *only* when `pb_decode()` itself
fails. A well-formed, no-fix Position emits no `on_position` event and
touches no counter — matching the NodeInfo path exactly, not even
`decode_skipped` (the portnum *is* in decode scope; nothing was skipped,
there was simply nothing to report).

Tests: `S03_debt_position_no_fix_yet_does_not_count_as_decode_error`,
`S03_debt_position_corrupt_protobuf_counts_decode_error`
(`meshclient/tests/test_meshclient.c`). Mutation-tested: reverting to the
old single-branch `decode_errors++` makes
`S03_debt_position_no_fix_yet_does_not_count_as_decode_error` fail
(`Expected 0 Was 1`) and nothing else.

### Bounded `mc_tick()` drain — `MC_TICK_MAX_FRAMES` (debt/meshclient-contracts)

`mc_tick()`'s read loop previously drained the transport completely in one
call. meshtasticd's `want_config` response can burst a NodeInfo dump of
the whole mesh's node database in one delivery, and `mc_tick()` shares its
calling tick with UI rendering (`ff_shell.c`'s `ff_shell_tick`) — decoding
an unbounded burst back-to-back in one call could stall that shared tick.

**Fix:** `mc_tick()` now decodes at most `MC_TICK_MAX_FRAMES` (32,
documented in `mc_client.h` next to `mc_tick()`) complete frames per call.
32 is chosen against the largest realistic burst: even a 100-node
want_config dump (on the order of Meshtastic's stock node-database size on
typical hardware) finishes draining in ~4 calls — well under 100 ms at the
spec's ~50 Hz cadence — while any single `mc_tick()` call never does more
than 32 frames' worth of `pb_decode()` + dispatch.

Implementation note: to guarantee nothing is lost when the cap lands
mid-burst, `mc_tick()` now reads the transport **one byte at a time**,
checking the cap before each read, instead of the previous 64-byte chunk
read. This means a byte is never pulled out of the transport unless it is
immediately fed to the framer — there is no partially-consumed chunk to
carry over between calls, at the cost of more (cheap) `transport.read()`
calls for the same total bytes. Bytes beyond the cap are simply left
buffered in the transport for the next call(s); frame order is preserved
because the framer and transport both simply resume where the previous
call left off.

Test: `S03_debt_mc_tick_bounded_drain_caps_frames_per_call` — feeds
`MC_TICK_MAX_FRAMES + 5` frames into a fake transport; the first
`mc_tick()` call delivers exactly the cap (transport left with bytes
buffered), the second delivers the remaining 5, and both calls' payloads
are checked in order end-to-end. The existing byte-dribble/resync (AC1)
tests exercise `mc_framer_feed()` directly and are unaffected; the AC8
fuzz-smoke test was updated to call `mc_tick()` in a loop until its mock
transport drains, since a single call is no longer guaranteed to.
Mutation-tested: disabling the cap check makes this test fail
(`Expected 32 Was 37`) and nothing else.
