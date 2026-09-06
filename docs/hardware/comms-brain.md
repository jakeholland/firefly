# Comms brain: wiring and setup

The UI puck (Waveshare ESP32-S3-Touch-LCD-1.46) talks to the comms brain
(Seeed XIAO ESP32S3 + Wio-SX1262 running stock Meshtastic) over a 4-wire UART
link at 115200 8N1, 3.3 V logic both sides, no level shifter. Wiring diagram
(same content, drawn): https://claude.ai/code/artifact/3fcda222-93b5-41fb-9b63-eed48aa1336a

Pin facts verified 2026-09-04 from docs.waveshare.com (ESP32-S3-Touch-LCD-1.46),
wiki.seeedstudio.com (XIAO ESP32S3) and meshtastic/firmware
`variants/esp32s3/seeed_xiao_s3/variant.h`.

## Pin map

| Wire   | Puck header      | XIAO pin      | Meshtastic setting |
|--------|------------------|---------------|--------------------|
| data   | TXD · GPIO43     | D3 · GPIO4    | `serial.rxd 4`     |
| data   | RXD · GPIO44     | D1 · GPIO2    | `serial.txd 2`     |
| ground | GND              | GND           |                    |
| power  | 3V3 (or shared battery, see below) | 3V3 (or BAT+) | |

- The puck's console is USB-Serial-JTAG ONLY as of the S15c review round (`firmware/targets/esp32s3/sdkconfig.defaults`/`sdkconfig.ci`: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, secondary NONE) — a fresh checkout's UART header pins GPIO43/44 are free for exactly this reason. ESP-IDF's stock default before that fix kept UART0 (GPIO43/44's own default peripheral) as the PRIMARY console with USB-Serial/JTAG only a secondary mirror, so the boot log was actually sharing these pins; see `docs/specs/S15-esp32s3-target.md`'s Amendments for the full reasoning (shared-FIFO corruption risk) and the maintainer action items below.
- **If your board's `sdkconfig` predates this fix** (drift rule, `docs/specs/S15-esp32s3-target.md`): a fresh `sdkconfig.defaults` entry does not reach an already-generated `sdkconfig`. Delete every `CONFIG_ESP_CONSOLE_*` key from the board's `sdkconfig` (or `idf.py fullclean` + reconfigure) and set `CONFIG_FF_LINK_UART=y` by hand before wiring up the comms brain — otherwise the boot log is still on GPIO43/44 despite this file's own claim above, and `FF_LINK` silently stays `NONE`.
- A residual, un-Kconfig-able boot-garbage window remains regardless: the ROM's own first-stage boot banner is hardware-fixed to UART0's default pins and prints before any Kconfig setting is even read. Brief, one-time per boot, and absorbed by whichever side's framer resync counter sees it — see the S15 Amendment for the full reasoning. This is why the mesh link itself stays on UART1 (`FF_UART_PORT` default), not UART0, even with the console moved off it.
- **Do not use the XIAO's D6/D7 (GPIO43/44)**: Meshtastic's `seeed_xiao_s3` variant
  assigns them to the L76K GPS (`GPS_TX_PIN 43`, `GPS_RX_PIN 44`). This supersedes
  S15 deliverable 3's "D6/D7" wording.
- D2 (GPIO3) is skipped (strapping pin). D8–D10 (GPIO7/8/9) are the radio's SPI;
  NSS 41, RST 42, BUSY 40, DIO1 39, RXEN 38 are on the B2B connector.

## Power

- **Option B (recommended for the field):** one LiPo, Y-split to both boards'
  battery inputs (puck MX1.25; XIAO BAT+/BAT- pads on the back). Each board keeps
  its own regulator. Charge through ONE board's USB at a time (two chargers on
  one cell must not run together). The XIAO stays on when the puck latches off.
- **Option A (bench):** puck `3V3` header → XIAO `3V3` pin. The puck's MP1605 buck
  is rated 2 A; the SX1262 TX burst is ~120 mA. The XIAO's 3V3 pin is its
  regulator OUTPUT; back-feeding it is common on XIAO but not documented by Seeed —
  measure before trusting it in the field.
- Never use the puck's `5V` pin on battery (USB-only). Never power the Wio-SX1262
  without its antenna.

## Configure the XIAO once (USB, Python CLI)

Flash stock Meshtastic "Seeed XIAO S3" with the web flasher, then:

