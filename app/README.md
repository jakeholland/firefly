# app — the Firefly companion app (A01)

A native Swift/SwiftUI companion for the Firefly mesh: **one multiplatform
app** for iOS 17+ and macOS 14+. The architecture, the reuse assessment
and the work slices live in
[`docs/specs/A01-companion-app.md`](../docs/specs/A01-companion-app.md);
this file is only how to build and run it.

The short version of why it exists: both Waveshare puck screens are
broken, so the mesh is currently two Heltec V3s running stock Meshtastic
plus a phone. The app is a Meshtastic client that runs **the puck's own C
core** — `firmware/core`, compiled in place, not ported — so crew
freshness, radar geometry, inbox threading and FIND behave identically on
both.

## Layout

```
app/
  FireflyKit/            SwiftPM package — everything that is not UI
    Sources/
      FireflyCore/       firmware/core + firmware/platform, SYMLINKED in
      MeshtasticProto/   generated SwiftProtobuf types (committed)
      FireflyMesh/       transports + Meshtastic client protocols
      FireflyModel/      view models, theme, presentation rules
    Tests/               one test target per source target
  Firefly/Sources/       the SwiftUI app shell
  Firefly/Resources/     Info.plist, entitlements
  Firefly.xcodeproj      committed; regenerate from project.yml
  project.yml            xcodegen input — the source of truth for the project
  tools/                 link_core_sources.sh, gen_swift_protos.sh
```

## Build and test the package

Nothing but a Swift toolchain is needed — no Xcode project, no simulator:

```sh
cd app/FireflyKit
swift build
swift test
```

`swift test` runs every unit test in the package, including the two
guards that keep the app and the puck from drifting apart:
`CoreSourceLinkTests` (every `firmware/core` source is still linked in)
and `ProtobufPinTests` (the Swift and nanopb generators still pin the
same `meshtastic/protobufs` commit).

## Build the app

```sh
cd app
xcodebuild -project Firefly.xcodeproj -scheme Firefly -destination 'platform=macOS' build
xcodebuild -project Firefly.xcodeproj -scheme Firefly -destination 'generic/platform=iOS Simulator' build
```

Or just `open app/Firefly.xcodeproj`.

Code signing is **off** in `project.yml` so a clean checkout builds with
no certificate. Turn it back on (set `CODE_SIGNING_ALLOWED = YES` and a
`DEVELOPMENT_TEAM`) when you want to run on a device.

### Run on the Mac

`⌘R` in Xcode with the **My Mac** destination, or:

```sh
open ~/Library/Developer/Xcode/DerivedData/Firefly-*/Build/Products/Debug/Firefly.app
```

The Mac build is the one that matters for hardware work: it has real
CoreBluetooth and (from slice 3) a USB-serial transport. macOS will
prompt for Bluetooth permission the first time.

### Run on the iPhone

1. In Xcode, select the **Firefly** scheme and your iPhone as the
   destination.
2. Signing & Capabilities → set your team; the bundle id is
   `com.jakeholland.firefly`.
3. `⌘R`. iOS prompts for Bluetooth, and for location the first time the
   app offers to push the phone's GPS to the node.

**The iOS Simulator has no Bluetooth.** It builds and runs, and every
screen works against the stub client, but it will never see a radio.
That is not a bug to chase.

### Run the hardware tests

Integration tests that need a real Heltec V3 are tagged and **skip
cleanly when no board is present**, so `swift test` on a laptop with
nothing plugged in stays green. To actually run them:

```sh
cd app/FireflyKit
FIREFLY_HARDWARE=1 swift test --filter Hardware
```

Prerequisites, all from [`docs/hardware/heltec-v3.md`](../docs/hardware/heltec-v3.md):

- a V3 flashed with stock Meshtastic **2.7.26**, region **US**;
- the **Firefly** primary channel with `position_precision: 32` — the
  default public channel truncates positions to a ~5.8 km grid and every
  distance the app shows would be quietly wrong;
- `bluetooth.mode NO_PIN` for bench use (and **reverted before the
  festival** — see that file's bench-boards section);
- an antenna attached **before** power, every time.

These tests never run in CI: a hosted runner has no radio. See
`.github/workflows/app.yml`.

## Regenerating things

**The C core symlinks** — after adding a file to `firmware/core/src` or
`firmware/core/include`:

```sh
app/tools/link_core_sources.sh
```

**The Swift protobufs** — only when deliberately moving the protobuf pin,
and then you must move `firmware/meshclient/tools/gen_nanopb.sh`'s pin in
the same PR or the script refuses to run:

```sh
app/tools/gen_swift_protos.sh     # needs protoc + protoc-gen-swift + network
```

**The Xcode project** — after changing targets, settings or Info.plist
wiring. Edit `project.yml`, never the pbxproj:

```sh
brew install xcodegen
cd app && xcodegen generate
```

Commit both `project.yml` and the regenerated `Firefly.xcodeproj`.
