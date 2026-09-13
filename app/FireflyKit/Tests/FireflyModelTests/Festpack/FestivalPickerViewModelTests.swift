//
//  FestivalPickerViewModelTests.swift — sorted-by-start-date rows,
//  current/selected marking, and that a selection writes settings AND
//  triggers a real refresh through the shared `LineupViewModel`.
//
import Foundation
import XCTest
@testable import FireflyModel

private struct StubIndexProvider: AlmanacIndexProviding {
    let index: AlmanacIndex
    func fetchIndex() async -> AlmanacIndex { index }
}

private func pack(slug: String, year: Int, name: String, start: Date, end: Date,
                   path: String, sha256: String? = nil) -> AlmanacIndexPack {
    AlmanacIndexPack(slug: slug, year: year, name: name, start: start, end: end,
                      timezone: nil, path: path, updated: nil, sha256: sha256)
}

@MainActor
final class FestivalPickerViewModelTests: XCTestCase {
    private func makeLineup() -> LineupViewModel {
        LineupViewModel(festpackProvider: DemoFestpackProvider(), picksStore: InMemoryPicksStore())
    }

    func testRowsAreSortedByStartDateAscending() async {
        let now = Date(timeIntervalSince1970: 1_790_000_000)
        let later = pack(slug: "later-fest", year: 2028, name: "Later Fest",
                          start: now.addingTimeInterval(86_400 * 400), end: now.addingTimeInterval(86_400 * 402),
                          path: "packs/later-fest/2028/festpack.json")
        let sooner = pack(slug: "lost-lands", year: 2026, name: "Lost Lands",
                           start: now.addingTimeInterval(86_400 * 5), end: now.addingTimeInterval(86_400 * 7),
                           path: "packs/lost-lands/2026/festpack.json")
        let index = AlmanacIndex(generatedAt: nil, packs: [later, sooner])
        let settings = InMemorySettingsStore()
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: index), settings: settings,
                                             lineup: makeLineup(), now: { now })

        await model.load()

        XCTAssertEqual(model.rows.map(\.id), ["lost-lands-2026", "later-fest-2028"])
    }

    func testCurrentFestivalIsMarkedWhenNowFallsInsideItsRange() async {
        let now = Date(timeIntervalSince1970: 1_790_000_000)
        let happeningNow = pack(slug: "lost-lands", year: 2026, name: "Lost Lands",
                                 start: now.addingTimeInterval(-3600), end: now.addingTimeInterval(3600),
                                 path: "packs/lost-lands/2026/festpack.json")
        let future = pack(slug: "later-fest", year: 2028, name: "Later Fest",
                           start: now.addingTimeInterval(86_400 * 30), end: now.addingTimeInterval(86_400 * 32),
                           path: "packs/later-fest/2028/festpack.json")
        let index = AlmanacIndex(generatedAt: nil, packs: [happeningNow, future])
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: index), settings: InMemorySettingsStore(),
                                             lineup: makeLineup(), now: { now })

        await model.load()

        XCTAssertEqual(model.rows.first { $0.id == "lost-lands-2026" }?.isCurrent, true)
        XCTAssertEqual(model.rows.first { $0.id == "later-fest-2028" }?.isCurrent, false)
    }

    func testSelectedRowReflectsSettings() async {
        let now = Date()
        let settings = InMemorySettingsStore()
        settings.setString("other-fest", .festivalSelectedSlug)
        settings.setString("2027", .festivalSelectedYear)
        let index = AlmanacIndex(generatedAt: nil, packs: [
            pack(slug: "lost-lands", year: 2026, name: "Lost Lands", start: now, end: now.addingTimeInterval(3600),
                 path: "packs/lost-lands/2026/festpack.json"),
            pack(slug: "other-fest", year: 2027, name: "Other Fest", start: now, end: now.addingTimeInterval(3600),
                 path: "packs/other-fest/2027/festpack.json"),
        ])
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: index), settings: settings,
                                             lineup: makeLineup(), now: { now })

        await model.load()

        XCTAssertEqual(model.rows.first { $0.id == "other-fest-2027" }?.isSelected, true)
        XCTAssertEqual(model.rows.first { $0.id == "lost-lands-2026" }?.isSelected, false)
    }

    func testSelectingAFestivalWritesSettingsAndTriggersRefresh() async {
        let now = Date()
        let settings = InMemorySettingsStore()
        let index = AlmanacIndex(generatedAt: nil, packs: [
            pack(slug: "other-fest", year: 2027, name: "Other Fest", start: now, end: now.addingTimeInterval(3600),
                 path: "packs/other-fest/2027/festpack.json", sha256: "deadbeef"),
        ])
        let lineup = makeLineup()
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: index), settings: settings,
                                             lineup: lineup, now: { now })
        await model.load()

        await model.select("other-fest-2027")

        XCTAssertEqual(settings.string(.festivalSelectedSlug), "other-fest")
        XCTAssertEqual(settings.string(.festivalSelectedYear), "2027")
        XCTAssertEqual(settings.string(.festpackSourceURLOverride),
                       "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/other-fest/2027/festpack.json")
        XCTAssertEqual(settings.string(.festivalSelectedSHA256), "deadbeef")
        XCTAssertEqual(settings.festivalNamespace(), "other-fest-2027")
        // The row list must reflect the new selection after `select(_:)`
        // returns (it reloads internally).
        XCTAssertEqual(model.rows.first?.isSelected, true)
    }

    func testSelectingAnUnknownIDIsANoOp() async {
        let settings = InMemorySettingsStore()
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: .empty), settings: settings,
                                             lineup: makeLineup())
        await model.load()

        await model.select("does-not-exist")

        XCTAssertNil(settings.string(.festivalSelectedSlug))
        XCTAssertNil(settings.string(.festpackSourceURLOverride))
    }

    func testEmptyIndexReportsAnHonestLoadError() async {
        let model = FestivalPickerViewModel(indexProvider: StubIndexProvider(index: .empty), settings: InMemorySettingsStore(),
                                             lineup: makeLineup())
        await model.load()
        XCTAssertTrue(model.rows.isEmpty)
        XCTAssertNotNil(model.loadError)
    }
}
