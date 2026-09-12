//
//  LineupViewModelTests.swift
//
//  Includes a regression case for a real bug caught against the actual
//  Firefly Fields demo pack (M4 screenshot session): its nights run
//  Sept 4-6, and the demo build's wall clock is the phone's REAL clock
//  (today, whatever that is) — so "which night does the wall clock
//  resolve to" can easily land outside every night the pack actually
//  has. The fix is `apply(_:)` falling back to the pack's own first
//  night rather than clamping to a "Day N of M" that highlights nothing.
//
import FireflyModel
import XCTest

@MainActor
final class LineupViewModelTests: XCTestCase {
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
    }

    private func loadDemoPack() throws -> Festpack {
        let url = repoRoot.appending(path: "firmware/assets/demo/firefly-fields.festpack.json")
        let data = try Data(contentsOf: url)
        guard case .success(let pack) = FestpackParser.parse(data) else {
            throw XCTSkip("demo fixture failed to parse")
        }
        return pack
    }

    private final class StaticFestpackProvider: FestpackProviding, @unchecked Sendable {
        let pack: Festpack
        init(_ pack: Festpack) { self.pack = pack }
        func current() async -> Festpack? { pack }
        func sourceState() async -> FestpackSourceState { .bundled }
        func festpackUpdates() -> AsyncStream<Festpack> {
            AsyncStream { continuation in
                continuation.yield(pack)
                continuation.finish()
            }
        }
        func refresh() async {}
    }

    /// Never a fixed sleep or a fixed-iteration poll budget (the house
    /// rule `ConnectSettingsViewModelTests.swift`'s own `eventually`
    /// documents at length) — polls until `condition` is true or
    /// `timeout` genuinely elapses.
    private func eventually(_ description: String = "condition", timeout: TimeInterval = 5,
                             file: StaticString = #filePath, line: UInt = #line,
                             _ condition: () -> Bool) async {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() {
            if Date() >= deadline {
                XCTFail("timed out after \(timeout)s waiting for \(description)", file: file, line: line)
                return
            }
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    func testWallClockOutsideEveryPackNightFallsBackToTheFirstNightNotAnInvalidOne() async throws {
        let pack = try loadDemoPack()
        let provider = StaticFestpackProvider(pack)
        // A fixed "now" far outside the demo pack's Sept 2026 dates —
        // the exact shape of the real bug (today's real wall clock vs.
        // a demo festival scheduled for a fixed date range).
        let farFutureNow: Date = {
            var components = DateComponents()
            components.year = 2027; components.month = 1; components.day = 1
            return Calendar(identifier: .gregorian).date(from: components) ?? Date()
        }()
        let model = LineupViewModel(festpackProvider: provider, picksStore: InMemoryPicksStore(), now: { farFutureNow })
        model.observe()
        await eventually("a selected night") { model.selectedNightDayOfYear != nil }

        XCTAssertEqual(model.selectedNightDayOfYear, model.nights.first)
        XCTAssertTrue(model.nights.contains(model.selectedNightDayOfYear!))
        // The honest consequence: a valid selected night, not a
        // dayLabel that clamps to an edge while nothing matches.
        XCTAssertNotNil(model.dayLabel)
    }

    func testTogglingPickUpdatesPickedGroups() async throws {
        let pack = try loadDemoPack()
        guard let firstSet = pack.sets.first else {
            throw XCTSkip("demo pack has no sets")
        }
        let provider = StaticFestpackProvider(pack)
        let model = LineupViewModel(festpackProvider: provider, picksStore: InMemoryPicksStore())
        model.observe()
        await eventually("festpack applied") { model.festpack != nil }

        XCTAssertFalse(model.isPicked(firstSet))
        model.togglePick(firstSet)
        XCTAssertTrue(model.isPicked(firstSet))
        XCTAssertTrue(model.pickedGroups.contains { $0.rows.contains { $0.set.id == firstSet.id } })

        model.togglePick(firstSet)
        XCTAssertFalse(model.isPicked(firstSet))
        XCTAssertFalse(model.pickedGroups.contains { $0.rows.contains { $0.set.id == firstSet.id } })
    }

    func testShareURLRoundTripsThroughImportOnTheSamePack() async throws {
        let pack = try loadDemoPack()
        guard pack.sets.count >= 2 else { throw XCTSkip("demo pack needs at least 2 sets") }
        let provider = StaticFestpackProvider(pack)
        let model = LineupViewModel(festpackProvider: provider, picksStore: InMemoryPicksStore())
        model.observe()
        await eventually("a selected night") { model.selectedNightDayOfYear != nil }

        let picks = Array(pack.sets.prefix(2))
        for set in picks { model.togglePick(set) }
        guard let url = model.shareURL else { return XCTFail("expected a share URL once picks exist") }

        // A second, independent view model — simulating a different
        // phone/session importing the link.
        let importer = LineupViewModel(festpackProvider: StaticFestpackProvider(pack), picksStore: InMemoryPicksStore())
        importer.observe()
        await eventually("importer festpack applied") { importer.festpack != nil }

        let result = importer.importPicks(from: url.absoluteString)
        XCTAssertEqual(result, .imported(count: 2, dropped: 0))
        for set in picks { XCTAssertTrue(importer.isPicked(set)) }
    }

    // MARK: - Pick conflicts

    /// A hand-built pack (not the demo fixture) so the exact abutting /
    /// overlapping boundary cases can be stated outright.
    private func makeConflictPack() -> Festpack {
        let stages = [FestpackStage(id: "a", name: "Stage A", colorRGB: 0xFFC66B),
                      FestpackStage(id: "b", name: "Stage B", colorRGB: 0x4FD8C4),
                      FestpackStage(id: "c", name: "Stage C", colorRGB: 0xB08CFF)]
        let sets = [
            FestpackScheduleSet(id: 0, artist: "Early", stageID: "a", nightDayOfYear: 261,
                                startMinute: 20 * 60, endMinute: 21 * 60, note: ""),
            // Starts exactly when "Early" ends: abutting, NOT a clash.
            FestpackScheduleSet(id: 1, artist: "Abutting", stageID: "a", nightDayOfYear: 261,
                                startMinute: 21 * 60, endMinute: 22 * 60, note: ""),
            // Straddles the boundary: clashes with BOTH of the above.
            FestpackScheduleSet(id: 2, artist: "Straddler", stageID: "b", nightDayOfYear: 261,
                                startMinute: 20 * 60 + 30, endMinute: 21 * 60 + 30, note: ""),
            // No published end, nothing after it on stage C -> the
            // 60-minute default, which is what puts it over "Straddler".
            FestpackScheduleSet(id: 3, artist: "Openended", stageID: "c", nightDayOfYear: 261,
                                startMinute: 20 * 60, endMinute: nil, note: ""),
        ]
        return Festpack(name: "Conflict Test", year: 2026, startDayOfYear: 261, endDayOfYear: 261,
                        utcOffsetMinutes: -240, utcOffsetAssumed: false, originKnown: false,
                        originApproximate: false, stages: stages, sets: sets,
                        features: [], landmarks: [], meta: .empty)
    }

    private func conflictModel(picking artists: [String]) async -> LineupViewModel {
        let pack = makeConflictPack()
        let model = LineupViewModel(festpackProvider: StaticFestpackProvider(pack),
                                    picksStore: InMemoryPicksStore())
        model.observe()
        await eventually("festpack applied") { model.festpack != nil }
        for set in pack.sets where artists.contains(set.artist) { model.togglePick(set) }
        return model
    }

    private func conflicts(_ model: LineupViewModel, of artist: String) -> [String] {
        model.pickedGroups.flatMap(\.rows).first { $0.set.artist == artist }?.conflictsWithArtists.sorted() ?? []
    }

    func testAbuttingPicksDoNotCountAsAConflictButOverlappingOnesDo() async {
        let model = await conflictModel(picking: ["Early", "Abutting", "Straddler"])
        // Half-open intervals: 20:00-21:00 and 21:00-22:00 touch, they
        // do not overlap.
        XCTAssertEqual(conflicts(model, of: "Early"), ["Straddler"])
        XCTAssertEqual(conflicts(model, of: "Abutting"), ["Straddler"])
        XCTAssertEqual(conflicts(model, of: "Straddler"), ["Abutting", "Early"])
    }

    func testTwoAbuttingPicksAloneReportNoConflictAtAll() async {
        let model = await conflictModel(picking: ["Early", "Abutting"])
        XCTAssertEqual(conflicts(model, of: "Early"), [])
        XCTAssertEqual(conflicts(model, of: "Abutting"), [])
        XCTAssertTrue(model.pickedGroups.flatMap(\.rows).allSatisfy { !$0.conflictsAreInferred })
    }

    /// A clash that only exists because of an INFERRED end is flagged
    /// as inferred, so the screen can say "may overlap" rather than
    /// asserting a collision the festpack never published.
    func testAConflictDecidedByAnInferredEndIsMarkedInferred() async {
        let model = await conflictModel(picking: ["Openended", "Straddler"])
        XCTAssertEqual(conflicts(model, of: "Openended"), ["Straddler"])
        let openEnded = model.pickedGroups.flatMap(\.rows).first { $0.set.artist == "Openended" }
        XCTAssertEqual(openEnded?.conflictsAreInferred, true)
        // The other side of the same pair is inferred too: one unknown
        // end makes the whole comparison an inference.
        let straddler = model.pickedGroups.flatMap(\.rows).first { $0.set.artist == "Straddler" }
        XCTAssertEqual(straddler?.conflictsWithArtists, ["Openended"])
        XCTAssertEqual(straddler?.conflictsAreInferred, true)
    }

    func testPublishedOnlyConflictIsNotMarkedInferred() async {
        let model = await conflictModel(picking: ["Early", "Straddler"])
        XCTAssertTrue(model.pickedGroups.flatMap(\.rows).allSatisfy { $0.conflictsAreInferred == false })
    }

    func testEffectiveEndCarriesProvenanceForTheDetailSheet() async {
        let model = await conflictModel(picking: [])
        let pack = try? XCTUnwrap(model.festpack)
        guard let pack else { return XCTFail("no pack") }
        let published = pack.sets.first { $0.artist == "Early" }!
        let inferred = pack.sets.first { $0.artist == "Openended" }!
        XCTAssertEqual(model.effectiveEnd(for: published), .init(minute: 21 * 60, source: .published))
        XCTAssertEqual(model.effectiveEnd(for: inferred), .init(minute: 21 * 60, source: .defaultLength))
    }
}
