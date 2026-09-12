//
//  MapViewModel.swift — Map tab slice: the view model behind the
//  Field/GPS segmented control. Reads crew from `CoreStore.crew` (real
//  `ff_crew` freshness, `CrewMapPinBuilder`), the viewer's own position
//  from `LocationProviding`, heading from `HeadingProviding`, and
//  festival geometry from a `MapFestpackSource` — never a second,
//  independent read of any of those (same "one composition root" rule
//  `AppGraph`'s own header comment states for the rest of this app).
//
//  `@MainActor`: every dependency here (`CrewStore` via `CoreStore`,
//  and this view model's own `@State`-observed properties) already
//  lives on that isolation domain, per docs/specs/A01-companion-app.md's
//  threading model.
//
import FireflyCore
import Foundation
import Observation

/// Connectivity as far as the GPS map's offline chip is concerned. See
/// `MapViewModel.offlineChipText` for why this app does NOT attempt to
/// pre-cache Apple Maps tiles for guaranteed offline use (no public API
/// for it — see that property's own doc comment for the full honesty
/// note), and offers the schematic Field map as the actually-guaranteed
/// offline path instead.
public enum MapConnectivity: Sendable, Equatable {
    case online
    case offline
}

/// The GPS-map segment's own seam onto "is there a network path right
/// now" — deliberately NOT tied to `Network.framework` directly in this
/// file, so a test can drive `MapViewModel` without a real `NWPathMonitor`.
public protocol NetworkConnectivityObserving: Sendable {
    func connectivityUpdates() -> AsyncStream<MapConnectivity>
}

/// The permanent stand-in for macOS/simulator/tests that don't care —
/// reports `.online` once and never changes. NOT used to fabricate a
/// working offline cache; only to avoid every test needing a real
/// `NWPathMonitor`.
public final class AlwaysOnlineConnectivity: NetworkConnectivityObserving, @unchecked Sendable {
    public init() {}
    public func connectivityUpdates() -> AsyncStream<MapConnectivity> {
        AsyncStream { continuation in
            continuation.yield(.online)
            continuation.finish()
        }
    }
}

@MainActor
@Observable
public final class MapViewModel {
    private let crew: CrewStore
    private let location: any LocationProviding
    private let heading: any HeadingProviding
    private let festpackSource: any MapFestpackSource
    private let connectivity: any NetworkConnectivityObserving
    private let imperialResolver: () -> Bool

    public private(set) var myFix: LocationFix?
    public private(set) var headingReading: HeadingReading?
    public private(set) var festpack: MapFestpack?
    public private(set) var pins: [CrewMapPin] = []
    public private(set) var connectivityState: MapConnectivity = .online
    public var selectedCrewID: UInt32?
    /// Test seam only (PR #283 review, BLOCKING 3): counts every
    /// `refreshPins()` call, the periodic 1 Hz loop's own included —
    /// exists so a test can mechanically prove the loop actually stops
    /// once `stopObserving()` runs. `ageText` alone can't tell a live
    /// loop from a stopped one within a short test window (`ff_fmt_age`
    /// deliberately reads "now" for a full minute — its own doc comment
    /// on `ff_crew.c`), so this is the honest, minimal alternative to a
    /// flaky wall-clock-string assertion.
    public private(set) var refreshTickCount = 0

    private var locationObservation: Task<Void, Never>?
    private var headingObservation: Task<Void, Never>?
    private var connectivityObservation: Task<Void, Never>?
    private var pinRefreshLoop: Task<Void, Never>?

    public init(crew: CrewStore, location: any LocationProviding, heading: any HeadingProviding,
                festpackSource: any MapFestpackSource, connectivity: any NetworkConnectivityObserving = AlwaysOnlineConnectivity(),
                imperial: @escaping () -> Bool = { false }) {
        self.crew = crew
        self.location = location
        self.heading = heading
        self.festpackSource = festpackSource
        self.connectivity = connectivity
        self.imperialResolver = imperial
    }

    /// Idempotent, same convention as every other view model's
    /// `observe()` in this codebase (`ConnectViewModel`, `RadarViewModel`).
    public func observe() {
        guard locationObservation == nil else { return }
        let fixes = location.fixes()
        let headings = heading.headings()
        let connectivityUpdates = connectivity.connectivityUpdates()

        locationObservation = Task { [weak self] in
            for await fix in fixes {
                guard let self else { return }
                self.myFix = fix
                self.refreshPins()
            }
        }
        headingObservation = Task { [weak self] in
            for await reading in headings {
                guard let self else { return }
                self.headingReading = reading
            }
        }
        connectivityObservation = Task { [weak self] in
            for await state in connectivityUpdates {
                guard let self else { return }
                self.connectivityState = state
            }
        }
        Task { [weak self] in
            guard let self else { return }
            let pack = await self.festpackSource.currentFestpack()
            self.festpack = pack
        }
        // A 1s recompute loop, same convention `RadarViewModel.observe()`
        // already uses (`recomputeLoop`) — crew updates arrive on
        // `CoreStore`'s own client subscriptions, a stream this view
        // model does not itself listen to (it would otherwise duplicate
        // `CoreStore.apply(nodeUpdate:)`'s one subscription — S1's
        // multicast rule is about not missing events, not about every
        // reader needing its own copy of every upstream feed). Polling
        // `crew.members(now:)` here is also what keeps every pin's AGE
        // text advancing while the tab is simply left open.
        pinRefreshLoop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.refreshPins()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
        refreshPins()
    }

