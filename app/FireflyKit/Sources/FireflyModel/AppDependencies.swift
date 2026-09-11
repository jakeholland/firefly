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
    public var store: any SettingsStoring

    public init(client: any MeshtasticClientProtocol, location: any LocationProviding,
                heading: any HeadingProviding, store: any SettingsStoring) {
        self.client = client
        self.location = location
        self.heading = heading
        self.store = store
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

    /// Milestone-1 stack, real BLE half: slice A's `MeshtasticClient`
    /// over `BLETransport` replaces `StubMeshtasticClient` here. Slice
    /// F's `LocationProvider`/`HeadingProvider` still owe the
    /// `location:`/`heading:` fields below — untouched by this edit, on
    /// purpose, so landing one slice's half of `.live()` does not block
    /// or collide with the other's.
    ///
    /// Constructing `BLETransport()` here does NOT construct a
    /// `CBCentralManager` — that only happens lazily inside `connect()`/
    /// `scan()` — so `.live()` stays safe to call from anywhere
    /// (including a bare `swift test` process) right up until something
    /// actually calls `connect()` on the resulting client. See
    /// `BLETransport.swift`'s file-level doc comment and B1.
    public static func live() -> AppDependencies {
        AppDependencies(
            client: MeshtasticClient(transport: BLETransport()),
            location: UnavailableLocationProvider(),
            heading: NoHeadingProvider(),
            store: InMemorySettingsStore())
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
