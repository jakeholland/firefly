//
//  InboxAvatarTests.swift — regression coverage for the "no
//  `Character("")` trap" avatar fix (PR #267 review, SHOULD-FIX item 2):
//  `InboxAvatar.avatarGlyph(for:)` (FireflyModel) must round-trip an
//  unnamed-but-paired member — `initial == nil`, `displayName.isEmpty`
//  (`ff_crew_member_t.initial`'s own "'\0' until known" precondition) —
//  without trapping on `Character("")`.
//
//  Owner decision, 2026-09-13 ("Nameless crew rows... a colour and
//  initial '?' — never a blank label") reverses the ORIGINAL fix's own
//  "blank, never '?'" choice: a blank glyph read as a broken row to
//  real reviewers (this exact case, demo mode's Mo), so the honest
//  "unknown" glyph is now "?", not a space.
//
import FireflyModel
import XCTest

// MARK: - InboxAvatar

@MainActor
final class InboxAvatarTests: XCTestCase {

    private func row(displayName: String, initial: Character? = nil) -> InboxConversationRow {
        InboxConversationRow(kind: .member(1), displayName: displayName, initial: initial)
    }

    /// The exact crash case this fix covers: paired via
    /// `ff_crew_set_paired`, no `NodeInfo` ever received — empty
    /// `displayName`, no `initial`. Must not trap; must read "?" (owner
    /// decision, 2026-09-13), never a blank cell that looks broken.
    func testUnnamedPairedMemberReadsQuestionMarkNotACrash() {
        XCTAssertEqual(InboxAvatar.avatarGlyph(for: row(displayName: "")), "?")
    }

    /// A known `initial` always wins over deriving one from the name.
    func testExplicitInitialWins() {
        XCTAssertEqual(InboxAvatar.avatarGlyph(for: row(displayName: "Dana", initial: "D")), "D")
    }

    /// No explicit `initial`, but a real name: falls back to the name's
    /// first character.
    func testFallsBackToFirstCharacterOfDisplayName() {
        XCTAssertEqual(InboxAvatar.avatarGlyph(for: row(displayName: "Taylor")), "T")
    }
}
