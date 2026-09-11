//
//  FlareTakeoverViewModelTests.swift — M2's inbound-FLARE takeover:
//  auto-dismiss timing (tick-driven, no real sleeps), manual dismiss,
//  "newest wins", and the honest bearing/no-bearing split
//  (docs/specs/S10-flare.md).
//
import FireflyModel
import XCTest

@MainActor
final class FlareTakeoverViewModelTests: XCTestCase {
    private func crewWithMember(nodeID: UInt32, latitude: Double?, longitude: Double?,
                                atMs: UInt32 = 0) -> CrewStore {
        let crew = CrewStore()
        _ = crew.setIdentity(nodeID: nodeID, shortName: "DAN", longName: "Dana")
        crew.setPaired(nodeID: nodeID, paired: true)
        if let latitude, let longitude {
            crew.onPosition(nodeID: nodeID, latitude: latitude, longitude: longitude, rxTimeMs: atMs,
                             meta: CrewStore.PositionMeta(asserted: false, precisionBits: nil))
        }
        return crew
    }

    // MARK: - Bearing honesty

    func testShowComputesBearingAndDistanceWhenBothPositionsAreKnown() {
        let crew = crewWithMember(nodeID: 1, latitude: 43.701000, longitude: -121.500000)
        let model = FlareTakeoverViewModel(crew: crew, currentFix: {
            LocationFix(latitude: 43.700000, longitude: -121.500000, altitude: nil, time: Date(),
                        horizontalAccuracyMeters: nil, groundSpeedMetersPerSecond: nil, groundTrackDegrees: nil)
        })

        model.show(senderNodeID: 1, durationSeconds: 300)

        XCTAssertTrue(model.isActive)
        XCTAssertEqual(model.senderName, "Dana")
        XCTAssertNotNil(model.bearingDegrees)
        XCTAssertNotNil(model.compassPoint)
        XCTAssertNotNil(model.distanceText)
    }

    func testShowIsHonestlyBearinglessWithNoFixOfOurOwn() {
        let crew = crewWithMember(nodeID: 1, latitude: 43.701000, longitude: -121.500000)
        let model = FlareTakeoverViewModel(crew: crew) // default currentFix: { nil }

        model.show(senderNodeID: 1, durationSeconds: 300)

        XCTAssertTrue(model.isActive)
        XCTAssertNil(model.bearingDegrees, "no fix of our own — never a fabricated bearing")
        XCTAssertNil(model.compassPoint)
        XCTAssertNil(model.distanceText)
    }

    func testShowIsHonestlyBearinglessWhenTheSenderHasNoKnownPosition() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        let model = FlareTakeoverViewModel(crew: crew, currentFix: {
            LocationFix(latitude: 43.700000, longitude: -121.500000, altitude: nil, time: Date(),
                        horizontalAccuracyMeters: nil, groundSpeedMetersPerSecond: nil, groundTrackDegrees: nil)
        })

        model.show(senderNodeID: 1, durationSeconds: 300)

        XCTAssertNil(model.bearingDegrees, "the sender's position is unknown — never a fabricated bearing")
    }

    func testUnknownSenderStillShowsWithAnHonestFallbackName() {
        let crew = CrewStore() // empty roster — the sender was never paired/identified
        let model = FlareTakeoverViewModel(crew: crew)

        model.show(senderNodeID: 99, durationSeconds: 300)

        XCTAssertTrue(model.isActive)
        XCTAssertEqual(model.senderName, "Someone", "no invented name for an unknown sender")
    }

    // MARK: - Auto-dismiss timing (tick-driven, no real sleeps)

    func testTickDoesNotAutoDismissBeforeTheDurationElapses() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        var now = Date(timeIntervalSince1970: 1_000_000)
        let model = FlareTakeoverViewModel(crew: crew, clock: { now })

        model.show(senderNodeID: 1, durationSeconds: 10)
        now = now.addingTimeInterval(9)
        model.tick(now: now)

        XCTAssertTrue(model.isActive, "must still be showing 1s before the duration elapses")
        XCTAssertEqual(model.remainingSeconds(now: now), 1)
    }

    func testTickAutoDismissesExactlyAtTheDuration() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        var now = Date(timeIntervalSince1970: 2_000_000)
        let model = FlareTakeoverViewModel(crew: crew, clock: { now })

        model.show(senderNodeID: 1, durationSeconds: 5)
        now = now.addingTimeInterval(5)
        model.tick(now: now)

        XCTAssertFalse(model.isActive, "auto-dismiss at dur_s (S10)")
        XCTAssertEqual(model.remainingSeconds(now: now), 0)
    }

    func testTickIsANoOpWhileNotActive() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        let model = FlareTakeoverViewModel(crew: crew)
        model.tick() // never shown — must not crash or flip anything
        XCTAssertFalse(model.isActive)
    }

    // MARK: - Manual dismiss

    func testManualDismissClearsImmediatelyRegardlessOfRemainingTime() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        let model = FlareTakeoverViewModel(crew: crew)
        model.show(senderNodeID: 1, durationSeconds: 300)
        XCTAssertTrue(model.isActive)

        model.dismiss()

        XCTAssertFalse(model.isActive)
        XCTAssertEqual(model.remainingSeconds(), 0)
    }

    // MARK: - "Newest wins the takeover" (S10) + FLARE_END semantics

    func testANewerFlareFromAnyoneOverwritesWhateverWasShowing() {
        let crew = CrewStore()
        _ = crew.setIdentity(nodeID: 1, shortName: "DAN", longName: "Dana")
        crew.setPaired(nodeID: 1, paired: true)
        _ = crew.setIdentity(nodeID: 2, shortName: "KEV", longName: "Kev")
        crew.setPaired(nodeID: 2, paired: true)
        let model = FlareTakeoverViewModel(crew: crew)

        model.show(senderNodeID: 1, durationSeconds: 300)
        XCTAssertEqual(model.senderNodeID, 1)

        model.show(senderNodeID: 2, durationSeconds: 60)
        XCTAssertEqual(model.senderNodeID, 2, "newest wins the takeover — Kev overwrites Dana")
        XCTAssertEqual(model.totalDurationSeconds, 60)
    }

    func testEndFromTheCurrentSenderClearsTheTakeover() {
        let crew = crewWithMember(nodeID: 1, latitude: nil, longitude: nil)
        let model = FlareTakeoverViewModel(crew: crew)
        model.show(senderNodeID: 1, durationSeconds: 300)

        model.end(from: 1)

        XCTAssertFalse(model.isActive)
    }

    func testEndFromAnyoneElseLeavesTheCurrentTakeoverUntouched() {
        let crew = CrewStore()
        _ = crew.setIdentity(nodeID: 1, shortName: "DAN", longName: "Dana")
        crew.setPaired(nodeID: 1, paired: true)
        let model = FlareTakeoverViewModel(crew: crew)
        model.show(senderNodeID: 1, durationSeconds: 300)

        // A stale FLARE_END naming a DIFFERENT (e.g. already-superseded)
        // sender must not clear the current takeover.
        model.end(from: 2)

        XCTAssertTrue(model.isActive)
        XCTAssertEqual(model.senderNodeID, 1)
    }
}
