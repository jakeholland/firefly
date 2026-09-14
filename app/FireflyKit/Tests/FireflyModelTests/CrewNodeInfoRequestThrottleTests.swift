//
//  CrewNodeInfoRequestThrottleTests.swift — the pure rate-limit type
//  behind "ask a nameless crew member for their NodeInfo" (bench
//  finding 2026-09-14, `docs/specs/A02-crew-join.md` §4.4), tested with
//  no roster, no client and no clock beyond the `Date`s these tests
//  supply directly — mirrors firmware's own
//  `firmware/core/tests/test_nodeinfo_req.c` coverage of
//  `ff_nodeinfo_req_should_send`.
//
import FireflyModel
import Foundation
import XCTest

final class CrewNodeInfoRequestThrottleTests: XCTestCase {
    private let epoch = Date(timeIntervalSince1970: 1_780_000_000)

    func testFirstRequestForANewNodeIsDueImmediately() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
    }

    func testASecondRequestRightAfterTheFirstIsNotDue() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
        XCTAssertFalse(throttle.shouldSend(nodeID: 1001, now: epoch.addingTimeInterval(1)))
    }

    /// A false return touches no state — the FIRST call's record stands,
    /// so a request 9:59 after it is still refused.
    func testJustUnderTheTenMinuteWindowIsStillRefused() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
        let almostTenMinutes = epoch.addingTimeInterval(CrewNodeInfoRequestThrottle.rateLimit - 1)
        XCTAssertFalse(throttle.shouldSend(nodeID: 1001, now: almostTenMinutes))
    }

    /// Exactly `rateLimit` later is due again — the boundary is
    /// inclusive (`age < rateLimit` refuses; `age == rateLimit` does
    /// not), matching firmware's `ff_nodeinfo_req_should_send`'s own
    /// `age < FF_NODEINFO_REQ_RATE_LIMIT_MS` comparison.
    func testExactlyTenMinutesLaterIsDueAgain() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
        let tenMinutesLater = epoch.addingTimeInterval(CrewNodeInfoRequestThrottle.rateLimit)
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: tenMinutesLater))
    }

    /// A due request records a NEW timestamp, not the old one — a
    /// third request right after the second (now-due) one is refused
    /// again from the fresh mark.
    func testADueRequestResetsTheWindowFromItsOwnMoment() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
        let tenMinutesLater = epoch.addingTimeInterval(CrewNodeInfoRequestThrottle.rateLimit)
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: tenMinutesLater))
        XCTAssertFalse(throttle.shouldSend(nodeID: 1001, now: tenMinutesLater.addingTimeInterval(1)))
    }

    /// Distinct node ids never share a rate-limit window.
    func testDifferentNodeIDsAreThrottledIndependently() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertTrue(throttle.shouldSend(nodeID: 1001, now: epoch))
        XCTAssertTrue(throttle.shouldSend(nodeID: 1002, now: epoch))
        XCTAssertFalse(throttle.shouldSend(nodeID: 1001, now: epoch.addingTimeInterval(1)))
        XCTAssertFalse(throttle.shouldSend(nodeID: 1002, now: epoch.addingTimeInterval(1)))
    }

    /// `node_id == 0` is never a valid Meshtastic node id (the wire
    /// protocol reserves it as "unset") — refused, and touches no state.
    func testNodeIDZeroIsAlwaysRefusedAndRecordsNothing() {
        var throttle = CrewNodeInfoRequestThrottle()
        XCTAssertFalse(throttle.shouldSend(nodeID: 0, now: epoch))
        XCTAssertFalse(throttle.shouldSend(nodeID: 0, now: epoch.addingTimeInterval(1_000_000)))
    }
}
