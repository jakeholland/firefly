#!/usr/bin/env python3
"""record_fixture.py — build meshclient test fixtures.

Two modes:

  synth   Hand-craft the binary fixtures under tests/fixtures/ directly from
          the meshtastic protobuf definitions (via the `meshtastic` pip
          package's compiled protobuf classes), byte-for-byte framed with
          the stream protocol (0x94 0xC3 len_hi len_lo <protobuf>). This is
          what actually produced the fixtures committed in this repo — we
          have no meshtasticd instance to record from in this environment
          (no docker), so AC2's handshake.bin is a *synthetic* capture, not
          a real one. See the S03 PR body for the explicit callout.

  record  Connect to a real, running meshtasticd (or a hardware node) over
          TCP and save the raw framed byte stream exchanged during a
          want_config handshake to a file. This is the "future real
          captures" tool the spec asks for (slice e) — run it against a
          live meshtasticd once one is available, to replace/augment the
          synthetic fixtures with a real capture.

Usage:
    python3 record_fixture.py synth  [--out-dir DIR]
    python3 record_fixture.py record --host HOST [--port 4403] --out FILE
                                      [--duration 10]

`synth` only needs `pip install meshtastic` (for the protobuf message
classes — it never opens a network connection). `record` additionally
needs a reachable meshtasticd.
"""
from __future__ import annotations

import argparse
import pathlib
import socket
import sys
import time

try:
    from meshtastic.protobuf import mesh_pb2, portnums_pb2
except ImportError:  # pragma: no cover
    print("error: pip install meshtastic (provides the protobuf classes)", file=sys.stderr)
    raise

START1 = 0x94
START2 = 0xC3


def frame(payload: bytes) -> bytes:
    """Wrap a serialized FromRadio/ToRadio protobuf in the stream framing."""
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large to frame")
    return bytes([START1, START2, (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) + payload


# ---------------------------------------------------------------------------
# synth — hand-crafted fixtures
# ---------------------------------------------------------------------------

WANT_CONFIG_NONCE = 0x1234ABCD
MY_NODE_NUM = 0x42424242
NODE_A_NUM = 0x0A0A0A0A
NODE_B_NUM = 0x0B0B0B0B


def build_handshake() -> bytes:
    """A synthetic want_config response: MyNodeInfo, two NodeInfo entries
    (one with a position), then config_complete_id matching the nonce the
    test drives mc_connect() with. Mirrors the real device's dump order
    closely enough for AC2 (my_info, then node_info x N, then complete)."""
    out = bytearray()

    my_info = mesh_pb2.FromRadio()
    my_info.my_info.my_node_num = MY_NODE_NUM
    out += frame(my_info.SerializeToString())

    node_a = mesh_pb2.FromRadio()
    node_a.node_info.num = NODE_A_NUM
    node_a.node_info.user.long_name = "Dana Test"
    node_a.node_info.user.short_name = "DANA"
    node_a.node_info.last_heard = 1_700_000_000
    out += frame(node_a.SerializeToString())

    node_b = mesh_pb2.FromRadio()
    node_b.node_info.num = NODE_B_NUM
    node_b.node_info.user.long_name = "Basecamp"
    node_b.node_info.user.short_name = "BASE"
    node_b.node_info.position.latitude_i = 407128000   # 40.7128
    node_b.node_info.position.longitude_i = -740060000  # -74.0060
    node_b.node_info.last_heard = 1_700_000_050
    out += frame(node_b.SerializeToString())

    complete = mesh_pb2.FromRadio()
    complete.config_complete_id = WANT_CONFIG_NONCE
    out += frame(complete.SerializeToString())

    return bytes(out)


def build_position_packet() -> bytes:
    """A single FromRadio.packet carrying a decoded POSITION_APP Data
    payload, as seen post-handshake mesh traffic. Exercises AC3."""
    pos = mesh_pb2.Position()
    pos.latitude_i = 407128000
    pos.longitude_i = -740060000
    pos.altitude = 42
    pos.time = 1_700_000_100

    pkt = mesh_pb2.FromRadio()
    setattr(pkt.packet, 'from', NODE_A_NUM)
    pkt.packet.to = 0xFFFFFFFF
    pkt.packet.id = 111
    pkt.packet.rx_time = 1_700_000_101
    pkt.packet.decoded.portnum = portnums_pb2.PortNum.POSITION_APP
    pkt.packet.decoded.payload = pos.SerializeToString()

    return frame(pkt.SerializeToString())


def build_text_packet() -> bytes:
    """A FromRadio.packet carrying a decoded TEXT_MESSAGE_APP Data payload."""
    pkt = mesh_pb2.FromRadio()
    setattr(pkt.packet, 'from', NODE_A_NUM)
    pkt.packet.to = MY_NODE_NUM
    pkt.packet.id = 222
    pkt.packet.decoded.portnum = portnums_pb2.PortNum.TEXT_MESSAGE_APP
    pkt.packet.decoded.payload = "hey from dana".encode("utf-8")

    return frame(pkt.SerializeToString())


def build_private_packet() -> bytes:
    """A FromRadio.packet on a private/experimental portnum (256-511) —
    the firefly protocol's range (spec S04). Payload is opaque bytes."""
    pkt = mesh_pb2.FromRadio()
    setattr(pkt.packet, 'from', NODE_B_NUM)
    pkt.packet.to = 0xFFFFFFFF
    pkt.packet.id = 333
    pkt.packet.decoded.portnum = 257  # PRIVATE_APP + 1
    pkt.packet.decoded.payload = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x01])

    return frame(pkt.SerializeToString())


