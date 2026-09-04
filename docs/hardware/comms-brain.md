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
