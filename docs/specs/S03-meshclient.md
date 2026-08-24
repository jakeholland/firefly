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
  void (*on_position)(void *u, uint32_t node, mc_position_t const *p);  // p.loc_source
  void (*on_text)(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len);
  void (*on_private)(void *u, uint32_t from, uint32_t portnum,
                     uint8_t const *payload, size_t len);      // firefly protocol rides here
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

## Slices
a) framing+resync · b) protobuf gen + decode path · c) handshake state machine · d) send path · e) TCP transport + fixture capture tool (Python script recording meshtasticd session) · f) packet metadata: position provenance + per-packet RSSI/SNR/hop path (AC9, AC10; issues #33, #35).

**f makes the data available; it does not consume it.** Wiring `on_rx_meta` into `ff_crew_on_rssi`, and giving `ff_crew` an asserted-position state off the live/stale/lost axis, both belong to S16 slice b1 (callback wiring) — see `docs/specs/S16-app-shell.md`. Until that lands, the Radar face's CLOSE mode still has no path from a radio to the screen.
