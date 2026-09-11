//
//  LocationProviderTests.swift — the honesty rules `LocationProvider`
//  and the phone-GPS push cadence must follow: permission denied and
//  no fix produce ABSENCE, never a coordinate (docs/specs/
//  A01-companion-app.md, Slice F "Must add").
//
import CoreLocation
import FireflyMesh
import FireflyModel
import XCTest

final class LocationProviderTests: XCTestCase {

    // MARK: - LocationProvider: real delegate callbacks, never fabricated

    func testValidLocationProducesAFix() async throws {
        let provider = LocationProvider()
        let stream = provider.fixes()
        let collector = Task<LocationFix?, Never> {
            for await fix in stream { return fix }
            return nil
        }

        let coordinate = CLLocationCoordinate2D(latitude: 47.708135, longitude: -122.2820993)
        let location = CLLocation(
            coordinate: coordinate, altitude: 40,
            horizontalAccuracy: 5, verticalAccuracy: 5,
            course: 90, speed: 2, timestamp: Date())
        provider.locationManager(CLLocationManager(), didUpdateLocations: [location])

        let fix = await collector.value
        let unwrapped = try XCTUnwrap(fix)
        XCTAssertEqual(unwrapped.latitude, 47.708135, accuracy: 0.0000001)
        XCTAssertEqual(unwrapped.longitude, -122.2820993, accuracy: 0.0000001)
        XCTAssertEqual(unwrapped.altitude, 40)
        XCTAssertEqual(unwrapped.groundTrackDegrees, 90)
        XCTAssertNotNil(unwrapped.groundSpeedMetersPerSecond)
    }

    /// CoreLocation's own convention: a negative accuracy means the
    /// reading is invalid. Never a fabricated fix from one.
    func testNegativeHorizontalAccuracyProducesNoFix() async throws {
        let provider = LocationProvider()
        let stream = provider.fixes()
        let coordinate = CLLocationCoordinate2D(latitude: 1, longitude: 1)
        let bad = CLLocation(
            coordinate: coordinate, altitude: 0,
            horizontalAccuracy: -1, verticalAccuracy: -1,
            course: -1, speed: -1, timestamp: Date())
        provider.locationManager(CLLocationManager(), didUpdateLocations: [bad])

        // Nothing should ever arrive for this invalid reading: prove it
        // by racing collection against a short timeout, rather than
        // asserting "eventually nothing," which a slow CI box could
        // flake on either way.
        let collected = await collectFixes(from: stream, count: 1, windowSeconds: 0.3)
        XCTAssertTrue(collected.isEmpty, "an invalid reading must never surface as a delivered event")
    }

    /// A denied/restricted authorization is an explicit, real "no fix"
    /// — `nil` — never silence that a screen could mistake for "still
    /// waiting," and never the last good coordinate either.
    func testDeniedAuthorizationYieldsExplicitNilFix() async throws {
        let provider = LocationProvider()
        let stream = provider.fixes()
        provider.locationManager(CLLocationManager(), didFailWithError: CLError(.denied))

        let collected = await collectFixes(from: stream, count: 1, windowSeconds: 2)
        XCTAssertEqual(collected.count, 1, "denial must surface as a delivered event, not silence")
        if let event = collected.first {
            XCTAssertNil(event, "denial must surface as an explicit nil fix")
        }
    }

    /// Races collecting `count` events off `stream` against a timeout,
    /// returning whatever was collected (possibly empty) when either
    /// side finishes first — the shared shape both "prove absence" and
    /// "prove exactly this arrives" tests above build on.
    private func collectFixes(from stream: AsyncStream<LocationFix?>, count: Int, windowSeconds: TimeInterval) async -> [LocationFix?] {
        await withTaskGroup(of: [LocationFix?].self) { group in
            group.addTask {
                var out: [LocationFix?] = []
                for await fix in stream {
                    out.append(fix)
                    if out.count >= count { break }
                }
                return out
            }
            group.addTask {
                try? await Task.sleep(nanoseconds: UInt64(windowSeconds * 1_000_000_000))
                return []
            }
            let first = await group.next() ?? []
            group.cancelAll()
            return first
        }
    }

    // MARK: - PhoneGPSUplinkPolicy: pure cadence rules

    private func fix(lat: Double, lon: Double) -> LocationFix {
        LocationFix(latitude: lat, longitude: lon, altitude: nil, time: Date(),
                    horizontalAccuracyMeters: 5, groundSpeedMetersPerSecond: nil, groundTrackDegrees: nil)
    }

    func testDisabledSharingNeverPushes() {
        let policy = PhoneGPSUplinkPolicy()
        let now = Date()
        let should = policy.shouldPush(
            enabled: false, previous: nil, candidate: fix(lat: 1, lon: 1), now: now, intervalSeconds: 30)
        XCTAssertFalse(should)
    }

    func testFirstFixAlwaysPushesWhenEnabled() {
        let policy = PhoneGPSUplinkPolicy()
        let should = policy.shouldPush(
            enabled: true, previous: nil, candidate: fix(lat: 1, lon: 1), now: Date(), intervalSeconds: 30)
        XCTAssertTrue(should)
    }

