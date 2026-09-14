//
//  CrewSettingsViewModelRowTests.swift — `CrewSettingsViewModel.Row
//  .displayName`'s fallback ladder (owner decision, 2026-09-13:
//  "Nameless crew rows... shows 'New crew member'... never a blank
//  label"). Run via `xcodebuild test -only-testing:FireflyAppTests`
//  (see project.yml) — app-target Swift.
//
import XCTest

final class CrewSettingsViewModelRowTests: XCTestCase {
    private func row(meshName: String, nickname: String?) -> CrewSettingsViewModel.Row {
        CrewSettingsViewModel.Row(id: 1, meshName: meshName, nickname: nickname, colorIndex: 0, initial: nil)
    }

    func testNicknameWinsOverEverything() {
        XCTAssertEqual(row(meshName: "Taylor", nickname: "T-Dawg").displayName, "T-Dawg")
    }

    func testMeshNameWinsWhenNoNickname() {
        XCTAssertEqual(row(meshName: "Taylor", nickname: nil).displayName, "Taylor")
    }

    /// The exact "paired, never named" state — never the raw `!nodeid`
    /// hex, which reads as an error code, not a person.
    func testNeitherNicknameNorMeshNameReadsAsNewCrewMember() {
        XCTAssertEqual(row(meshName: "", nickname: nil).displayName, "New crew member")
    }

    func testEmptyNicknameFallsThroughRatherThanShowingBlank() {
        XCTAssertEqual(row(meshName: "", nickname: "").displayName, "New crew member")
    }
}
