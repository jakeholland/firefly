# Two Heltec V3 boards: bench mesh, landmark beacons, range data

> **Status: written, mostly not yet run.** Unlike [`firmware/tools/dev/README.md`](../../firmware/tools/dev/README.md),
> most of this file has not been executed against real hardware yet — the
> boards exist, the procedure doesn't have results behind it. Claims below
> were checked against primary sources (Heltec's datasheet, Meshtastic's docs
> and firmware source) in PR #32's review, but checked ≠ run. The exception
> is marked **hardware-verified** inline (bring-up step 6, the channel
> position-precision trap — measured on both boards, 2026-08-23).
> **Correct this file as you go**, the same way the dev README calls out
> where behaviour surprised us. The range-test table at the end is
> deliberately empty; an empty cell means we don't know, which is the whole
> point of this project.

## What the board is

**Heltec WiFi LoRa 32 V3** — ESP32-S3, SX1262 radio, 0.96" SSD1306 OLED,
USB-C, JST-1.25 battery connector with charging. 915 MHz (US) variant.

**No GNSS. No magnetometer.** Confirmed three ways: Heltec's own HTIT-WB32LA
datasheet mentions neither, Meshtastic's device page lists "No GPS", and
`variants/esp32s3/heltec_v3/variant.h` in the firmware defines no `HAS_GPS`
and no magnetometer pins. That rules the board out as a puck or as any node
that has to know where it is — and makes it an exact fit for the
installed-landmark tier in [issue #30](https://github.com/jakeholland/firefly/issues/30),
which is defined by asserting a configured position rather than measuring one.

⚠️ **Attach the antenna before powering the board on, every time.** Meshtastic's
own documentation is unambiguous about this. The commonly repeated mechanism —
that the datasheet forbids it — isn't quite what Semtech says: the SX1262
datasheet specifies a maximum operating VSWR of 10:1 rather than an
antenna-detached damage clause, and Heltec's own docs carry no warning at all.
Follow the practice regardless; the cost of being wrong is a dead radio and
the cost of being right is nothing.

## Flashing stock Meshtastic

The comms brain runs stock, unmodified Meshtastic ([why](../ARCHITECTURE.md)),
and so do these.

1. Attach the antenna.
2. Open <https://flasher.meshtastic.org> in Chrome or Edge (it needs WebSerial;
   Safari and Firefox won't work). Select **Heltec V3**, pick the current
   stable release, flash.
3. If no serial port appears, install the USB-UART driver for the board's
   bridge chip and re-plug. A cable that only carries power and not data is
   the other usual cause.
4. **Set the LoRa region.** Until you do, the firmware disables both transmit
   *and* receive (`RadioLibInterface.cpp`) — the node is not merely quiet, it
   is deaf as well as mute. On a screen-equipped board like this one you can't
   miss it: `Screen.cpp` forces a region picker that deliberately never times
   out, so an unset region is the loudest failure in the whole bring-up rather
   than a silent one. US 915 MHz here.
5. Set the same **channel and PSK** on both boards, and eventually on the
   pucks. Nodes on different channels are invisible to each other, not
   merely quiet — and this one *is* silent.
6. **Make it Firefly's own channel, with `position_precision` set to 32
   (or 24).** ⚠️ **Hardware-verified** — unlike most of this file, this
   step has real measurements behind it (both V3s, firmware 2.7.26,
   2026-08-23; raw numbers in
   [issue #47](https://github.com/jakeholland/firefly/issues/47)): the
   default public channel ships `positionPrecision: 13`, which truncates
   every broadcast position to a **~5.8 km grid** — we set a fixed
   position on one board and read it back **2673 m off** on the other.
   It's a deliberate, sensible privacy feature of the public channel, and
   it silently destroys Firefly's entire distance vocabulary: the
   position arrives *fresh*, the freshness logic is satisfied, and the
   arrow points confidently at a cell containing most of a festival.
   Step 5's "same channel and PSK" is necessary but **not sufficient** —
   if the shared channel is the default public one, everything appears
   to work and every distance is quietly wrong at the kilometre scale.
   A distinct PSK is needed for pairing anyway; `position_precision: 32`
   is full precision, 24 is ~3 m (comfortably below GPS error). Detecting
   and surfacing degraded precision on the puck side is follow-on
   meshclient work tracked in #47.

Record the firmware version you flashed in the range-test table — LoRa
behaviour changes between releases, and a range number without a version
attached isn't reproducible.

## Use 1 — a real two-node mesh (closes the `xfail` gap)

Two `xfail(strict=False)` end-to-end tests, `test_pulse_reaches_feed` and
`test_text_roundtrip`, are blocked for a reason that has nothing to do with
our code: a single `meshtasticd` accepts only one client connection at a time,
so there's no window in which `ffsim` can observe a transient packet. (The
related finding that two *containers* can't hear each other either — the
native build compiles out the UDP-multicast source — is why the obvious
workaround also fails. Two separate limitations, both documented in the dev
README with the log lines that proved them.)

Two real radios sidestep the whole problem: a packet has to make an actual RF
hop to arrive, which exercises more than the simulated version ever would have.

**Connect over WiFi, not serial.** This matters, because the existing tooling
can't do it any other way: `crew_sim.py` is TCP-only (it builds a
`TCPInterface`, and its CLI takes `--host/--port`), and `ffsim --connect`
parses `HOST:PORT` and nothing else (`ff_parse_host_port`). Neither speaks
serial today. Both V3s have WiFi, and Meshtastic exposes the same client API
on **TCP 4403** — so join each board to the network, point `crew_sim.py` at
node A's address and `ffsim --connect` at node B's, and everything works with
zero new plumbing. Tracked as [issue #31](https://github.com/jakeholland/firefly/issues/31);
the hardware-in-the-loop suite should skip cleanly when no boards are
reachable, matching how the docker tests skip today.

## Use 2 — prototyping the landmark tier

A landmark node (medical, water, gate, art car) is placed by someone who knows
where it is, so it can carry a configured position. Meshtastic supports exactly
this: set `position.fixed_position`, and set coordinates with `--setlat`,
`--setlon` and `--setalt` — each of which enables fixed-position mode on its
own. `--remove-position` clears it again.

⚠️ **The trap, and it's the exact thing this tier exists to avoid.** The docs
note that a fixed-position node uses *the last saved coordinates*. So a board
that once had a real GPS fix and was later pinned will broadcast a **stale
measured** position flying an **asserted** flag — the ambiguity we're trying to
design out, arriving through the very mechanism meant to resolve it. Always set
coordinates explicitly when pinning a node; never assume the flag alone means
someone chose that spot. Folded into
[issue #33](https://github.com/jakeholland/firefly/issues/33).

Two Firefly-side rules make this honest:

- **Provenance.** A fixed position is *asserted*, not *measured*. The puck must
  render it differently from a live fix and must never age it into looking
  like a stale measurement — the two are different in kind, the same way LOST
  differs from STALE on the radar face. See #33: this is a real freshness bug,
  not a nicety.
- **Event tag.** A node configured for last year's site, powered up at this
  year's, will confidently point at a field. Tagging the configured event lets
  the puck flag the mismatch instead of trusting it.

Role matters too: a landmark sitting at chest height in a crowd is a poor relay.
**`CLIENT_MUTE`** is the right answer — it receives and reports without
rebroadcasting. Verify the enum against the firmware you actually flash, though,
because this part of Meshtastic is genuinely moving: `ROUTER_CLIENT` was removed
in 2.3.15, `REPEATER` is deprecated as of 2.7.11, and `ROUTER_LATE` is now the
middle ground. Reserve router roles for genuinely elevated, well-sited nodes.

## Use 3 — the range numbers we don't have

Every range figure in the plan comes from datasheets and other people's
reports. None of it is measured, and the number that decides whether the
premise holds isn't line-of-sight distance — it's **distance with a human body
between the antennas**, because that's the normal case in a crowd.

**Terrain is the number one limitation on LoRa range**, per Meshtastic's own
guidance — which is why the scenarios below are about trees, bodies and
pockets rather than settings. Modem preset matters, but it is not a knob you
tune per node: it must be identical across the whole mesh to work at all.
Record which preset a run used so results are comparable; don't expect to
optimise your way out of a treeline.

Meshtastic ships a range-test module that sends numbered packets on an
interval. On ESP32 boards like this one it can log to `rangetest.csv` — but
**that CSV records SNR and not RSSI** (open FRs #2372, #7731), so fill the
RSSI column from a paired phone or leave it blank rather than inventing it.

Method: one board stationary with a known position, walk the other away, note
the distance where packets stop arriving reliably (not where the first one
drops — where it stops being usable).

| Scenario | Preset | Firmware | Distance | SNR at edge | RSSI (phone) | Notes |
|---|---|---|---|---|---|---|
| Open line of sight | | | | | | Baseline only — terrain dominates everything below |
| Through a treeline | | | | | | Lost Lands is wooded — this is the one that matters |
| Body-blocked (board against chest, walk away) | | | | | | The realistic crowd case |
| Both at chest height, ~100 people between | | | | | | Hard to stage honestly; approximate and say so |
| Board in a pocket | | | | | | People will do this |

Fill it in, note the date and site, and treat a blank cell as unknown rather
than fine.
