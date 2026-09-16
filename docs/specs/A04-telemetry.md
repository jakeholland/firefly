# A04 · field telemetry — offline-first connectivity log, Crashlytics + Firestore, export

> **A-series.** Same rules as A01/A02/A03: acceptance criteria become
> test names (`A04_AC3_…`), unknowns are represented rather than papered
> over, and anything cut is cut out loud.

## Why this spec exists

Lost Lands (Sep 18–20 2026) is the first real field test, and it runs on
poor cell coverage. When something goes wrong out there — a radio that
never reconnects, a crew join that silently fails, a battery that dies
faster than expected — the only record of what actually happened is
whatever this app wrote down *at the time*, on the phone, before anyone
had signal to explain it to a server. **Offline-first is not a nice-to-
have for this feature; it is the whole feature.** A telemetry pipeline
that only works when Firestore is reachable would produce a blank record
for exactly the failures worth understanding.

So the design has two halves, in priority order:

1. **A durable local log.** Every event this app records is appended to
   a JSON-lines file in Application Support before anything else happens
   to it — no sink, no network call, no Firebase configuration state can
   prevent that write. `docs/specs/A04-telemetry.md` (this file) exists
   so the exact shape of that log — names, attributes, units — is
   written down once, and every emission call site is checked against it
   (`TelemetryEventCatalogTests`, the wiring-guard tests).
