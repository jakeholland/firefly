//
//  AppDependencies.swift — the DI composition root (docs/specs/
//  A01-companion-app.md, "Dependency injection", and S5).
//
//  Landed here as a real, working seam so slice C (which depends on it)
//  does not have to invent it, and so `FireflyApp.swift`'s one stored
//  property has somewhere honest to come from. Constructor injection,
//  one composition root, no service locator and no singletons — exactly
//  as specified.
//
import FireflyMesh
import FireflyTelemetry
import Foundation

public struct AppDependencies: Sendable {
    public var client: any MeshtasticClientProtocol
    public var location: any LocationProviding
    public var heading: any HeadingProviding
    /// `FireflyExtraSettingsStoring`, not bare `SettingsStoring`: the
    /// Settings screen needs the six shared keys AND its own four from
    /// ONE instance (that protocol's own doc comment), which is what
    /// closes slice C's INTEGRATION TASK — before this, Settings held a
    /// private `SettingsStore()` and agreed with everything else only
    /// by `UserDefaults.standard` coincidence.
    public var store: any FireflyExtraSettingsStoring
    /// The BLE node picker's scan seam (`FireflyMesh.NodeScanning`) —
    /// `nil` whenever there is no radio to scan with (the stub stack,
    /// the iOS Simulator), which the Connect screen renders as an empty
    /// picker rather than a spinner that will never resolve.
    ///
    /// Optional, and appended after the four original fields, so every
    /// existing `AppDependencies(...)` call site keeps compiling
    /// unchanged (S5 is landed, frozen infra other slices already
    /// depend on by its current shape).
    public var scanner: (any NodeScanning)?
    /// M2's persisted crew-pairing seam (`CrewPairingStore.swift`) —
    /// appended after `scanner`, same "every existing call site keeps
    /// compiling" reasoning, with a default so `.stub()` (which does not
    /// specify it below) gets the same "same shape, nothing persisted"
    /// in-memory stand-in `.stub()`'s `store` already gets.
    /// `AppGraph.init` restores this into `core.crew` before anything
    /// can observe a client (`CrewPairingRestorer`'s own doc comment).
    public var crewPairingStore: any CrewPairingStoring
    /// A02 slice C's per-crew-code local state: the hide list (§4.5) and
    /// the join log (§2.3). Appended after `crewPairingStore` under the
    /// same append-only convention, with the same in-memory default, so
    /// every existing `AppDependencies(...)` call site keeps compiling.
    public var crewLocalStateStore: any CrewLocalStateStoring
    /// A04 (docs/specs/A04-telemetry.md) — the field-test telemetry
    /// seam, appended after `crewLocalStateStore` under the same
    /// append-only convention as every field above it: every existing
    /// `AppDependencies(...)` call site keeps compiling unchanged.
    /// `.stub()` gets `InMemoryTelemetryRecorder()` (records to memory,
    /// nothing touches disk); `.live()` gets a real, durable
    /// `TelemetryRecorder`. `Telemetry.shared`-free by design — this is
    /// the ONE place a call site gets one, exactly like `client`/
    /// `location`/`store` above it.
    public var telemetry: any TelemetryRecording

    public init(client: any MeshtasticClientProtocol, location: any LocationProviding,
                heading: any HeadingProviding, store: any FireflyExtraSettingsStoring,
                scanner: (any NodeScanning)? = nil,
                crewPairingStore: any CrewPairingStoring = InMemoryCrewPairingStore(),
                crewLocalStateStore: any CrewLocalStateStoring = InMemoryCrewLocalStateStore(),
                telemetry: any TelemetryRecording = InMemoryTelemetryRecorder()) {
        self.client = client
        self.location = location
        self.heading = heading
        self.store = store
        self.scanner = scanner
        self.crewPairingStore = crewPairingStore
        self.crewLocalStateStore = crewLocalStateStore
        self.telemetry = telemetry
    }

    /// The stub stack: `StubMeshtasticClient` over `LoopbackTransport`,
    /// plus location/heading providers that report **unavailable**, not
    /// fake coordinates — "the stub client's defining property is what
    /// it *refuses* to do". Used by every unit test that wants no
    /// radio, and by the iOS Simulator automatically via `.current()`.
    public static func stub() -> AppDependencies {
        AppDependencies(
            client: StubMeshtasticClient(),
            location: UnavailableLocationProvider(),
            heading: NoHeadingProvider(),
            store: InMemorySettingsStore())
    }

