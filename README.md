# Firefly

**An open-source festival compass that points at your friends.**

Firefly is a palm-sized round-screen puck for music festivals: a compass arrow that points at your crew over LoRa mesh radio, off-grid messaging (pulses, canned replies, T9), the full lineup with set alarms, and a vector map of the grounds — so your phone stays at camp.

> Status: **pre-hardware**. Parts are ordered, screens are designed, firmware starts when the boards land. First field test: Lost Lands 2026.

## How it works

Firefly is a **dual-brain** device:

- **Comms brain** — a Seeed XIAO ESP32S3 + Wio-SX1262 stack running **stock, unmodified [Meshtastic](https://meshtastic.org)** with an L76K GPS. It owns the mesh, encryption, and positions, and gets free interop with every Meshtastic node on site.
- **UI brain** — a Waveshare ESP32-S3-Touch-LCD-1.46 (412×412 round touchscreen, IMU, mic) running the Firefly app on LVGL. It talks to the comms brain over UART using Meshtastic's documented client API — the same protocol the official phone apps speak.

No fork to maintain, no phone required, ~$80 in parts, multi-day battery.

## Feature set (v1)

| Face | What it does |
|---|---|
| **Radar** | Big honest arrow + distance to a crew member; whole-crew dots; live/stale/close-range states |
| **Now** | Lineup per stage, set progress, starred-set alarms |
| **Signals** | Pulses, rally points, canned replies, T9 composer |
| **Map** | Vector festival grounds from a festival pack, crew dots included |
| **Flare** | Press-and-hold "come find me" — crew pucks buzz and lock their arrows on you |

Festival data (map polygons + lineup + set times) loads as a **festpack** — see [fest-almanac](https://github.com/jakeholland/fest-almanac), the open per-festival data repo.

## Repo layout

```
firmware/   UI-brain application (LVGL, Meshtastic client API)
case/       3D-printable enclosure (~82mm puck, PETG, glow diffuser ring)
web/        Browser flasher + puck setup tools
docs/       Architecture, power budget, build guide
```

## Prior art & thanks

Standing on the shoulders of [Meshtastic](https://meshtastic.org), the [Friend Finder Edition](https://github.com/LeapYeet/Meshtastic-Firmware-Friend-Finder-Edition) fork, [iBurn](https://github.com/iBurnApp/iBurn-Data), and the badge scene (EMF Tildagon, badge.team). The consumer product that proved this market — Lynq — sold 20k units and then left for defense contracts. This one can't be taken away.

## History

This repo previously held a Meshtastic iOS/macOS client app — preserved at the [`archive/meshtastic-ios-app`](../../tree/archive/meshtastic-ios-app) branch and the `v0-ios-app` tag. Its BLE + protobuf client code remains a useful reference for the UI brain.

## License

TBD (before first release).