    func testIntervalFloorIsEnforcedEvenIfSettingsSayLower() {
        let policy = PhoneGPSUplinkPolicy()
        let start = Date()
        let previous = (fix: fix(lat: 47.7, lon: -122.28), pushedAt: start)
        // 3s elapsed, interval "requested" as 1s (below the 5s floor) —
        // must NOT push yet, because the floor wins.
        let tooSoon = policy.shouldPush(
            enabled: true, previous: previous, candidate: previous.fix,
            now: start.addingTimeInterval(3), intervalSeconds: 1)
        XCTAssertFalse(tooSoon)

        let pastFloor = policy.shouldPush(
            enabled: true, previous: previous, candidate: previous.fix,
            now: start.addingTimeInterval(5.5), intervalSeconds: 1)
        XCTAssertTrue(pastFloor)
    }

    func testMovementTriggersAPushBeforeTheIntervalElapses() {
        let policy = PhoneGPSUplinkPolicy(minimumMovementMeters: 25)
        let start = Date()
        let previous = (fix: fix(lat: 47.708135, lon: -122.2820993), pushedAt: start)
        // ~1s later, well under the 30s default interval, but moved
        // roughly 100m north.
        let moved = fix(lat: 47.709035, lon: -122.2820993)
        let should = policy.shouldPush(
            enabled: true, previous: previous, candidate: moved,
            now: start.addingTimeInterval(1), intervalSeconds: 30)
        XCTAssertTrue(should)
    }

    func testTinyJitterNeitherTriggersMovementNorFakesDistance() {
        let policy = PhoneGPSUplinkPolicy(minimumMovementMeters: 25)
        let start = Date()
        let previous = (fix: fix(lat: 47.708135, lon: -122.2820993), pushedAt: start)
        // ~1m of GPS jitter.
        let jittered = fix(lat: 47.708144, lon: -122.2820993)
        let should = policy.shouldPush(
            enabled: true, previous: previous, candidate: jittered,
            now: start.addingTimeInterval(1), intervalSeconds: 30)
        XCTAssertFalse(should)
    }

    /// NIT from the slice F review: the movement trigger's own tests
    /// (~100m well above, ~1m well below) never exercise the 25m
    /// boundary itself — `moved >= minimumMovementMeters` is a one-line
    /// comparison, but the `>=` (inclusive at exactly the threshold)
    /// deserves the same close-boundary coverage the time-floor test
    /// already gets (3s vs 5.5s around a 5s floor). Both deltas here
    /// are pure-north (`dLon == 0`), so `approximateDistanceMeters`
    /// reduces to `dLat * metersPerDegreeLat` and the inverse is exact
    /// enough to land on either side of 25.0m without relying on
    /// `shouldPush`'s own rounding.
    func testMovementBoundaryJustUnderVersusAtTheThreshold() {
        let policy = PhoneGPSUplinkPolicy(minimumMovementMeters: 25)
        let start = Date()
        let baseLat = 47.708135
        let baseLon = -122.2820993
        let previous = (fix: fix(lat: baseLat, lon: baseLon), pushedAt: start)
        let metersPerDegreeLat = 111_320.0

        // 24.9m north: below the 25.0m threshold, must not trigger.
        let justUnder = fix(lat: baseLat + 24.9 / metersPerDegreeLat, lon: baseLon)
        let underDistance = PhoneGPSUplinkPolicy.approximateDistanceMeters(
            (justUnder.latitude, justUnder.longitude), (baseLat, baseLon))
        XCTAssertEqual(underDistance, 24.9, accuracy: 0.01)
        let shouldNotPush = policy.shouldPush(
            enabled: true, previous: previous, candidate: justUnder,
            now: start.addingTimeInterval(1), intervalSeconds: 30)
        XCTAssertFalse(shouldNotPush, "24.9m is below the 25m threshold and must not trigger a push")

        // Effectively exactly 25.0m north (a 1e-6m nudge over, so
        // floating-point rounding in the dLat/cos chain cannot land it
        // a hair under 25.0 and flip the assertion for the wrong
        // reason): at the threshold — `>=` means this DOES trigger.
        let atThreshold = fix(lat: baseLat + (25.0 + 0.000_001) / metersPerDegreeLat, lon: baseLon)
        let atDistance = PhoneGPSUplinkPolicy.approximateDistanceMeters(
            (atThreshold.latitude, atThreshold.longitude), (baseLat, baseLon))
        XCTAssertEqual(atDistance, 25.0, accuracy: 0.01)
        let shouldPush = policy.shouldPush(
            enabled: true, previous: previous, candidate: atThreshold,
            now: start.addingTimeInterval(1), intervalSeconds: 30)
        XCTAssertTrue(shouldPush, "25.0m meets the inclusive >= threshold and must trigger a push")
    }

    // MARK: - PhoneGPSUplink: end to end with fakes, no radio

    private final class RecordingSink: PositionPushSending, @unchecked Sendable {
        private let lock = NSLock()
        private(set) var pushes: [(fix: LocationFix, destination: UInt32)] = []

