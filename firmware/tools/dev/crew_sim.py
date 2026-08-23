#!/usr/bin/env python3
"""crew_sim.py — S13 slice d: drive scenarios against a dockerized
meshtasticd (see compose.yml) for the ffsim dev loop and firmware/tests/e2e/.

Uses the `meshtastic` pip package (TCPInterface) to talk to meshtasticd's
client TCP API — the exact API surface ffsim's mc_client also speaks (see
firmware/meshclient/), so anything crew_sim.py sends here is exactly what
a real Meshtastic phone app would send, no shortcuts.

    pip install meshtastic

## IMPORTANT: verified constraints of a single simulated meshtasticd (read
## this before assuming --node spawns concurrent fake nodes)

This is not theoretical — it was verified in this repo's own dev session
by actually pulling and running the pinned image
(meshtastic/meshtasticd:GHA-2.6.1.499ea56-debian-linux_amd64) and driving
it with the `meshtastic` package. Findings, load-bearing for this file's
design:

  1. **One client connection at a time.** meshtasticd's TCP API force-
     closes any existing client connection the instant a new one connects
     ("[ApiServer] Force close previous TCP connection" in its logs). Two
     simultaneous TCPInterface connections to one instance is not a thing.
  2. **One node identity per instance.** A connected client cannot make
     the daemon emit packets claiming to be from a different node number
     (no spoofing) — `--node NAME` below is a display label for scenario
     narration, not a way to fabricate distinct simultaneous node
     identities on a single instance.
  3. **`setOwner()` (renaming the node) triggers a reboot, and this
     container image does not survive it** — its `execv()` re-exec fails
     ("Rebooting... execv() returned -1! error: No such file or
     directory") and the container dies. So this script deliberately
     does **not** call `setOwner()` in the hot path of walk/pulse/status/
     text — doing so would crash your meshtasticd on every single call.
  4. **Position persists (via the daemon's NodeDB) across a
     disconnect/reconnect; transient Data packets (PULSE/STATUS/plain
     text) do not.** A `sendPosition()` is visible to a client that
     connects *after* the sender disconnected (this is what makes
     `walk` + a later `ffsim --connect` work). A `sendData()`/`sendText()`
     is only observed by whoever is connected at the moment the daemon's
     async "send completed" event fires — with no other real or simulated
     radio node to relay it, and given constraint #1, that is at best a
     narrow, racy window, not a reliable delivery path.

Net effect: `crew_sim.py walk` genuinely drives ffsim's radar face end to
end against real meshtasticd (see firmware/tests/e2e/
test_position_reaches_radar). `pulse`/`status`/`text` are implemented
here for real (correct wire format, verified against the pinned image)
and are exactly what a real multi-node mesh would carry, but delivering
them to a *second*, already-connected observer needs either real
Meshtastic hardware or genuine multi-instance radio simulation (the
Meshtasticator project's interactive-sim tooling, not something stock
`docker compose up meshtasticd` provides) — flagged as follow-up work,
not silently assumed to work. See this slice's PR body for the full
writeup and firmware/tests/e2e/test_scenarios.py's comments for how the
e2e suite handles this honestly.

## Scenario API + CLI

    crew_sim.py walk   --node Dana --from-stage prehistoric --to-stage wompy-woods --speed 1.2
    crew_sim.py pulse  --from Dana
    crew_sim.py text   --from Dana --message "omw!"
    crew_sim.py status --from Dana --text "RAGING"

All commands accept --host/--port (default 127.0.0.1:4403, matching
compose.yml's published port).
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
import sys
import time

try:
    from meshtastic.tcp_interface import TCPInterface
except ImportError:  # pragma: no cover
    print("error: pip install meshtastic", file=sys.stderr)
    raise

# ---------------------------------------------------------------------------
# Firefly protocol (S04) — payload encoding, matching firmware/core/include/
# ff_proto.h byte-for-byte: [ver:1][type:1][body...], little-endian.
# ---------------------------------------------------------------------------

FF_PORTNUM = 269  # meshtastic PortNum this protocol rides on (mc_client.h)
FF_PROTO_VERSION = 1

FF_PROTO_TYPE_PULSE = 0x01
FF_PROTO_TYPE_FLARE = 0x02
FF_PROTO_TYPE_FLARE_END = 0x03
FF_PROTO_TYPE_RALLY = 0x04
FF_PROTO_TYPE_RALLY_CLEAR = 0x05
FF_PROTO_TYPE_STATUS = 0x06


def ff_encode_pulse() -> bytes:
    """[ver:1][type:1], no body — ff_proto_encode_pulse()'s wire shape."""
    return struct.pack("<BB", FF_PROTO_VERSION, FF_PROTO_TYPE_PULSE)


def ff_encode_flare(dur_s: int) -> bytes:
    """[ver:1][type:1][dur_s:2] — ff_proto_encode_flare()'s wire shape."""
    return struct.pack("<BBH", FF_PROTO_VERSION, FF_PROTO_TYPE_FLARE, dur_s)


