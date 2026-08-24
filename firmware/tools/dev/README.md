# firmware/tools/dev — the live dev loop (S13 slice d)

Brings up a dockerized meshtasticd stand-in for the comms brain, connects
`ffsim` to it, and drives scenarios against it with `crew_sim.py` — the
loop described in docs/specs/S13-sim-target.md's deliverable 5.

**Everything in this file was actually run, end to end, against the
pinned image (`meshtastic/meshtasticd:GHA-2.6.1.499ea56-debian-
linux_amd64`) while building this slice** — not just written and hoped
to work. Where behavior surprised us, it's called out below and in
`crew_sim.py`'s own docstring, with the exact log lines that proved it.

## Quick start

```sh
# 1. Build ffsim first (from firmware/):
cmake -S . -B build -DFF_TARGET=sim && cmake --build build -j8

# 2. Bring up meshtasticd:
docker compose -f tools/dev/compose.yml up -d
#   (Apple Silicon Macs: the pinned tag is amd64-only, verified to run
#   fine under emulation — `export DOCKER_DEFAULT_PLATFORM=linux/amd64`
#   first if your docker doesn't already default to emulating. Also
#   requires the `docker compose` CLI plugin — `brew install
#   docker-compose` if `docker compose version` errors.)

# 3. pip install the Python side (once):
pip install -r ../tests/e2e/requirements.txt

# 4. Drive a scenario:
python3 tools/dev/crew_sim.py walk --node Dana --from-stage prehistoric --to-stage wompy-woods --speed 1.2

# 5. Connect ffsim and look. --dev-trust-all matters (S16b2): live mode
#    drives the app shell, which drops unpaired senders — and the dev
#    daemon's single node is also "us" — so without the flag a --connect
#    session honestly shows nothing. The flag auto-pairs NodeInfo
#    senders, suspends the self filter, and latches the wall clock from
#    the host (sim-only; compiled out of device builds; logged).
./build/ffsim --headless --ctl 9000 --connect 127.0.0.1:4403 --dev-trust-all \
    --pack festpack/tests/fixtures/lost-lands-2026.festpack.json &
printf '{"cmd":"state"}\n' | nc 127.0.0.1 9000
#   -> {"ok":true,"state":{"radar":{"mode":"live","dist_str":"260 m",...},...,"wall":{"src":"mesh",...}}}

# 6. Tear down:
printf '{"cmd":"quit"}\n' | nc 127.0.0.1 9000
docker compose -f tools/dev/compose.yml down
```

`ffsim --window --connect 127.0.0.1:4403 --pack ...` is *not* wired up
yet — `--ctl` (and therefore live mode's practical use today) requires
`--headless`; see `targets/sim/main.c`'s "ctl loop" comment for why.

## What's real here, and what's a documented, verified limitation

`crew_sim.py`'s module docstring has the full writeup (constraints,
evidence, and exactly what does/doesn't work); the short version:

- **`walk` genuinely works end to end.** A node's position, set via
  `crew_sim.py walk`, is picked up by `ffsim --connect`'s handshake and
  flows through `ff_crew`/`ff_radar_compute` into the radar face's
  `dist_str`/`arrow_deg`/etc — verified repeatedly against the real
  container in this repo's dev session (and pinned down as
  `test_position_reaches_radar` in `firmware/tests/e2e/`).
- **`pulse`/`status`/`text` are real and correctly wire-format-encoded**
  (verified against the pinned image), but **cannot reach an
  already-connected `ffsim`** through a single stock meshtasticd
  instance: it only accepts one client TCP connection at a time (a new
  one force-closes the old one — see its own logs:
  `"[ApiServer] Force close previous TCP connection"`), and transient
  Data packets (unlike positions, which persist in the daemon's NodeDB)
  are only observed by whoever's connected at the instant they're sent.
  Since crew_sim.py must itself be the connected client to send at all,
  and doing so kicks ffsim off, there is no window in which ffsim can
  observe the packet. `firmware/tests/e2e/test_pulse_reaches_feed` and
  `test_text_roundtrip` encode this as `xfail(strict=False)` with the
  same explanation, not a skip and not a deletion — the scenarios are
  real and worth having ready to go once the gap below is closed.
- **Two separate meshtasticd containers do not hear each other by
  default either — and it's not a missing config flag, it's dead code.**
  Verified directly (two instances on one docker network, broadcast from
  one, nothing arrives at the other), then root-caused in PR #19's
  independent review by trying the real documented feature for this
  (`network.enabled_protocols = UDP_BROADCAST`, still no delivery) and
  reading the firmware source: the native/Linux build's
  `variants/native/portduino.ini` sets `HAS_UDP_MULTICAST=1` for header
  compatibility, but its `build_src_filter` excludes `mesh/wifi/` and
  `mesh/eth/` — the only places that start the UDP-multicast thread. The
  macro is defined; the code that would act on it isn't compiled in. See
  `firmware/tests/e2e/test_scenarios.py`'s module docstring for the full
  writeup. Genuine multi-node RF simulation needs either real hardware or
  the Meshtastic project's separate Meshtasticator interactive-sim
  tooling (a different project, not something `docker compose up`
  provides) — flagged as follow-up work, not attempted here.
- **`crew_sim.py rename` (renaming the connected node's identity) reboots
  meshtasticd, and the pinned container image does not survive that
  reboot** (`execv() returned -1! error: No such file or directory`,
  then the container exits). This is why `walk`/`pulse`/`status`/`text`
  don't rename the node on every call — `--node NAME` is a display label
  for scenario narration, not an in-mesh identity change. Use `rename`
  deliberately, and expect to restart the container afterward.
- **Position updates from the client API are rate-limited and
  precision-truncated — both silent, neither obvious from the Python API
  alone.** A tight loop of `sendPosition()` calls gets most of them
  dropped (`WARN ... [ServerAPI] Rate limit portnum 3` in the daemon's
  own logs only); an *accepted* update's readback doesn't exactly equal
  what was sent (`Truncate phone position to channel precision 13` — a
  real per-channel privacy feature). `crew_sim.py walk` accounts for
  both: it throttles real sends to at most one per
  `MIN_POSITION_SEND_INTERVAL_S` and waits for *a* position to land
  rather than an exact match (see that module's docstring and
  `_wait_for_a_position`). Net effect: `walk` is reliable but can take up
  to about a minute — this is real daemon behavior, not something
  crew_sim.py can make faster.

## Files

- `compose.yml` — the pinned meshtasticd container (see its own header
  comment for the exact tag and why "no radio" needs zero extra config:
  meshtasticd logs `"No 'config.yaml' found, running simulated."` and
  `"Use SIMULATED radio!"` on its own when no hardware is passed
  through — confirmed, not assumed).
- `crew_sim.py` — scenario API + CLI (`walk`/`pulse`/`status`/`text`/
  `rename`). `python3 crew_sim.py --help` / `crew_sim.py <cmd> --help`
  for the full flag list.
- `CTL.md` — ffsim's control socket protocol reference (S13 slice c).
- `../tests/e2e/` — the pytest suite this loop feeds (S14 slice d).
