//
//  LocationProvider.swift — the real, CoreLocation-backed
//  `LocationProviding` implementation (docs/specs/A01-companion-app.md,
//  "Phone GPS -> node"), plus the phone-GPS-to-node push cadence built
//  on top of it.
//
//  `LocationProviding` and `LocationFix` themselves are landed
//  elsewhere (`LocationProviding.swift`, S5) and are NOT this file's —
//  this is the slice-F implementation that fills that seam in for real,
//  on both iOS and macOS, the way `AppDependencies.live()`'s doc
//  comment says it will.
//
//  Never fabricate: every `nil` this file yields — denied authorization,
//  no signal yet, an invalid reading — is a real "no fix", never papered
//  over with the last good coordinate.
//
import CoreLocation
import FireflyMesh
import Foundation

// MARK: - LocationProvider (real CoreLocation)

/// `LocationProviding` over `CLLocationManager`. Works the same way on
/// iOS and macOS — CoreLocation is the same framework on both, and
/// there is no reason for two implementations (the same argument the
/// spec makes for BLE).
// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` is justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access to `_authorization`/`pendingAuthContinuations` goes
// through `lock`, never unguarded. `manager` itself is only ever
// touched from `init` and from `CLLocationManagerDelegate` callbacks,
// which CoreLocation delivers serially on the queue this object was
// created on.
public final class LocationProvider: NSObject, LocationProviding, @unchecked Sendable {
    private let manager: CLLocationManager
    private let hub = EventHub<LocationFix?>()
    private let lock = NSLock()
    private var pendingAuthContinuations: [CheckedContinuation<Void, Never>] = []
    private var _authorization: LocationAuthorization

    public override init() {
        let manager = CLLocationManager()
        self.manager = manager
        // "Best" is a battery choice that has to be earned, not a
        // default — the spec is explicit about this.
        manager.desiredAccuracy = kCLLocationAccuracyHundredMeters
        manager.distanceFilter = 10
        self._authorization = LocationProvider.map(manager.authorizationStatus)
        super.init()
        manager.delegate = self
    }

    /// Finding 3: checks the SYSTEM-WIDE "Location Services" toggle
    /// first, live, every read — `CLLocationManager.locationServicesEnabled()`
    /// is the one CoreLocation call that tells the system-wide switch
    /// apart from a per-app denial (both otherwise report `.denied` at
    /// the per-app authorization level, which is all `_authorization`
    /// below ever tracks). Not cached: the user can flip this in System
    /// Settings while Firefly is running, same as authorization itself.
    public var authorization: LocationAuthorization {
        guard CLLocationManager.locationServicesEnabled() else { return .locationServicesDisabled }
        lock.lock(); defer { lock.unlock() }
        return _authorization
    }

    public func requestWhenInUseAuthorization() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            lock.lock()
            pendingAuthContinuations.append(continuation)
            lock.unlock()
            manager.requestWhenInUseAuthorization()
        }
    }

    /// Only ever called once the user has explicitly turned location
    /// sharing on (Settings, a different slice) — this method itself
    /// does not gate that; it just relays CoreLocation's own prompt.
    public func requestAlwaysAuthorization() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            lock.lock()
            pendingAuthContinuations.append(continuation)
            lock.unlock()
            manager.requestAlwaysAuthorization()
        }
    }

    public func fixes() -> AsyncStream<LocationFix?> {
        hub.subscribe()
    }

    private func startIfAuthorized() {
        switch manager.authorizationStatus {
        case .authorizedAlways, .authorizedWhenInUse:
            manager.startUpdatingLocation()
            #if os(iOS)
            manager.allowsBackgroundLocationUpdates = manager.authorizationStatus == .authorizedAlways
            #endif
        default:
            break
        }
    }

    private static func map(_ status: CLAuthorizationStatus) -> LocationAuthorization {
        switch status {
        case .notDetermined: return .notDetermined
        case .authorizedWhenInUse: return .whenInUse
        case .authorizedAlways: return .always
        case .restricted, .denied: return .deniedOrRestricted
        @unknown default: return .deniedOrRestricted
        }
    }

    fileprivate func resumePendingAuthContinuations() {
        lock.lock()
        let pending = pendingAuthContinuations
        pendingAuthContinuations.removeAll()
        lock.unlock()
        for c in pending { c.resume() }
    }
}

