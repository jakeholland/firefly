"""test_scenarios.py — S14 slice d: e2e scenarios.

docker compose up meshtasticd -> start `ffsim --headless --ctl PORT
--connect ... --pack ...` -> drive scenarios with crew_sim.py -> assert
via the ctl socket's state dump. Skips cleanly (not an error) whenever
docker, the `docker compose` plugin, the `meshtastic` pip package, or a
built `ffsim` binary isn't available — see conftest.py's fixtures.

## Read before touching test_pulse_reaches_feed / test_text_roundtrip

These two are marked `xfail(strict=False)`, not skipped and not deleted.
This is not a wishy-washy "might not work" hedge — it's the documented
result of actually running this exact scenario against the pinned
meshtasticd image (meshtastic/meshtasticd:GHA-2.6.1.499ea56-debian-
linux_amd64) in this repo's own dev session:

  1. meshtasticd's client TCP API allows exactly one connected client at
     a time (a new connection force-closes any existing one — confirmed
     via its own logs: "[ApiServer] Force close previous TCP connection").
  2. A `sendData()`/`sendText()` (unlike `sendPosition()`, which persists
     in the daemon's NodeDB and IS redelivered on a fresh handshake) is a
     transient event, observed only by whichever client happens to be
     connected at the moment its async "send completed" callback fires.

Since crew_sim.py must itself be the connected client to trigger a send
at all, and doing so force-disconnects ffsim's own connection (#1), and
the packet isn't queued for ffsim to see once it reconnects (#2), a
PULSE/text message crew_sim.py sends cannot reach an already-connected
(or later-reconnecting) ffsim through a single stock meshtasticd
instance — full stop, not a timing flake. Genuine cross-client delivery
needs either real Meshtastic hardware or true multi-instance radio
simulation (the Meshtastic project's separate Meshtasticator tooling;
verified in this same session that two independent meshtasticd
containers do NOT hear each other by default), which is out of scope
here and flagged as follow-up work in this slice's PR body.

test_position_reaches_radar has no such problem (position persists) and
is a real, currently-passing e2e test — verified end to end against the
same pinned image in this session, not just written and hoped-for.
"""
from __future__ import annotations

import pytest

from ctl_client import CtlError

pytestmark = [pytest.mark.usefixtures("meshtastic_package_ready")]


def test_position_reaches_radar(run_ffsim, crew_sim):
    """A walking node's position appears with plausible distance/bearing
    on the radar face. Sequential handoff (per this file's module
    docstring): crew_sim.py connects, walks, and disconnects BEFORE ffsim
    connects — meshtasticd's NodeDB carries the position across that
    handoff, which a fresh `--connect` handshake picks up."""
    crew_sim(["walk", "--node", "Dana", "--from-stage", "prehistoric", "--to-stage", "wompy-woods",
              "--speed", "50", "--update-hz", "2"])

    fp = run_ffsim(["--connect", "127.0.0.1:4403"])

    # Observed (in this repo's own dev session, running this exact
    # scenario against real dockerized meshtasticd) transiently reporting
    # "0 m" for a poll or two right after the handshake completes, before
    # settling on the real ~260 m reading a moment later — meshtasticd-
    # side handshake/position-sync timing, not something ffsim/mc_client
    # controls. Waiting past the exact "0 m" reading (never itself a
    # plausible answer for a node that walked 261 m from the venue
    # origin) rather than accepting the first non-empty dist_str avoids
    # that transient without weakening what's actually being checked.
    state = fp.ctl.wait_for(
        lambda s: s["radar"]["mode"] not in ("nosel", "nofix") and s["radar"]["dist_str"] not in ("", "0 m"),
        timeout=40.0,
    )

    radar = state["radar"]
    assert radar["mode"] in ("live", "stale", "close")
    assert radar["dist_str"].endswith(("m", "km", "ft", "mi"))
    # A ~260m walk from Prehistoric Stage's origin should read as at most
    # a couple hundred meters, not thousands (a wrong-unit or wrong-origin
    # bug would blow this bound way past a sanity-check margin).
    dist_value = float(radar["dist_str"].split()[0])
    dist_unit = radar["dist_str"].split()[1]
    dist_m = dist_value * (1000.0 if dist_unit == "km" else 1.0)
    assert 0.0 < dist_m < 2000.0
    assert radar["arrow_valid"] is True


@pytest.mark.xfail(
    reason="verified: meshtasticd's single-client TCP API + non-persisted Data packets mean crew_sim's "
           "send can't reach an already-connected ffsim — see this file's module docstring",
    strict=False,
)
def test_pulse_reaches_feed(run_ffsim, crew_sim):
    """A firefly PULSE from a paired node is received into the signals
    feed."""
    fp = run_ffsim(["--connect", "127.0.0.1:4403"])
    # Let the handshake settle before crew_sim's connection kicks it.
    fp.ctl.wait_for(lambda s: True, timeout=5.0)

    crew_sim(["pulse", "--from", "Dana"])

    try:
        state = fp.ctl.wait_for(
            lambda s: any(item["kind"] == "pulse" for item in s["signals"]["items"]),
            timeout=15.0,
        )
    except CtlError as e:
        pytest.fail(str(e))

    pulses = [item for item in state["signals"]["items"] if item["kind"] == "pulse"]
    assert pulses
    assert pulses[0]["from_name"] == "DANA"


@pytest.mark.xfail(
    reason="verified: meshtasticd's single-client TCP API + non-persisted Data packets mean crew_sim's "
           "send can't reach an already-connected ffsim — see this file's module docstring",
    strict=False,
)
def test_text_roundtrip(run_ffsim, crew_sim):
    """A plain Meshtastic text message from a paired node is received
    into the signals feed."""
    fp = run_ffsim(["--connect", "127.0.0.1:4403"])
    fp.ctl.wait_for(lambda s: True, timeout=5.0)

    crew_sim(["text", "--from", "Dana", "--message", "omw!"])

    try:
        state = fp.ctl.wait_for(
            lambda s: any(item["kind"] == "text" for item in s["signals"]["items"]),
            timeout=15.0,
        )
    except CtlError as e:
        pytest.fail(str(e))

    texts = [item for item in state["signals"]["items"] if item["kind"] == "text"]
    assert texts
    assert texts[0]["from_name"] == "DANA"
    assert texts[0]["text"] == "omw!"