def ff_encode_status(text: str) -> bytes:
    """[ver:1][type:1][status_len:1][status] — ff_proto_encode_status().
    FF_PROTO_STATUS_MAX (20) enforced here, matching the C encoder's cap."""
    raw = text.encode("utf-8")
    if len(raw) > 20:
        raise ValueError(f"status text too long ({len(raw)} > 20 bytes, FF_PROTO_STATUS_MAX)")
    return struct.pack("<BBB", FF_PROTO_VERSION, FF_PROTO_TYPE_STATUS, len(raw)) + raw


# ---------------------------------------------------------------------------
# Legend Valley geometry: venue origin from the Lost Lands festpack fixture
# (per the task brief: "use the venue origin from the Lost Lands festpack
# fixture"), stage layout is NOT in that fixture's schema (fest-almanac v0.1
# carries festival.venue.{lat,lon} but no per-stage coordinates — see
# firmware/festpack/include/fp_pack.h) — so this is a small, clearly-labeled
# synthetic offset table (meters east/north of the venue origin), good
# enough to exercise walk-path/distance/bearing math honestly, not a claim
# about Legend Valley's real festival map.
# ---------------------------------------------------------------------------

_FESTPACK_FIXTURE = (
    pathlib.Path(__file__).resolve().parents[2] / "festpack" / "tests" / "fixtures" / "lost-lands-2026.festpack.json"
)

# (east_m, north_m) offsets from festival.venue — synthetic, see note above.
STAGE_OFFSETS_M = {
    "prehistoric": (0.0, 0.0),
    "wompy-woods": (220.0, 140.0),
    "subsidia": (-180.0, 90.0),
    "forest": (-260.0, -120.0),
    "crater": (160.0, -200.0),
    "raptor-alley": (340.0, -40.0),
    "grove": (-90.0, 260.0),
}

_M_PER_DEG_LAT = 111_320.0  # constant enough at festival scale


def venue_origin() -> tuple[float, float]:
    """Legend Valley's lat/lon, read from the Lost Lands festpack fixture."""
    pack = json.loads(_FESTPACK_FIXTURE.read_text())
    venue = pack["festival"]["venue"]
    return float(venue["lat"]), float(venue["lon"])


def latlon_at_offset(origin_lat: float, origin_lon: float, east_m: float, north_m: float) -> tuple[float, float]:
    """Equirectangular approximation — plenty accurate at festival scale
    (a few hundred meters), same trade-off firmware/core/src/ff_geo.c's
    projection makes for the same reason."""
    lat = origin_lat + (north_m / _M_PER_DEG_LAT)
    m_per_deg_lon = _M_PER_DEG_LAT * math.cos(math.radians(origin_lat))
    lon = origin_lon + (east_m / m_per_deg_lon)
    return lat, lon


def stage_latlon(stage_id: str) -> tuple[float, float]:
    if stage_id not in STAGE_OFFSETS_M:
        known = ", ".join(sorted(STAGE_OFFSETS_M))
        raise SystemExit(f"error: unknown stage \"{stage_id}\" (known: {known})")
    origin_lat, origin_lon = venue_origin()
    east_m, north_m = STAGE_OFFSETS_M[stage_id]
    return latlon_at_offset(origin_lat, origin_lon, east_m, north_m)


# ---------------------------------------------------------------------------
# Scenario API
# ---------------------------------------------------------------------------


def connect(host: str, port: int) -> TCPInterface:
    return TCPInterface(hostname=host, portNumber=port)


def rename_node(iface: TCPInterface, name: str) -> None:
    """Relabels the single connected daemon's own node (long/short name).
    **Opt-in only, via the `rename` subcommand** — see this file's
    top-of-module note #3: this reboots the daemon, which crashes the
    pinned container image outright. Not called from walk/pulse/status/
    text's hot path. short_name is Meshtastic's "ideally two
    characters... suitable for a tiny OLED screen" field; truncated to 4
    here to stay legible without crowding."""
    short = (name[:4] or "?").upper()
    iface.localNode.setOwner(long_name=name, short_name=short)


