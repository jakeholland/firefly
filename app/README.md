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
  ExportOptions.plist    TestFlight export options (app/tools/testflight.sh)
  tools/                 link_core_sources.sh, gen_swift_protos.sh,
                         testflight.sh
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
app with no extra flags, and the bundle id is `com.jakeholland.Firefly`
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

### Debug launch arguments

- `-FireflyDemo` (or `FIREFLY_DEMO=1` in the environment) — the
  scripted, no-radio demo world (`DemoLaunch.isRequested`). Only ever
  consulted inside `#if targetEnvironment(simulator)`, so a real device
  ignores it whether or not it is passed.
- `-FireflyDemoScreen <name>` — opens straight on that screen under
  demo mode (`DemoLaunch.requestedScreen`); see `RootView` for the
  recognised names.
- `-FireflyAutoConnect <name>` — scans for, selects, and CONNECTs to the
  peripheral advertising that exact name, automatically, once the
  Connect screen appears (`FireflyAutoConnectLaunch`). **`#if DEBUG`
  only** — a Release/TestFlight/App Store build compiles this out
  entirely and always reads `nil`, unlike `-FireflyDemo` above: this one
  drives a real CONNECT against a real peripheral rather than synthetic
  demo data, so it is not something a shipping build should honor even
  inertly.

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
Bluetooth permission dialog for `com.jakeholland.Firefly`. Click
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
`Sub:{com.jakeholland.Firefly}` means the dialog is waiting on you.
Never reach for `tccutil` — the grant is the user's to give, and a
signed build (see "Signed local runs") is what makes it stick across
rebuilds instead of re-prompting.

**This grant is per bundle id.** Because the app's bundle id changed
from `com.jakeholland.firefly` to `com.jakeholland.Firefly`, macOS sees
it as a different app for TCC purposes — the first signed run after
that change puts up the Bluetooth dialog **one more time**, on an
otherwise-already-granted machine. That is expected, not a regression.
On an iPhone the effect is more visible: iOS also keys app identity by
bundle id, so the new build installs **beside** the old one as a
separate app rather than replacing it. Delete the old `com.jakeholland.firefly`
copy by hand once the new one is confirmed working.

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

### Run the UI smoke tests

`FireflyUITests` (`docs/specs/A01-companion-app.md`, "UI") holds one
XCUITest per platform — see the file's own header comment for what
each one does and why they differ. Both live in their own,
**non-default** test plan (`FireflyUITests.xctestplan`), not the
scheme's default `Firefly.xctestplan` — see `app/project.yml`'s scheme
`test:` comment for the full reasoning. Select it explicitly with
`-testPlan FireflyUITests`.

**iOS Simulator — this is what CI runs, no setup needed:**

```sh
cd app
xcodebuild test \
  -project Firefly.xcodeproj -scheme Firefly \
  -testPlan FireflyUITests \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' \
  -only-testing:FireflyUITests
```