extension LocationProvider: CLLocationManagerDelegate {
    public func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let mapped = LocationProvider.map(manager.authorizationStatus)
        lock.lock(); _authorization = mapped; lock.unlock()
        switch mapped {
        case .whenInUse, .always:
            startIfAuthorized()
        case .deniedOrRestricted:
            manager.stopUpdatingLocation()
            // A real, explicit "no fix" — not silence. A screen that
            // never hears from this provider again after a denial would
            // just show whatever it last had, which is exactly the
            // fabrication-by-omission the spec forbids.
            hub.yield(nil)
        case .notDetermined:
            break
        case .locationServicesDisabled:
            // `map(_:)` (per-app status only) never produces this case
            // itself — `authorization`'s own getter is what folds in
            // the system-wide check — so this branch is unreachable in
            // practice. Handled honestly anyway, matching `.deniedOrRestricted`:
            // stop and report absence, never leave a stale fix standing.
            manager.stopUpdatingLocation()
            hub.yield(nil)
        }
        resumePendingAuthContinuations()
    }

    public func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else { return }
        // CoreLocation's own convention: a negative accuracy means the
        // reading is invalid. Never turn that into a fabricated fix —
        // simply do not report this one. A later, valid reading (or an
        // explicit nil on denial/stop) is what callers see next.
        guard location.horizontalAccuracy >= 0 else { return }

        let course = location.course
        let fix = LocationFix(
            latitude: location.coordinate.latitude,
            longitude: location.coordinate.longitude,
            altitude: location.verticalAccuracy >= 0 ? location.altitude : nil,
            time: location.timestamp,
            horizontalAccuracyMeters: location.horizontalAccuracy,
            groundSpeedMetersPerSecond: location.speed >= 0 ? location.speed : nil,
            // `LocationFix.groundTrackDegrees`'s own doc comment: "the
            // provider is responsible for" the `0 < value <= 360` check
            // — CLLocation's -1-means-invalid sentinel falls outside
            // that range already, but 0 itself is excluded too, per the
            // wire payload rule this exists for.
            groundTrackDegrees: (course > 0 && course <= 360) ? course : nil)
        hub.yield(fix)
    }

    public func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        if let clError = error as? CLError, clError.code == .denied {
            hub.yield(nil)
        }
        // Other errors (kCLErrorLocationUnknown, a transient signal
        // loss) are not reported as "no fix" — CoreLocation itself
        // keeps trying, and a momentary GPS hiccup is not the same fact
        // as "there is no fix," which is the only thing `nil` may mean.
    }
}

// MARK: - Phone GPS -> node push cadence

/// What `PhoneGPSUplink` hands a fix to. Deliberately NOT
/// `MeshtasticClientProtocol` — that protocol only knows how to send
/// text (S3's `sendText`) — and deliberately typed in plain
/// `LocationFix` terms rather than a protobuf: "protobuf types never
/// leave `FireflyMesh`" (docs/specs/A01-companion-app.md, "Data flow")
/// is a hard rule, and `FireflyModel` does not even depend on
/// `MeshtasticProto` (see `Package.swift`) — encoding the
/// `POSITION_APP`/`LOC_EXTERNAL` packet itself belongs on the
/// `FireflyMesh` side of that boundary, in whatever adapter wires this
/// protocol to a real client once one exists (slice A's, once merged).
/// That split is exactly what keeps the cadence policy below testable
/// with no client, no radio, and no protobuf import at all.
public protocol PositionPushSending: Sendable {
    /// Push `fix` to the connected node itself as a
    /// `POSITION_APP`/`LOC_EXTERNAL` position — an external FIX with a
    /// time on it, never `AdminMessage.set_fixed_position` (spec:
    /// "Phone GPS -> node" > "Mechanism"). `destination` is the
    /// connected node's own `num`.
    func sendPosition(_ fix: LocationFix, to destination: UInt32) async throws
}

/// Pure decision logic for "should we push a position now?" — no I/O,
/// fully unit-testable. Two independent triggers, both from the spec's
/// cadence rule as the coordinator asked it be read: a time floor
/// (default 30s, spec's own floor of 5s enforced by clamping) OR a
/// minimum-movement trigger, whichever comes first. The spec's own text
/// states only the time cadence; the movement trigger is this
/// implementation's addition, on the coordinator's explicit
/// instruction ("every N s or on >= M m movement") — flagged here
/// rather than silently folded into "the spec," since the spec doc
/// itself does not mention it.
public struct PhoneGPSUplinkPolicy: Sendable {
    /// Never below this — the spec's own floor, enforced regardless of
    /// what Settings holds (a corrupt or absent default must not become
    /// a 0s hammer on the radio).
    public static let minimumIntervalSeconds: Double = 5
    public static let defaultIntervalSeconds: Double = 30
    /// Default minimum-movement trigger. Comfortably above the
    /// CoreLocation `distanceFilter` (10 m) so it is a deliberate
    /// second trigger, not noise from the filter itself.
    public static let defaultMinimumMovementMeters: Double = 25

    public var minimumMovementMeters: Double

    public init(minimumMovementMeters: Double = PhoneGPSUplinkPolicy.defaultMinimumMovementMeters) {
        self.minimumMovementMeters = minimumMovementMeters
    }

