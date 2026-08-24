#!/usr/bin/env python3
"""probe_node.py — answer the questions our specs are guessing at, using a
real radio.

Three design decisions currently rest on assumptions we could only check
against vendored protobufs, never against hardware. This prints what a
physical Meshtastic node actually puts on the wire, so we can stop guessing:

  1. **location_source** (#33, landmark tier) — does a node configured with
     a fixed position actually report LOC_MANUAL? The whole "asserted vs
     measured" provenance design rests on this. If stock firmware leaves it
     UNSET, the landmark tier needs the firefly-protocol beacon fallback.
  2. **rx_rssi / rx_snr** (#35, Radar CLOSE mode) — are they populated on
     packets a client API consumer actually receives? mc_client now decodes
     them; nobody has seen a real one.
  3. **last_heard** (S16 b0, wall clock) — is it populated in practice, and
     is it plausible? It bootstraps the puck's entire sense of time.

Usage (from firmware/):
    pip install -r tests/e2e/requirements.txt
    python3 tools/dev/probe_node.py --host 192.168.1.50
    python3 tools/dev/probe_node.py --port /dev/cu.usbserial-0001
    python3 tools/dev/probe_node.py --host 192.168.1.50 --listen 120

`--listen N` waits N seconds for live packets, which is the only way to see
rx_rssi: it rides MeshPacket, so it exists only on packets received over the
air. The node DB replay at connect carries none (see mc_client.c's NodeInfo
path, which hardcodes has_rx_time=false for exactly this reason).

Nothing here writes to the node. To *set* a fixed position for question 1,
use the meshtastic CLI, then re-run this against the OTHER board:
    meshtastic --host <A> --setlat 39.31 --setlon -82.10 --setalt 200
"""
import argparse
import sys
import time

try:
    from meshtastic import BROADCAST_ADDR  # noqa: F401  (import probe)
    from meshtastic.tcp_interface import TCPInterface
    from meshtastic.serial_interface import SerialInterface
    from pubsub import pub
except ImportError:
    sys.exit("pip install -r tests/e2e/requirements.txt")

# meshtastic.protobuf.config_pb2.Config.PositionConfig / mesh_pb2 name these;
# printed by name where we can resolve one, by number when we can't, because
# an unknown number is data too — never silently mapped to a known value.
LOC_SOURCE_NAMES = {
    0: "LOC_UNSET",
    1: "LOC_MANUAL",
    2: "LOC_INTERNAL",
    3: "LOC_EXTERNAL",
}


def _name(table, value):
    if value is None:
        return "absent"
    return f"{table.get(value, '?')} ({value})"


def _fmt_unix(ts):
    if not ts:
        return "0 (unknown)"
    return f"{ts} ({time.strftime('%Y-%m-%d %H:%M:%SZ', time.gmtime(ts))})"


def dump_nodedb(iface):
    print("\n=== node DB (from the connect handshake) ===")
    nodes = getattr(iface, "nodes", None) or {}
    if not nodes:
        print("  (empty — the handshake returned no nodes)")
        return

    for node_id, n in nodes.items():
        user = n.get("user") or {}
        print(f"\n  {node_id}  {user.get('shortName', '?')} / {user.get('longName', '?')}")
        print(f"    last_heard   : {_fmt_unix(n.get('lastHeard'))}")
        print(f"    hops_away    : {n.get('hopsAway', 'absent')}")
        # snr here is the cached NodeInfo value — deliberately NOT surfaced by
        # mc_client (no rx time, can't feed a trend window). Printed anyway so
        # we can see what it actually holds.
        print(f"    cached snr   : {n.get('snr', 'absent')}")

        pos = n.get("position")
        if not pos:
            print("    position     : absent")
            continue
        print(f"    position     : lat={pos.get('latitude')} lon={pos.get('longitude')} alt={pos.get('altitude')}")
        print(f"    ** location_source : {_name(LOC_SOURCE_NAMES, pos.get('locationSource'))}")
        print(f"    pos time     : {_fmt_unix(pos.get('time'))}")
        print(f"    precision    : {pos.get('precisionBits', 'absent')}")


def listen(iface, seconds):
    print(f"\n=== listening {seconds}s for live packets ===")
    print("(rx_rssi rides MeshPacket — it exists ONLY here, never in the DB replay)")
    seen = {"n": 0}

    def on_receive(packet=None, interface=None):  # noqa: ARG001
        if packet is None:
            return
        seen["n"] += 1
        d = packet.get("decoded") or {}
        print(
            f"\n  #{seen['n']} from={packet.get('fromId')} portnum={d.get('portnum', '?')}"
        )
        print(f"    rx_rssi      : {packet.get('rxRssi', 'ABSENT')}")
        print(f"    rx_snr       : {packet.get('rxSnr', 'ABSENT')}")
        print(f"    rx_time      : {_fmt_unix(packet.get('rxTime'))}")
        # hop_start/hop_limit is how mc_rx_path_t decides direct vs relayed.
        # A relayed packet's rssi measures the RELAY, not the originator.
        hs, hl = packet.get("hopStart"), packet.get("hopLimit")
        print(f"    hop_start    : {hs if hs is not None else 'ABSENT'}   hop_limit: {hl}")
        if hs is None or hs == 0:
            print("    -> path     : UNKNOWN (no hop_start; pre-2.3.0 firmware can't be told from direct)")
        elif hs == hl:
            print("    -> path     : DIRECT (rssi is this sender's own signal)")
        else:
            print(f"    -> path     : RELAYED {hs - hl} hop(s) — rssi is the RELAY's signal, not theirs")
        if d.get("portnum") == "POSITION_APP":
            p = d.get("position") or {}
            print(f"    ** location_source : {_name(LOC_SOURCE_NAMES, p.get('locationSource'))}")

    pub.subscribe(on_receive, "meshtastic.receive")
    deadline = time.time() + seconds
    while time.time() < deadline:
        time.sleep(0.5)
    print(f"\n  {seen['n']} packet(s) in {seconds}s.")
    if seen["n"] == 0:
        print("  Nothing arrived. If the other board is on and in range, check both")
        print("  are on the same channel+PSK and the same region — a mismatch is silent.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--host", help="node's IP (WiFi, TCP 4403)")
    g.add_argument("--port", help="serial device path")
    ap.add_argument("--listen", type=int, default=0, metavar="SECONDS",
                    help="wait for live packets (the only way to see rx_rssi)")
    a = ap.parse_args()

    iface = TCPInterface(hostname=a.host) if a.host else SerialInterface(devPath=a.port)
    try:
        mi = getattr(iface, "myInfo", None)
        if mi is not None:
            print(f"connected: my_node_num={getattr(mi, 'my_node_num', '?')}")
        dump_nodedb(iface)
        if a.listen:
            listen(iface, a.listen)
    finally:
        iface.close()

    print("\n--- what to do with this ---")
    print("location_source LOC_MANUAL on a fixed-position node  -> #33's design holds")
    print("location_source LOC_UNSET  on a fixed-position node  -> #33 needs the protocol fallback")
    print("rx_rssi present on live packets                      -> #35 unblocked, b1 can wire it")
    print("last_heard plausible                                 -> S16 b0's clock bootstrap works")
    return 0


if __name__ == "__main__":
    sys.exit(main())
