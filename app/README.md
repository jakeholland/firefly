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
      FireflyModel/      view models, theme, presentation rules, DI seams
    Tests/               one test target per source target
  Firefly/Sources/       the SwiftUI app shell
  Firefly/Resources/     Info.plist, entitlements
  FireflyHardwareTests/  app-hosted BLE hardware tests (xcodebuild test only)
  Config/                Firefly.xcconfig (committed) + git-ignored Local.xcconfig
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

Code signing is **off** in `project.yml` (`CODE_SIGNING_ALLOWED: NO`) so
a clean checkout builds with no certificate and no team. That stays true
for everyone's checkout — signing for an on-device run is wired in
per-developer, never committed. See "On-device signing" below.

### On-device signing

To run on a real iPhone (the Mac never needs this — `platform=macOS`
doesn't require a team), give the project your own team id without
committing it:

```sh
cp app/Config/Local.xcconfig.example app/Config/Local.xcconfig
# edit app/Config/Local.xcconfig: set DEVELOPMENT_TEAM to your own team id
```

`app/Config/Local.xcconfig` is git-ignored. `app/Config/Firefly.xcconfig`
(committed, wired into the `Firefly` target via `project.yml`)
`#include?`s it — the `?` means "if it exists", so its absence changes
nothing for anyone else's clean checkout. With `Local.xcconfig` in
place, open Xcode, select your iPhone as the destination, and turn
`CODE_SIGNING_ALLOWED` back on for that run (Signing & Capabilities, or
flip it in `project.yml`+regenerate if you want it to stick locally —
just don't commit that flip). The bundle id is `com.jakeholland.firefly`
either way.

### Run on the Mac

`⌘R` in Xcode with the **My Mac** destination, or:

```sh
open ~/Library/Developer/Xcode/DerivedData/Firefly-*/Build/Products/Debug/Firefly.app
```

The Mac build is the one that matters for hardware work: it has real
CoreBluetooth and (from slice F) a USB-serial transport. macOS will
prompt for Bluetooth permission the first time.

### Run on the iPhone

1. Wire your team in per "On-device signing" above (once).
2. In Xcode, select the **Firefly** scheme and your iPhone as the
   destination.
3. `⌘R`. iOS prompts for Bluetooth, and for location the first time the
   app offers to push the phone's GPS to the node.

**The iOS Simulator has no Bluetooth.** It builds and runs, and every
screen works against the stub client, but it will never see a radio.
That is not a bug to chase.

### Run the hardware tests

Integration tests that need a real Heltec V3 are tagged and **skip
cleanly when no board is present**, so a routine run stays green on a
laptop with nothing plugged in. They come in two kinds, run two
different ways, for a reason that is not optional:

**BLE — app-hosted, `xcodebuild test`.** macOS aborts
(`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`) any CoreBluetooth process
that is not inside a signed `.app` bundle carrying
`NSBluetoothAlwaysUsageDescription` and launched via LaunchServices — a
bare `swift test` binary is none of those three things, so these tests
live in `FireflyHardwareTests`, a unit-test bundle hosted inside
`Firefly.app`:

```sh
cd app
FIREFLY_HARDWARE=1 xcodebuild test \
  -project Firefly.xcodeproj -scheme Firefly \
  -destination 'platform=macOS' \
  -only-testing:FireflyHardwareTests
```

Without `FIREFLY_HARDWARE=1` the suite still builds and runs, and the
one placeholder test skips cleanly — that is what CI exercises (never
with a board or the env var; see below).

**Serial and TCP — plain `swift test`.** These transports are not
CoreBluetooth, so they are unaffected by the TCC restriction above and
stay in `FireflyKit/Tests/HardwareTests`, gated the same way:

```sh
cd app/FireflyKit
FIREFLY_HARDWARE=1 swift test --filter Hardware
```

Prerequisites for either, all from
[`docs/hardware/heltec-v3.md`](../docs/hardware/heltec-v3.md):

- a V3 flashed with stock Meshtastic **2.7.26**, region **US**;
- the **Firefly** primary channel with `position_precision: 32` — the
  default public channel truncates positions to a ~5.8 km grid and every
  distance the app shows would be quietly wrong;
- `bluetooth.mode NO_PIN` for bench use (and **reverted before the
  festival** — see that file's bench-boards section);
- an antenna attached **before** power, every time.

Neither suite ever runs WITH a board or `FIREFLY_HARDWARE=1` in CI: a
hosted runner has no radio and no serial device. CI only proves both
suites still build and skip cleanly. See `.github/workflows/app.yml`.

### The serial + TCP rig (slice F)

`FireflyKit/Tests/HardwareTests` needs a second env var alongside
`FIREFLY_HARDWARE=1`, naming the port:

```sh
cd app/FireflyKit
FIREFLY_HARDWARE=1 FIREFLY_SERIAL_PORT=/dev/cu.usbserial-4 \
  swift test --filter Hardware
```

Without `FIREFLY_SERIAL_PORT` (or with `FIREFLY_HARDWARE` unset) the
suite skips cleanly — both are required, and the skip message says
which is missing.

`SerialHardwareTests` does not depend on slice A's `MeshtasticClient` —
that class is not merged as of this PR — so it drives the two-phase
`want_config` handshake directly through `SerialTransport` +
`StreamFramer` with hand-built `ToRadio` protobufs from
`MeshtasticProto`, and asserts the bench board's own identity read back
off the wire: node num, owner name, and the asserted fixed position from
`docs/hardware/heltec-v3.md`'s bench table. It never sends an admin
message and never exercises the phone-GPS `LOC_EXTERNAL` push against
the bench board — that push would risk overwriting Firefly 1's asserted
fixed position with a measured one, exactly the provenance trap that
file's "Use 2" section warns about, so the push path is covered by
unit tests (`LocationProviderTests`, no radio) instead.

**Single client, one port, one script.** A Meshtastic serial port
accepts exactly one client — this is `HardwareTests`' own "Contention
warning" above, restated because it bites in practice: while
`FIREFLY_HARDWARE=1 swift test --filter Hardware` holds
`/dev/cu.usbserial-4` open, nothing else (a `meshtastic` CLI session, a
console, Xcode) can also open it, and the failure looks like a hang, not
a clean error.

`app/tools/bench_friend.sh` sends a text broadcast on Firefly 1's
primary (Firefly) channel via the `meshtastic` CLI, so the *phone side*
of the app can be exercised by hand — connect the app to Firefly 2 (or
watch Firefly 1's own traffic), then:

```sh
app/tools/bench_friend.sh "hey crew"          # defaults to /dev/cu.usbserial-4
FIREFLY_SERIAL_PORT=/dev/cu.usbserial-4 \
MESHTASTIC_BIN=/Users/jakeholland/.local/bin/meshtastic \
  app/tools/bench_friend.sh                   # explicit, same defaults
```

Never run it while `HardwareTests` holds the port — it checks with
`lsof` first and refuses rather than racing the test for the fd, but
that check is best-effort, not a lock: treat "one client at a time" as
the actual rule, `bench_friend.sh`'s check as a courtesy that catches
the common mistake.

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
