# A01 · companion app — native Swift/SwiftUI client for the Firefly mesh

> **A-series.** The S-specs describe the puck. This is the first spec for
> something that is not the puck. It follows the same rules: acceptance
> criteria become test names (`A01_AC3_...`), unknowns are represented
> rather than papered over, and anything cut is cut out loud.

## Motivation

Both Waveshare puck screens are broken. The mesh itself is not — two
Heltec WiFi LoRa 32 V3 boards running stock Meshtastic 2.7.26 can carry
the whole protocol today (`docs/hardware/heltec-v3.md`). What is missing
is a *client*: something with a screen that shows crew, presence,
messages and delivery state, so the protocol work can keep moving while
the display hardware is sorted out.

A phone is the obvious client. It also turns out to be the thing this
project needed anyway:

- **It unblocks everything downstream of a screen.** S24's inbox, S29's
  no-GPS signal view and FIND, S06's radar — all of them are currently
  specified, implemented in C, unit-tested, and unobservable.
- **It is the only honest hardware test rig we have.** The iOS Simulator
  has no Bluetooth. A Mac does, and it also has USB. So the macOS build,
  talking to a real Heltec over CoreBluetooth or over a serial cable, is
  where "does this actually work against a radio" gets answered.
- **It is a second consumer of `firmware/core`**, which is the claim
  `docs/ARCHITECTURE.md` has been making since the first commit ("C ABI
  bindable from Swift") without ever testing it. A second consumer is how
  you find out whether a seam is a seam.

What this is **not**: a better Meshtastic app. Meshtastic-Apple exists,
it is excellent, and it does the general-purpose job far better than this
ever will. Firefly's app is narrow on purpose — one crew, one channel,
four screens, and a hard rule that nothing on any of them is invented.

## Decisions already made

Not up for re-litigation in this spec; recorded so the reasoning is not
lost.

| Decision | Why |
|---|---|
| Native Swift + SwiftUI, one multiplatform target, iOS 17+ / macOS 14+ | `@Observable` (iOS 17) removes the Combine boilerplate the archived app drowned in; one target means the Mac build cannot rot, and the Mac build is the test rig. |
| MVVM, protocol-injected services, mocks | The app must be fully exercisable with no radio at all — that is not a testing nicety, it is the only way the Simulator is usable. |
| `firmware/core` linked in as a SwiftPM C target | The phone and the puck must agree about freshness, distance, presence and threading. Two implementations of that would be two products. |
| Meshtastic protobufs generated from the **same pinned** `.proto` set as the puck's nanopb sources | A wire-version skew between our own two clients is a failure that only shows up at a festival. |
| macOS is the integration-test rig; hardware tests skip cleanly without a board | Hosted CI has no radio, and a test suite that goes red when the hardware is on a different desk is a test suite people stop reading. |
| Music / Swarm (S31) out of scope | It is a puck LED-and-motion feature. A phone reproducing it adds nothing. |

## Building blocks reused

Nothing here is new work that already exists somewhere in the tree:

| Reused | From | How |
|---|---|---|
| Crew model, freshness, presence, close-range, RSSI trend | `firmware/core/ff_crew.c` | compiled into the app, called through the C bridge |
| Radar view compute, signal tiers, battery icon thresholds | `firmware/core/ff_radar.c` | same |
| Inbox → conversations → thread, unread, previews | `firmware/core/ff_inbox.c`, `ff_feed.c` | same |
| FIND session state machine, ping cadence, warmer/colder | `firmware/core/ff_find.c` | same |
| Firefly packet encode/decode (FLARE/RALLY/STATUS/PING/PONG, portnum 269) | `firmware/core/ff_proto.c` | same |
| Bearing, distance, compass points, projection | `firmware/core/ff_geo.c` | same |
| Name sanitising + Meshtastic short-name derivation | `firmware/core/ff_meshname.c` | same |
| Honest presence classifier | `firmware/core/ff_sigview.c` | same |
| Palette and crew colours | `firmware/app/theme/ff_theme.h` | transcribed into `FireflyTheme`, pinned by a test that parses the header |
| Stream framing, handshake shape, metadata rules | `docs/specs/S03-meshclient.md` + `firmware/meshclient` | re-implemented in Swift against the same spec; the C library itself is **not** linked (see below) |
| Protobuf pin and `.proto` file list | `firmware/meshclient/tools/gen_nanopb.sh` | shared literally — the Swift generator refuses to run on drift |

**Why `firmware/meshclient` is not linked in too.** It would have been
tempting: it is extraction-grade C with a transport vtable, and it
already implements the handshake. But it is built on nanopb static
allocation and a `tick()` pump designed for a 50 Hz embedded main loop,
and wrapping that in Swift concurrency means either blocking a thread on
a polling loop or reimplementing the pump anyway. SwiftProtobuf plus
`AsyncStream` is a better fit for the platform, and the piece that
actually matters — the wire *format* — is shared exactly, through the
pinned protobufs. The C client stays the puck's. `StreamFramer.swift`
carries a pointer back to S03 AC1 so the two framers stay honest against
the same fixtures.

## Scope cuts, flagged

Called out rather than quietly omitted:

- **Music / Swarm (S31).** Out. A puck feature.
- **Map face (S09), Lineup (S07), festpack (S05).** Out of M1–M3. The
  app has no festpack parser and no schedule; it is a mesh client, not a
  second festival guide. `ff_map`/`ff_wall` compile into the target (they
  are part of core) but are not bound.
- **T9 (S08).** Out, permanently. A phone has a keyboard.
- **Channel *editing*.** M1 imports a channel by QR/URL and shows it.
  Writing channel config back to a node is admin-message territory and is
  M3 at the earliest.
- **Node-database persistence across launches.** M1 keeps the nodeDB in
  memory only, rebuilt by each `want_config`. A persisted DB that gets
  replayed as if it were live is precisely the honesty failure S03's
  "deliberately not surfaced: `NodeInfo.snr`" note is about; persistence
  lands in M3 with explicit "from storage, last seen X ago" rendering.
- **Push notifications, widgets, watch app, visionOS.** Out.
- **PKI / encrypted DMs.** Out of M1–M3. The Firefly channel's PSK is the
  security model for now; this is a real limitation, written down, not a
  claim that it is handled.
- **Swift 6 strict concurrency.** The package builds in Swift 5 language
  mode with `SWIFT_STRICT_CONCURRENCY: minimal`. Tightening it is a
  deliberate later pass, not a thing to fight while the app has no
  features.

## Module layout

```
app/
  FireflyKit/                      SwiftPM package — no UI anywhere in it
    Sources/
      FireflyCore/                 C target. src/ and include/ are symlink
                                   farms over firmware/core + firmware/platform
      MeshtasticProto/             generated SwiftProtobuf types (committed)
      FireflyMesh/                 transports + Meshtastic client
        Transport.swift            MeshTransport, TransportEvent, LoopbackTransport
        StreamFramer.swift         0x94 0xC3 framing (serial + TCP)
        MeshtasticBLE.swift        GATT UUIDs + the drain/pairing policy
        MeshtasticClientProtocol.swift  the seam view models depend on
        DeliveryState.swift        WAITING/SENT/DELIVERED/NO ACK/DROPPED
        BLE/ Serial/ TCP/          concrete transports (slices A, F)
      FireflyModel/                view models, presentation rules, theme
        FireflyTheme.swift         the palette, pinned against ff_theme.h
        SignalPresentation.swift   tiers as words, never as distance
        ConnectViewModel.swift     the MVVM template every screen follows
        Bridge/                    Swift-safe wrappers over the C core (slice B)
    Tests/
      FireflyCoreTests/            the C bridge + the anti-drift guards
      MeshtasticProtoTests/        wire round-trips + the pin guard
      FireflyMeshTests/            framer, delivery states, BLE contract
      FireflyModelTests/           theme, honesty rules, view models
      HardwareTests/               tagged; skipped without a board (slice F)
  Firefly/Sources/                 the SwiftUI shell
  Firefly/Resources/               Info.plist, entitlements
  Firefly.xcodeproj + project.yml  committed project, regenerable
  tools/                           link_core_sources.sh, gen_swift_protos.sh
```

The stack is strictly one-directional: `FireflyCore` knows nothing;
`MeshtasticProto` knows nothing; `FireflyMesh` depends on both;
`FireflyModel` depends on `FireflyMesh` and `FireflyCore`; the app target
depends on all four and is the only place SwiftUI appears. Each layer can
be built and tested without the one above it — the same discipline
`docs/ARCHITECTURE.md` states for the firmware.

## Data flow

```
  radio  ──BLE / serial / TCP──▶  MeshTransport
                                      │  TransportEvent.received(Data)
                                      ▼
                          (stream transports only) StreamFramer
                                      │  one FromRadio protobuf
                                      ▼
                             MeshtasticClient (actor)
                   ┌──────────────────┼──────────────────┐
                   │                  │                  │
           linkState stream    nodeUpdates stream   deliveryUpdates stream
                   │                  │                  │
                   └──────────────────┼──────────────────┘
                                      ▼
                              CoreStore  (@MainActor)
                     owns ff_crew_t / ff_feed_t / ff_find_t
                     feeds them with ff_crew_on_position,
                     ff_crew_on_rssi, ff_crew_on_heard, ff_feed_push …
                                      │
                       ff_radar_compute / ff_inbox_build
                                      │  plain Swift value types
                                      ▼
                        view models (@Observable) ──▶ SwiftUI
```

Two rules make this readable and keep it honest:

1. **Protobuf types never leave `FireflyMesh`.** The client translates
   them at its boundary into `MeshNodeSnapshot` / `NodePosition` /
   `DeliveryState`, the same way `mc_client.h` translates Meshtastic
   enums into `mc_loc_source_t` and friends so protobuf values never
   reach `core/` (S03 AC7). A `Meshtastic_Position` in a view model would
   be a layering violation *and* an honesty hazard, because proto3
   implicit presence makes "absent" and "zero" the same bytes.
2. **C types never leave the bridge.** No `UnsafeMutablePointer` and no
   imported C tuple ever appears in a view model, a view or a test
   outside `FireflyCoreTests`.

## Threading model

- **The C core is not thread-safe.** It has no locks, by design — it was
  written for a single embedded main loop. So every `ff_*` context in the
  app (`ff_crew_t`, `ff_feed_t`, `ff_find_t`, `ff_radar_smooth_t`) lives
  behind **one** isolation domain and is touched from nowhere else.
- That domain is **`@MainActor`**, not a custom actor. The core's work is
  microseconds of pure arithmetic on ≤8 KB structs (`ff_crew_t` has a
  static assert keeping it under 8 KB), it is the direct input to
  rendering, and a separate actor would buy a hop on every frame in
  exchange for nothing. If a profile ever says otherwise, the bridge is
  the one place that has to change.
- **Transports and the client are actors.** CoreBluetooth delegate
  callbacks, the serial read source and the TCP receive loop all land off
  the main thread and are funnelled into the client actor, which publishes
  `AsyncStream`s. The Meshtastic-Apple TCP reader's own note — that
  main-actor-isolating the receive drain stalled it enough for the OS to
  drop the connection — is the reason this is not simply main-actor
  everywhere.
- **Back-pressure is bounded, and drops the oldest.** Every
  `AsyncStream` uses `.bufferingNewest(4096)`, matching Meshtastic-Apple.
  A stalled consumer must not grow memory without limit, and for live
  presence the newest packet is the one that matters.
- **No Combine.** `@Observable` + `AsyncStream` throughout. The archived
  app's Combine/continuation hybrid is the single largest source of its
  Swift 6 incompatibility.

## MVVM conventions

Every view model in this app:

1. is `@MainActor @Observable final class`;
2. takes its dependencies as **protocol existentials** in `init`, and
   stores no concrete service type;
3. owns no I/O — it consumes `AsyncStream`s and exposes plain values;
4. exposes a `func observe()` that is idempotent and a `stopObserving()`
   called from `.onDisappear` (**not** a `deinit`: a `@MainActor` type's
   `deinit` is nonisolated and cannot touch its own state);
5. converts state into display strings **in the view model**, not in the
   view, so the string is testable. `ConnectViewModel.statusLabel` is the
   template: `.handshaking` renders as `HANDSHAKING`, never `CONNECTED`,
   because the node database is meaningless until `config_complete_id`
   matches and a screen saying "connected" over an empty crew is a lie.

`ConnectViewModel` + `ConnectViewModelTests` are the worked example; new
view models copy that shape.

## Dependency injection

Constructor injection, one composition root, no service locator and no
singletons.

```swift
struct AppDependencies {
    var client: any MeshtasticClientProtocol
    var location: any LocationProviding
    var heading: any HeadingProviding
    var store: any SettingsStoring
}
```

- `AppDependencies.live()` builds the real BLE (or serial, or TCP) stack.
- `AppDependencies.stub()` builds `StubMeshtasticClient` over
  `LoopbackTransport` and a location/heading provider that reports
  **unavailable**, not fake coordinates.
- The iOS Simulator gets `.stub()` automatically via
  `#if targetEnvironment(simulator)` — an idea taken directly from the
  archived app's `DependencyContainer.simulatorContainer()`, which existed
  because instantiating `CBCentralManager` under the Simulator is a
  dead end.

The stub client's defining property is what it *refuses* to do: it
reaches `.ready`, records what was sent, and invents no nodes, no
positions, no inbound messages and no `DELIVERED`. An empty Radar on a
stub is the honest answer, and it is the same empty Radar a real radio
with nothing in range produces. Tests that need traffic inject exact
bytes.

## The C-core bridge

### What is compiled

`app/tools/link_core_sources.sh` creates two directories of **per-file
symlinks**: every `firmware/core/src/*.c` into
`Sources/FireflyCore/src/`, and every header from *both*
`firmware/core/include` and `firmware/platform/include` into one flat
`Sources/FireflyCore/include/`. Per-file rather than two directory links
for two concrete reasons, both in the script's header: SwiftPM's source
scanner is not guaranteed to descend into a symlinked directory, and the
core headers include `"ff_latlon.h"` / `"ff_clock.h"` with quoted
includes that resolve relative to the including header — so
`firmware/platform`'s two headers have to sit in the *same* directory as
the core's.

Symlinks, never copies. A copy is a fork with a grace period.
`CoreSourceLinkTests` asserts the farm matches `firmware/core` and that
each entry really is a symlink, failing by name — the app-side twin of
`ff_check_sources_complete()` in `firmware/CMakeLists.txt`.

### Memory ownership

The core allocates nothing and owns nothing; the caller provides every
buffer. In Swift that means:

- Each C context is heap-allocated **once**, by exactly one Swift class,
  with `UnsafeMutablePointer<T>.allocate(capacity: 1)`, initialised by
  the module's own `ff_*_init`, and `deinitialize` + `deallocate`d in
  that class's `deinit`. No `withUnsafeMutablePointer(to: &someProperty)`
  on a stored property: that pointer is only valid for the duration of
  the call, and `ff_crew_t` **stores** the pointer it is given.
- `ff_clock_t` is the sharp edge. `ff_crew_init(c, clock)` keeps
  `ff_clock_t const *clock` — a borrowed pointer with a lifetime
  requirement the C header states and the compiler cannot enforce. So the
  clock struct is heap-allocated alongside the context, by the same
  owner, and freed after it. Its `now_ms` is a `@convention(c)` function
  (no captures possible) and its `user` is
  `Unmanaged.passUnretained(owner).toOpaque()` — unretained deliberately,
  because the owner outlives the clock by construction and a retain here
  would be a cycle.
- **No C pointer escapes its owner.** Accessors return Swift value types.
  Fixed C char arrays (`char name[16]`) import as tuples; the bridge
  converts them with a `withUnsafeBytes` + `String(cString:)` helper and
  never hands the tuple out.
- Anything the core fills in — `ff_radar_view_t`, `ff_inbox_t`,
  `ff_inbox_thread_t` — is a stack or heap struct the bridge owns for the
  duration of one `compute`/`build` call, read into Swift values, and
  discarded. Those structs are large (`ff_inbox_thread_t` is asserted
  under 4 KB) but fixed; nothing grows.

### Which modules are bound in M1

Bound (a Swift wrapper, called from a view model, covered by tests):

| Module | Used for |
|---|---|
| `ff_geo` | bearing, distance, compass point, wrap/angdiff |
| `ff_crew` | the roster, freshness, presence, close-range, RSSI trend, `ff_fmt_distance` / `ff_fmt_age` |
| `ff_radar` | Radar's whole view model, including the signal tiers and the no-GPS dot ordering |
| `ff_sigview` | the honest presence classifier shared with the inbox |
| `ff_feed` + `ff_inbox` | Inbox conversations, threads, unread, and the outbox delivery states |
| `ff_find` | FIND sessions, ping cadence, warmer/colder |
| `ff_proto` | encode/decode of Firefly's own portnum-269 packets |
| `ff_meshname` | name sanitising + short-name derivation in Settings |

Compiled but **not bound** in M1 (they are part of core and must keep
compiling, which is itself worth having): `ff_map`, `ff_wall`,
`ff_demofeed`, `ff_t9*`, `ff_touchcal`, `ff_power_fsm`, `ff_idle`,
`ff_button`, `ff_multitap`, `ff_gesture`, `ff_sound`, `ff_batt`,
`ff_beat`, `ff_swarm`, `ff_bandenergy`, `ff_miclevel`, `ff_wav`,
`ff_dbgcmd`, `ff_settings`, `ff_store`, `ff_heard`, `ff_flare`,
`ff_rally`, `ff_notify`, `ff_version`, `ff_base64`.

## Transports

One protocol, `MeshTransport`, with a `kind` that says whether framing is
this layer's problem (`.stream`) or already done (`.message`).

### BLE — iOS and macOS, the same code

CoreBluetooth is the same framework on both; there is no reason for two
implementations. GATT vocabulary (`MeshtasticBLE.swift`):

| Role | UUID |
|---|---|
| service | `6BA1B218-15A8-461F-9FA8-5DCAE273EAFD` |
| TORADIO (write) | `F75C76D2-129E-4DAD-A1DD-7866124401E7` |
| FROMRADIO (read, **not** notify) | `2C55E69E-4993-11ED-B878-0242AC120002` |
| FROMNUM (notify) | `ED9DA18C-A800-4F66-A670-AA7547E34453` |
| LOGRADIO (notify, optional) | `5A3D6E49-06E6-4423-9944-E9DE8CDF9547` |

Rules, all of them borrowed and all of them pinned by tests:

- **Scan on the service UUID**, never on a `Meshtastic` name prefix. A
  renamed node still advertises the service, and a name is spoofable.
- **Do not send `want_config` until the FROMNUM subscription is ACKed.**
  Subscribing is also what makes iOS present the pairing sheet, so the
  connect continuation is held open until
  `didUpdateNotificationStateFor` fires. Resolving early races the
  subscription ACK and the radio's notifications are silently dropped —
  it presents as "connects fine, receives nothing, sometimes".
- **Drain FROMRADIO by reading until an empty read.** Re-kick the drain
  on three triggers: subscription ACK, every FROMNUM notification, and
  after every successful TORADIO write (the radio can queue a reply
  before the notification lands). Dropping the third is the subtle one.
- **MTU is not negotiated, it is observed.** Log
  `maximumWriteValueLength(for:)` at discovery; a ~20 byte value means
  MTU negotiation did not take. No chunking: one `ToRadio` per write.
  Retry `CBATTError.insufficientResources` up to 4 attempts with a
  120/240/360 ms backoff — that error is buffer exhaustion on the radio,
  not a size problem, and it is recoverable.
- **Pairing.** The mode is the *radio's* setting, not the app's. Both
  bench boards are `NO_PIN` for now and both get reverted before the
  festival (`docs/hardware/heltec-v3.md`). Treat
  `insufficientAuthentication` / `insufficientEncryption` /
  `encryptionTimedOut` / `peerRemovedPairingInformation` as pairing
  failures, and `peerRemovedPairingInformation` specifically as
  "the bond is gone, stop retrying and tell the user to forget the device
  in Settings" — retrying that one forever is a known dead end.
- **Reconnect** is discovery-driven, not a timer: remember the last
  peripheral id, keep scanning, reconnect when it reappears. Cap the
  connect-step retries (2, 2 s apart) so a node that is off does not hot
  loop. First-ever bond gets a 90 s connect timeout (the user is typing a
  PIN or tapping Pair); a remembered bond gets 5 s.

### Serial — macOS only

`/dev/cu.*` at 115200 8N1, `CS8|CREAD|CLOCAL`, `VMIN=0 VTIME=1`, read via
a `DispatchSourceRead`, framed with `StreamFramer`. Enumerate ports via
IOKit `kIOSerialBSDServiceValue` and keep only those with a USB serial
number. This is the transport that makes the Mac a real test rig, and it
is `#if os(macOS)` — there is no iOS equivalent and pretending otherwise
would just produce a dead code path.

**Contention warning, in the spec because it will bite:** a Meshtastic
node's serial port is single-client. If `meshtastic --info` or a console
session owns `/dev/cu.usbserial-XXXX`, the app cannot, and the failure
looks like a hang.

### TCP — meshtasticd and the sim

Port 4403, same `StreamFramer`, Bonjour `_meshtastic._tcp.` for
discovery plus a manual `host[:port]` field. This is what lets the app
talk to the same `meshtasticd` the firmware e2e tests use, and — via
`ffsim`'s TCP transport — eventually to the sim itself. Same caveat as
the firmware's: one `meshtasticd` accepts one client at a time.

## Meshtastic client

### Handshake

1. Transport reaches `.ready` (for BLE: subscription ACKed).
2. Send a `Heartbeat` with `nonce = UInt32.random(in: 2...UInt32.max)` —
   never 1, which firmware may special-case.
3. `ToRadio.want_config_id = <nonce A>` → the radio streams `my_info`,
   `metadata`, `channel`s, `config`, `module_config`, terminated by
   `config_complete_id == nonce A`. Timeout 30 s.
4. `ToRadio.want_config_id = <nonce B>` → the node database dump,
   terminated by `config_complete_id == nonce B`. Timeout 120 s; do
   **not** re-send this if a dump is already in progress — a re-request
   restarts it from the top and interleaves two dumps.
5. Check the firmware version; below the supported floor, say so plainly
   instead of failing mysteriously later.
6. `.ready`. Only now is the nodeDB meaningful.

Two nonces rather than one, so a large mesh's node dump cannot delay the
config the UI needs to draw anything. Both the archived app and
Meshtastic-Apple do this; the nonce values themselves carry no meaning
and ours are our own.

`FromRadio.rebooted` is an **immediate session loss**, not something to
discover via a silence timeout: reissue `want_config` at once. The puck
learned this the expensive way — a `set_owner` admin write reboots the
radio, other traffic keeps the watchdog fed, and the client sits in a
dead session forever (S03's reboot amendment). The Swift client inherits
the lesson rather than the bug.

A 15 s heartbeat with a 5 s response timer runs on **stream transports
only**; BLE gets its liveness from the link itself.

### NodeDB

In-memory, rebuilt by each handshake in M1. `num` → `!%08x` for display,
Meshtastic's own convention. Positions are `latitudeI * 1e-7`, stored
only when the coordinates are actually present. Every field that can be
absent is `Optional`, and the three S03 rules carry over verbatim because
they are properties of the wire, not of C:

- **`location_source`**: absent, `LOC_UNSET`, and an unrecognised future
  value all read `.unknown` — never `.internalGPS`. MANUAL is *asserted*,
  not measured, and must never age into looking like a stale
  measurement.
- **RSSI/SNR are per-packet and only attributable when the packet came
  directly.** `hopsAway` derives from `hop_start`/`hop_limit`; a bare
  `hop_start == 0` is UNKNOWN, not DIRECT. Implausible readings are
  reported **absent**, never clamped — `ff_crew`'s close-range predicate
  is `rssi > -60 dBm`, which a clamped value satisfies, fabricating a
  CLOSE lock out of a malformed packet.
- **`precision_bits`**: present ⇒ 1–32; wire `0` and values `> 32` read
  **absent**, and absent is *not* "full precision". The default public
  channel truncates to a ~5.8 km grid; this field is the only tell, and
  the Radar refuses a confident metre-level distance without it.

### Routing ACK → delivery state

Outbound text goes out on `PortNum.TEXT_MESSAGE_APP` with a random
`packet.id`. Direct messages set `want_ack = true`; broadcasts do not,
because nothing acks a broadcast.

| Observation | State |
|---|---|
| handed to the client, no id yet | `WAITING` |
| radio accepted it, id assigned | `SENT` |
| `Routing` with `request_id == our id` and `error_reason == NONE`, on a DM | `DELIVERED` |
| same, on a broadcast | stays `SENT` — "delivered to the mesh" is not delivery to a person |
| `error_reason != NONE` | `DROPPED`, with the reason shown (no route, max retransmit, duty cycle…) |
| `want_ack`, and 5 minutes elapsed with no routing packet | `NO ACK` |

The five states are exactly `ff_feed_send_status_t`
(`FF_SEND_WAITING/SENT/DELIVERED/NO_ACK/DROPPED`) and
`DeliveryStateTests` pins the mapping, so a reorder of the C enum fails
here rather than making the two products disagree about what DELIVERED
means. The 5-minute window is **derived at render time** from the
message's timestamp, not driven by a timer — a timer that fires while the
app is suspended is a timer that lies.

Inbound text is deduplicated on `packet.id` before it reaches the feed:
the mesh echoes your own packet back within seconds, and without the
guard your own sent row is overwritten and a phantom notification fires.

## Phone GPS → node

The Heltec V3 has **no GNSS at all**. So on the bench the phone is the
only source of a real position, and pushing it to the node is what makes
Radar testable.

- **Mechanism:** a normal `Position` message on
  `PortNum.POSITION_APP`, addressed to *the connected node itself*, with
  `location_source = LOC_EXTERNAL`. This is an external fix, not a fixed
  position, and the distinction is the whole point: an external fix is a
  measurement with a time on it. It is **not** an admin message.
  `AdminMessage.set_fixed_position` is the other thing entirely — it
  asserts a coordinate — and the app uses it only for the explicit
  "pin this node to a spot" action, never for live GPS.
- **Cadence:** default 30 s, floor 5 s, off by default. Opt-in, with the
  interval in Settings.
- **Payload:** `latitudeI`/`longitudeI` at 1e7, `time`, `altitude`,
  `satsInView`, `groundSpeed` when > 0, `groundTrack` only when
  `0 < course ≤ 360`.
- **Authorisation and background modes:** `NSLocationWhenInUse` for the
  basic case; `Always` (with `allowsBackgroundLocationUpdates`) only when
  the user turns on location sharing, because a festival's entire premise
  is that the phone is in a pocket. `UIBackgroundModes` =
  `bluetooth-central`, `location`. `desiredAccuracy` is
  `kCLLocationAccuracyHundredMeters` with a 10 m distance filter —
  `Best` is a battery choice that has to be earned, not a default.
- **Never fabricate.** No fix means no position message and a Radar that
  says NO FIX. There is no last-known-position fallback that gets
  broadcast as current.

## Compass heading

- **iOS:** `CLLocationManager.startUpdatingHeading`, using
  `trueHeading` when it is valid and `magneticHeading` otherwise, with
  the accuracy value carried through — a heading whose accuracy is
  negative is *invalid*, and the Radar must render `NOHDG`, not a
  confidently wrong arrow. `ff_radar_compute`'s existing `arrow_valid`
  flag already expresses this; the app feeds it honestly.
- **macOS:** there is no magnetometer. The Mac build is permanently
  `NOHDG` and the Radar shows bearings without a rotating arrow. This is
  correct, not a gap: the puck's own `RADAR_NOHDG` mode exists for
  exactly this case.
- `ff_geo_heading_deg` (the tilt-compensated mag+accel fusion) is **not**
  used. CoreLocation already fuses; running our own on top would be two
  filters fighting.

## Persistence

Deliberately small in M1–M3:

- **`UserDefaults`**: last connected peripheral id, the set of
  successfully bonded peripherals (the bond hint that picks the connect
  timeout), units (metric/imperial), location-sharing on/off and
  interval, the chosen transport.
- **Keychain**: channel PSKs. They are keys; they do not belong in
  `UserDefaults`.
- **Nothing else in M1.** No node database on disk, no message history on
  disk. M3 adds SwiftData for message history and, if it earns its place,
  the nodeDB — and anything restored from disk must be rendered as
  restored, with its age, never as live.

## Test strategy

**Unit (`swift test`, every PR, no hardware, no simulator).** The whole
package. Notable classes of test, each of which exists because something
could silently go wrong:

- *Bridge tests* call `ff_geo`, `ff_crew`, `ff_radar` through the C
  module. If the symlink farm, the include path or the C11 build breaks,
  the target stops compiling.
- *Drift guards*: `CoreSourceLinkTests` (every core source linked, and
  linked not copied), `ProtobufPinTests` (both generators pin the same
  protobuf commit), `ThemeTests` (palette parsed out of `ff_theme.h` at
  test time).
- *Honesty rules as tests, not comments*: no signal-tier label may
  contain a number or a unit; the stub client never emits `DELIVERED`.
- *Protocol tests*: framer dribble/resync/oversize, delivery-state
  mapping, the BLE drain triggers.

**Integration, with hardware (manual, from a Mac).** A `HardwareTests`
target, skipped unless `FIREFLY_HARDWARE=1` **and** a board is reachable,
so a green suite never depends on what is plugged in. It covers what
cannot be faked: real BLE discovery and pairing, a real two-phase
`want_config` reaching `.ready`, a real nodeDB dump, a real DM between
the two Heltecs with a real routing ACK, and a real phone-position push
read back with `meshtastic --info`.

**UI.** One XCUITest smoke test per platform — launch, visit all four
destinations, assert nothing crashes and that the placeholder screens do
not claim to have data. Deliberately thin: the logic is in view models
that are tested directly.

## CI plan

`.github/workflows/app.yml`, macOS runner, path-filtered to `app/**`,
`firmware/core/**`, `firmware/platform/**`, `ff_theme.h` and
`gen_nanopb.sh`. Two jobs:

- **package** — verify the symlink farm survived checkout, then
  `swift build` and `swift test`.
- **xcode** — `xcodebuild build` for `platform=macOS` and for
  `generic/platform=iOS Simulator` (generic: no booted device needed, and
  it does not depend on which iPhone models the runner image ships).

`firmware/core/**` is in the filter on purpose: `FireflyCore` *is* the
core, so a change under `firmware/core/src` can break the app without
touching `app/`. Hardware tests never run in CI — a hosted runner has no
radio, and a job that is red for want of a cable teaches people to ignore
red.

macOS minutes cost ~10× Linux, which is why this is a separate,
filtered workflow rather than jobs bolted onto `ci.yml`.

## Milestones

### M1 — it connects, and it tells the truth about what it sees

- Connect screen: node picker (BLE on both platforms), connection state
  including a distinct HANDSHAKING, channel import from a
  `https://meshtastic.org/e/#…` QR or pasted URL.
- Radar: live bearing/distance when a node reports a position, the
  no-GPS signal view when none does, and FIND.
- Inbox → thread, with `WAITING / SENT / DELIVERED / NO ACK` on every
  outbound message.
- Settings/Diagnostics: name, units, location sharing, link state, frame
  counters, firmware version.

**Acceptance criteria**

1. **A01_AC1** — `swift build` and `swift test` pass on macOS with no
   hardware and no network beyond dependency resolution; `xcodebuild`
   builds for `platform=macOS` and `generic/platform=iOS Simulator`.
2. **A01_AC2** — the app links `firmware/core`'s C sources in place. A
   new `firmware/core/src/*.c` that is not linked fails
   `CoreSourceLinkTests` by name; a linked entry that is a copy rather
   than a symlink fails too.
3. **A01_AC3** — the Swift protobufs and the puck's nanopb sources come
   from the same pinned `meshtastic/protobufs` commit; a drift fails both
   the generator script and `ProtobufPinTests`, naming both values.
4. **A01_AC4** — against a real Heltec V3 (2.7.26, US, Firefly channel,
   `position_precision: 32`), the macOS build discovers the node,
   completes both `want_config` phases, reaches `.ready`, and lists the
   other board in its node list.
5. **A01_AC5** — a DM from the app to the other board shows
   `WAITING → SENT → DELIVERED` on a real routing ACK; with the target
   powered off it shows `WAITING → SENT → NO ACK` and never `DELIVERED`.
6. **A01_AC6** — a node whose position arrives with `location_source =
   MANUAL` renders as **asserted**, with no age treated as staleness; a
   node whose position arrives with `precision_bits` absent never renders
   a metre-level distance.
7. **A01_AC7** — with no position for anyone, Radar renders the signal
   view; no label in it contains a number or a distance unit
   (`SignalTierTests` enforces this mechanically).
8. **A01_AC8** — with the stub client (and therefore in the iOS
   Simulator), no screen shows a node, a position, a message or a
   delivery state that did not come from an injected byte.
9. **A01_AC9** — the phone's GPS reaches the connected node as a
   `POSITION_APP` message with `location_source = LOC_EXTERNAL`,
   confirmed by `meshtastic --info` on that node; with location
   permission denied the app says so and sends nothing.

### M2 — it is usable in a field with one hand

- Crew pairing and colours, driven by `ff_crew`.
- FLARE / RALLY / STATUS on portnum 269 via `ff_proto`, interoperating
  with a puck when one exists again.
- Background BLE: stays connected in a pocket; reconnects on its own.
- Serial and TCP transports on macOS; the hardware test rig.

**Acceptance criteria:** background-connected for ≥30 min with the screen
off and the app not foregrounded, reconnecting after the node is power
cycled; a FLARE sent from the app decodes on a second client; the serial
transport completes the same handshake as BLE against the same board;
hardware tests skip cleanly with no board and pass with one.

### M3 — it remembers, and it is honest about remembering

- Message history and (if earned) the nodeDB in SwiftData.
- Restored data rendered **as restored**, with its age — never as live.
- Channel write-back (admin messages) behind an explicit confirmation.
- Swift 6 strict concurrency; XCUITest smoke tests in CI.

**Acceptance criteria:** a cold launch shows history with an explicit
"from storage, last seen …" treatment and no restored position is ever
drawn as a live fix; the package builds clean under
`SWIFT_STRICT_CONCURRENCY: complete`.

## Reuse assessment

### The archived iOS app (commit `8b0967f`)

~5,300 lines of hand-written Swift plus ~25,000 generated. Read with
`git show 8b0967f:<path>`.

**Worth taking, in order of value:**

1. **The FROMNUM-subscription-ACK gate** in
   `Firefly/Services/CoreBluetoothService.swift` — the `.connected`
   transition is deliberately deferred from `didDiscoverCharacteristicsFor`
   to `didUpdateNotificationStateFor`, with a comment saying the
   `want_config` write otherwise races the subscription ACK. It presents
   as an intermittent "connects but receives nothing" and is the single
   most expensive thing in the archive to re-derive. Taken as a *policy
   with a test*, not as a comment in a delegate.
2. **The two-phase `want_config`** with nonce-keyed completion
   continuations and per-phase timeouts (30 s config, 60 s nodeDB), and
   the heartbeat nonce ≥ 2. Taken as the algorithm.
3. **The empty-read-terminates-drain loop.** Taken.
4. **The five GATT UUIDs.** Taken verbatim — and cross-checked against
   Meshtastic-Apple rather than trusted.
5. **The protocol/mock/DI seam layout**, including the
   `targetEnvironment(simulator)` mock container. Taken as a shape.

**Not taken, with reasons:**

- `CoreBluetoothService.swift` and `MeshtasticClient.swift` as *files*.
  Combine subjects over unisolated mutable dictionaries, a FIFO
  continuation queue that resumes by `removeFirst()` without matching the
  callback to its write (safe only because exactly one characteristic is
  ever written), continuations that leak on disconnect because `cleanup()`
  never fails them, no MTU handling at all, and roughly 40% `NSLog` with
  emoji. The knowledge is worth more than the code.
- **The ViewModels and Views.** `ObservableObject` / `@Published` /
  `.receive(on: RunLoop.main)` is the world `@Observable` replaced, and
  the platform patches were bolted on late.
- **The generated protobufs.** Pinned to a five-month-old submodule and
  SwiftProtobuf 1.35; regenerating from *our* pin takes minutes and is
  the only way the app and the puck stay in sync.
- **`scripts/gen_protos.sh`.** Broken as committed — a stray `--protoc=`,
  an input glob pointing at a directory that does not exist, and a brew
  formula that installs the plugin rather than `protoc`. Rewritten as
  `app/tools/gen_swift_protos.sh`, which additionally refuses to run on a
  pin mismatch with the nanopb generator.
- **`Firefly.xcodeproj`.** Genuinely modern — objectVersion 77, two
  `PBXFileSystemSynchronizedRootGroup`s, only 4 file references, 573
  lines — and resurrecting it was seriously considered. Rejected: its
  three deployment targets are all 26.0 against our 17.0/14.0 floor, it
  links MapKit from an `iOSSupport` path that is wrong for a real macOS
  build, it carries a 130-line junk `contents.xcworkspacedata`, it has no
  local-package reference (which is the *only* structurally new thing
  this project needs), and its bundle id is `com.jakeholland.Firefly`
  rather than the specified `com.jakeholland.firefly`. A generated
  project from a 60-line `project.yml` was faster than that edit list and
  leaves the project regenerable. **The one thing taken from it: its
  Info.plist key list, plus `INFO_PLIST_GUIDE.md`'s note of the two keys
  the project never actually added** — `NSLocationWhenInUseUsageDescription`
  and `UIBackgroundModes: bluetooth-central`. Both are in ours.
- **The docs.** `Architecture.md`, `PROJECT_SUMMARY.md` and
  `FILE_INDEX.md` describe files that never existed and "test statistics"
  for tests that were never written (`FireflyTests.swift` is the 16-line
  Xcode template stub). `DATA_FLOW.md`'s send/receive diagrams are
  accurate and useful; its connection-flow diagram is stale and
  contradicts the code's own ACK gate. Mined, not carried over.

**Honest summary:** the archive's value is protocol knowledge and seam
shapes, not code. About 300 lines of it are worth more than the other
5,000, and they are the 300 that took someone a bad afternoon with a
radio.

### Meshtastic-Apple (`~/Developer/Meshtastic-Apple`, GPL-3.0)

Firefly is GPL-3.0, so this is license-compatible; even so, what is taken
here is **behaviour, re-implemented**, not copied source, and every
borrowed rule is cited at its use site. `docs/developer/transport.md` in
that repo is worth reading in full before touching the transports.

Borrowed:

- **The GATT UUID set**, as the second independent confirmation.
- **The pairing-sheet hold**: subscribing to an encrypted characteristic
  is what makes iOS show the PIN sheet, so the connect continuation must
  stay open until the notify state updates. Resolving optimistically
  tears down the connection *and* the sheet.
- **The bond hint drives the connect timeout** — 90 s for a first-ever
  bond, 5 s for a remembered one — and forgetting the hint self-heals a
  bond the user removed in iOS Settings.
- **Pairing-failure classification** and the rule that
  `peerRemovedPairingInformation` is terminal, not retryable.
- **Do not re-request a node dump that is already running.**
- **Pause scanning for the duration of a connect** — duplicate
  advertisements during the pairing window break the handshake.
- **`.bufferingNewest(4096)`** on the event streams, and keeping the TCP
  receive drain off the main actor (their note: main-actor isolation
  stalled it enough for the OS to drop the connection).
- **`insufficientResources` write retry** with a 120 ms × n backoff, and
  the observation that it is buffer exhaustion, not packet size.
- **Duplicate-packet suppression on `packet.id`** before insert, and
  suppressing notifications for self-originated packets.
- **Routing-ack semantics**: `request_id` matching, `error_reason`
  taxonomy, DM-vs-broadcast distinction, and a ~5 minute render-time
  no-ack window.
- **Phone GPS as a `POSITION_APP` message to the connected node** with
  `LOC_EXTERNAL`, at a default 30 s / 5 s-floor cadence, and the
  `UIBackgroundModes` pair.
- **The channel URL format**: `https://meshtastic.org/e/#<base64url
  ChannelSet>` (also `meshtastic://e/#…`, `?add=true` in query or
  fragment), with base64url ⇄ base64 translation `-`→`+`, `_`→`/`,
  re-pad to a multiple of 4.
- **`position_precision` defaulting**: a QR-imported `ChannelSettings`
  with no `moduleSettings` must have `positionPrecision` written
  explicitly, because the firmware otherwise defaults it to 32 and leaks
  exact coordinates. The inverse of our own #47 trap, and just as sharp.

Deliberately **not** borrowed: their app architecture. It is a
general-purpose client with SwiftData, MQTT, store-and-forward, PKI,
route discovery, a watch app and a TV app. Firefly's app is four screens
with a rule about honesty; adopting their structure would import a great
deal of machinery for features that are explicitly out of scope.

## Slices

Six slices, written to be built in parallel by separate agents. File
ownership is disjoint with exactly **one** declared exception, noted in
slice C.

### Slice A — BLE transport + the real Meshtastic client

**Owns:** `FireflyKit/Sources/FireflyMesh/BLE/*`,
`FireflyMesh/MeshtasticClient.swift`, `FireflyMesh/NodeDB.swift`,
`Tests/FireflyMeshTests/{ClientHandshakeTests,NodeDBTests}.swift`.
**Depends on:** `MeshTransport`, `TransportEvent`,
`MeshtasticClientProtocol`, `DeliveryState`, `MeshtasticBLE`,
`StreamFramer` (all existing), `MeshtasticProto`.
**Must add:** handshake tests driven by injected `FromRadio` bytes over
`LoopbackTransport` (both nonces, the stale-nonce case, `rebooted`
mid-session); nodeDB tests for the three absence rules (loc source,
RSSI/hop path, precision bits); routing-ack → delivery-state tests
including the broadcast case and the no-ack window.
**Acceptance:** a `MeshtasticClient` reaches `.ready` from injected bytes
with no radio; every absence rule is asserted; `FromRadio.rebooted`
reissues `want_config` with a *new* nonce; the BLE transport compiles and
runs on both platforms.

### Slice B — the C-core bridge

**Owns:** `FireflyKit/Sources/FireflyModel/Bridge/*` (`CoreClock.swift`,
`CrewStore.swift`, `RadarBridge.swift`, `InboxBridge.swift`,
`FindBridge.swift`, `FireflyPacket.swift`, `CString+Swift.swift`),
`Tests/FireflyCoreTests/Bridge*.swift`.
**Depends on:** `FireflyCore` only. Must **not** import `FireflyMesh` —
the bridge takes plain values, so it is testable with no client at all.
**Must add:** a lifetime test (allocate/free a `CrewStore` in a loop
under the address sanitiser without a leak or a use-after-free); tests
that `ff_radar_compute`'s output survives the round trip into Swift
values; a test that a `char[16]` name containing no terminator does not
over-read.
**Acceptance:** no `UnsafeMutablePointer` or imported C tuple appears in
any public API; the clock struct outlives every context that borrows it;
all eight M1 modules are bound.

### Slice C — app shell, Connect, Settings/Diagnostics

**Owns:** `app/Firefly/Sources/RootView.swift` (and the destination
registry in it), `app/Firefly/Sources/Connect/*`,
`app/Firefly/Sources/Settings/*`,
`FireflyKit/Sources/FireflyModel/{SettingsStore,ChannelURL}.swift`,
`Tests/FireflyModelTests/ChannelURLTests.swift`.
**Shared file, declared:** `RootView.swift`'s destination switch has one
line per screen. Slices D and E each change exactly one of those lines.
Merge order C → D → E; nothing else in the file is touched.
**Depends on:** `ConnectViewModel`, `MeshtasticClientProtocol`,
`FireflyTheme`.
**Must add:** channel-URL import tests — `https://meshtastic.org/e/#…`
and `meshtastic://e/#…`, `?add=true` in both query and fragment,
base64url padding, a malformed payload rejected rather than
half-applied, and the `positionPrecision`-written-explicitly rule.
**Acceptance:** all four destinations reachable on both platforms; a
scanned or pasted channel URL round-trips to a `ChannelSet`; Diagnostics
shows link state, frame counters and firmware version, and shows
"unknown" where it does not know.

### Slice D — Radar

**Owns:** `app/Firefly/Sources/Radar/*`,
`FireflyKit/Sources/FireflyModel/RadarViewModel.swift`,
`Tests/FireflyModelTests/RadarViewModelTests.swift`. One line in
`RootView.swift`.
**Depends on:** slice B's `RadarBridge` and `CrewStore`,
`SignalPresentation`, `HeadingProviding`.
**Must add:** view-model tests for every `radar_mode_t` the app can
reach — LIVE / STALE / LOST / CLOSE / NOFIX / NOHDG / SIGNAL / NOSEL —
each asserting the exact strings shown; a test that an asserted position
never renders as stale; a test that macOS (no magnetometer) renders
NOHDG rather than a stuck arrow.
**Acceptance:** the signal view appears when nobody has a position and
carries no distance-shaped text; FIND runs a session at the core's own
cadence and stops at its own limits; every position on screen shows its
source and its age.

### Slice E — Inbox, Thread, outbox

**Owns:** `app/Firefly/Sources/Inbox/*`,
`FireflyKit/Sources/FireflyModel/{InboxViewModel,ThreadViewModel}.swift`,
`Tests/FireflyModelTests/Inbox*.swift`. One line in `RootView.swift`.
**Depends on:** slice B's `InboxBridge`, slice A's
`deliveryUpdates`, `DeliveryState`.
**Must add:** conversation-list and thread tests built from injected feed
items (unread counts, previews, direction); a delivery-state progression
test per state including the broadcast-never-DELIVERED case; a duplicate
`packet.id` echo test proving the sent row is not overwritten.
**Acceptance:** CREW plus one conversation per paired member, exactly as
`ff_inbox` builds them; every outbound row shows one of the five states
and never an invented one.

### Slice F — location, heading, serial + TCP transports, hardware rig

**Owns:** `FireflyKit/Sources/FireflyMesh/{Serial,TCP}/*`,
`FireflyKit/Sources/FireflyModel/{LocationProvider,HeadingProvider}.swift`,
`FireflyKit/Tests/HardwareTests/*`, the hardware section of
`app/README.md`.
**Depends on:** `MeshTransport`, `StreamFramer`, slice A's client.
**Must add:** serial framing tests against recorded bytes (no port
needed); a TCP transport test against a local socket; provider tests
that "permission denied" and "no fix" produce *absence*, never a
coordinate; and the hardware suite itself, which must skip cleanly with
no board.
**Acceptance:** `swift test` stays green on a machine with no radio and
no serial device; `FIREFLY_HARDWARE=1 swift test --filter Hardware`
completes a handshake, a DM with a real ACK, and a phone-position push
verified by `meshtastic --info`, against a Heltec V3.

## Open questions for the owner

1. **Bundle id case.** The spec says `com.jakeholland.firefly`; the
   archived project used `com.jakeholland.Firefly`. Ours is lowercase as
   specified — confirm, since changing it later is an App Store identity
   change.
2. **Signing.** Signing is off so a clean checkout builds. Which team /
   provisioning do you want wired in for on-device runs?
3. **Firefly 1's bench position.** `docs/hardware/heltec-v3.md` now
   records it as pending; what coordinates do you want asserted, and
   should it be `CLIENT_MUTE` while it plays landmark?
4. **Minimum firmware floor.** The handshake checks a version and says so
   plainly below the floor. 2.7.26 is what is on the bench — do we
   declare that the floor, or support older?
5. **Channel PSK handling.** M1 imports a channel from a QR/URL and keeps
   the PSK in the Keychain. Do you also want the app able to *generate* a
   channel (so the phone can provision a new puck), or is the CLI the
   only thing that ever mints a PSK?
