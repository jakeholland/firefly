//
//  InboxAvatarTests.swift — regression coverage for the "no
//  `Character("")` trap" avatar fix (PR #267 review, SHOULD-FIX item 2):
//  `InboxAvatar.avatarGlyph(for:)` (FireflyModel) must round-trip an
//  unnamed-but-paired member — `initial == nil`, `displayName.isEmpty`
//  (`ff_crew_member_t.initial`'s own "'\0' until known" precondition) —
//  to a blank glyph instead of trapping on `Character("")`.
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
    /// `displayName`, no `initial`. Must not trap; must read blank, the
    /// same "no invented placeholder" rule as everywhere else an
    /// initial can be unknown, never a "?" stand-in.
    func testUnnamedPairedMemberReadsBlankNotACrash() {
        XCTAssertEqual(InboxAvatar.avatarGlyph(for: row(displayName: "")), " ")
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
