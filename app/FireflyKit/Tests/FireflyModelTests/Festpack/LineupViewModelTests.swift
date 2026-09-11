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
        let model = LineupViewModel(festpackProvider: provider, starredStore: InMemoryStarredArtistsStore(), now: { farFutureNow })
        model.observe()
        // `observe()` kicks off two Tasks (the stream subscriber and
        // an initial refresh()); yield to let them run.
        try await Task.sleep(nanoseconds: 200_000_000)

        XCTAssertNotNil(model.selectedNightDayOfYear)
        XCTAssertEqual(model.selectedNightDayOfYear, model.nights.first)
        XCTAssertTrue(model.nights.contains(model.selectedNightDayOfYear!))
        // The honest consequence: a valid selected night, not a
        // dayLabel that clamps to an edge while nothing matches.
        XCTAssertNotNil(model.dayLabel)
    }

    func testTogglingStarUpdatesStarredRows() async throws {
        let pack = try loadDemoPack()
        guard let firstArtist = pack.sets.first?.artist else {
            throw XCTSkip("demo pack has no sets")
        }
        let provider = StaticFestpackProvider(pack)
        let model = LineupViewModel(festpackProvider: provider, starredStore: InMemoryStarredArtistsStore())
        model.observe()
        try await Task.sleep(nanoseconds: 200_000_000)

        XCTAssertFalse(model.isStarred(firstArtist))
        model.toggleStar(firstArtist)
        XCTAssertTrue(model.isStarred(firstArtist))
        XCTAssertTrue(model.starredRows.contains { $0.set.artist == firstArtist })
    }
}
