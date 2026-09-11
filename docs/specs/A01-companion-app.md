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
        EventHub.swift             multicast AsyncStream fan-out (S1) — shared seam
        StreamFramer.swift         0x94 0xC3 framing (serial + TCP)
        MeshtasticBLE.swift        GATT UUIDs + the drain/pairing policy
        MeshtasticClientProtocol.swift  the seam view models depend on
        DeliveryState.swift        WAITING/SENT/DELIVERED/NO ACK/DROPPED (+ NONE = absent)
        BLE/ Serial/ TCP/          concrete transports (slices A, F)
      FireflyModel/                view models, presentation rules, theme
        FireflyTheme.swift         the palette, pinned against ff_theme.h
        SignalPresentation.swift   tiers as words, never as distance
        ConnectViewModel.swift     the MVVM template every screen follows
        CoreStore.swift            the @MainActor seam the data flow routes
                                   through — shared, landed as a skeleton (S5)
        AppDependencies.swift      the DI composition root — shared, landed
                                   as a skeleton with .stub()/.live() (S5)
        LocationProviding.swift    phone-GPS seam + UnavailableLocationProvider
        HeadingProviding.swift     compass seam + NoHeadingProvider (NOHDG)
        SettingsStoring.swift      UserDefaults-shaped seam + InMemorySettingsStore
        Bridge/                    Swift-safe wrappers over the C core (slice B)
    Tests/
      FireflyCoreTests/            the C bridge + the anti-drift guards
      MeshtasticProtoTests/        wire round-trips + the pin guard
      FireflyMeshTests/            framer, delivery states, BLE contract, EventHub
      FireflyModelTests/           theme, honesty rules, view models, CoreStore
      HardwareTests/               serial + TCP; tagged, skipped without a board
                                   (slice F). BLE hardware tests are NOT here — see
                                   FireflyHardwareTests below and B1.
  Firefly/Sources/                 the SwiftUI shell
  Firefly/Resources/               Info.plist, entitlements
  FireflyHardwareTests/            BLE hardware tests, HOSTED in Firefly.app —
                                   `xcodebuild test`, not `swift test` (B1)
  Config/                          Firefly.xcconfig (committed, #include?s
                                   Local.xcconfig) + Local.xcconfig.example
  Firefly.xcodeproj + project.yml  committed project, regenerable
  tools/                           link_core_sources.sh, gen_swift_protos.sh
```

The stack is strictly one-directional: `FireflyCore` knows nothing;
`MeshtasticProto` knows nothing; `FireflyMesh` depends on both;
`FireflyModel` depends on `FireflyMesh` and `FireflyCore`; the app target
depends on all four and is the only place SwiftUI appears. Each layer can
be built and tested without the one above it — the same discipline
`docs/ARCHITECTURE.md` states for the firmware.

`EventHub.swift`, `CoreStore.swift`, `AppDependencies.swift`,
`LocationProviding.swift`, `HeadingProviding.swift` and
`SettingsStoring.swift` are landed in this PR as real, tested,
minimal-but-working seams — not claimed by any one slice's file list
(the same way `MeshtasticClientProtocol.swift` and `DeliveryState.swift`
already were not) — so slices C, D, E and F each depend on a symbol that
already exists instead of inventing their own shape for it (S5).

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
         linkState()EventHub  nodeUpdates()EventHub  deliveryUpdates()EventHub
           (multicast, S1)      (multicast, S1)        (multicast, S1)
                   │                  │                  │
          ┌────────┴────────┐        │                  │
          ▼                 ▼        ▼                  ▼
   view models         CoreStore  (@MainActor) ◀─────────┘
   (@Observable)   owns ff_crew_t / ff_feed_t / ff_find_t
        │            feeds them with ff_crew_on_position,
        │            ff_crew_on_rssi, ff_crew_on_heard, ff_feed_push …
        │                          │
        │            ff_radar_compute / ff_inbox_build
        │                          │  plain Swift value types
        ▼                          ▼
      SwiftUI  ◀───────── view models (@Observable)
```

Every stream is `EventHub`-backed (S1): `CoreStore` and a screen's own
view model each hold an INDEPENDENT subscription obtained by calling
`linkState()` / `nodeUpdates()` / `deliveryUpdates()` once and keeping
the returned `AsyncStream` — not two consumers racing over one shared
stream, which a stored `AsyncStream` property would have been. See
"Threading model" below for the ordering rule that subscribing before
publishing depends on.

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
  the main thread and are funnelled into the client actor, which
  publishes through `EventHub`s (below). The Meshtastic-Apple TCP
  reader's own note — that main-actor-isolating the receive drain stalled
  it enough for the OS to drop the connection — is the reason this is not
  simply main-actor everywhere.
