//
//  LineupGridLayoutTests.swift — time axis, block geometry, and
//  end-time inference for the by-stage grid.
//
import FireflyModel
import XCTest

final class LineupGridLayoutTests: XCTestCase {
    private let stageA = FestpackStage(id: "a", name: "Stage A", colorRGB: 0xFFC66B)
    private let stageB = FestpackStage(id: "b", name: "Stage B", colorRGB: 0x4FD8C4)

    private func set(_ id: Int, _ artist: String, _ stageID: String?, start: Int?, end: Int?) -> FestpackScheduleSet {
        FestpackScheduleSet(id: id, artist: artist, stageID: stageID, nightDayOfYear: 1,
                             startMinute: start, endMinute: end, note: "")
    }

    func testAxisSpansEarliestStartToLatestEffectiveEndAtQuarterHourResolution() {
        let daySets = [
            set(0, "A1", "a", start: 19 * 60, end: 20 * 60),         // 19:00-20:00
            set(1, "A2", "a", start: 20 * 60, end: 21 * 60 + 30),    // 20:00-21:30
            set(2, "B1", "b", start: 19 * 60 + 30, end: nil),        // 19:30, no end -> infers 21:00 (next on B)
            set(3, "B2", "b", start: 21 * 60, end: 22 * 60),         // 21:00-22:00
            set(4, "B3", "b", start: 22 * 60, end: 23 * 60),         // 22:00-23:00 (last on B)
        ]
        let layout = LineupGridLayout.build(daySets: daySets, stages: [stageA, stageB])

        XCTAssertEqual(layout.axisStartMinute, 19 * 60)
        XCTAssertEqual(layout.axisEndMinute, 23 * 60)
        XCTAssertEqual(layout.hourLines.map(\.offsetMinutes), [0, 60, 120, 180, 240])
        XCTAssertEqual(layout.hourLines.map(\.label), ["7 PM", "8 PM", "9 PM", "10 PM", "11 PM"])

        XCTAssertEqual(layout.columns.map(\.id), ["a", "b"])
        let columnA = layout.columns[0]
        XCTAssertEqual(columnA.blocks.map(\.offsetMinutes), [0, 60])
        XCTAssertEqual(columnA.blocks.map(\.durationMinutes), [60, 90])

        let columnB = layout.columns[1]
        XCTAssertEqual(columnB.blocks.map(\.set.artist), ["B1", "B2", "B3"])
        // B1 has no published end -> infers the next B set's start (21:00).
        XCTAssertEqual(columnB.blocks[0].offsetMinutes, 30)
        XCTAssertEqual(columnB.blocks[0].durationMinutes, 90) // 21:00 - 19:30
        XCTAssertEqual(columnB.blocks[1].offsetMinutes, 120)
        XCTAssertEqual(columnB.blocks[1].durationMinutes, 60)
        XCTAssertEqual(columnB.blocks[2].offsetMinutes, 180)
        XCTAssertEqual(columnB.blocks[2].durationMinutes, 60)
    }

    func testLastSetOnAStageWithNoEndAndNoFollowingSetGetsTheDefaultHour() {
        let daySets = [set(0, "Solo", "a", start: 19 * 60, end: nil)]
        let layout = LineupGridLayout.build(daySets: daySets, stages: [stageA])
        XCTAssertEqual(layout.columns[0].blocks[0].durationMinutes, LineupTimeInference.defaultSetMinutes)
        XCTAssertEqual(layout.axisEndMinute, 20 * 60) // 19:00 + 60 default minutes
    }

    func testUnknownStartSetsAreExcludedFromTheAxisEntirely() {
        let daySets = [
            set(0, "Known", "a", start: 19 * 60, end: 20 * 60),
            set(1, "TBD", "a", start: nil, end: nil),
        ]
        let layout = LineupGridLayout.build(daySets: daySets, stages: [stageA])
        XCTAssertEqual(layout.columns[0].blocks.count, 1)
        XCTAssertEqual(layout.columns[0].blocks[0].set.artist, "Known")
    }

    func testASetOnAnUnrecognisedStageStillGetsAnHonestColumnRatherThanBeingDropped() {
        let daySets = [set(0, "Mystery", "ghost-stage", start: 19 * 60, end: 20 * 60)]
        let layout = LineupGridLayout.build(daySets: daySets, stages: [stageA])
        XCTAssertEqual(layout.columns.count, 1)
        XCTAssertNil(layout.columns[0].stage)
        XCTAssertEqual(layout.columns[0].blocks[0].set.artist, "Mystery")
    }

    func testEmptyNightProducesAnEmptyAxisRatherThanACrash() {
        let layout = LineupGridLayout.build(daySets: [], stages: [stageA])
        XCTAssertTrue(layout.columns.isEmpty)
        XCTAssertTrue(layout.hourLines.isEmpty)
        XCTAssertEqual(layout.axisLengthMinutes, 0)
        XCTAssertNil(layout.nowOffsetMinutes(nowMinute: 19 * 60))
    }

    func testNowOffsetIsNilOutsideTheAxisAndAnOffsetInsideIt() {
        let daySets = [set(0, "A1", "a", start: 19 * 60, end: 21 * 60)]
        let layout = LineupGridLayout.build(daySets: daySets, stages: [stageA])
        XCTAssertNil(layout.nowOffsetMinutes(nowMinute: 18 * 60)) // before the axis
        XCTAssertNil(layout.nowOffsetMinutes(nowMinute: 22 * 60)) // after the axis
        XCTAssertEqual(layout.nowOffsetMinutes(nowMinute: 19 * 60 + 30), 30)
    }
}
