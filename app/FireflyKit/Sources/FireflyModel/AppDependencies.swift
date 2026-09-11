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

    /// Milestone-1 placeholder for the real stack. Slice A's BLE client
    /// replaces `StubMeshtasticClient` here; slice F's
    /// `LocationProvider`/`HeadingProvider` replace the unavailable
    /// stand-ins. `.live()` and `.stub()` are identical ON PURPOSE until
    /// those slices land — nothing above this seam should behave
    /// differently depending on which one is picked, which is exactly
    /// what makes it safe to land this seam before the slices that fill
    /// it in do.
    public static func live() -> AppDependencies { stub() }

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