    /// The milestone-1 stack, now real end to end — no stand-in left in
    /// it: slice A's `MeshtasticClient` over `BLETransport`, slice F's
    /// CoreLocation-backed `LocationProvider` and `HeadingProvider`, and
    /// slice C's `UserDefaults`-backed `SettingsStore`.
    ///
    /// `HeadingProvider` is written without a `#if os(...)` on purpose:
    /// on macOS that name IS `NoHeadingProvider` (a typealias in
    /// `HeadingProvider.swift`), because a Mac has no magnetometer and
    /// "permanently NOHDG" is the correct answer there, not a gap. The
    /// platform split lives in that one file so every composition root
    /// can say the same thing.
    ///
    /// Constructing `BLETransport()` here does NOT construct a
    /// `CBCentralManager` — that only happens lazily inside `connect()`/
    /// `scan()` — so `.live()` stays safe to call from anywhere
    /// (including a bare `swift test` process) right up until something
    /// actually calls `connect()` or `scan()` on it. See
    /// `BLETransport.swift`'s file-level doc comment and B1.
    ///
    /// Constructing `LocationProvider()` DOES construct a
    /// `CLLocationManager`, which is harmless: it requests nothing and
    /// starts no updates until `requestWhenInUseAuthorization()` /
    /// `startUpdatingLocation()` is called, and no permission dialog
    /// appears merely from existing.
    public static func live() -> AppDependencies {
        let store = SettingsStore()
        // A04 — ONE recorder, held by `BLETransport`, `MeshtasticClient`
        // AND `AppDependencies.telemetry` itself: the same "one instance,
        // several holders" rule `transport` (below) follows for the
        // identical reason — two recorders would mean two `seq`
        // counters and two session ids disagreeing about the same
        // process. `Self.telemetryDirectory()` is Application Support,
        // never Documents/tmp (that directory is exposed to iCloud
        // backup and Files.app; telemetry is diagnostic, not user data,
        // and does not belong there). Firebase sinks (app-target-only,
        // behind `#if canImport(FirebaseCore)`) are attached AFTER this
        // returns, via `TelemetrySinkAttaching` — `FireflyModel` cannot
        // depend on the app target's Firebase wiring.
        let telemetry = TelemetryRecorder(directory: Self.telemetryDirectory())
        // M2 — "remembering the last connected peripheral identifier"
        // (docs/specs/A01-companion-app.md): loaded once here, at
        // construction, and kept current afterward by the two closures
        // below. `FireflyMesh` cannot depend on `FireflyModel`
        // (`SettingsStoring` lives here; `Package.swift`'s dependency
        // graph runs the other way), so `BLETransport` takes plain
        // closures rather than a settings reference of its own — this is
        // the one place that seam gets wired to the real store.
        let lastPeripheralID = store.string(.lastPeripheralID).flatMap(UUID.init(uuidString:))
        let bondedPeripheralIDs = Self.parsePeripheralIDs(store.string(.bondedPeripheralIDs))
        // ONE transport instance, held twice on purpose: the client
        // connects through it and the node picker scans through it.
        // Two `BLETransport`s would mean two `CBCentralManager`s, two
        // scans, and a picker whose selection the connecting transport
        // never sees.
        let transport = BLETransport(
            preferredPeripheralID: lastPeripheralID,
            bondedPeripheralIDs: bondedPeripheralIDs,
            onPreferredPeripheralChanged: { id in store.setString(id.uuidString, .lastPeripheralID) },
            onBonded: { id in
                var ids = Self.parsePeripheralIDs(store.string(.bondedPeripheralIDs))
                ids.insert(id)
                store.setString(ids.map(\.uuidString).joined(separator: ","), .bondedPeripheralIDs)
            },
            telemetry: telemetry)
        return AppDependencies(
            client: MeshtasticClient(transport: transport, telemetry: telemetry),
            location: LocationProvider(),
            heading: HeadingProvider(),
            store: store,
            scanner: transport,
            crewPairingStore: CrewPairingStore(),
            crewLocalStateStore: CrewLocalStateStore(),
            telemetry: telemetry)
    }

