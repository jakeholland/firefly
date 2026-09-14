//
//  PresenceWordsTests.swift — the plain-language presence vocabulary
//  (owner decision, 2026-09-13: "Presence/status words everywhere they
//  appear"). Pins `PresenceAge.words`/`.ago` and
//  `PresenceTag.plainLabel(age:)` against the owner's own examples —
//  "No signal · 40 min", "6 min ago", "Paired · not seen yet" — and
//  `InboxDisplayName.label(for:)` against the "never a blank label"
//  rule.
//
import FireflyModel
import XCTest

final class PresenceAgeTests: XCTestCase {
    func testUnderAMinuteReadsJustNow() {
        XCTAssertEqual(PresenceAge.words(0), "just now")
        XCTAssertEqual(PresenceAge.words(59), "just now")
    }

    func testMinutes() {
        XCTAssertEqual(PresenceAge.words(60), "1 min")
        XCTAssertEqual(PresenceAge.words(6 * 60), "6 min")
        XCTAssertEqual(PresenceAge.words(40 * 60), "40 min")
    }

    func testHoursAndDays() {
        XCTAssertEqual(PresenceAge.words(2 * 3600), "2 hr")
        // PR #304 review: "3 day" is not English, on a screen whose
        // entire point is plain English. "min"/"hr" are abbreviations
        // and stay singular; "day" is a whole word.
        XCTAssertEqual(PresenceAge.words(24 * 3600), "1 day")
        XCTAssertEqual(PresenceAge.words(3 * 24 * 3600), "3 days")
    }

    /// The owner's own example: "STALE 6M" -> "6 min ago".
    func testAgoGluesAgoOntoEveryWordButJustNow() {
        XCTAssertEqual(PresenceAge.ago(6 * 60), "6 min ago")
        XCTAssertEqual(PresenceAge.ago(0), "just now")
    }
}

final class PresenceTagPlainLabelTests: XCTestCase {
    /// "keep LIVE/HERE style for fresh" — HEARD is untouched.
    func testHeardKeepsItsOwnWordPlusAge() {
        XCTAssertEqual(PresenceTag.heard.plainLabel(age: 6 * 60), "HEARD 6M")
    }

    /// The owner's own example: "STALE 6M" -> just the age, "6 min ago".
    func testStaleDropsItsWordAndShowsOnlyTheAge() {
        XCTAssertEqual(PresenceTag.stale.plainLabel(age: 6 * 60), "6 min ago")
    }

    /// The owner's own example: "No signal · 40 min" — age is required.
    func testLostAlwaysCarriesAnAge() {
        XCTAssertEqual(PresenceTag.lost.plainLabel(age: 40 * 60), "No signal \u{00B7} 40 min")
    }

    func testLinkedNeverHasAnAgeToShow() {
        XCTAssertEqual(PresenceTag.linked.plainLabel(age: nil), "Paired \u{00B7} not seen yet")
    }
}

final class InboxDisplayNameTests: XCTestCase {
    private func row(displayName: String) -> InboxConversationRow {
        InboxConversationRow(kind: .member(1), displayName: displayName)
    }

    /// Owner decision, 2026-09-13: "any row without a name shows 'New
    /// crew member'... never a blank label."
    func testEmptyDisplayNameReadsAsNewCrewMember() {
        XCTAssertEqual(InboxDisplayName.label(for: row(displayName: "")), "New crew member")
    }

    func testRealNamePassesThroughUnchanged() {
        XCTAssertEqual(InboxDisplayName.label(for: row(displayName: "Taylor")), "Taylor")
    }
}
