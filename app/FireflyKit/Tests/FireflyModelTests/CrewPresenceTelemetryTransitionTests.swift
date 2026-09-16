//
//  CrewPresenceTelemetryTransitionTests.swift — A04: the transition
//  table for `crew.member.seen`/`crew.member.lost`, pinned with no
//  radio, no `AppGraph`, and no real elapsed time — every case a member
//  can actually move through.
//
import FireflyModel
import XCTest

final class CrewPresenceTelemetryTransitionTests: XCTestCase {
    func testFirstEverHeardIsSeen() {
        XCTAssertEqual(CrewPresenceTelemetryTransition.decide(previous: nil, current: .heard), .seen)
    }

    func testReheardAfterLostIsSeen() {
        XCTAssertEqual(CrewPresenceTelemetryTransition.decide(previous: .lost, current: .heard), .seen)
    }

    func testCrossingIntoLostFromHeardIsLost() {
        XCTAssertEqual(CrewPresenceTelemetryTransition.decide(previous: .heard, current: .lost), .lost)
    }

    func testCrossingIntoLostFromStaleIsLost() {
        XCTAssertEqual(CrewPresenceTelemetryTransition.decide(previous: .stale, current: .lost), .lost)
    }

    /// The proxy check: a node-info REPLAY of an already-`.heard` member
    /// must not fire a second `seen` — the exact double-count the spec's
    /// "Known gaps" section calls out.
    func testStayingHeardIsNotATransition() {
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: .heard, current: .heard))
    }

    func testStayingLostIsNotATransition() {
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: .lost, current: .lost))
    }

    func testHeardToStaleIsNotATransitionEvent() {
        // .stale is still "has signal" — neither seen nor lost fires on
        // this edge; only crossing the LOST boundary itself does.
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: .heard, current: .stale))
    }

    func testStaleToHeardIsNotATransitionEvent() {
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: .stale, current: .heard))
    }

    func testFirstEverReadAsLostIsNotSeenOrLost() {
        // Cannot happen from a real admission (a member is always
        // admitted via a `.heard`-producing packet first), but the pure
        // function must still answer honestly rather than assume.
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: nil, current: .lost))
    }

    func testNeverToNeverIsNotATransition() {
        XCTAssertNil(CrewPresenceTelemetryTransition.decide(previous: .never, current: .never))
    }
}