    /// Application Support/Firefly/Telemetry — sibling to `HistoryStore
    /// .storeURL`'s Application Support/Firefly (that method's own doc
    /// comment), its own subdirectory so `TelemetryRecorder`'s rotated
    /// `.jsonl` files never mix with `History.sqlite`.
    private static func telemetryDirectory() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return base.appending(path: "Firefly/Telemetry", directoryHint: .isDirectory)
    }

    /// `SettingsKey.bondedPeripheralIDs`'s on-disk shape: a comma-joined
    /// list of UUID strings — `SettingsStoring` has no array/set
    /// primitive of its own, and this preference never needs to be more
    /// than that. Malformed entries (there should never be any) are
    /// dropped rather than failing the whole read.
    private static func parsePeripheralIDs(_ raw: String?) -> Set<UUID> {
        guard let raw, !raw.isEmpty else { return [] }
        return Set(raw.split(separator: ",").compactMap { UUID(uuidString: String($0)) })
    }

    /// The iOS Simulator has no Bluetooth at all — `CBCentralManager` is
    /// a dead end there (the archived app's
    /// `DependencyContainer.simulatorContainer()` exists for the same
    /// reason) — so it always gets `.stub()` or, when `-FireflyDemo`/
    /// `FIREFLY_DEMO=1` asked for it (`DemoLaunch.isRequested()`),
    /// `.demo()`. Regardless of which build configuration asked for
    /// `.live()`.
    ///
    /// The `DemoLaunch` check lives INSIDE `#if targetEnvironment
    /// (simulator)`, never outside it: a real device must never be
    /// turned into a fictional festival by a stray launch argument, so
    /// this is the one call site that makes "live mode never constructs
    /// the demo client" true by construction, not by convention —
    /// `DemoRunnerTests.testLiveDependenciesNeverConstructTheDemoClient`
    /// pins it.
    public static func current() -> AppDependencies {
        #if targetEnvironment(simulator)
        return DemoLaunch.isRequested() ? .demo() : .nonDemo()
        #else
        return .nonDemo()
        #endif
    }

    /// `.current()`'s non-demo half, on its own: the honest stack for
    /// this environment when nothing is asking for the demo world — the
    /// iOS Simulator's `.stub()`, `.live()` everywhere else. `.current()`
    /// itself is `DemoLaunch.isRequested() ? .demo() : .nonDemo()` inside
    /// its own simulator gate; this exists as a separate call so a
    /// runtime "leave the demo" switch (`AppRuntimeBundle.build`, the
    /// app target) can ask for the SAME answer without re-consulting
    /// `DemoLaunch` — a launch argument describes how THIS PROCESS
    /// started, not what a person just tapped mid-session, and a leave
    /// action must win regardless of what the process was launched with.
    public static func nonDemo() -> AppDependencies {
        #if targetEnvironment(simulator)
        return .stub()
        #else
        return .live()
        #endif
    }

    /// Firefly Fields (`docs/specs/S20-demo-mode.md`, `DemoWorld`): the
    /// same client/location/heading seam every other stack uses, wired
    /// to a `DemoMeshtasticClient` that plays a scripted timeline
    /// through the real `CoreStore`/`ff_crew`/`ff_feed` bridges instead
    /// of a real radio. See `DemoBundle` when the caller also needs the
    /// CONCRETE demo types (`DemoRunner` does, to drive the timeline
    /// and to toggle the phone's fix for the no-GPS signal screenshot);
    /// this plain `AppDependencies` is enough for anything that only
    /// needs the protocol-shaped seam, same as `.stub()`/`.live()`.
    public static func demo() -> AppDependencies { demoBundle().dependencies }

    /// `.demo()`'s own dependencies, plus the concrete demo instances
    /// `DemoRunner` needs a handle on. Building both from ONE call
    /// (rather than `.demo()` internally constructing one set and a
    /// caller building a second) is what keeps `FireflyApp`'s demo
    /// branch from ever running two independent demo worlds that
    /// disagree with each other.
    public static func demoBundle(world: DemoWorld = .fireflyFields()) -> DemoBundle {
        let client = DemoMeshtasticClient(myNodeNum: world.myNodeNum, nodes: world.nodeDB)
        let location = DemoLocationProvider(initialFix: nil) // DemoRunner sets it once observers are live
        let heading = DemoHeadingProvider(initialHeading: nil)
        let dependencies = AppDependencies(client: client, location: location, heading: heading,
                                            store: InMemorySettingsStore())
        return DemoBundle(dependencies: dependencies, client: client, location: location, heading: heading,
                           world: world)
    }
}

/// `AppDependencies.demoBundle()`'s return shape — the protocol-typed
/// `dependencies` for `AppGraph`, plus the concrete demo instances only
/// `DemoRunner` (FireflyApp's demo composition) ever touches directly.
public struct DemoBundle: Sendable {
    public let dependencies: AppDependencies
    public let client: DemoMeshtasticClient
    public let location: DemoLocationProvider
    public let heading: DemoHeadingProvider
    public let world: DemoWorld
}
