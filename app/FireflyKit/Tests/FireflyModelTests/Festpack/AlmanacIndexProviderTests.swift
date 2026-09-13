//
//  AlmanacIndexProviderTests.swift — the Settings festival picker's
//  data source: live-fetch success, malformed entries skipped rather
//  than failing the whole index, and the bundled fallback so the
//  picker is never empty offline.
//
import Foundation
import XCTest
@testable import FireflyModel

private final class MockIndexFetcher: FestpackHTTPFetching, @unchecked Sendable {
    enum Behavior {
        case respond(Data)
        case fail
    }
    private let behavior: Behavior
    init(_ behavior: Behavior) { self.behavior = behavior }
    func fetch(_ url: URL, ifNoneMatch etag: String?) async throws -> FestpackHTTPResponse? {
        switch behavior {
        case .respond(let data): return FestpackHTTPResponse(body: data, etag: nil)
        case .fail: throw URLError(.notConnectedToInternet)
        }
    }
}

private struct MockBundleLoader: FestpackBundleLoading {
    var files: [String: Data] = [:]
    func festpackData(forResource name: String, extension ext: String) -> Data? {
        files["\(name).\(ext)"]
    }
}

final class AlmanacIndexProviderTests: XCTestCase {
    private let validIndexJSON = Data("""
    {"schema":"fest-almanac-index/1","generated":"2026-09-13T00:00:00Z",
     "packs":[
       {"slug":"lost-lands","year":2026,"name":"Lost Lands","start":"2026-09-18","end":"2026-09-20",
        "timezone":"America/New_York","path":"packs/lost-lands/2026/festpack.json","updated":"2026-09-01",
        "sha256":"abc123"},
       {"slug":"other-fest","year":2027,"name":"Other Fest","start":"2027-08-01T00:00:00Z","end":"2027-08-03T00:00:00Z",
        "path":"packs/other-fest/2027/festpack.json"}
     ]}
    """.utf8)

    func testLiveFetchDecodesEveryValidEntry() async {
        let provider = AlmanacIndexProvider(fetcher: MockIndexFetcher(.respond(validIndexJSON)), bundleLoader: MockBundleLoader())
        let index = await provider.fetchIndex()
        XCTAssertEqual(index.packs.count, 2)
        XCTAssertEqual(index.packs.map(\.slug).sorted(), ["lost-lands", "other-fest"])
        XCTAssertEqual(index.packs.first { $0.slug == "lost-lands" }?.sha256, "abc123")
        XCTAssertNil(index.packs.first { $0.slug == "other-fest" }?.sha256, "no sha256 in the source must decode as nil, never a fabricated one")
    }

    func testMalformedEntriesAreSkippedNotFatal() async {
        let json = Data("""
        {"schema":"fest-almanac-index/1","generated":"2026-09-13T00:00:00Z",
         "packs":[
           {"slug":"lost-lands","year":2026,"name":"Lost Lands","start":"2026-09-18","end":"2026-09-20",
            "path":"packs/lost-lands/2026/festpack.json"},
           {"slug":"missing-fields-only"},
           {"slug":"bad-year","year":"not-a-number","name":"Bad Year","start":"2026-01-01","end":"2026-01-02","path":"x"},
           {"slug":"bad-dates","year":2026,"name":"Bad Dates","start":"not-a-date","end":"also-not-a-date","path":"y"},
           "not even an object",
           null
         ]}
        """.utf8)
        let provider = AlmanacIndexProvider(fetcher: MockIndexFetcher(.respond(json)), bundleLoader: MockBundleLoader())
        let index = await provider.fetchIndex()
        XCTAssertEqual(index.packs.count, 1, "only the one well-formed entry should survive")
        XCTAssertEqual(index.packs.first?.slug, "lost-lands")
    }

    func testTotallyMalformedTopLevelJSONFallsBackToBundled() async {
        let bundled = MockBundleLoader(files: ["fest-almanac-index.json": validIndexJSON])
        let provider = AlmanacIndexProvider(fetcher: MockIndexFetcher(.respond(Data("not json".utf8))), bundleLoader: bundled)
        let index = await provider.fetchIndex()
        XCTAssertEqual(index.packs.count, 2, "a totally malformed document must fall through to the bundled index, not return zero silently")
    }

    func testNetworkFailureFallsBackToBundledIndex() async {
        let bundled = MockBundleLoader(files: ["fest-almanac-index.json": validIndexJSON])
        let provider = AlmanacIndexProvider(fetcher: MockIndexFetcher(.fail), bundleLoader: bundled)
        let index = await provider.fetchIndex()
        XCTAssertEqual(index.packs.count, 2)
    }

