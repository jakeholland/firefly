//
//  CrewCopyTests.swift — PR #308 review. The Crew page and the Start
//  screen must speak PR #304's shipped presence vocabulary, not a
//  second copy of it (A02 §6.3's own table is the pre-#304 draft and is
//  superseded; a docs PR is bringing it in line).
//
//  These are DELEGATION tests on purpose. They do not re-type the
//  strings and then check `CrewCopy` returns them — that proxy passes
//  just as happily against a forked second copy of the vocabulary,
//  which is precisely the failure being guarded against. They assert
//  that `CrewCopy` produces the same value the shipped source of truth
//  produces, so the two cannot drift.
//
import FireflyCore
import FireflyModel
import XCTest

final class CrewCopyTests: XCTestCase {
    // MARK: - The mapping is total and order-preserving

    func testEveryHeardPresenceMapsToItsOwnPresenceTag() {
        XCTAssertEqual(CrewCopy.tag(for: .heard), .heard)
        XCTAssertEqual(CrewCopy.tag(for: .stale), .stale)
        XCTAssertEqual(CrewCopy.tag(for: .lost), .lost)
        // Core's "no packet ever received" is the phone's "paired, never
        // heard" — the one case where the two enums use different names
        // for the same state.
        XCTAssertEqual(CrewCopy.tag(for: .never), .linked)
    }

    func testMappingIsInjectiveAcrossEveryCase() {
        let tags = HeardPresence.allCases.map(CrewCopy.tag(for:))
        XCTAssertEqual(Set(tags).count, HeardPresence.allCases.count,
                       "two presence states must never collapse onto one label")
    }

    // MARK: - The labels ARE the shipped labels, not a second copy

    func testPresenceLabelDelegatesToTheShippedVocabulary() {
        for presence in HeardPresence.allCases {
            for ageMs: UInt32? in [nil, 0, 40_000, 6 * 60_000, 40 * 60_000, 3 * 3_600_000] {
                let age = ageMs.map { TimeInterval($0) / 1000 }
                XCTAssertEqual(CrewCopy.presenceLabel(presence, ageMs: ageMs),
                               CrewCopy.tag(for: presence).plainLabel(age: age),
                               "\(presence)/\(String(describing: ageMs)) drifted from PresenceTag")
            }
        }
    }

    /// The four examples the owner gave in PR #304, reached through
    /// `CrewCopy` — the words a reader actually sees on the Crew page.
    func testTheOwnersOwnExamplesRenderThroughCrewCopy() {
        XCTAssertEqual(CrewCopy.presenceLabel(.stale, ageMs: 6 * 60_000), "6 min ago")
        XCTAssertEqual(CrewCopy.presenceLabel(.lost, ageMs: 40 * 60_000), "No signal \u{00B7} 40 min")
        XCTAssertEqual(CrewCopy.presenceLabel(.never, ageMs: nil), "Paired \u{00B7} not seen yet")
        XCTAssertTrue(CrewCopy.presenceLabel(.heard, ageMs: 0).hasPrefix("HEARD"))
    }

    /// The words this PR shipped before review, all four of which the
    /// app had already stopped using everywhere else.
    func testTheSupersededDraftVocabularyIsGoneFromEveryState() {
        let retired = ["QUIET", "waiting to hear from them", "NAME?", "quiet for", "not heard"]
        for presence in HeardPresence.allCases {
            for ageMs: UInt32? in [nil, 0, 40_000, 6 * 60_000, 40 * 60_000] {
                let label = CrewCopy.presenceLabel(presence, ageMs: ageMs)
                for word in retired {
                    XCTAssertFalse(label.localizedCaseInsensitiveContains(word),
                                   "\(presence) still says \"\(word)\": \(label)")
                }
            }
        }
    }

    // MARK: - Nameless rows

    func testNamelessRowsUseTheOneSharedFallbackLiteral() {
        XCTAssertEqual(CrewCopy.displayName(nil), CrewDisplayFallback.namelessMember)
        XCTAssertEqual(CrewCopy.displayName(""), CrewDisplayFallback.namelessMember)
        XCTAssertEqual(CrewCopy.displayName("Taylor"), "Taylor")
    }

    /// A nil age is "we do not know", never "just now" — the honest-data
    /// rule applied to the one field that is easiest to fake.
    func testAnUnknownAgeNeverRendersAsJustNow() {
        XCTAssertFalse(CrewCopy.presenceLabel(.stale, ageMs: nil).contains("just now"))
        XCTAssertFalse(CrewCopy.presenceLabel(.lost, ageMs: nil).contains("just now"))
        XCTAssertFalse(CrewCopy.presenceLabel(.never, ageMs: nil).contains("just now"))
    }
}