2. **A best-effort upload**, opt-out (Settings > Diagnostics > "Share
   diagnostics"), for the case where the phone DOES have signal:
   Crashlytics breadcrumbs for whatever a real crash report should carry
   alongside it, and batched Firestore writes so the owner can watch a
   live field test without waiting for someone to hand over a phone.

## Privacy rules (read this before adding a new event)

These bind every event this file names, and every one a future PR adds:

- **Never a node id, raw.** A Meshtastic node number identifies a real
  person's radio. `crew.member.seen`/`crew.member.lost` carry
  `id_hash` — `TelemetryHash.nodeID(_:)`, SHA-256 truncated to 16 hex
  characters, deterministic and salt-free (the same node hashes the
  same way across the whole recording, which is what makes "this
  id_hash keeps dropping off" answerable at all) — never the raw
  `UInt32`.
- **Never the crew code.** Not hashed, not truncated — absent. A code
  is a shared secret this app already treats as sensitive everywhere
  else; telemetry does not get a special exemption.
- **Never message text.** Nothing in this event catalogue has a `text`,
  `body`, or `message` attribute, for a FLARE, a crew message or a DM.
- **Never raw coordinates.** `gps.fix` carries a coarse
  `accuracy_bucket` (four buckets — see that event's own row below) and
  nothing else about position. There is no event, anywhere in this
  catalogue, with a latitude or longitude.
- **Enforced at two levels**, deliberately redundant:
  `TelemetryValue` is closed over string/int/double/bool (never a
  free-form payload a future call site could smuggle a coordinate pair
  or a message body through), and `TelemetryAttributeAllowlist.strip(_:)`
  removes a fixed set of forbidden KEYS — `lat`/`lon`/`coordinate`/
  `position`/`text`/`body`/`message`/`code`/`node_id`/`node_num`/`from`/
  `to` and near-spellings of each — from every event before it reaches
  disk or a sink. A call site that violates this loses the offending
  key, never the whole event; `TelemetryAttributeAllowlistTests` pins
  the exact list.

## 1. Types (`FireflyKit/Sources/FireflyTelemetry`)

A fifth SwiftPM target, `FireflyTelemetry` — `Foundation`/`CryptoKit`
only, no dependency on `FireflyCore`, sitting below `FireflyMesh` so both
`FireflyMesh` (`BLETransport`/`MeshtasticClient`) and `FireflyModel`
(`AppGraph`/`CrewController`/`LocationProvider`) can depend on it. See
`FireflyKit/Package.swift`'s own header comment for why this could not
simply be folded into `FireflyModel`.

- **`TelemetryEvent`** — `name`, `timestamp`, `seq`, `sessionID`,
  `attributes: [String: TelemetryValue]`. A call site supplies only
  `name`/`attributes`/(optionally) `timestamp`; `seq`/`sessionID` are
  stamped exactly once, by whichever `TelemetryRecording` actually
  records the event (`TelemetryEvent.stamped(seq:sessionID:)`).
- **`TelemetryValue`** — `.string`/`.int`/`.double`/`.bool`. Closed on
  purpose (see Privacy rules above).
- **`TelemetryRecording`** — the seam every call site holds:
  `func record(_ event: TelemetryEvent) async`. `NoopTelemetryRecorder`
  is the harmless default every pre-existing constructor got appended,
  optional-with-default — no call site that predates A04 had to change.
- **`TelemetryRecorder`** — the durable, offline-first heart of this
  feature. An `actor` appending one JSON-lines line per event to
  `<Application Support>/Firefly/Telemetry/telemetry.jsonl`, rotating to
  `telemetry.1.jsonl`…`telemetry.4.jsonl` (5 files total, oldest
  deleted) the moment appending a line would push the current file over
  ~5 MB. Survives a relaunch mid-file (never truncates an existing
  current file — the first `record(_:)` after a fresh launch picks up
  wherever the previous process instance left off) —
  `TelemetryRecorderDurabilityTests` pins this by constructing two
  separate instances over the same directory. Fans every stamped event
  out to zero or more `TelemetrySink`s AFTER the local append succeeds,
  never instead of it.
- **`InMemoryTelemetryRecorder`** — `.stub()`'s recorder, and every unit
  test's own. Same stamping/allowlist rules, held in memory only.
- **`TelemetrySink`** — `func send(_ event: TelemetryEvent) async` +
  `func flushOnBackground() async` (defaulted to a no-op via a protocol
  extension). `NoopSink` is the harmless default; `FirebaseSink` (app
  target, §5) is the only other conformer this PR ships.
- **`TelemetryBatchPolicy`** — the pure "should I flush now" decision
  behind Firestore's own batching rule (§5): `pendingCount >=
  maxEventCount` (default 50), or `now - oldestPendingEventAt >=
  maxInterval` (default 60s), or `isBackgrounding`. Deterministic,
  tested with an injected clock (`TelemetryBatchPolicyTests`) with no
  Firebase SDK involved at all.
- **`TelemetryHash`** — `nodeID(_:)`, described under Privacy rules.
- **`TelemetryAttributeAllowlist`** — described under Privacy rules.

## 2. Event catalogue (`TelemetryEventCatalog.swift`)

Every name/key below is a constant in `TelemetryEventName`/
`TelemetryAttributeKey`; `TelemetryEventCatalogTests` pins the exact
spelling of every one of them against this table, so the doc and the
code cannot quietly drift apart.

### BLE / connectivity

| event | attributes | where it fires |
|---|---|---|
| `ble.scan.start` | — | `BLETransport.scan()` |
| `ble.scan.stop` | — | `BLETransport.stopScanning()` |
| `ble.discovered` | `name` (string), `rssi` (int) | `BLETransport`'s `didDiscover` handling |
| `ble.connect.attempt` | `trigger` (`launch`\|`auto`\|`manual`\|`ladder`\|`restore`), `attempt` (int) | `ConnectViewModel.connect()` (manual), `AppGraph.autoConnectToLastKnownPeripheral()` (auto), `MeshtasticClient.handleTransportReconnected()` (auto, per retry attempt) |
| `ble.connected` | — | `MeshtasticClient.connect()`, right after `transport.connect()` returns |
| `ble.handshake.phase` | `phase` (`config`\|`nodeDB`), `ms` (int) | `MeshtasticClient.requestConfig(nonce:timeout:)`, one event per want_config phase |
| `ble.ready` | `ms_since_attempt` (int) | `MeshtasticClient.publish(.ready)` |
| `ble.disconnected` | `reason` (string), `session_s` (double), `expected` (bool) | `MeshtasticClient.publish(.disconnected)` — both the user-initiated `disconnect()` path (`expected: true`, `reason: "user"`) and an unasked-for transport loss (`expected: false`, `reason:` the transport's own reason string) |
| `ble.ladder.scheduled` | `delay_s` (double), `step` (int) | `BLETransport.armReconnectFallback` |
| `ble.ladder.fired` | `step` (int) | `BLETransport`'s ladder scan-window opening |
| `ble.restore` | `action` (`adoptConnected`\|`keepPendingConnect`\|`reconnect`\|`none`) | `BLETransport.handleWillRestoreState(peripherals:)` |
| `ble.power` | `state` (`unknown`\|`resetting`\|`unsupported`\|`unauthorized`\|`poweredOff`\|`poweredOn`) | `BLETransport.handleCentralStateUpdate(_:)` |

### App lifecycle

| event | attributes | where it fires |
|---|---|---|
| `app.launch` | `build` (string), `device` (string), `os` (string) | `AppGraph.handleDidFinishLaunching(isForegrounded:)` |
| `app.foreground` | — | `AppGraph.handleScenePhaseChange(.foreground)` |
| `app.background` | — | `AppGraph.handleScenePhaseChange(.background)` |
| `app.terminate` | — | **Known gap — see §6.** |

### Notifications

| event | attributes | where it fires |
|---|---|---|
| `notif.posted` | `kind` (`flare`\|`rally`\|`message` — `NotificationCategory`'s own constants) | `AppGraph.post(_ event: NotificationEvent)`, the one place this app calls `NotificationSending.post(_:)` |
| `notif.tapped` | `kind` (string, same vocabulary) | `NotificationTapRouter.userNotificationCenter(_:didReceive:withCompletionHandler:)` |
| `notif.authorization` | `status` (`notDetermined`\|`denied`\|`authorized`\|`provisional`) | `AppGraph.requestNotificationAuthorizationIfNeeded()`, read back right after the prompt |

### Crew / admin

| event | attributes | where it fires |
|---|---|---|
| `admin.write` | `kind` (string — `"crew_channel"` for every write this PR wires), `ms` (int), `outcome` (`applied`\|`failed`) | `CrewController.confirmApply()`, timed around the ONE call site that calls `ChannelImportViewModel.confirmApply()` |
| `crew.join` | `outcome` (`joined`\|`failed`\|`unverified`), `ms` (int) | `CrewController.confirmApply()`, when `pending` is `.join` |
| `crew.leave` | `outcome` (`left`\|`failed`), `ms` (int) | `CrewController.leaveCrew()` |
| `crew.start` | `outcome` (`joined`\|`failed`\|`unverified`), `ms` (int) | `CrewController.confirmApply()`, when `pending` is `.start` |
| `crew.member.seen` | `id_hash` (string), `age_s` (double) | **Known gap — see §6.** |
| `crew.member.lost` | `id_hash` (string), `age_s` (double) | **Known gap — see §6.** |

### Radio

| event | attributes | where it fires |
|---|---|---|
| `radio.snapshot` | `node_count` (int); `batt_pct`/`rssi_last` omitted, never fabricated — see §6 | `AppGraph.observeRadioSnapshot()`, every 5 minutes while `currentLinkState == .ready` |

### Position

| event | attributes | where it fires |
|---|---|---|
| `gps.fix` | `accuracy_bucket` (`fine`\|`medium`\|`coarse`\|`very_coarse`\|`unknown` — buckets ONLY, see Privacy rules), `source` (string, currently always `"core-location"`) | `PhoneGPSUplink.handle(fix:)`, for every fix this uplink sees (whether or not it is actually sent) |
| `gps.uplink` | `outcome` (`sent`\|`failed`) | `PhoneGPSUplink.handle(fix:)`, after `sink.sendPosition(_:to:)` |

### Errors

| event | attributes | where it fires |
|---|---|---|
| `error` | `domain` (string), `code` (string), `where` (string) | Reserved for future connectivity-path error wiring — see §6. |

## 3. Wiring discipline

Every call site above is additive at an EXISTING `Self.log(...)`/
`BLETransport.log(...)`-style stderr log point named in this app's own
history (A01/A02/A03's own "wired at the existing log points" rule) —
none of those log lines were removed or changed; telemetry is a second,
structured channel alongside them, not a replacement. Every
`TelemetryRecording`/`TelemetryEvent`-typed parameter this PR adds is
appended LAST to its constructor, defaulted to the harmless
`NoopTelemetryRecorder()`/`NoopSink()`, so every pre-existing call site
(every test in this repo) keeps compiling and behaving exactly as it did
before this PR, with zero telemetry recorded, unless it is explicitly
handed a real recorder.

## 4. Recorder, sinks, and `AppDependencies`

- `AppDependencies.telemetry: any TelemetryRecording` — `.stub()` gets
  `InMemoryTelemetryRecorder()` (records to memory, nothing touches
  disk, never Firebase); `.live()` gets ONE real `TelemetryRecorder`,
  held by `AppDependencies` itself AND by `BLETransport` AND by
  `MeshtasticClient` — the same "one instance, several holders" rule
  `AppDependencies.live()`'s own `transport` follows, for the identical
  reason (two recorders would mean two `seq` counters and two session
  ids disagreeing about the same process).
- The recorder's directory is `<Application Support>/Firefly/Telemetry`
  — a sibling of `HistoryStore`'s own `Firefly` subdirectory, never
  Documents/tmp (exposed to iCloud backup and Files.app; telemetry is
  diagnostic, not user data).
- `FirebaseSink` (app target, §5) is attached to this SAME recorder
  AFTER `AppDependencies.live()` returns, through `TelemetrySinkAttaching`
  — `FireflyModel` cannot depend on the app target's Firebase wiring, so
  the composition root (`AppRuntimeBundle.build`, via
  `FirebaseTelemetryBootstrap.attachSink(to:buildString:deviceString:)`)
  does the cast-and-attach instead: `dependencies.telemetry as? any
  TelemetrySinkAttaching`. A no-op for `.stub()`/demo (whose telemetry is
  `InMemoryTelemetryRecorder`, which does not conform to
  `TelemetrySinkAttaching` at all) and for any build with no
  `GoogleService-Info.plist`.

## 5. Settings: Share diagnostics / Export diagnostics

Both rows live on the Diagnostics screen (Settings > DIAGNOSTICS), not
the main Settings list — Diagnostics is already where this app explains
what it does and does not know about itself.

- **"Share diagnostics"** — a toggle,
  `SettingsStore.shareDiagnosticsEnabled` /
  `FireflyExtraSettingsKey.shareDiagnosticsEnabled`. Same three-state
  read `backgroundConnectEnabled` already uses: nothing persisted ->
  `SettingsStore.defaultShareDiagnosticsEnabled()` (`true` in `#if
  DEBUG`, or a TestFlight build — `Bundle.appStoreReceiptURL`'s
  `"sandboxReceipt"` signal — `false` on a plain App Store install); an
  explicit write -> exactly that value, forever. Copy: *"Sends
  connection diagnostics when the phone has signal. Never your messages
  or exact location."* — read fresh by `FirebaseSink.isSharingEnabled()`
  on every send/flush, so flipping it mid-festival takes effect on the
  very next event, not the next launch. **OFF never touches the local
  JSONL** — `TelemetryRecorder.record(_:)` always appends locally first,
  unconditionally; OFF only ever stops the NEXT upload from leaving the
  phone.
- **"Export diagnostics"** — a button that fetches
  `TelemetryRecorder.exportFiles()` (oldest file first, current file
  last) and hands them to a `ShareLink`/share sheet — the phone's own
  files, un-uploaded, for a Bailey with no signal at all to hand a
  laptop over USB/AirDrop instead. Hidden when
  `AppDependencies.telemetry` is not a `TelemetryExporting` (`.stub()`).

## 6. Known gaps (not finished in this PR — reported, not hidden)

Per this repo's own rule ("anything cut is cut out loud"):

- **`app.terminate`** — iOS gives no reliable app-terminate hook at all
  (this is a documented platform limitation, not an oversight); `
  app.background` already covers the practical "the app stopped running"
  signal a diagnostics reader needs. A `UIApplication
  .willTerminateNotification`/`NSApplication.willTerminateNotification`
  observer could add a best-effort version of this event in a follow-up,
  but it would only ever fire for a small minority of terminations (a
  user swipe-kill almost never runs it) and is left undone here rather
  than shipped as a false sense of coverage.
- **`crew.member.seen`/`crew.member.lost`** — the presence-tracking logic
  these two events would hook into (`CrewMembershipEngine`/the "heard"
  vocabulary `CrewCopy.swift` renders) lives deep enough in the crew
  roster machinery that wiring it correctly — without double-counting a
  member across a reconnect, and without recording a `seen` for every
  single node-info replay rather than a genuine new sighting — needs its
  own pass. Not wired in this PR.
- **`radio.snapshot`'s `batt_pct`/`rssi_last`** — `MeshtasticClientProtocol`
  does not currently surface this device's own connected-radio battery
  level or last RSSI anywhere `AppGraph` can read. Rather than fabricate
  a number, this PR ships `radio.snapshot` with `node_count` only (a real
  tally of distinct node numbers seen via the existing ordered
  `inboundPackets()` pipeline — deliberately NOT a second `nodeUpdates()`
  subscription, which would reopen the 2026-09-14 bench race
  `testStartSubscribesEachClientStreamExactlyOnceAndIsIdempotent` exists
  to prevent). Both attributes are reserved in the catalogue for when
  that data becomes available.
- **`error {domain, code, where}`** — the catalogue reserves this name
  and its three attributes, but no call site emits it yet in this PR.
  The connectivity paths that would want it (a caught `AdminWriteError`,
  a handshake timeout past its retry budget) already log to stderr at
  today's existing log points; routing those same catches through
  `telemetry.record(TelemetryEvent(name: .error, ...))` is
  straightforward follow-up work, not a design gap.

## 7. Tests

- **Recorder durability/rotation** — `TelemetryRecorderDurabilityTests`
  (temp directory; rotation at the byte threshold; survives a relaunch
  mid-file; `notifyBackground()` fans out to every attached sink).
- **Catalogue names pinned** — `TelemetryEventCatalogTests` (every name
  follows the dotted-domain convention; every BLE/app/notif/crew/radio/
  position/error name and `TelemetryTrigger` case spelled exactly as
  this file's own tables above).
- **Hashing** — `TelemetryHashTests` (deterministic, lowercase hex,
  fixed length, not the raw node number, two different nodes hash
  differently).
- **Batching/flush with an injected clock** — `TelemetryBatchPolicyTests`
  (no flush on an empty buffer; flush at the count threshold; flush at
  the time threshold with few events; backgrounding forces a flush
  regardless of thresholds, unless nothing is pending).
- **Attribute allowlist guard** — `TelemetryAttributeAllowlistTests`
  (forbidden keys stripped case-insensitively; `TelemetryRecorder`
  actually calls `strip(_:)` before persisting; the predicate is right
  independent of the wiring).
- **Settings toggle persistence** — `SettingsStoreTests`
  (`testShareDiagnosticsDefaultsTrueInDebugAndRoundTrips`,
  `testShareDiagnosticsExplicitFalseSurvivesAFreshInstance`,
  `testShareDiagnosticsInMemoryStoreAgreesAboutTheDefault`,
  `testDefaultShareDiagnosticsIsTestFlightReceiptOutsideDebug`).
- **Wiring guards** — source-level: every emission call site above
  references a `TelemetryEventName`/`TelemetryAttributeKey` constant,
  never a literal string, which is what makes
  `TelemetryEventCatalogTests` an effective guard against a call site
  drifting from this table (a misspelled literal would not match the
  catalogue constant at all, so it would not compile).

## 8. Firebase (app target only)

See `app/README.md`, "Field telemetry (A04)" for the operational side —
how to read the data, what the owner has already set up, and the
project's own setup steps. See `firebase/firestore.rules` (+
`firebase/README.md`) for the security rules and how to deploy them.
`FirebaseSink.swift`/`FirebaseTelemetryBootstrap.swift`
(`app/Firefly/Sources/Telemetry/`) are the only files in this PR that
import a Firebase module, both behind `#if canImport(FirebaseCore)`.