    func testNoNetworkAndNoBundleIsAnEmptyIndexNotACrash() async {
        let provider = AlmanacIndexProvider(fetcher: MockIndexFetcher(.fail), bundleLoader: MockBundleLoader())
        let index = await provider.fetchIndex()
        XCTAssertTrue(index.packs.isEmpty)
    }

    /// The bundled fallback the app actually ships
    /// (`firmware/assets/field/fest-almanac-index.json`) must itself
    /// decode and contain Lost Lands 2026 — this test reads the REAL
    /// file straight off disk, the same way `FestpackParserTests`
    /// exercises the real bundled festpack.
    /// "app: automatic almanac refresh + festival picker" — demo mode's
    /// index provider must NEVER touch the network (`DemoAlmanacIndexProvider`
    /// 's own header, mirroring `DemoFestpackProvider`) — proven here by
    /// injecting a bundle loader with the real fixture and NO fetcher at
    /// all (there is nowhere for one to plug in).
    func testDemoIndexProviderNeverTouchesNetworkAndReadsTheBundledFixture() async {
        let provider = DemoAlmanacIndexProvider(bundleLoader: MockBundleLoader(files: ["fest-almanac-index.json": validIndexJSON]))
        let index = await provider.fetchIndex()
        XCTAssertEqual(index.packs.count, 2)
    }

    func testDemoIndexProviderWithNoBundledFixtureIsAnEmptyIndexNotACrash() async {
        let provider = DemoAlmanacIndexProvider(bundleLoader: MockBundleLoader())
        let index = await provider.fetchIndex()
        XCTAssertTrue(index.packs.isEmpty)
    }

    /// Review finding: every entry in the LIVE index publishes bare
    /// dates ("2026-09-13"), and reading `end` as midnight made a
    /// festival un-current for the whole of its final day — measured
    /// against the live index on 2026-09-13, Sacred Acre 2026 (running
    /// that day) came back unmarked. Mutation check: dropping the
    /// `endOfDaySeconds` extension in `decodePack` fails this test.
    func testDateOnlyEndCoversTheWholeOfTheFestivalsFinalDay() throws {
        let data = Data(#"""
        {"schema":"fest-almanac-index/1","packs":[
          {"slug":"sacred-acre","year":2026,"name":"Sacred Acre","start":"2026-09-11","end":"2026-09-13",
           "timezone":"America/Anchorage","path":"packs/sacred-acre/2026/festpack.json"}]}
        """#.utf8)
        let index = try XCTUnwrap(AlmanacIndexProvider.decode(data))
        let pack = try XCTUnwrap(index.packs.first)
        let middayOnFinalDay = try XCTUnwrap(ISO8601DateFormatter().date(from: "2026-09-13T12:00:00Z"))
        XCTAssertTrue(pack.start <= middayOnFinalDay && middayOnFinalDay <= pack.end,
                      "a festival is still happening on its own last day")
        let dayAfter = try XCTUnwrap(ISO8601DateFormatter().date(from: "2026-09-14T00:30:00Z"))
        XCTAssertFalse(pack.start <= dayAfter && dayAfter <= pack.end,
                       "and is over the day after — the window is extended, not made open-ended")
    }

    /// A FULL timestamp is taken at its word — the end-of-day extension
    /// applies only to the date-only form.
    func testFullTimestampEndIsNotExtended() throws {
        let data = Data(#"""
        {"schema":"fest-almanac-index/1","packs":[
          {"slug":"precise","year":2026,"name":"Precise","start":"2026-09-11T18:00:00Z","end":"2026-09-13T04:00:00Z",
           "path":"packs/precise/2026/festpack.json"}]}
        """#.utf8)
        let index = try XCTUnwrap(AlmanacIndexProvider.decode(data))
        let pack = try XCTUnwrap(index.packs.first)
        XCTAssertEqual(pack.end, ISO8601DateFormatter().date(from: "2026-09-13T04:00:00Z"))
    }

    func testTheRealBundledIndexDecodesAndContainsLostLands2026() throws {
        // .../app/FireflyKit/Tests/FireflyModelTests/Festpack/<this file>
        // — same traversal `PicksCodecRealPackTests.repoRoot` uses.
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Festpack
            .deletingLastPathComponent() // FireflyModelTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .deletingLastPathComponent() // app
            .deletingLastPathComponent() // <repo root>
        let url = repoRoot.appending(path: "firmware/assets/field/fest-almanac-index.json")
        let data = try Data(contentsOf: url)
        let index = try XCTUnwrap(AlmanacIndexProvider.decode(data))
        XCTAssertEqual(index.packs.count, 1)
        XCTAssertEqual(index.packs.first?.slug, "lost-lands")
        XCTAssertEqual(index.packs.first?.year, 2026)
    }
}