```
meshtastic --set lora.region US
meshtastic --set serial.enabled true --set serial.mode PROTO \
           --set serial.baud BAUD_115200 --set serial.txd 2 --set serial.rxd 4
meshtastic --set bluetooth.enabled false        # optional, saves power
meshtastic --set-owner "Jake"
meshtastic --ch-set name Firefly --ch-set psk random --ch-index 0   # copy the PSK to every crew node
```

PROTO mode exposes the protobuf client API (the same one the phone app uses) on
those pins; the puck's meshclient (S03) speaks it.

## Bring-up order

1. Before the boards arrive: jumper puck TXD↔RXD on its header, flash with
   `CONFIG_FF_UART_LOOPBACK_SELFTEST=y`, confirm the loopback PASS line in the
   boot log (proves the driver + header pins alone).
2. Flash + configure the XIAO over USB (above). Antenna on. Confirm it in the
   Meshtastic app/CLI.
3. Power both off. Wire the four lines. Check the cross: puck TXD → XIAO D3,
   puck RXD ← XIAO D1.
4. Power up. Puck log: `link: UART… TXD=43 RXD=44 115200`, then the meshclient
   handshake and READY within a few seconds. Silence = TX/RX swapped; garbage +
   rising resync counters = wrong baud.
5. A second node (T1000-E / Heltec) sends a position → Radar within 10 s (S15 AC2).

## Pairing (crew roster) — bench/field stopgap until S12 ships

A CONNECTED link is not enough on its own: pairing v1 is "channel membership
+ explicit crew list" (`docs/specs/S04-firefly-protocol.md`), and the shell's
roster-trust policy (S16) refuses to grow the paired crew list from anything
the radio says — only an explicit user pairing action does that
(`ff_shell_pair`). Until the real Crew screen (S12) exists to drive that
action, a puck that is CONNECTED to its comms brain but has never been
paired shows "no crew linked" and silently drops every position/text from
the other node — not a bug, the policy working as specified, just with no UI
yet to use it.

**Bench/field stopgap:** set `CONFIG_FF_DEV_TRUST_CHANNEL=y`
(`idf.py menuconfig` → Firefly bring-up, or by hand in `sdkconfig`) until
S12 ships. Every crew channel is already private (its own PSK, set once when
configuring the XIAO above), so on this bench/field setup channel membership
already IS the crew — this option auto-pairs any node heard on that channel
(NodeInfo only, the same S16 AC6 mechanism the sim's `ffsim --dev-trust-all`
uses in dev). Off by default; it must never ship on by default, and the
Kconfig help text says so. Watch for
`firefly: DEV_TRUST_CHANNEL on — auto-pairing every heard node` in the boot
log to confirm it took. See `firmware/app/include/ff_shell.h`'s
`ff_shell_dev_trust_all` doc comment for exactly what this does and does not
change versus the sim flag it mirrors.

## Field festpack (CONFIG_FF_FIELD_PACK)

