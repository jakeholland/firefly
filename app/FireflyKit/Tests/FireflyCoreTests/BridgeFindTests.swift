//
//  BridgeFindTests.swift — FindBridge (ff_find, S29 FIND mode) through
//  the Swift bridge: ping cadence, the session cap, and the
//  warmer/colder haptic crossing.
//
@testable import FireflyModel
import XCTest

final class BridgeFindTests: XCTestCase {

    /// The very first tick after `start()` sends immediately (no 10s
    /// wait for the first ping of a session), and a second tick inside
    /// the same 10s window does not.
    func testPingCadenceIsRateLimitedRegardlessOfCallFrequency() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)

        guard case .sendPing = find.tick(now: 0) else {
            return XCTFail("the first tick after start() must send immediately")
        }
        XCTAssertEqual(find.tick(now: 1), .none, "well under the 10s floor")
        XCTAssertEqual(find.tick(now: 9_999), .none, "still under the 10s floor")
        guard case .sendPing = find.tick(now: 10_000) else {
            return XCTFail("exactly one interval later must send again")
        }
    }

    func testSessionCapsAtThirtyPings() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)
        var sent = 0
        for i in 0..<40 {
            if case .sendPing = find.tick(now: UInt32(i) * 10_000) { sent += 1 }
        }
        XCTAssertEqual(sent, 30)
        XCTAssertFalse(find.isActive, "the session must auto-stop once its own cap is reached")
    }

    func testStartOnANewTargetCancelsThePriorSessionOutright() {
        let find = FindBridge()
        find.start(targetNodeID: 1, now: 0)
        find.start(targetNodeID: 2, now: 0)
        XCTAssertEqual(find.targetNodeID, 2)
    }

    func testNoTheirReadingUntilTheFirstPong() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)
        XCTAssertNil(find.theirReading)
    }

    func testPongFromTheWrongNodeIsIgnored() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)
        guard case .sendPing(let nonce) = find.tick(now: 0) else { return XCTFail("expected a ping") }
        let haptic = find.onPong(fromNodeID: 999, nonce: nonce, rssiDbm: -70, snrDb: nil, now: 100)
        XCTAssertEqual(haptic, .none)
        XCTAssertNil(find.theirReading)
    }

    func testPongWithMatchingNonceRecordsTheirReading() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)
        guard case .sendPing(let nonce) = find.tick(now: 0) else { return XCTFail("expected a ping") }
        _ = find.onPong(fromNodeID: 42, nonce: nonce, rssiDbm: -70, snrDb: 4.5, now: 100)
        XCTAssertEqual(find.theirReading?.rssiDbm, -70)
        XCTAssertEqual(find.theirReading?.snrDb, 4.5)
    }

    func testStopEndsTheSessionAndFurtherTicksAreNoOps() {
        let find = FindBridge()
        find.start(targetNodeID: 42, now: 0)
        find.stop()
        XCTAssertFalse(find.isActive)
        XCTAssertEqual(find.tick(now: 0), .none)
    }
}