    /// Great-circle-ish flat-earth approximation, adequate at the
    /// festival scale this app cares about (metres to low kilometres) —
    /// full geodesy is `ff_geo`'s job for crew bearings, not this
    /// cadence check.
    public static func approximateDistanceMeters(_ a: (lat: Double, lon: Double), _ b: (lat: Double, lon: Double)) -> Double {
        let metersPerDegreeLat = 111_320.0
        let latRad = a.lat * .pi / 180
        let dLat = (a.lat - b.lat) * metersPerDegreeLat
        let dLon = (a.lon - b.lon) * metersPerDegreeLat * cos(latRad)
        return (dLat * dLat + dLon * dLon).squareRoot()
    }

    /// - Parameters:
    ///   - enabled: the "share phone GPS" setting. `false` means never
    ///     push, full stop — this is opt-in, off by default, per spec.
    ///   - intervalSeconds: from Settings; clamped to
    ///     `minimumIntervalSeconds` here so a bad stored value cannot
    ///     defeat the floor.
    public func shouldPush(
        enabled: Bool,
        previous: (fix: LocationFix, pushedAt: Date)?,
        candidate: LocationFix,
        now: Date,
        intervalSeconds: Double
    ) -> Bool {
        guard enabled else { return false }
        guard let previous else { return true } // never pushed yet
        let clampedInterval = max(intervalSeconds, Self.minimumIntervalSeconds)
        if now.timeIntervalSince(previous.pushedAt) >= clampedInterval { return true }
        let moved = Self.approximateDistanceMeters(
            (candidate.latitude, candidate.longitude),
            (previous.fix.latitude, previous.fix.longitude))
        return moved >= minimumMovementMeters
    }
}

/// Subscribes to a `LocationProviding`'s fix stream and, while location
/// sharing is on (`SettingsKey.locationSharingEnabled`), hands fixes to
/// `PositionPushSending` at `PhoneGPSUplinkPolicy`'s cadence. An `actor`
/// (not `@MainActor`): this
/// consumes an `AsyncStream` off the UI and does no rendering — it is
/// the same kind of background-work owner the spec's threading model
/// describes for transports and the client, not view-model work.
public actor PhoneGPSUplink {
    private let location: any LocationProviding
    private let settings: any SettingsStoring
    private let sink: any PositionPushSending
    private let destinationNodeNum: @Sendable () -> UInt32?
    private let policy: PhoneGPSUplinkPolicy
    private let now: @Sendable () -> Date

    private var previous: (fix: LocationFix, pushedAt: Date)?
    private var runningTask: Task<Void, Never>?

    public init(
        location: any LocationProviding,
        settings: any SettingsStoring,
        sink: any PositionPushSending,
        destinationNodeNum: @escaping @Sendable () -> UInt32?,
        policy: PhoneGPSUplinkPolicy = PhoneGPSUplinkPolicy(),
        now: @escaping @Sendable () -> Date = { Date() }
    ) {
        self.location = location
        self.settings = settings
        self.sink = sink
        self.destinationNodeNum = destinationNodeNum
        self.policy = policy
        self.now = now
    }

    /// Idempotent, same convention as the view-model `observe()`
    /// pattern (MVVM conventions, #4) even though this is not a view
    /// model — starting it twice must not double-subscribe.
    ///
    /// Subscribes to `location.fixes()` SYNCHRONOUSLY, right here,
    /// before spawning the `Task` that consumes it — not inside the
    /// task body. `EventHub` is multicast, not replayed (S1): a
    /// subscription registered after a fix was already yielded simply
    /// misses it, the exact ordering trap the spec calls out for
    /// `ConnectViewModel.observe()` and `CoreStore.observe(client:)`.
    /// Getting this backwards here would mean an uplink that
    /// intermittently misses the very first fix depending on task
    /// scheduling.
    public func start() {
        guard runningTask == nil else { return }
        let stream = location.fixes()
        runningTask = Task { [weak self] in
            for await maybeFix in stream {
                guard let fix = maybeFix else { continue } // no fix: nothing to push, never fabricate
                guard let self else { return }
                await self.handle(fix: fix)
            }
        }
    }

    public func stop() {
        runningTask?.cancel()
        runningTask = nil
    }

    private func handle(fix: LocationFix) async {
        guard let destination = destinationNodeNum() else { return } // not connected to anything yet
        let enabled = settings.bool(.locationSharingEnabled)
        let interval = settings.double(.locationSharingIntervalSeconds) ?? PhoneGPSUplinkPolicy.defaultIntervalSeconds
        let moment = now()
        guard policy.shouldPush(enabled: enabled, previous: previous, candidate: fix, now: moment, intervalSeconds: interval) else {
            return
        }
        do {
            try await sink.sendPosition(fix, to: destination)
            previous = (fix, moment)
        } catch {
            // A failed push does not update `previous` — the next fix
            // (or the next cadence tick) retries rather than silently
            // giving up for the rest of the session.
        }
    }
}