    public func stopObserving() {
        locationObservation?.cancel(); locationObservation = nil
        headingObservation?.cancel(); headingObservation = nil
        connectivityObservation?.cancel(); connectivityObservation = nil
        pinRefreshLoop?.cancel(); pinRefreshLoop = nil
    }

    public func refreshPins(now: UInt32 = FireflyClock.nowMillis()) {
        refreshTickCount += 1
        let myPosition = myFix.map { GeoCoordinate(latitude: $0.latitude, longitude: $0.longitude) }
        pins = CrewMapPinBuilder.build(from: crew.members(now: now), myPosition: myPosition)
    }

    /// The resolved units preference (`SettingsStoring.resolvedImperial()`,
    /// read fresh each access) — PR #283 review, SHOULD-FIX 4: exposed so
    /// a view (`GPSMapView.selectedCard`) can pass the REAL setting into
    /// `distanceBearingText(for:imperial:)` instead of a hardcoded value.
    public var imperial: Bool { imperialResolver() }

    public var selectedPin: CrewMapPin? {
        guard let selectedCrewID else { return nil }
        return pins.first { $0.id == selectedCrewID }
    }

    public func select(nodeID: UInt32?) {
        selectedCrewID = nodeID
    }

    public var myCoordinate: GeoCoordinate? {
        myFix.map { GeoCoordinate(latitude: $0.latitude, longitude: $0.longitude) }
    }

    /// The Field map's own projection, computed fresh from whatever
    /// `festpack`/`pins`/`myFix` currently hold — see
    /// `FieldMapProjector.project` for the actual geometry (through
    /// `ff_map`/`ff_geo`, never reimplemented). `nil` only when no
    /// festpack has loaded yet.
    public func fieldMapProjection(radiusPx: Float, marginPx: Float) -> FieldMapProjection? {
        guard let festpack else { return nil }
        return FieldMapProjector.project(festpack: festpack, crewPins: pins, myPosition: myCoordinate,
                                          headingDegrees: headingReading?.headingDegrees, radiusPx: radiusPx,
                                          marginPx: marginPx)
    }

    /// The selected-crew card's two text lines — "source" (what kind of
    /// fix this is, honestly) and "age". A pure function of one
    /// `CrewMapPin`, independently testable (this slice's own test
    /// plan: "selected-card text (source + age)").
    public static func selectedCardText(for pin: CrewMapPin) -> (source: String, age: String) {
        let source: String
        switch pin.treatment {
        case .live: source = "LIVE GPS"
        case .staleRing: source = "LAST KNOWN"
        case .lostRing: source = "LAST KNOWN · LOST"
        case .asserted: source = "ASSERTED POSITION"
        case .imprecise:
            if let grid = pin.precisionGridMeters {
                source = "LOW PRECISION · ~\(Int(grid.rounded())) m"
            } else {
                source = "LOW PRECISION"
            }
        }
        return (source, pin.ageText)
    }

    /// The selected-crew card's distance/bearing line — via
    /// `GeoBridge`, never reimplemented, and honestly `nil` when either
    /// side of the pair is unknown (never "0 m" for an unknown distance).
    public func distanceBearingText(for pin: CrewMapPin, imperial: Bool) -> String? {
        guard let distanceMeters = pin.distanceMeters, let bearingDegrees = pin.bearingDegrees else { return nil }
        // `DistanceFormatting` — PR #283 review, SHOULD-FIX 4: the ONE
        // seam every OTHER Swift-computed distance in this app renders
        // through (`DistanceFormatting.swift`'s own header comment),
        // rather than this file's own direct call into
        // `CrewStore.formatDistance`.
        let distanceText = DistanceFormatting.distance(meters: distanceMeters, imperial: imperial)
        let compass = GeoBridge.compassPoint(bearingDegrees: bearingDegrees)
        return "\(distanceText) · \(Int(bearingDegrees.rounded()))° \(compass)"
    }

    /// GPS-map offline chip text — `nil` means "don't show a chip"
    /// (online). See this file's header + `MapConnectivity`'s doc
    /// comment: this app does NOT pretend to have pre-cached Apple Maps
    /// tiles (no public API exists for a third-party app to fetch/cache
    /// Apple's own basemap tiles — `MKTileOverlay` is designed for a
    /// THIRD-PARTY tile server, and Apple's Maps & Location Services
    /// terms do not license bulk/offline caching of its own tiles
    /// outside the built-in, OS-level Maps app "offline maps" feature,
    /// which has no public API surface for another app's `MKMapView`/
    /// SwiftUI `Map` to draw from). So the honest state, offline, is
    /// "this view needs data" — never a fabricated "N MB cached"
    /// figure. The Field map (`FieldMapView`) is this feature's actual
    /// guaranteed-offline path: it draws from `festpack`, which loads
    /// once from `MapFestpackSource` and needs no network at all.
    public var offlineChipText: String? {
        connectivityState == .offline ? "OFFLINE — GPS map needs data" : nil
    }
}