A live (non-demo) build loads the real field festpack by default: `CONFIG_FF_FIELD_PACK=y`
(default `y`, `depends on !CONFIG_FF_DEMO_MODE` — mutually exclusive with demo
mode, enforced by both the Kconfig `depends on` and a compile-time `#error`
in `app_main.c`) embeds `firmware/assets/field/lost-lands-2026.festpack.json`
(a verbatim copy of fest-almanac's real Lost Lands 2026 pack — see that
directory's own README for how to refresh it; never hand-edit it) and, on
boot, parses it into the shell via `ff_shell_load_pack` at the same point a
demo build's `ff_demo_seed` runs. That is ALL it does: no crew, no
positions, and no clock are seeded (honest data — `CLAUDE.md`) — only the
festpack itself, so the MAP face gets a real origin and Settings gets a real
UTC offset, from the real mesh. Watch for `firefly: S05 field pack loaded: ...`
(or the parse-error log line) in the boot log to confirm it took.

**Wall-clock consequence:** loading the pack also tightens the S18 wall-clock
plausibility window to the festival's own dates via `ff_wall_window_from_pack`
(±14 days either side of `start_doy`/`end_doy`). For Lost Lands 2026 (Sep
18–20) that window is **2026-09-04T00:00:00Z through 2026-10-05T00:00:00Z**
(unix 1788480000 through 1791158400, exclusive ceiling) — every mesh
timestamp outside it is rejected by the plausibility gate at ANY trust tier
(the same gate `CONFIG_FF_DEV_TRUST_CHANNEL` above has no effect on). A bench
run before **2026-09-04** would have had every incoming timestamp rejected
and the wall clock stuck on the fixed bootstrap window — keep that in mind
scheduling any pre-field bench test.

## Bench console

An opt-in line-command console lets an end-to-end test (or a human at a
terminal) trigger sends, flares, and state dumps over the puck's USB
port — no touchscreen tap needed. Off by default (never ship it on for
a field build): set `CONFIG_FF_DEBUG_CONSOLE=y` (`idf.py menuconfig` →
Firefly bring-up → "Bench/debug console", or by hand in `sdkconfig`).

It reads line commands off the USB-Serial-JTAG port — the SAME port
that already carries the boot log — and is polled only while USB is
connected (the S26f amendment's own connection sample), so it changes
nothing about battery/field operation. Connect with any plain serial
terminal at the puck's USB-Serial-JTAG port (e.g. `idf.py monitor`, or
`screen /dev/tty.usbmodem* 115200` — the console doesn't care about
baud, it's USB-CDC), type a command, press Enter.

Commands (replies are `dbg: `-prefixed; an unrecognized or malformed
line replies `dbg: ? try help`):

| Command | Effect |
|---|---|
| `help` | list the commands below |
| `me` | my node id, link state, my position (ok/lat/lon/age), wall clock (latched?/trust/unix now) |
| `roster` | paired crew: id, name, presence, position |
| `heard` | heard-but-unpaired node ids |
| `send <text>` | crew broadcast — the same send mechanism the composer's SEND button uses |
| `dm <node_hex> <text>` | addressed send to one node (hex, optional `!` or `0x` prefix) |
| `flare` | start a quick flare (same path as the physical 5-tap gesture) |
| `flare cancel` | cancel a flare in progress |
| `wall` | wall-clock latch dump: latched, trust tier, UTC offset, last observation's source node |

Every acting command dispatches through `ff_shell_intent` (the SAME
`FF_INTENT_QUICK_FLARE`/`FF_INTENT_FLARE_END` intents the physical
5-tap gesture and the sender overlay's CANCEL button use) or the
debug-only `ff_shell_debug_send_text` seam (`app/include/ff_shell.h`)
— never a new roster-growth path, never a direct mesh send bypassing
the shell. See that header's doc comment and
`app/include/ff_debug_console.h` for the full seam discipline, and
`core/include/ff_dbgcmd.h` for the line parser (table-driven, bounded,
CRLF-tolerant, unit-tested independent of any shell).

Example session (paired with one crew member, `Dana`, `!0000da1a`):

```
me
dbg: me node=!00001000 link=CONNECTED
dbg: me pos ok=1 lat=40.712800 lon=-74.006000 age_ms=1500
dbg: me wall latched=1 trust=TRUSTED unix=1789768900

roster
dbg: roster n=1
dbg: roster id=!0000da1a name=Dana presence=LIVE has_pos=1 lat=40.700000 lon=-74.010000

send crew, meet at the tent
dbg: send ok dest=broadcast

dm da1a on my way
dbg: dm ok dest=!0000da1a

flare
dbg: flare started dur_s=300

flare cancel
dbg: flare cancelled

wall
dbg: wall latched=1 latch_unix=1789768800 trust=TRUSTED offset_min=-240 assumed=0 last_src=!0000da1a rejected=0

xyzzy
dbg: ? try help
```

## The puck's back header (photo, 2026-09-04)

2×10 at 1.27 mm pitch. Left column top→bottom: `13 · 12 · RXD · TXD · G · 3V3 · SDA · SCL · G · BAT`.
Right column: `17 · 16 · 15 · 14 · 1 · 0 · DP · DN · G · 5V`. The four link wires sit
together on left rows 3–6 (RXD 44, TXD 43, G, 3V3). Of the extras: 14/16/17 are the
SD card, 15 is the mic clock; 12/13/0/1 are free GPIO.

**Power option C (via the header):** `BAT` → XIAO `BAT+` pad, `G` → `BAT-`. Measure `BAT`
with the puck OFF first: ~3.7–4.2 V = raw cell (XIAO stays on when the puck latches off);
0 V = behind the latch (XIAO powers down with the puck — preferable). One charger at a time.

**Soldering an IDC ribbon directly:** on a 2×10 IDC, conductors alternate rows
(conductor 1 → pin 1, 2 → the opposite pin, 3 → next down the first row, …); pin 1 is the
red-stripe edge. Confirm each stripped conductor against the silkscreen with a continuity
beep before soldering; only 4–5 conductors are needed (RXD, TXD, G, 3V3, optionally BAT).
Heat-shrink each joint and strain-relieve the ribbon to the case.

## Compass (magnetometer)

S15's heading driver (`firmware/targets/esp32s3/components/ff_compass/`) reads
a GY-273 magnetometer off the SAME back header, plus the board's own onboard
QMI8658 6-axis IMU (soldered to the main PCB, no wiring needed) for tilt
compensation. Before this driver landed, `heading_deg` read -1 forever
(docs/specs/S12-first-run.md's 2026-09-03 amendment) — see
`firmware/targets/esp32s3/components/ff_compass/include/ff_compass.h` for the
full contract and register-level citations; this section is the wiring +
bench-facing summary.

**Wiring:** the GY-273's four pins go on the same back header used for the
comms-brain link above — `VCC`→`3V3`, `GND`→`G`, `SDA`→the header's `SDA`
(GPIO11), `SCL`→the header's `SCL` (GPIO10). `DRDY` is unconnected (this
driver polls, it does not use the data-ready interrupt). This is the SAME
physical I2C bus the SPD2010 touch controller (0x53) and TCA9554 IO expander
(0x20) already share — the compass driver adds its own device(s) onto that
existing bus (`ff_display_i2c_bus()`), it never opens a second I2C master on
these pins.

**Chip auto-detection:** most GY-273 boards actually carry a **QMC5883L**
(I2C address `0x0D`) even when silkscreened "HMC5883L"; a genuine
**HMC5883L** (`0x1E`) does turn up on some. `ff_compass_init` probes both at
boot via each chip's own identification registers and logs which it found
(or "no magnetometer" if neither ACKs/identifies — the puck still boots and
runs; the Radar arrow just cannot point). The onboard **QMI8658** IMU is at
`0x6B` (alt strap `0x6A`); if it fails to identify, the driver falls back to
an assumed-level accel and logs `compass: no IMU — assuming level` — tilt
rejection is unavailable on that path (a synthesized always-level reading can
never indicate tilt).

**Orientation — verify on the bench.** The magnetometer's mounting
orientation (which physical axis is which) is an ASSUMPTION, documented and
isolated to one small `#define` table in `ff_compass.c` ("Axis mapping:
sensor frame -> board frame"), not verified against a real GY-273 glued into
a case yet. To check/correct it:

1. Flash a build with `CONFIG_FF_COMPASS=y` (default) and get to the Radar
   face with at least one paired friend so the arrow renders.
2. Rotate the puck flat (screen up) through a full circle by hand, slowly.
   The arrow should track — pointing at the same real-world friend direction
   regardless of which way the puck is held.
3. If the arrow doesn't move at all: check `ff_compass: no magnetometer` /
   `no IMU` in the boot log first — a wiring or address problem, not an axis
   problem.
4. If the arrow moves but in the wrong direction: 90°-off (arrow leads or
   lags the true rotation) means the X/Y source axes are swapped in the
   `FF_MAG_BOARD_*_SRC` defines; mirrored (arrow turns the opposite way from
   the puck) means a sign needs flipping (`FF_MAG_BOARD_*_SIGN`). Both are
   isolated to that one table — no other file encodes the mapping.
5. Tilting the puck should not make the arrow swing wildly (that's what the
   QMI8658 tilt compensation is for); if it does with the IMU confirmed
   present, check the `FF_IMU_BOARD_*` half of the same table.

**Calibration status:** `ff_settings_t.compass_cal` / `.cal_valid`
(`core/include/ff_settings.h`) is the persisted calibration slot; this driver
loads it at boot via `ff_shell_settings()` and applies it if `cal_valid` is
set, else runs uncalibrated (identity — no offset, unit scale, zero
declination). No calibration ritual UI exists yet to ever set `cal_valid`
true (S12's figure-eight ritual, still unimplemented per that spec's own
2026-09-03 amendment) — `ff_compass_set_cal()` is the runtime seam that
ritual will call once it exists. Expect headings to be off by whatever the
local hard/soft-iron environment (case magnets, nearby electronics) imposes
until that ritual ships.