- **Every event stream is multicast, via `EventHub`, not a stored
  `AsyncStream`.** `AsyncStream` itself is single-consumer: a second
  `for await` over the same instance competes with the first for
  elements rather than getting its own copy. `linkState`, `nodeUpdates`
  and `deliveryUpdates` each need independent readers — `CoreStore`
  *and* a view model, and for `linkState` also Diagnostics — so
  `MeshtasticClientProtocol` and `MeshTransport` expose them as methods
  (`func linkState() -> AsyncStream<LinkState>`, and so on), each
  handing the caller a fresh subscription from a small
  `FireflyMesh.EventHub<Element>` (a class that fans one `yield(_:)` out
  to every current subscriber's own continuation). A caller that needs
  every value must call the method — and capture the returned stream —
  **before** triggering whatever will publish into it: a subscription
  registered after a value was yielded simply misses that value,
  multicast rather than replayed. `ConnectViewModel.observe()` and
  `CoreStore.observe(client:)` are the worked examples, and
  `EventHubTests`/`CoreStoreTests` pin both the fan-out and the ordering
  rule.
- **Back-pressure is bounded, and drops the oldest.** Every `EventHub`
  subscription uses `.bufferingNewest(4096)`, matching Meshtastic-Apple.
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
singletons. Landed in this PR as a real, working, tested seam
(`FireflyModel/AppDependencies.swift`, S5) rather than left for a slice
to invent — `FireflyApp.swift`'s one stored `ConnectViewModel` is built
from it today.

```swift
struct AppDependencies: Sendable {
    var client: any MeshtasticClientProtocol
    var location: any LocationProviding
    var heading: any HeadingProviding
    var store: any SettingsStoring
}
```

- `AppDependencies.stub()` builds `StubMeshtasticClient` over
  `LoopbackTransport` and a location/heading provider
  (`UnavailableLocationProvider` / `NoHeadingProvider`) that reports
  **unavailable**, not fake coordinates.
- `AppDependencies.live()` is the real stack, and as of M1 integration
  every field in it is real: slice A's `MeshtasticClient` over one
  `BLETransport` (held twice, as the client's transport AND the node
  picker's `NodeScanning` — two instances would mean two
  `CBCentralManager`s and a picker whose selection the connecting
  transport never sees), slice F's CoreLocation-backed
  `LocationProvider`/`HeadingProvider`, and slice C's
  `UserDefaults`-backed `SettingsStore`. It was identical to `.stub()`
  while those slices were still in flight, on purpose — nothing above
  this seam behaves differently depending on which one is picked, which
  is exactly what made it safe to land before them.
- `FireflyModel/Live/AppGraph.swift` is the other half of the
  composition root: `AppDependencies` answers "which implementations",
  `AppGraph` answers "how many, owned by whom, subscribed when". It owns
  the one `CoreStore` (and therefore the one `ff_crew_t`/`ff_feed_t`/
  `ff_radar_smooth_t`/`ff_find_t`), the one `PhoneGPSUplink`, the
  portnum-269 reader and the ack-timeout tick, and it builds every view
  model. Nothing below it may call `.current()` for itself.
- `AppDependencies.current()` is what callers actually use: `.stub()` in
  the iOS Simulator via `#if targetEnvironment(simulator)` — an idea
  taken directly from the archived app's
  `DependencyContainer.simulatorContainer()`, which existed because
  instantiating `CBCentralManager` under the Simulator is a dead end —
  and `.live()` everywhere else.

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

`want_config_id` is not an arbitrary correlation nonce — it is one of
two **firmware-recognized sentinel values**, and using anything else
does not do what the spec previously (incorrectly) implied:

```swift
/// Meshtastic's own sentinels for ToRadio.want_config_id — NOT
/// arbitrary. Confirmed two ways: Meshtastic-Apple's
/// AccessoryManager.swift:150-151 defines exactly these two constants
/// and AccessoryManager.swift:1188,1199 dispatch config_complete_id on
/// them by name ("Unknown nonce completed" for anything else); this
/// repo's own archived app (`git show 8b0967f:
/// Firefly/Core/Models/MeshtasticClient.swift:53-56,121`) documents
/// 69420 as "the Meshtastic firmware constant" for the same split.
/// Meshtastic TV's MeshClient.swift:21-24 states the mechanism
/// outright: sending wantConfigID with NONCE_ONLY_CONFIG triggers the
/// config dump, NONCE_ONLY_DB the node-database dump — and older
/// firmware returns the FULL dump for either one, so a client that
/// sends a random value where the firmware expects one of these two
/// gets a full dump on phase A and a second full dump on phase B,
/// exactly the interleaved-double-dump failure the reboot/rebooted
/// guard below exists to prevent.
enum MeshtasticConfigNonce {
    static let onlyConfig: UInt32 = 69420
    static let onlyNodeDB: UInt32 = 69421
}
```

1. Transport reaches `.ready` (for BLE: subscription ACKed).
2. Send a `Heartbeat` with its OWN nonce,
   `nonce = UInt32.random(in: 2...UInt32.max)` — never 1, which firmware
   may special-case. **This is a different field from
   `want_config_id` below** — `Heartbeat.nonce` is a keepalive value with
   no firmware-recognized meaning, while `want_config_id` in steps 3–4
   MUST be one of the two sentinels above. Conflating them (as an
   earlier draft of this section did, by numbering both "nonce A") reads
   as though the heartbeat's random value drives the handshake; it does
   not.
3. `ToRadio.want_config_id = MeshtasticConfigNonce.onlyConfig` (69420) →
   the radio streams `my_info`, `metadata`, `channel`s, `config`,
   `module_config`, terminated by
   `config_complete_id == MeshtasticConfigNonce.onlyConfig`. Timeout
   30 s.
4. `ToRadio.want_config_id = MeshtasticConfigNonce.onlyNodeDB` (69421) →
   the node database dump, terminated by
   `config_complete_id == MeshtasticConfigNonce.onlyNodeDB`. Timeout
   120 s; do **not** re-send this if a dump is already in progress — a
   re-request restarts it from the top and interleaves two dumps.
5. Check the firmware version; below the supported floor (2.7.26 — see
   "Decisions already made"), say so plainly instead of failing
   mysteriously later.
6. `.ready`. Only now is the nodeDB meaningful.

Two phases rather than one, so a large mesh's node dump cannot delay the
config the UI needs to draw anything. Both the archived app and
Meshtastic-Apple do this, and both use these exact two sentinel values —
not because the wire format requires any particular number (proto3 just
sees a `uint32`), but because that is what the firmware on the other end
actually branches on. A01_AC4 ("completes both `want_config` phases") is
only satisfiable with these two values; a client that generates its own
per-connection nonces here (as opposed to `Heartbeat.nonce`, which
legitimately is random) will get a full dump twice and never reach a
clean `.ready`.

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

The five states in the table above are five of the SIX values of
`ff_feed_send_status_t`
(`FF_SEND_WAITING/SENT/DELIVERED/NO_ACK/DROPPED`), and `DeliveryState`
pins the mapping both directions: `ffSendStatus` (Swift → C, for what
this app sends) and `init?(ffSendStatus:)` (C → Swift, for what
`InboxBridge` reads back). The sixth C value, `FF_SEND_NONE` — the zero
value every *inbound* feed item carries, deliberately zero so a
zero-initialized or legacy item never accidentally claims a delivery
fact it doesn't have (`ff_feed.h`'s own doc comment) — has **no**
`DeliveryState` case; `init?(ffSendStatus:)` returns `nil` for it rather
than inventing a sixth Swift case or crashing. `DeliveryStateTests` pins
both the five-way forward mapping and the `nil`-for-`NONE` reverse one,
so a reorder of the C enum fails here rather than making the two
products disagree about what DELIVERED means, and a bridge that reads
`send_status` off an inbound item cannot silently invent a state for it.
The 5-minute window is **derived at render time** from the message's
timestamp, not driven by a timer — a timer that fires while the app is
suspended is a timer that lies.

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

Deliberately small:

- **`UserDefaults`**: last connected peripheral id, the set of
  successfully bonded peripherals (the bond hint that picks the connect
  timeout), units (metric/imperial), location-sharing on/off and
  interval, the chosen transport.
- **Keychain**: channel PSKs. They are keys; they do not belong in
  `UserDefaults`.
- **Nothing else in M1/M2.** No node database on disk, no message history
  on disk.
- **M3 adds SwiftData for message history** (`app/FireflyKit/Sources/
  FireflyModel/Persistence/`) — and nothing else. The nodeDB was
  considered and NOT persisted; see "Why the nodeDB is not persisted"
  below.

### What M3 persists, and how

`HistoryStore` owns one SwiftData `ModelContainer` holding
`PersistedMessage` rows — the durable mirror of every `FeedMessage` this
app has ever pushed, live or restored. `PersistingInboxProvider` (an
`InboxProviding` decorator) is the only writer: it wraps the real
`CoreInboxProvider` and mirrors every `push`/`markSent`/`setStatus` call
into `HistoryStore` after forwarding it, so there is exactly one code
path for "this needs to survive a relaunch" — never a second one that
could drift from what the live ring actually shows.

**Cold-launch restore (`HistoryRestorer`).** Runs inside `AppGraph.init`,
before anything can observe a client (the same ordering rule
`CrewPairingRestorer.restore` already follows, for the identical
reason): it reads every persisted message, then pushes the most recent
ones back into the live `ff_feed_t` ring with their ORIGINAL timestamps
— never `Date()` — so the core's own age/presence math renders them
exactly as honestly as an uninterrupted session would have.

- **N = 32**, mirroring `FF_FEED_CAP` (`firmware/core/include/ff_feed.h`)
  — the live ring's own hard cap. Reseeding more would just evict the
  oldest of those on the very next push, so there is no honest way to
  show more than this many restored messages in the ring a screen
  actually reads from. This is not a new limit M3 introduces: a session
  that never restarted already loses anything past the 32 most recent
  items to the same ring. The rest of a longer history stays on disk,
  unreachable to any live screen until a future feature reads it
  directly.
- Every currently-**WAITING** item is reseeded unconditionally,
  regardless of that cap — a message that never left the device before
  the process ended is unfinished business, not history, and must not
  be silently forgotten.
- **SENT restores as NO ACK.** A routing ack cannot arrive for a packet
  this process no longer has a live send in flight for: either the ack
  already came back (DELIVERED, untouched) or it did not, and after a
  relaunch there is no future in which it still could. WAITING is left
  alone — unlike SENT, it never left the device, so flushing it on the
  next connect (below) is a genuine continuation, not a claim about what
  already happened.
- Every restored message renders with an explicit **"FROM STORAGE"**
  tag next to its own honest age (`InboxAge`) — in the Thread's message
  bubbles and the Inbox row's preview alike — and never as a live
  delivery/presence claim. The tag is earned once, permanently, for a
  given message: a LIVE event arriving later for the same conversation
  never retroactively "un-restores" an older message, and is itself
  never mistaken for restored (`PersistingInboxProvider`'s own
  `restoredMessageIDs`, populated exactly once from `HistoryRestorer
  .restore`'s return value).

**Outbox flush on connect.** A WAITING item restored from disk has no
live `ThreadViewModel` watching it — that type's own in-memory outbox
only ever holds what it personally queued in the CURRENT session.
`AppGraph.flushPersistedOutbox()` owns these instead: a dedicated,
independent `client.linkState()` subscription (S1) that, on the link's
next not-ready → ready edge, re-attempts every persisted WAITING item —
bounded at `ThreadViewModel.outboxCap` (8), oldest first, the same
drop-oldest discipline a live thread's own outbox already follows.

**Migration policy.** One schema, versioned (`HistorySchemaV1` /
`HistoryMigrationPlan`), plus **drop-and-recreate** on any mismatch the
migration plan does not cover. Message history is convenience/context,
not a safety- or identity-critical record — unlike `CrewPairingStore`'s
persisted pairing, or the Keychain-held channel PSKs, this app never
promises to keep it. A future schema this binary predates, or a
corrupted store file, is deleted and rebuilt empty rather than crashing
the app on launch (`HistoryStore.makeContainer`). This is the one place
in the app that silently discards user data on purpose, and it is
disclosed in three places: here, `HistoryStore`'s own header comment,
and the Settings **"Clear history"** action (with confirmation), which
exercises the identical "wipe the store" path deliberately, on request.

**Demo isolation.** `.stub()`/`.demo()`/`.demoBundle()` all get a
disposable `HistoryStore.inMemory()` — never `.live()` — picked
automatically inside `AppGraph.init` from `dependencies.store is
InMemorySettingsStore` (true for both, and for nothing `.live()` ever
builds), so demo mode never persists across a real relaunch. For
screenshots, `-FireflyDemoRestored` (or `FIREFLY_DEMO_RESTORED=1`)
seeds a fresh in-memory store (`DemoHistorySeed`) with a small
"yesterday" history BEFORE `AppGraph.init` runs its own restore pass —
so the exact same restore code path a real relaunch takes is what
renders the "FROM STORAGE" treatment, never a parallel "looks restored"
fake. Gated the same way every other `DemoLaunch` check is: only inside
`#if targetEnvironment(simulator)`, so a stray launch argument can never
turn a real device's history into fictional festival data.

### Why the nodeDB is not persisted

M3 considered persisting each paired crew member's last-known position
alongside their message history — the parenthetical in this project's
own M3 task description even sketches how it would have to work (feed a
restored position into `ff_crew` with its original timestamp, so the
core's own freshness math renders it as an honest LOST/last-known ghost,
never a live fix). It was **not built**, for three reasons:

1. **The durable half of "nodeDB" already exists.** `CrewPairingStore`
   (M2) already persists exactly the part of a crew member's identity
   that is worth keeping across a relaunch — who is paired, their
   colour, their local nickname — and `CrewPairingRestorer` replays it
   onto a fresh `ff_crew_t` before anything else can observe a client.
   What M3 would add on top is only the VOLATILE half: position, RSSI,
   heard-timestamps — precisely the fields whose entire value is being
   current.
2. **A live reconnect supersedes it almost immediately, honestly.** The
   moment the app reconnects, a real want_config replay repopulates the
   whole current nodeDB from the radio itself — fresh, live, and not a
   guess. A persisted position would buy, at best, a few seconds of
   "last known" ghost display before the real data arrives and replaces
   it — a narrow benefit for a second SwiftData model, a second restore
   path into `ff_crew`, and a second "duplicate on live replay" hazard
   to build and test (message history already needed exactly that
   machinery once; building it twice roughly doubles the M3 surface for
   a payoff measured in seconds).
3. **The honest answer is "nothing," and that is already correct.**
   Radar's own rule is that no restored position is ever drawn as a
   live fix — the strongest way to prove that mechanically is to feed
   it NOTHING to draw at cold launch: a paired member with no
   `ff_crew_on_position`/`ff_crew_on_heard` call since the process
   started renders LINKED (`ff_sigview`'s own "no evidence yet" state),
   never a fabricated ghost dot. That is the same empty-but-honest
   Radar a real radio with nothing in range produces (`AppDependencies
   .stub()`'s own defining property, restated for a cold launch instead
   of a stub radio) — not a gap M3 leaves open, but the deliberately
   simpler, equally honest answer.

If a future milestone wants "last seen HERE" on Radar across a
relaunch, `HistoryStore`'s own schema-versioning/migration machinery
already generalizes to a second `@Model` type — this decision can be
revisited without redesigning the persistence layer underneath it.

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
- *M3 persistence*: `HistoryStoreTests` (the SwiftData round trip, an
  in-memory `ModelContainer`, pruning, isolation between stores),
  `HistoryRestorerTests` (reseed cap/ordering, WAITING always included,
  SENT → NO ACK), `AppGraphTests`' own M3 section (a restored member
  later heard live flips to live with no duplicates, through a REAL
  `CoreInboxProvider` and a scripted want_config-shaped replay; the
  persisted-outbox flush on connect, bounded, oldest first),
  `InboxAgeTests` (the age-rendering table), `DemoHistoryIsolationTests`
  (demo/stub never share or persist history; `-FireflyDemoRestored`'s
  own seed path).

**Integration, with hardware (manual, from a Mac). Two suites, run two
different ways — not a stylistic split, a TCC constraint (B1):**

- **BLE — `FireflyHardwareTests`, an app-hosted test target, run with
  `xcodebuild test`.** macOS aborts
  (`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`) any CoreBluetooth process
  that is not inside a signed `.app` bundle carrying
  `NSBluetoothAlwaysUsageDescription` and launched via LaunchServices. A
  `swift test` xctest binary is none of those three things — constructing
  a `CBCentralManager` there does not fail one test, it **aborts the
  whole process**. So the BLE half of the hardware suite lives in
  `app/FireflyHardwareTests`, a unit-test bundle whose host application
  is `Firefly.app` (`project.yml`'s `FireflyHardwareTests` target,
  `TEST_HOST`/`BUNDLE_LOADER` pointed at the built app), run with:

  ```
  FIREFLY_HARDWARE=1 xcodebuild test -scheme Firefly \
    -destination 'platform=macOS' -only-testing:FireflyHardwareTests
  ```

  Skipped (not failed, not hung) without `FIREFLY_HARDWARE=1` — the one
  placeholder test in this PR proves exactly that skip, and `xcodebuild
  test` running it (skipped) is checked into CI.
- **Serial and TCP — `HardwareTests`, plain `swift test`.** These
  transports are not CoreBluetooth and are unaffected by the TCC
  restriction above, so they stay a normal SwiftPM test target, skipped
  unless `FIREFLY_HARDWARE=1` **and** a board is reachable:

  ```
  FIREFLY_HARDWARE=1 swift test --filter Hardware
  ```

Between the two suites, hardware tests cover what cannot be faked: real
BLE discovery and pairing, a real two-phase `want_config` reaching
`.ready`, a real nodeDB dump, a real DM between the two Heltecs with a
real routing ACK, and a real phone-position push read back with
`meshtastic --info`. Neither suite ever runs WITH a board or the env var
in CI — a green run there only proves both build and skip cleanly.

**UI.** One XCUITest smoke test per platform — launch, visit all four
destinations, assert nothing crashes and that the placeholder screens do
not claim to have data. Deliberately thin: the logic is in view models
that are tested directly.

## CI plan

`.github/workflows/app.yml`, macOS runner, path-filtered to `app/**`,
`firmware/core/**`, `firmware/platform/**`, `ff_theme.h` and
`gen_nanopb.sh`. Two jobs:

- **package** — verify the symlink farm (sources AND headers, S8)
  survived checkout, then `swift build` and `swift test`.
- **xcode** — `xcodebuild build` for `platform=macOS` and for
  `generic/platform=iOS Simulator` (generic: no booted device needed, and
  it does not depend on which iPhone models the runner image ships);
  then `xcodebuild test -only-testing:FireflyHardwareTests` with NO
  `FIREFLY_HARDWARE` and no board, proving the app-hosted BLE hardware
  suite builds and skips cleanly (B1) — never proving it passes with a
  board, which no hosted runner has.

`firmware/core/**` is in the filter on purpose: `FireflyCore` *is* the
core, so a change under `firmware/core/src` can break the app without
touching `app/`. Hardware tests never run WITH a board or
`FIREFLY_HARDWARE=1` in CI — a hosted runner has no radio and no serial
device, and a job that is red for want of a cable teaches people to
ignore red.

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
   builds for `platform=macOS` and `generic/platform=iOS Simulator`; and
   `FIREFLY_HARDWARE=1` unset, `xcodebuild test
   -only-testing:FireflyHardwareTests` builds and skips (B1) — it must
   never abort the test process the way constructing a `CBCentralManager`
   under bare `swift test` does.
2. **A01_AC2** — the app links `firmware/core`'s C sources AND headers
   in place. A new `firmware/core/src/*.c` or a new header under
   `firmware/core/include`/`firmware/platform/include` that is not
   linked fails `CoreSourceLinkTests` by name; a linked entry — source or
   header — that is a copy rather than a symlink fails too (S8).
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

- Message history in SwiftData; the nodeDB was considered and NOT
  persisted (disclosed decision — see "Persistence" > "Why the nodeDB is
  not persisted").
- Restored data rendered **as restored**, with its age — never as live.
- Channel write-back (admin messages) behind an explicit confirmation.
- Swift 6 strict concurrency; XCUITest smoke tests in CI.

**Acceptance criteria:** a cold launch shows history with an explicit
"FROM STORAGE" + honest-age treatment (Thread bubbles and Inbox row
previews alike — see "Persistence") and no restored position is ever
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
2. **The two-phase `want_config`**, taken as the algorithm: the
   firmware-recognized sentinel nonces themselves (`NONCE_ONLY_CONFIG =
   69420`, `NONCE_ONLY_DB = 69421` — see "Meshtastic client" > "Handshake"
   for the full citation), completion continuations keyed on them, and
   per-phase timeouts (30 s config, 120 s nodeDB). The heartbeat's OWN
   nonce (≥ 2, a separate field) is taken too, but is not part of this
   mechanism.
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

Six slices, written to be built in parallel by separate agents. Most
files are owned by exactly one slice. Four files are **shared, and
each slice edits them append-only, one declared hunk per slice** (S6) —
the same rule `RootView.swift`'s destination switch already followed,
now stated for all four:

| Shared file | Each slice's one hunk |
|---|---|
| `app/Firefly/Sources/RootView.swift` | one line in the destination `switch` (already declared, below, in slice C) |
| `app/Firefly/Sources/FireflyApp.swift` | its own view model, constructed from `AppDependencies` and injected into `RootView`/the destination it owns — never touching another slice's line |
| `app/Firefly.xcodeproj/project.pbxproj` | regenerate via `xcodegen generate` after adding files under `app/Firefly/Sources/` or `app/FireflyKit/**`, and commit only the resulting diff — never hand-edit |
| `app/FireflyKit/Package.swift` | a new `.testTarget` (slice F needs one for `HardwareTests`) or a new dependency, appended to the relevant array — never reordering another slice's entry |

Landed in this PR, and depended on by name rather than owned by any one
slice below (S5) — the same way `MeshtasticClientProtocol.swift` and
`DeliveryState.swift` already were not slice-owned: `EventHub.swift`,
`CoreStore.swift`, `AppDependencies.swift`, `LocationProviding.swift`,
`HeadingProviding.swift`, `SettingsStoring.swift`.

### Slice A — BLE transport + the real Meshtastic client

**Owns:** `FireflyKit/Sources/FireflyMesh/BLE/*`,
`FireflyMesh/MeshtasticClient.swift`, `FireflyMesh/NodeDB.swift`,
`Tests/FireflyMeshTests/{ClientHandshakeTests,NodeDBTests}.swift`,
`app/FireflyHardwareTests/*` (the real BLE hardware tests — app-hosted,
`xcodebuild test`, see B1; the placeholder in this PR is replaced, not
moved).
**Depends on:** `MeshTransport`, `TransportEvent`,
`MeshtasticClientProtocol`, `EventHub`, `DeliveryState`, `MeshtasticBLE`,
`StreamFramer` (all existing), `MeshtasticProto`.
**Must add:** handshake tests driven by injected `FromRadio` bytes over
`LoopbackTransport` — both phases via the two firmware sentinel nonces
(`MeshtasticConfigNonce.onlyConfig` / `.onlyNodeDB`, see B2), a
`config_complete_id` that matches neither sentinel, and `rebooted`
mid-session; nodeDB tests for the three absence rules (loc source,
RSSI/hop path, precision bits); routing-ack → delivery-state tests
including the broadcast case and the no-ack window; real
`FireflyHardwareTests` BLE tests (discovery, pairing, both sentinel
phases reaching `.ready` against a real Heltec V3), gated
`FIREFLY_HARDWARE=1` the same way the placeholder is.
**Acceptance:** a `MeshtasticClient` reaches `.ready` from injected bytes
with no radio; every absence rule is asserted; `FromRadio.rebooted`
reissues both `want_config` phases from scratch (same two sentinels, not
fresh ones); the BLE transport compiles and runs on both platforms;
`FIREFLY_HARDWARE=1 xcodebuild test -only-testing:FireflyHardwareTests`
completes a real two-phase handshake against a Heltec V3.

### Slice B — the C-core bridge

**Owns:** `FireflyKit/Sources/FireflyModel/Bridge/*` (`CoreClock.swift`,
`CrewStore.swift`, `RadarBridge.swift`, `InboxBridge.swift`,
`FindBridge.swift`, `FireflyPacket.swift`, `CString+Swift.swift`),
`Tests/FireflyCoreTests/Bridge*.swift`. **Fills in** (does not move)
`CoreStore.swift`'s two `apply()` bodies, which this PR lands as
deliberate no-ops (S5) — routing them into `ff_crew_on_*` /
`ff_feed_set_send_status_by_outbox_id` through the `Bridge/*` types
above is this slice's job.
**Depends on:** `FireflyCore` only for `Bridge/*` itself. Must **not**
import `FireflyMesh` there — the bridge takes plain values, so it is
testable with no client at all. (`CoreStore.swift`, which this slice
edits but does not own, depends on `FireflyMesh` for the client
protocol and `EventHub`-backed streams it subscribes to — that is
`CoreStore`'s seam, not the bridge's.)
**Must add:** a lifetime test (allocate/free a `CrewStore` in a loop
under the address sanitiser without a leak or a use-after-free); tests
that `ff_radar_compute`'s output survives the round trip into Swift
values; a test that a `char[16]` name containing no terminator does not
over-read.
**Acceptance:** no `UnsafeMutablePointer` or imported C tuple appears in
any public API; the clock struct outlives every context that borrows it;
all eight M1 modules are bound; `CoreStore`'s `apply()` hooks are real
and `CoreStoreTests` (already pinning the multicast fan-out, S1) grows
tests that they land in `ff_crew_t`/`ff_feed_t`.

### Slice C — app shell, Connect, Settings/Diagnostics

**Owns:** `app/Firefly/Sources/RootView.swift` (and the destination
registry in it), `app/Firefly/Sources/Connect/*`,
`app/Firefly/Sources/Settings/*`,
`FireflyKit/Sources/FireflyModel/{SettingsStore,ChannelURL}.swift`
(`SettingsStore` is the real `UserDefaults`-backed `SettingsStoring`
implementation — the protocol and its `InMemorySettingsStore` mock are
already landed, S5),
`Tests/FireflyModelTests/ChannelURLTests.swift`.
**Shared file, declared:** `RootView.swift`'s destination switch has one
line per screen. Slices D and E each change exactly one of those lines.
Merge order C → D → E; nothing else in the file is touched. (See the
table above for the other three shared files.)
**Depends on:** `ConnectViewModel`, `MeshtasticClientProtocol`,
`FireflyTheme`, `AppDependencies`, `SettingsStoring` (all existing, S5) —
`SettingsStore` from the original file list is `SettingsStoring` plus
`InMemorySettingsStore`, already landed; this slice adds the
`UserDefaults`-backed real implementation, not the protocol.
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
`SignalPresentation`, `HeadingProviding` (protocol landed in this PR,
S5 — this slice consumes it, slice F supplies the real
`HeadingProvider`).
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
**Depends on:** slice B's `InboxBridge`, slice A's client via
`deliveryUpdates()` (a fresh `EventHub` subscription, S1 — this view
model's own, independent of `CoreStore`'s), `DeliveryState` (five
non-`NONE` cases; `FF_SEND_NONE` reads as `nil`, S3).
**Must add:** conversation-list and thread tests built from injected feed
items (unread counts, previews, direction); a delivery-state progression
test per state including the broadcast-never-DELIVERED case; a duplicate
`packet.id` echo test proving the sent row is not overwritten.
**Acceptance:** CREW plus one conversation per paired member, exactly as
`ff_inbox` builds them; every outbound row shows one of the five
non-`NONE` states and never an invented one.

### Slice F — location, heading, serial + TCP transports, hardware rig

**Owns:** `FireflyKit/Sources/FireflyMesh/{Serial,TCP}/*`,
`FireflyKit/Sources/FireflyModel/{LocationProvider,HeadingProvider}.swift`
(the real, CoreLocation-backed implementations of the
`LocationProviding`/`HeadingProviding` protocols landed in this PR, S5 —
not the protocols themselves), `FireflyKit/Tests/HardwareTests/*`
(**serial and TCP only** — BLE hardware tests are slice A's
`app/FireflyHardwareTests`, per B1, not this target), the hardware
section of `app/README.md`.
**Depends on:** `MeshTransport`, `StreamFramer`, slice A's client,
`LocationProviding`, `HeadingProviding` (protocols, existing, S5).
**Must add:** serial framing tests against recorded bytes (no port
needed); a TCP transport test against a local socket; provider tests
that "permission denied" and "no fix" produce *absence*, never a
coordinate; and the serial/TCP hardware suite itself, which must skip
cleanly with no board — a new `HardwareTests` `.testTarget` in
`FireflyKit/Package.swift` (a shared file, see the table above).
**Acceptance:** `swift test` stays green on a machine with no radio and
no serial device; `FIREFLY_HARDWARE=1 swift test --filter Hardware`
completes a serial handshake, a DM with a real ACK, and a
phone-position push verified by `meshtastic --info`, against a Heltec
V3.

## Decisions from the owner (formerly open questions)

All five settled; recorded here so the reasoning is not lost, the same
way "Decisions already made" is at the top of this spec.

1. **Bundle id case: `com.jakeholland.firefly`, lowercase.** Confirmed
   as specified, not the archived project's `com.jakeholland.Firefly` —
   an App Store identity change is a one-way door, so this is settled
   now rather than revisited after M1 ships.
2. **Signing: git-ignored `app/Config/Local.xcconfig`, committed project
   stays unsigned.** `app/Config/Firefly.xcconfig` (committed, wired into
   the `Firefly` target) `#include?`s a personal, git-ignored
   `Local.xcconfig` carrying `DEVELOPMENT_TEAM` — the `?` makes the
   include optional, so its absence changes nothing for a clean
   checkout. `app/Config/Local.xcconfig.example` is the committed
   template; copy it, fill in a team id, never commit the copy. See
   `app/README.md`, "On-device signing".
3. **Firefly 1's bench position: `CLIENT`, not `CLIENT_MUTE`, asserted
   explicitly.** `47.708135, -122.2820993`, altitude 40, set with
   `--setlat`/`--setlon`/`--setalt` (never the fixed-position flag
   alone), configured 2026-09-10 over its serial console. `CLIENT`
   rather than `CLIENT_MUTE` is deliberate: the point of the bench pair
   is exercising a real mesh with the app, and a node that acks and
   relays like a real friend node is more useful for that than a mute
   landmark stand-in would be. `docs/hardware/heltec-v3.md`'s bench
   table is updated to **Done** with this — see S7. Both boards remain
   flagged REVERT BEFORE THE FESTIVAL.
4. **Minimum firmware floor: 2.7.26.** What is on both bench boards
   today; the handshake's version check (`Meshtastic client` >
   "Handshake", step 5) declares this the floor rather than trying to
   support older releases nobody here is running.
5. **Channel PSK handling: the app never mints one.** M1 imports a
   channel from a QR/URL and keeps the PSK in the Keychain; it does not
   gain a "generate a new channel" action in M1–M3. Provisioning a new
   puck's channel stays CLI territory. If that changes later it is a
   deliberate scope expansion, not an oversight to paper over here.
