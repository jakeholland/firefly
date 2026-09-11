//
//  InboxAgeTests.swift — the age-rendering table every restored (and
//  live) message/row age label goes through (docs/specs/
//  A01-companion-app.md, M3: "age rendering table").
//
import FireflyModel
import XCTest

final class InboxAgeTests: XCTestCase {
    func testUnderAMinuteReadsNow() {
        XCTAssertEqual(InboxAge.short(0), "NOW")
        XCTAssertEqual(InboxAge.short(59), "NOW")
    }

    func testMinutesBoundary() {
        XCTAssertEqual(InboxAge.short(60), "1M")
        XCTAssertEqual(InboxAge.short(90), "1M")
        XCTAssertEqual(InboxAge.short(59 * 60), "59M")
    }

    func testHoursBoundary() {
        XCTAssertEqual(InboxAge.short(60 * 60), "1H")
        XCTAssertEqual(InboxAge.short(2 * 3600 + 59 * 60), "2H")
        XCTAssertEqual(InboxAge.short(23 * 3600 + 59 * 60), "23H")
    }

    func testDaysBoundary() {
        XCTAssertEqual(InboxAge.short(24 * 3600), "1D")
        XCTAssertEqual(InboxAge.short(2 * 24 * 3600 + 3600), "2D")
        // The "from storage · last seen 2 h ago" scale M3's own spec
        // language calls out, restated as a table entry so it stays
        // pinned as a real assertion rather than only a comment.
        XCTAssertEqual(InboxAge.short(2 * 3600), "2H")
    }

    func testNegativeIntervalNeverReadsAsNegative() {
        // A clock skew or a restored message whose statusAt is
        // momentarily "in the future" relative to `now` must never
        // render as a negative age — `max(0, ...)` is the guard.
        XCTAssertEqual(InboxAge.short(-5), "NOW")
    }

    func testMultiDayHistoryStillReadsInWholeDays() {
        XCTAssertEqual(InboxAge.short(10 * 24 * 3600), "10D")
    }
}
