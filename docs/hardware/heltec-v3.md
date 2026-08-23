# Two Heltec V3 boards: bench mesh, landmark beacons, range data

> **Status: written, not yet run.** Unlike [`firmware/tools/dev/README.md`](../../firmware/tools/dev/README.md),
> nothing in this file has been executed against real hardware yet — the
> boards exist, the procedure doesn't have results behind it. Every claim
> below is from documentation and datasheets. **Correct this file as you go**,
> the same way the dev README calls out where behaviour surprised us. The
> range-test table at the end is deliberately empty; an empty cell means we
> don't know, which is the whole point of this project.

## What the board is

**Heltec WiFi LoRa 32 V3** — ESP32-S3, SX1262 radio, 0.96" SSD1306 OLED,
USB-C, JST-1.25 battery connector with charging. 915 MHz (US) variant.

**No GNSS. No magnetometer.** That rules it out as a puck or as any node
that has to know where it is — and makes it an exact fit for the
installed-landmark tier in [issue #30](https://github.com/jakeholland/firefly/issues/30),
which is defined by asserting a configured position rather than measuring one.

⚠️ **Never power the board with the antenna detached.** Transmitting into an
open port can destroy the SX1262's output stage. Attach the antenna first,
every time, including on the bench.

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
4. **Set the LoRa region before anything else.** The radio will not transmit
   until a region is set — a silent no-op that reads exactly like a broken
   board. US 915 MHz here.
5. Set the same **channel and PSK** on both boards, and eventually on the
   pucks. Nodes on different channels are invisible to each other, not
   merely quiet.

Record the firmware version you flashed in the range-test table — LoRa
behaviour changes between releases, and a range number without a version
attached isn't reproducible.

## Use 1 — a real two-node mesh (closes the `xfail` gap)

Two `xfail(strict=False)` end-to-end tests, `test_pulse_reaches_feed` and
`test_text_roundtrip`, are blocked for a reason that has nothing to do with
our code: a single `meshtasticd` accepts only one client connection at a
time, and the native build compiles out the UDP-multicast code that would
let two simulated instances hear each other. The dev README documents both,
with the log lines that proved them.

Two real radios sidestep the whole problem. `crew_sim.py` drives node A over
serial, `ffsim --connect` attaches to node B, and a packet has to make an
actual RF hop to arrive — which exercises more than the simulated version
ever would have. Tracked as [issue #31](https://github.com/jakeholland/firefly/issues/31);
the hardware-in-the-loop suite should skip cleanly when no boards are
attached, matching how the docker tests skip today.

## Use 2 — prototyping the landmark tier

A landmark node (medical, water, gate, art car) is placed by someone who
knows where it is, so it can carry a configured position. Meshtastic supports
exactly this with a fixed position — set latitude/longitude/altitude and
enable fixed-position mode, and the node reports that spot without a GPS.

Two Firefly-side rules make it honest, and both need protocol support before
v1 freezes:

- **Provenance.** A fixed position is *asserted*, not *measured*. The puck must
  render it differently from a live fix and must never age it into looking
  like a stale measurement — the two are different in kind, the same way LOST
  differs from STALE on the radar face.
- **Event tag.** A node configured for last year's site, powered up at this
  year's, will confidently point at a field. Tagging the configured event lets
  the puck flag the mismatch instead of trusting it.

Role matters too: a landmark sitting at chest height in a crowd is a poor
relay. Prefer a role that receives and reports without rebroadcasting;
reserve router roles for genuinely elevated, well-sited nodes, per Meshtastic's
own guidance. Verify the exact role names against the firmware you flash.

## Use 3 — the range numbers we don't have

Every range figure in the plan comes from datasheets and other people's
reports. None of it is measured, and the number that decides whether the
premise holds isn't line-of-sight distance — it's **distance with a human
body between the antennas**, because that's the normal case in a crowd and
2.4 GHz-style body attenuation is exactly what kills wearable radios.

Meshtastic ships a range-test module that sends numbered packets on an
interval; pairing a phone to the receiving node gives per-packet RSSI and
SNR regardless of whether on-device logging works on this board. Record the
**modem preset** with every run — it changes range more than anything else
you can control, and a result without it is meaningless.

Method: one board stationary with a known position, walk the other away,
note the distance where packets stop arriving reliably (not where the first
one drops — where it stops being usable).

| Scenario | Preset | Firmware | Distance | RSSI / SNR at edge | Notes |
|---|---|---|---|---|---|
| Open line of sight | | | | | |
| Through a treeline | | | | | Lost Lands is wooded — this is the one that matters |
| Body-blocked (board against chest, walk away) | | | | | The realistic crowd case |
| Both at chest height, ~100 people between | | | | | Hard to stage honestly; approximate and say so |
| Board in a pocket | | | | | People will do this |

Fill it in, note the date and site, and treat a blank cell as unknown rather
than fine.
