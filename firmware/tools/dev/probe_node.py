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

def _require_meshtastic():
    """Imported lazily so --help and --selftest work without the library."""
    try:
        from meshtastic.tcp_interface import TCPInterface
        from meshtastic.serial_interface import SerialInterface
        from pubsub import pub
    except ImportError:
        sys.exit("pip install -r tests/e2e/requirements.txt")
    return TCPInterface, SerialInterface, pub

# The meshtastic library converts protobufs with a bare MessageToDict, which
# has two consequences this tool has to respect or it reports nonsense:
#
#   1. Enums arrive as STRINGS ('LOC_MANUAL'), not ints. A number-keyed table
#      would never match and every lookup would read as unresolvable.
#   2. proto3 omits zero-valued fields entirely. So a Position with
#      location_source == LOC_UNSET has NO locationSource key at all — and
#      LOC_UNSET is one of the two answers we're here to obtain. Reading the
#      missing key as "absent" would make that answer unprintable.
#
# (2) is the interesting one, and it's the opposite of this project's usual
# rule. Normally absence must not carry meaning; here the wire format has
# already decided that it does, and decoding it as anything else loses a real
# reading. A key present on the parent object is what tells us we're looking
# at a decoded Position at all.
LOC_SOURCE_KNOWN = {"LOC_UNSET", "LOC_MANUAL", "LOC_INTERNAL", "LOC_EXTERNAL"}


def _loc_source(pos):
    """Read location_source from a position dict, honouring proto3 defaults.

    `pos` must be a real decoded Position; pass None if there wasn't one.
    """
    if pos is None:
        return "no position at all"
    v = pos.get("locationSource")
    if v is None:
        return "LOC_UNSET (field omitted — proto3 drops the zero value)"
    if isinstance(v, int):  # in case a future lib version stops stringifying
        return f"{['LOC_UNSET', 'LOC_MANUAL', 'LOC_INTERNAL', 'LOC_EXTERNAL'][v]} ({v})" \
            if 0 <= v < 4 else f"UNKNOWN VALUE ({v})"
    return str(v) if v in LOC_SOURCE_KNOWN else f"UNKNOWN VALUE ({v!r})"


def _rx_path(packet):
    """Classify how a packet reached us. MUST mirror mc_rx_path_from_pkt()
    in meshclient/src/mc_client.c — if the tool and the firmware disagree
    about "direct", the tool validates the wrong thing, and RSSI is only a
    distance proxy on a genuinely direct packet.

    proto3 again: absent hopStart/hopLimit/viaMqtt all mean zero/false.
    """
    if bool(packet.get("viaMqtt", False)):
        # Checked FIRST, as the firmware does. Whatever our radio measured,
        # it was not this sender's transmission.
        return "INDIRECT (via MQTT — arrived over the internet, not our radio)"
    hs = packet.get("hopStart", 0)
    hl = packet.get("hopLimit", 0)
    if hs > 0:
        if hl > hs:
            return "UNKNOWN (malformed — hops travelled would be negative)"
        if hl == hs:
            return "DIRECT (rssi is this sender's own signal)"
        return f"INDIRECT, {hs - hl} hop(s) — rssi is the RELAY's signal, not theirs"
    # hop_start == 0: a real zero-hop packet, or pre-2.3.0 firmware that never
    # set the field. Only the sender's Data.bitfield separates them, and the
    # library doesn't surface it — so this tool cannot resolve what the
    # firmware can. Say so rather than guessing.
    return "UNKNOWN (hop_start 0: zero-hop, or a pre-2.3.0 sender — can't tell here)"


