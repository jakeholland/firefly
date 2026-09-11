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
swift test --sanitize=address   # the C core is real C; ASan is not optional
```

`swift test` runs every unit test in the package, including the two
guards that keep the app and the puck from drifting apart:
`CoreSourceLinkTests` (every `firmware/core` source is still linked in)
and `ProtobufPinTests` (the Swift and nanopb generators still pin the
same `meshtastic/protobufs` commit).

The app-target view models (Connect, Settings, Diagnostics, the node
picker) are NOT part of the SwiftPM package, so `swift test` cannot see
them. They run through the Xcode project:

```sh
cd app
xcodebuild test -project Firefly.xcodeproj -scheme Firefly \
  -destination 'platform=macOS' -only-testing:FireflyAppTests
```

## How the app is wired together

`FireflyModel/Live/AppGraph.swift` is the composition root: ONE
`AppDependencies` (`.current()` — the stub stack in the iOS Simulator,
which has no Bluetooth at all; the real one everywhere else), ONE
`MeshtasticClient` over ONE `BLETransport`, ONE set of `firmware/core`
contexts (`ff_crew` / `ff_feed` / `ff_radar` / `ff_find`, via
`CoreStore`), and every view model built from them. `FireflyApp.init`
constructs it once and hands the view models down.

Nothing below that line constructs its own dependencies — a screen that
called `AppDependencies.current()` for itself would get a second client
over a second radio and then observe the one nothing connected.

## Build the app

```sh
cd app
xcodebuild -project Firefly.xcodeproj -scheme Firefly -destination 'platform=macOS' build
xcodebuild -project Firefly.xcodeproj -scheme Firefly -destination 'generic/platform=iOS Simulator' build
```

Or just `open app/Firefly.xcodeproj`.

Code signing is **off by default** — `CODE_SIGNING_ALLOWED = NO` in
`app/Config/Firefly.xcconfig` (committed) — so a clean checkout builds
with no certificate and no team, which is what CI needs. That stays true
for everyone's checkout: signing is wired in per-developer, never
committed.

### Signed local runs

Those two defaults live in the **xcconfig**, not in `project.yml`'s
per-target `settings:`, and the difference matters: a target-level build
setting beats an xcconfig, so while they lived there nothing local could
turn signing on without either editing a committed file or passing
`CODE_SIGNING_ALLOWED=YES` on every `xcodebuild` command line.

Now one file copy is the whole story:

```sh
cp app/Config/Local.xcconfig.example app/Config/Local.xcconfig
# then edit it — your own team id, and the four signing settings:
#   DEVELOPMENT_TEAM = <your team id>
#   CODE_SIGN_STYLE = Automatic
#   CODE_SIGN_IDENTITY = Apple Development
#   CODE_SIGNING_ALLOWED = YES
#   CODE_SIGNING_REQUIRED = YES
```

`app/Config/Local.xcconfig` is git-ignored and stays that way — nobody's
team id reaches this repo's history. `app/Config/Firefly.xcconfig`
(committed, wired into **every** target via `project.yml`'s
`configFiles:`) `#include?`s it at the BOTTOM of the file: `?` means "if
it exists", and last-wins ordering is what lets your local values beat
the committed defaults. Do not move that include.

With it in place, `xcodebuild ... build` and `⌘R` both produce a signed
app with no extra flags, and the bundle id is `com.jakeholland.firefly`
either way. A signed Mac build also gives the app a STABLE code identity,
which is what makes macOS's one-time Bluetooth grant survive a rebuild
instead of re-prompting every time.

Both test bundles set `GENERATE_INFOPLIST_FILE: YES` for the same
reason: `codesign` needs an Info.plist in the bundle, and a unit-test
target has no `INFOPLIST_FILE` of its own, so a signed `xcodebuild test`
without it fails with *"Cannot code sign because the target does not
have an Info.plist file"*.

### On-device signing (iPhone)

Same `Local.xcconfig` as above — that is all an iPhone run needs. In
Xcode, select the **Firefly** scheme and your iPhone as the destination,
then `⌘R`.

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

Two tests live there: the `want_config` handshake against Firefly 2
(A01_AC4 — node num, owner name, channel, and Firefly 1 present in the
nodeDB with its asserted position) and a DM to Firefly 1 with `want_ack`
that must show `WAITING -> SENT -> DELIVERED` off a real routing ack
(A01_AC5's first half). AC5's second half — powered off shows
`WAITING -> SENT -> NO ACK` and never `DELIVERED` — stays a MANUAL
check: it needs a human to power a board down mid-run, and a test that
passed because the board happened to be out of range would be worse than
no test.

Without `FIREFLY_HARDWARE=1` the suite still builds and runs and both
tests skip cleanly — that is what CI exercises (never with a board or
the env var; see below).

**The one-time Bluetooth Allow.** The first signed run puts up macOS's
Bluetooth permission dialog for `com.jakeholland.firefly`. Click
**Allow**. Until somebody does, the run does not fail — it HANGS, and
`xcodebuild` eventually reports:

```
Firefly (NNNNN) encountered an error (The test runner hung before establishing connection.)
```

To confirm that is what you are looking at:

```sh
log show --last 5m --predicate 'subsystem == "com.apple.TCC"' \
  | grep AUTHREQ_PROMPTING
```

A line naming `kTCCServiceBluetoothAlways` and
`Sub:{com.jakeholland.firefly}` means the dialog is waiting on you.
Never reach for `tccutil` — the grant is the user's to give, and a
signed build (see "Signed local runs") is what makes it stick across
rebuilds instead of re-prompting.

The app itself does NOT scan on launch, deliberately: building a
`CBCentralManager` is what triggers that dialog, and showing the Connect
screen is not the moment to ask. Press **RESCAN** in the node picker.
That also keeps the dialog out of the way of the test host, which
launches this same app.

**Why Debug is not sandboxed.** A signed, sandboxed host cannot complete
XCTest's connection to its controller — the run hangs with that same
"test runner hung before establishing connection" message and the host
logs nothing after `libsystem_secinit.dylib AppSandbox`. An unsigned
build hid this, because an unsigned build carries no entitlements and so
is not sandboxed at all. So `Firefly/Resources/Firefly.Debug.entitlements`
(the Debug `CODE_SIGN_ENTITLEMENTS`) is `Firefly.entitlements` with
`com.apple.security.app-sandbox` set to **false**; Release keeps the
sandbox on. B1 needs a SIGNED bundle with
`NSBluetoothAlwaysUsageDescription` launched via LaunchServices — none of
which is the sandbox — so the rig loses nothing. The cost, stated rather
than buried: **a Debug build no longer exercises the sandbox**, so a
sandbox-only failure (realistically, the serial transport opening
`/dev/cu.*` under `device.serial`) will not show up until a Release
build. Check that against Release before the festival.

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

`SerialHardwareTests` predates slice A's `MeshtasticClient` landing, so
it drives the two-phase `want_config` handshake directly through
`SerialTransport` + `StreamFramer` with hand-built `ToRadio` protobufs
from `MeshtasticProto`, and asserts the bench board's own identity read
back off the wire: node num, owner name, and the asserted fixed position
from `docs/hardware/heltec-v3.md`'s bench table. It never sends an admin
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