**macOS — opt-in only, never run in CI (Review PR #275, BLOCKING 1).**
macOS XCUITest automation needs the Accessibility permission granted to
the process driving it — normally Xcode itself (`Xcode.app`) when you
run tests from the IDE, or the `xcodebuild`/`Terminal` process when you
run from the command line, and a hosted GitHub Actions runner can
neither grant this ahead of time nor persist it between ephemeral runs.
Locally, one time:

1. **System Settings > Privacy & Security > Accessibility.**
2. Add and enable **Xcode** (running tests via `⌘U`) or your terminal
   app (running `xcodebuild` from the command line) — whichever one is
   actually going to drive the test.
3. Then run:

   ```sh
   cd app
   xcodebuild test \
     -project Firefly.xcodeproj -scheme Firefly \
     -testPlan FireflyUITests \
     -destination 'platform=macOS' \
     -only-testing:FireflyUITests
   ```

   or, in Xcode: select the **FireflyUITests** test plan (Test
   navigator > the scheme's test plan picker, or Product > Test Plan),
   pick **My Mac** as the destination, and run
   `FireflyUITests/testLaunchesToConnectScreen` directly.

Without that grant the run does not fail cleanly — the runner crashes
on launch ("Early unexpected exit... Test crashed with signal kill
before establishing connection"), which is exactly the failure mode
BLOCKING 1 found when this leg was still wired into CI.

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

### Manual test procedure — background BLE (M2)

M2's acceptance criterion — "background-connected for ≥30 min with the
screen off and the app not foregrounded, reconnecting after the node is
power cycled" — needs a phone, a real Heltec V3, and a human, none of
which CI or `FireflyHardwareTests`' skip-clean run can stand in for.
Both procedures below assume the app is already connected to a board
(the CONNECT button, Connect screen) and use the **Diagnostics** screen
(Settings > DIAGNOSTICS) as the only source of truth — never a guess
about what "should" be happening.

**Before either test:** Settings > CONNECTIVITY > "Stay connected in
background" must be **ON**. With it off, backgrounding disconnects the
app on purpose (`AppGraph.handleScenePhaseChange`) — that is a separate,
much quicker check, below.

**The 30-minute pocket test**

1. Connect to the node. Open Diagnostics and confirm **Link state** =
   `CONNECTED` and **Link uptime** is counting up from `0s`.
2. Lock the phone (or switch to another app) and leave it in a pocket or
   on the desk, screen off, for at least 30 minutes. Do not touch the
   node.
3. Unlock the phone and reopen Firefly straight to Diagnostics (Settings
   > DIAGNOSTICS).
4. **What to observe:** **Link state** reads `CONNECTED` and **Link
   uptime** is at or above the elapsed wall-clock time (allow a few
   seconds of slack for the app itself waking up) — NOT reset to a small
   number, which would mean the link actually dropped and silently
   reconnected while backgrounded rather than staying up the whole time.
   If the OS killed the process outright (visible as the app performing
   a full cold launch rather than resuming), CoreBluetooth state
   restoration is what is being exercised instead — Link state should
   still reach `CONNECTED` on its own within well under a minute of
   reopening, with no CONNECT tap.

**The power-cycle test**

1. Connect to the node. Confirm Diagnostics shows `CONNECTED`.
2. Background the app (do not disconnect by hand).
3. At the node, power it off, wait ~10 seconds, power it back on.
4. Watch Diagnostics (bring the app back to the foreground after giving
   the node a little time to reboot and re-advertise — 15–30s is
   typical for a Heltec V3).
5. **What to observe:** **Link state** should show `RECONNECTING
   (attempt N)` at some point — an honest report of the bounded
   handshake-retry backoff, not silence — and then settle back to
   `CONNECTED` on its own, with no CONNECT tap. **Link uptime** should
   read a SMALL number afterward (it resets when a new `.ready` streak
   begins) — a large or unchanged uptime here means the app is showing
   the OLD session's clock, not proof of a real reconnect. If Link state
   instead sits on `FAILED`, the bounded retry was exhausted (default: 6
   attempts, 2s/4s/8s/16s/32s apart between them — 5 delays across 6
   attempts, never reaching the 60s cap `handshakeRetryMaxDelay` defines;
   that cap only matters if `handshakeRetryLimit` is ever raised past 6)
   — tap the now-visible RETRY action (or reopen Connect and tap CONNECT
   by hand), and note how long the node actually took to re-advertise,
   since that is the number the retry bound is tuned against.

**The "off means off" check** (quick, no 30-minute wait): with "Stay
connected in background" OFF, background the app — Diagnostics (checked
immediately on returning to the foreground) should show `NOT CONNECTED`,
and the Connect screen's CONNECT button should be enabled again (never
auto-reconnected). This is `AppGraph.stop()`'s own contract: the setting
being off means the link — and the radio's own reconnect-on-loss loop
underneath it — actually stands down when backgrounded, not just that
the screen stops updating.

`app/FireflyHardwareTests/BLEHardwareTests.swift`'s
`testReconnectsOnItsOwnAfterFirefly2IsPowerCycled` automates the
power-cycle test's ASSERTIONS (gated behind `FIREFLY_HARDWARE=1` AND
`FIREFLY_MANUAL_POWER_CYCLE=1` — the power cycle itself still needs the
human step above) for anyone who wants the same check with less manual
Diagnostics-watching.

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
cd app && FIREFLY_BUILD_NUMBER=1 xcodegen generate
```

`FIREFLY_BUILD_NUMBER` must be set (`project.yml`'s `CURRENT_PROJECT_VERSION`
comment explains why — TestFlight build numbering, below); a bare
`xcodegen generate` with it unset leaves that one setting as a literal,
unresolved placeholder instead of a number. Commit both `project.yml`
and the regenerated `Firefly.xcodeproj`.

**The test plans** (`Firefly.xctestplan`, `FireflyUITests.xctestplan`) —
xcodegen only wires the *reference* to these from `project.yml`'s scheme
`test.testPlans`; their content is hand-edited JSON, not generated. Edit
them directly, then `xcodegen generate` to pick up any reference change
(which plan is default, which plans are attached) and commit both.

## Shipping a TestFlight build

```sh
app/tools/testflight.sh                 # archive, export, upload
app/tools/testflight.sh --archive-only  # archive only — no export/upload,
                                         # no App Store Connect credentials needed
```

Release is signed automatically for team `SU4T96VBX6` (the same
`Config/Local.xcconfig` mechanism as "Signed local runs" above — the
script wires it up on first use), and `CURRENT_PROJECT_VERSION` is set
to `git rev-list --count HEAD` on every archive so no upload ever
repeats a build number. See
[`docs/app/testflight.md`](../docs/app/testflight.md) for the one-time
App Store Connect setup (app record, API key, adding a tester) and the
full walkthrough, and `app/tools/testflight.sh`'s own header comment
for exactly what each step does.