def build_send_text_golden() -> bytes:
    """The exact ToRadio frame mc_send_text(dest=NODE_A_NUM, "hi") produces
    against a freshly mc_init()'d client (packet id 1, from unset ⇒ omitted,
    proto3 implicit presence). Used as a byte-golden in AC4."""
    to_radio = mesh_pb2.ToRadio()
    to_radio.packet.id = 1
    to_radio.packet.to = NODE_A_NUM
    to_radio.packet.want_ack = True
    to_radio.packet.decoded.portnum = portnums_pb2.PortNum.TEXT_MESSAGE_APP
    to_radio.packet.decoded.payload = "hi".encode("utf-8")

    return frame(to_radio.SerializeToString())


def cmd_synth(args: argparse.Namespace) -> None:
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    fixtures = {
        "handshake.bin": build_handshake(),
        "position_packet.bin": build_position_packet(),
        "text_packet.bin": build_text_packet(),
        "private_packet.bin": build_private_packet(),
        "send_text_golden.bin": build_send_text_golden(),
    }
    for name, data in fixtures.items():
        path = out_dir / name
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")


# ---------------------------------------------------------------------------
# record — real capture against a live meshtasticd
# ---------------------------------------------------------------------------

def cmd_record(args: argparse.Namespace) -> None:
    want_config = mesh_pb2.ToRadio()
    want_config.want_config_id = WANT_CONFIG_NONCE

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        sock.sendall(frame(want_config.SerializeToString()))

        sock.settimeout(1.0)
        deadline = time.monotonic() + args.duration
        chunks = []
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            chunks.append(chunk)

    data = b"".join(chunks)
    out_path = pathlib.Path(args.out)
    out_path.write_bytes(data)
    print(f"wrote {out_path} ({len(data)} bytes) from {args.host}:{args.port}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)

    synth_p = sub.add_parser("synth", help="hand-craft fixtures from protobuf definitions")
    synth_p.add_argument("--out-dir", default="tests/fixtures")
    synth_p.set_defaults(func=cmd_synth)

    record_p = sub.add_parser("record", help="capture a real meshtasticd session")
    record_p.add_argument("--host", required=True)
    record_p.add_argument("--port", type=int, default=4403)
    record_p.add_argument("--out", required=True)
    record_p.add_argument("--duration", type=float, default=10.0)
    record_p.set_defaults(func=cmd_record)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
