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

    public init(client: any MeshtasticClientProtocol, location: any LocationProviding,
                heading: any HeadingProviding, store: any FireflyExtraSettingsStoring,
                scanner: (any NodeScanning)? = nil) {
        self.client = client
        self.location = location
        self.heading = heading
        self.store = store
        self.scanner = scanner
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
        // ONE transport instance, held twice on purpose: the client
        // connects through it and the node picker scans through it.
        // Two `BLETransport`s would mean two `CBCentralManager`s, two
        // scans, and a picker whose selection the connecting transport
        // never sees.
        let transport = BLETransport()
        return AppDependencies(
            client: MeshtasticClient(transport: transport),
            location: LocationProvider(),
            heading: HeadingProvider(),
            store: SettingsStore(),
            scanner: transport)
    }

    /// The iOS Simulator has no Bluetooth at all — `CBCentralManager` is
    /// a dead end there (the archived app's
    /// `DependencyContainer.simulatorContainer()` exists for the same
    /// reason) — so it always gets `.stub()`, regardless of which build
    /// configuration asked for `.live()`.
    public static func current() -> AppDependencies {
        #if targetEnvironment(simulator)
        return .stub()
        #else
        return .live()
        #endif
    }
}