        // M3 / Swift 6: the locked mutation happens in this synchronous
        // helper, never lexically inside `sendPosition`'s own `async`
        // body — `NSLock.lock()`/`unlock()` are `noasync`, the same rule
        // `DemoMeshtasticClient`'s own record helpers document.
        private func record(_ fix: LocationFix, destination: UInt32) {
            lock.lock(); pushes.append((fix, destination)); lock.unlock()
        }

        func sendPosition(_ fix: LocationFix, to destination: UInt32) async throws {
            record(fix, destination: destination)
        }

        func snapshot() -> [(fix: LocationFix, destination: UInt32)] {
            lock.lock(); defer { lock.unlock() }
            return pushes
        }
    }

    private final class ScriptedLocationProvider: LocationProviding, @unchecked Sendable {
        let authorization: LocationAuthorization = .whenInUse
        private let hub = EventHub<LocationFix?>()
        func requestWhenInUseAuthorization() async {}
        func requestAlwaysAuthorization() async {}
        func fixes() -> AsyncStream<LocationFix?> { hub.subscribe() }
        func emit(_ fix: LocationFix?) { hub.yield(fix) }
    }

    /// Test-only, lock-protected clock override for `PhoneGPSUplink`'s
    /// `now:` closure. `PhoneGPSUplink` is an `actor`; its own isolated
    /// context calls this closure independently of whenever the test
    /// method advances the clock between assertions, so a plain
    /// captured `var Date` (what this used to be) is exactly the real
    /// race Swift 6 is right to flag — not a false positive to silence.
    private final class LockedTestClock: @unchecked Sendable {
        private let lock = NSLock()
        private var value: Date
        init(_ initial: Date) { value = initial }
        func get() -> Date { lock.lock(); defer { lock.unlock() }; return value }
        func advance(by interval: TimeInterval) {
            lock.lock(); value = value.addingTimeInterval(interval); lock.unlock()
        }
    }

    func testUplinkNeverPushesWhileSharingIsDisabled() async throws {
        let location = ScriptedLocationProvider()
        let settings = InMemorySettingsStore()
        settings.setBool(false, .locationSharingEnabled) // off by default, explicit here
        let sink = RecordingSink()
        let uplink = PhoneGPSUplink(location: location, settings: settings, sink: sink, destinationNodeNum: { 48629424 })

        await uplink.start()
        location.emit(fix(lat: 47.708135, lon: -122.2820993))
        try await Task.sleep(nanoseconds: 200_000_000)
        await uplink.stop()

        XCTAssertTrue(sink.snapshot().isEmpty, "sharing is off: nothing should ever be pushed")
    }

    func testUplinkPushesOnceEnabledAndRespectsCadence() async throws {
        let location = ScriptedLocationProvider()
        let settings = InMemorySettingsStore()
        settings.setBool(true, .locationSharingEnabled)
        settings.setDouble(30, .locationSharingIntervalSeconds)
        let sink = RecordingSink()

        let now = LockedTestClock(Date())
        let uplink = PhoneGPSUplink(
            location: location, settings: settings, sink: sink,
            destinationNodeNum: { 48629424 }, now: { now.get() })

        await uplink.start()
        location.emit(fix(lat: 47.708135, lon: -122.2820993)) // first ever: always pushes
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(sink.snapshot().count, 1)
        XCTAssertEqual(sink.snapshot().first?.destination, 48629424)

        // A second fix moments later, no meaningful movement, before the
        // interval: must NOT push again yet.
        now.advance(by: 2)
        location.emit(fix(lat: 47.708136, lon: -122.2820993))
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(sink.snapshot().count, 1, "too soon and too little movement: must not push")

        await uplink.stop()
    }

    func testUplinkNeverPushesWithNoDestination() async throws {
        let location = ScriptedLocationProvider()
        let settings = InMemorySettingsStore()
        settings.setBool(true, .locationSharingEnabled)
        let sink = RecordingSink()
        let uplink = PhoneGPSUplink(location: location, settings: settings, sink: sink, destinationNodeNum: { nil })

        await uplink.start()
        location.emit(fix(lat: 47.708135, lon: -122.2820993))
        try await Task.sleep(nanoseconds: 200_000_000)
        await uplink.stop()

        XCTAssertTrue(sink.snapshot().isEmpty, "not connected to any node: nothing to address a position to")
    }

    func testUplinkNeverPushesANilFix() async throws {
        let location = ScriptedLocationProvider()
        let settings = InMemorySettingsStore()
        settings.setBool(true, .locationSharingEnabled)
        let sink = RecordingSink()
        let uplink = PhoneGPSUplink(location: location, settings: settings, sink: sink, destinationNodeNum: { 48629424 })

        await uplink.start()
        location.emit(nil) // no fix
        try await Task.sleep(nanoseconds: 200_000_000)
        await uplink.stop()

        XCTAssertTrue(sink.snapshot().isEmpty, "no fix means nothing to push, never fabricated")
    }

}
