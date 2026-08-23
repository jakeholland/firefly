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

typedef struct {
  void (*on_state)(void *u, mc_state_t s);                     // DISCONNECTED/HANDSHAKE/READY
  void (*on_node)(void *u, mc_nodeinfo_t const *n);            // id, names, hw, battery
  void (*on_position)(void *u, uint32_t node, mc_position_t const *p);
  void (*on_text)(void *u, uint32_t from, uint32_t to, char const *utf8, size_t len);
  void (*on_private)(void *u, uint32_t from, uint32_t portnum,
                     uint8_t const *payload, size_t len);      // firefly protocol rides here
  void (*on_my_info)(void *u, uint32_t my_node_id);
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

## Slices
a) framing+resync · b) protobuf gen + decode path · c) handshake state machine · d) send path · e) TCP transport + fixture capture tool (Python script recording meshtasticd session).
