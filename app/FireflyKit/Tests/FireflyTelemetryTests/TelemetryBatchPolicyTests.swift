//
//  TelemetryBatchPolicyTests.swift — A04: "flush every 60 s or 50
//  events, and on background", driven with a fixed `Date` rather than a
//  real 60-second wait — see `TelemetryBatchPolicy`'s own header
//  comment for why the decision is split out from `FirebaseSink` at all.
//
import Foundation
import XCTest
@testable import FireflyTelemetry

final class TelemetryBatchPolicyTests: XCTestCase {
    private let epoch = Date(timeIntervalSince1970: 1_700_000_000)

    func testDoesNotFlushAnEmptyBuffer() {
        let policy = TelemetryBatchPolicy()
        XCTAssertFalse(policy.shouldFlush(pendingCount: 0, oldestPendingEventAt: nil, now: epoch))
    }

    func testFlushesAtTheEventCountThreshold() {
        let policy = TelemetryBatchPolicy(maxEventCount: 50, maxInterval: 60)
        XCTAssertFalse(policy.shouldFlush(pendingCount: 49, oldestPendingEventAt: epoch, now: epoch))
        XCTAssertTrue(policy.shouldFlush(pendingCount: 50, oldestPendingEventAt: epoch, now: epoch))
    }

    func testFlushesAtTheTimeThresholdEvenWithFewEvents() {
        let policy = TelemetryBatchPolicy(maxEventCount: 50, maxInterval: 60)
        XCTAssertFalse(policy.shouldFlush(pendingCount: 3, oldestPendingEventAt: epoch,
                                           now: epoch.addingTimeInterval(59)))
        XCTAssertTrue(policy.shouldFlush(pendingCount: 3, oldestPendingEventAt: epoch,
                                          now: epoch.addingTimeInterval(60)))
    }

    func testDoesNotFlushBeforeEitherThresholdIsReached() {
        let policy = TelemetryBatchPolicy(maxEventCount: 50, maxInterval: 60)
        XCTAssertFalse(policy.shouldFlush(pendingCount: 10, oldestPendingEventAt: epoch,
                                           now: epoch.addingTimeInterval(30)))
    }

    func testBackgroundingForcesAFlushRegardlessOfThresholds() {
        let policy = TelemetryBatchPolicy(maxEventCount: 50, maxInterval: 60)
        XCTAssertTrue(policy.shouldFlush(pendingCount: 1, oldestPendingEventAt: epoch, now: epoch,
                                          isBackgrounding: true))
    }

    func testBackgroundingWithNothingPendingStillDoesNotFlush() {
        let policy = TelemetryBatchPolicy()
        XCTAssertFalse(policy.shouldFlush(pendingCount: 0, oldestPendingEventAt: nil, now: epoch,
                                           isBackgrounding: true))
    }
}