def _selftest():
    """Exercise the pure decoders against MessageToDict-shaped input.

    Exists because the first version of this tool could not print either
    answer it was built to obtain: it keyed the enum table by int when the
    library yields strings, and read a proto3-omitted LOC_UNSET as "absent".
    Both were invisible without hardware. These cases make them visible with
    none. See PR #42's review.
    """
    cases = [
        # (what, got, expected-substring)
        ("manual position", _loc_source({"locationSource": "LOC_MANUAL"}), "LOC_MANUAL"),
        ("unset is omitted, not absent", _loc_source({"latitude": 39.0}), "LOC_UNSET"),
        ("int form still works", _loc_source({"locationSource": 2}), "LOC_INTERNAL"),
        ("future value not folded", _loc_source({"locationSource": "LOC_NEW"}), "UNKNOWN VALUE"),
        ("no position at all", _loc_source(None), "no position"),
        ("mqtt beats hop match", _rx_path({"viaMqtt": True, "hopStart": 3, "hopLimit": 3}), "INDIRECT"),
        ("direct", _rx_path({"hopStart": 3, "hopLimit": 3}), "DIRECT"),
        ("relayed", _rx_path({"hopStart": 3, "hopLimit": 1}), "2 hop"),
        ("malformed", _rx_path({"hopStart": 1, "hopLimit": 3}), "malformed"),
        ("absent hops", _rx_path({}), "UNKNOWN"),
    ]
    bad = [(w, g, e) for w, g, e in cases if e not in g]
    for what, got, _ in cases:
        print(f"  {'FAIL' if any(w == what for w, _, _ in bad) else 'ok  '}  {what}: {got}")
    if bad:
        print(f"\n{len(bad)} case(s) failed")
        return 1
    print(f"\n{len(cases)} cases pass")
    return 0


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
        print(f"    ** location_source : {_loc_source(pos)}")
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
        hs = packet.get("hopStart", 0)
        hl = packet.get("hopLimit", 0)
        via_mqtt = bool(packet.get("viaMqtt", False))
        print(f"    hop_start    : {hs}   hop_limit: {hl}   via_mqtt: {via_mqtt}")
        print(f"    -> path     : {_rx_path(packet)}")
        if d.get("portnum") == "POSITION_APP":
            print(f"    ** location_source : {_loc_source(d.get('position'))}")

    pub.subscribe(on_receive, "meshtastic.receive")
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n  (interrupted)")
    print(f"\n  {seen['n']} packet(s) seen.")
    if seen["n"] == 0:
        print("  Nothing arrived. If the other board is on and in range, check both")
        print("  are on the same channel+PSK and the same region — a mismatch is silent.")
    return seen["n"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--host", help="node's IP (WiFi, TCP 4403)")
    g.add_argument("--port", help="serial device path")
    g.add_argument("--selftest", action="store_true",
                   help="check the decoders against known field shapes; no hardware needed")
    ap.add_argument("--listen", type=int, default=0, metavar="SECONDS",
                    help="wait for live packets (the only way to see rx_rssi)")
    a = ap.parse_args()

    if a.selftest:
        return _selftest()

    TCPInterface, SerialInterface, pub = _require_meshtastic()
    globals()["pub"] = pub  # listen() subscribes through it

    target = a.host or a.port
    try:
        iface = TCPInterface(hostname=a.host) if a.host else SerialInterface(devPath=a.port)
    except Exception as e:  # noqa: BLE001 — the library raises many types here
        sys.exit(
            f"could not connect to {target}: {type(e).__name__}: {e}\n"
            "  --host wants the node's IP with WiFi enabled (TCP 4403).\n"
            "  --port wants a serial device path (ls /dev/cu.* on macOS).\n"
            "  A charge-only USB cable looks exactly like a missing device."
        )

    packets = 0
    try:
        mi = getattr(iface, "myInfo", None)
        if mi is not None:
            print(f"connected: my_node_num={getattr(mi, 'my_node_num', '?')}")
        dump_nodedb(iface)
        if a.listen:
            packets = listen(iface, a.listen)
    finally:
        iface.close()

    # Only claim to have answered what was actually observed. Printing the
    # rx_rssi verdict after a run with no --listen would be the tool making
    # exactly the kind of unearned claim it exists to prevent.
    print("\n--- what this run can and can't tell you ---")
    print("location_source LOC_MANUAL on a fixed-position node  -> #33's design holds")
    print("location_source LOC_UNSET  on a fixed-position node  -> #33 needs the protocol fallback")
    print("last_heard plausible                                 -> S16 b0's clock bootstrap works")
    if not a.listen:
        print("rx_rssi                                             -> NOT TESTED: re-run with --listen N")
    elif packets == 0:
        print("rx_rssi                                             -> NOT TESTED: no packets arrived")
    else:
        print("rx_rssi present above                                -> #35 unblocked, b1 can wire it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