def walk(iface: TCPInterface, node: str, from_stage: str, to_stage: str, speed_mps: float,
          update_hz: float = 1.0) -> None:
    """Sends a straight-line walk from `from_stage` to `to_stage` at
    `speed_mps`, emitting one sendPosition() per 1/update_hz seconds —
    "Dana walks 300 m NE at 1.2 m/s" as the task brief's one-liner
    example puts it. Blocks until the walk completes. `node` is a display
    label only (see top-of-module note #3 for why this doesn't call
    setOwner())."""
    lat0, lon0 = stage_latlon(from_stage)
    lat1, lon1 = stage_latlon(to_stage)

    origin_lat, origin_lon = venue_origin()
    m_per_deg_lon = _M_PER_DEG_LAT * math.cos(math.radians(origin_lat))

    def to_en(lat: float, lon: float) -> tuple[float, float]:
        return (lon - origin_lon) * m_per_deg_lon, (lat - origin_lat) * _M_PER_DEG_LAT

    e0, n0 = to_en(lat0, lon0)
    e1, n1 = to_en(lat1, lon1)
    dist_m = math.hypot(e1 - e0, n1 - n0)
    duration_s = dist_m / speed_mps if speed_mps > 0 else 0.0
    steps = max(1, int(duration_s * update_hz))

    print(f"crew_sim: {node} walking {from_stage} -> {to_stage} "
          f"({dist_m:.0f} m at {speed_mps:.2f} m/s, ~{duration_s:.0f}s, {steps} updates)")

    for i in range(steps + 1):
        t = i / steps
        e = e0 + (e1 - e0) * t
        n = n0 + (n1 - n0) * t
        lat = origin_lat + n / _M_PER_DEG_LAT
        lon = origin_lon + e / m_per_deg_lon
        iface.sendPosition(latitude=lat, longitude=lon, altitude=0)
        if i < steps:
            time.sleep(1.0 / update_hz)

    # Resend the final (destination) position once more, with a settle
    # delay before and after: a rapid burst of sendPosition() calls
    # (this loop can fire several per second at high --update-hz) was
    # observed, in this repo's own e2e test runs against real
    # meshtasticd, to occasionally leave the daemon's NodeDB holding an
    # *earlier* (even the very first/origin) position instead of the
    # last one sent — some internal update/dedup timing on meshtasticd's
    # side, not something crew_sim.py controls. A final, isolated resend
    # well after the burst reliably lands as the last word.
    time.sleep(0.5)
    iface.sendPosition(latitude=lat1, longitude=lon1, altitude=0)
    time.sleep(1.5)
    print(f"crew_sim: {node} arrived at {to_stage} ({lat1:.6f}, {lon1:.6f})")


def pulse(iface: TCPInterface, node: str, dest: str = "^all") -> None:
    iface.sendData(ff_encode_pulse(), destinationId=dest, portNum=FF_PORTNUM, wantAck=False)
    print(f"crew_sim: {node} sent PULSE to {dest}")


def send_status(iface: TCPInterface, node: str, text: str, dest: str = "^all") -> None:
    iface.sendData(ff_encode_status(text), destinationId=dest, portNum=FF_PORTNUM, wantAck=False)
    print(f"crew_sim: {node} sent STATUS \"{text}\" to {dest}")


def send_text(iface: TCPInterface, node: str, message: str, dest: str = "^all") -> None:
    iface.sendText(message, destinationId=dest)
    print(f"crew_sim: {node} sent text \"{message}\" to {dest}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4403)
    sub = parser.add_subparsers(dest="cmd", required=True)

    walk_p = sub.add_parser("walk", help="walk a node between two named stages at a given speed")
    walk_p.add_argument("--node", required=True)
    walk_p.add_argument("--from-stage", required=True, dest="from_stage")
    walk_p.add_argument("--to-stage", required=True, dest="to_stage")
    walk_p.add_argument("--speed", type=float, default=1.2, help="m/s, default 1.2 (a brisk walk)")
    walk_p.add_argument("--update-hz", type=float, default=1.0, dest="update_hz")

    pulse_p = sub.add_parser("pulse", help="send a firefly PULSE (portnum 269)")
    pulse_p.add_argument("--from", required=True, dest="node")
    pulse_p.add_argument("--to", default="^all")

    status_p = sub.add_parser("status", help="send a firefly STATUS (portnum 269)")
    status_p.add_argument("--from", required=True, dest="node")
    status_p.add_argument("--text", required=True)
    status_p.add_argument("--to", default="^all")

    text_p = sub.add_parser("text", help="send a plain Meshtastic text message")
    text_p.add_argument("--from", required=True, dest="node")
    text_p.add_argument("--message", required=True)
    text_p.add_argument("--to", default="^all")

    rename_p = sub.add_parser(
        "rename",
        help="(opt-in, reboots the daemon) rename the connected instance's own node identity",
        description="Calls setOwner() to relabel the connected meshtasticd's node. WARNING: this "
                     "reboots the daemon, and the pinned container image (verified in this repo's "
                     "own dev session) does not survive that reboot — it crashes outright. Only use "
                     "this against a meshtasticd you're prepared to restart afterward.",
    )
    rename_p.add_argument("--name", required=True)

    args = parser.parse_args()

    iface = connect(args.host, args.port)
    try:
        if args.cmd == "walk":
            walk(iface, args.node, args.from_stage, args.to_stage, args.speed, args.update_hz)
        elif args.cmd == "pulse":
            pulse(iface, args.node, args.to)
        elif args.cmd == "status":
            send_status(iface, args.node, args.text, args.to)
        elif args.cmd == "text":
            send_text(iface, args.node, args.message, args.to)
        elif args.cmd == "rename":
            rename_node(iface, args.name)
            print(f"crew_sim: renamed node to \"{args.name}\" (meshtasticd is rebooting now)")
    finally:
        iface.close()


if __name__ == "__main__":
    main()
