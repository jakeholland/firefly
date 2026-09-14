//
//  ChannelApplySummaryTests.swift — the "Apply channel" confirmation
//  sheet's own one-sentence `primaryText` (owner decision, 2026-09-13:
//  "primary text becomes one plain sentence ... with the index/
//  precision/preset/region lines behind a 'Technical details'
//  disclosure"). Run via `xcodebuild test -only-testing:FireflyAppTests`
//  (see project.yml) — `ChannelApplySummary` is app-target Swift, not
//  part of the FireflyKit SwiftPM package `swift test` covers.
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

final class ChannelApplySummaryTests: XCTestCase {
    /// The owner's own example sentence, crew name substituted in.
    func testPrimarySentenceNamesThePrimaryChannelAsTheCrew() throws {
        var primary = ChannelSettings()
        primary.name = "Firefly Fields"
        primary.moduleSettings.positionPrecision = 32

        let result = ChannelImportResult(channelSet: ChannelSet(settings: [primary]), addMode: false)
        let plan = try result.makeChannelWritePlan()
        let summary = ChannelApplySummary(plan: plan)

        XCTAssertEqual(summary.primarySentence,
                       "This puts you on the Firefly Fields crew. Your puck will blink off for a few "
                       + "seconds while it saves this, then reconnect on its own.")
    }

    /// An "add" plan writes only a secondary — there is no primary
    /// among `writtenChannels`, so the sentence falls back to the first
    /// (and, for this fixture, only) written channel's own name rather
    /// than silently naming nothing.
    func testPrimarySentenceFallsBackToFirstWrittenChannelWhenAddingASecondary() throws {
        var secondary = ChannelSettings()
        secondary.name = "Ops"
        secondary.moduleSettings.positionPrecision = 24

        let result = ChannelImportResult(channelSet: ChannelSet(settings: [secondary]), addMode: true)
        let plan = try result.makeChannelWritePlan(occupiedIndexes: [0])
        let summary = ChannelApplySummary(plan: plan)

        XCTAssertTrue(summary.primarySentence.hasPrefix("This puts you on the Ops crew."))
    }

    /// The disclosure content — index/precision/region/preset — never
    /// disappears; it just moves out of `primarySentence`.
    func testTechnicalDetailLinesStillCarryIndexAndPrecision() throws {
        var primary = ChannelSettings()
        primary.name = "Firefly"
        primary.moduleSettings.positionPrecision = 32

        let result = ChannelImportResult(channelSet: ChannelSet(settings: [primary]), addMode: false)
        let plan = try result.makeChannelWritePlan()
        let summary = ChannelApplySummary(plan: plan)

        XCTAssertEqual(summary.channelLines, ["Firefly (index 0, primary) — precision 32 bits"])
    }
}
