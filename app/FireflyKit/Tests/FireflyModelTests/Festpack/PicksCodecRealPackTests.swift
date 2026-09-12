//
//  PicksCodecRealPackTests.swift — byte-compatibility evidence against
//  the REAL bundled Lost Lands 2026 festpack, parsed by the app's own
//  `fp_parse` bridge rather than a hand-built fixture.
//
//  `PicksCodecTests` (next to this file) pins the codec's behaviour on
//  a small hand-built pack. This file closes the remaining gap: every
//  expected string below was produced by running settimes' OWN
//  TypeScript (`src/lib/festival.ts`'s `setId`/`shortCode`, bundled
//  with esbuild and executed under node) over
//  `firmware/assets/field/lost-lands-2026.festpack.json`, whose
//  `schedule`/`stages` arrays are byte-identical to settimes'
//  `data/lost-lands/2026.json`. The Swift side reaches the same
//  strings only by round-tripping that JSON through `fp_parse` — the
//  day/night fold, the >= 1440 after-midnight minute space and the
//  stage-id table included — so a match here is cross-language
//  evidence over real data, not a restatement of the Swift port.
//
//  Coverage chosen deliberately: an after-midnight set billed under
//  the previous night (Riot 02:05, Sippy 00:15, Secret Takeover
//  00:00), the one published-end set the TS suite itself exercises
//  (Excision 22:10-00:10), and artists whose names exercise every
//  branch of `slugifyArtist` — "&" ("JKYL & HYDE"), a comma ("Nikita,
//  The Wicked"), a trailing "!" ("HOL!"), a trailing "*" ("ROI*"), a
//  leading "$" ("$J", which slugs to a bare "j"), a "." ("Dr. Ushuu"),
//  a "+" ("Mefjus + Daxta MC"), a ":" plus b2b ("Aeon:Mode b2b
//  Blossom") and a b2b2b2b run ("Mega B2B2B2B Pre-Party").
//
import FireflyModel
import XCTest

final class PicksCodecRealPackTests: XCTestCase {
    /// .../app/FireflyKit/Tests/FireflyModelTests/Festpack/<this file>
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Festpack
            .deletingLastPathComponent() // FireflyModelTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .deletingLastPathComponent() // app
            .deletingLastPathComponent() // <repo root>
    }

    private func loadLostLands() throws -> Festpack {
        let url = repoRoot.appending(path: "firmware/assets/field/lost-lands-2026.festpack.json")
        switch FestpackParser.parse(try Data(contentsOf: url)) {
        case .success(let pack): return pack
        case .failure(let error): throw error
        }
    }

    /// (artist, stage id, expected settimes id, expected settimes code).
    /// Artist+stage is unique for every row below inside this pack
    /// except "Excision", who plays twice — hence the stage column.
    private static let expected: [(artist: String, stageID: String, id: String, code: String)] = [
        ("Nikita, The Wicked", "grove", "grove-2026-09-16-18:20-nikita-the-wicked", "7kjnfi"),
        ("Mega B2B2B2B Pre-Party", "prehistoric", "prehistoric-2026-09-17-20:00-mega-b2b2b2b-pre-party", "10x9v0t"),
        ("Tynan", "prehistoric", "prehistoric-2026-09-18-14:00-tynan", "uvfs9r"),
        ("JKYL & HYDE", "prehistoric", "prehistoric-2026-09-18-16:00-jkyl-and-hyde", "t8mzy8"),
        ("Dr. Ushuu", "forest", "forest-2026-09-18-16:00-dr-ushuu", "lseeix"),
        ("$J", "subsidia", "subsidia-2026-09-18-17:00-j", "9n8464"),
        ("HOL!", "prehistoric", "prehistoric-2026-09-18-19:00-hol", "muueq5"),
        ("Levity", "prehistoric", "prehistoric-2026-09-18-21:00-levity", "1230q9o"),
        // Published end (00:10 with an explicit `end_day`).
        ("Excision", "prehistoric", "prehistoric-2026-09-18-22:10-excision", "1jksh97"),
        // After midnight, billed under Friday night: the id carries
        // SATURDAY's calendar date, which is the whole point of the fold.
        ("Sippy", "wompy-woods", "wompy-woods-2026-09-19-00:15-sippy", "1izp4lt"),
        ("Yookie", "wompy-woods", "wompy-woods-2026-09-19-01:10-yookie", "1h6ic40"),
        ("Riot", "wompy-woods", "wompy-woods-2026-09-19-02:05-riot", "4yx2gt"),
        ("Oliverse", "wompy-woods", "wompy-woods-2026-09-19-03:00-oliverse", "17jhdwu"),
        ("Mefjus + Daxta MC", "forest", "forest-2026-09-19-22:00-mefjus-daxta-mc", "mvomki"),
        ("Aeon:Mode b2b Blossom", "forest", "forest-2026-09-19-23:00-aeon-mode-b2b-blossom", "7r1tt9"),
        ("ROI*", "subsidia", "subsidia-2026-09-20-18:00-roi", "p6k7h8"),
        ("Excision", "wompy-woods", "wompy-woods-2026-09-20-19:00-excision", "96j5tq"),
    ]

    func testEveryFixtureSetIDMatchesSettimesOverTheRealPack() throws {
        let pack = try loadLostLands()
        for row in Self.expected {
            let matches = pack.sets.filter { $0.artist == row.artist && $0.stageID == row.stageID }
            XCTAssertEqual(matches.count, 1, "\(row.artist) on \(row.stageID) is not unique in the real pack")
            let set = try XCTUnwrap(matches.first)
            XCTAssertEqual(PicksCodec.setID(for: set, in: pack), row.id, "set id for \(row.artist)")
            XCTAssertEqual(PicksCodec.shortCode(row.id), row.code, "share code for \(row.artist)")
        }
    }

    /// A whole share link, end to end: ids -> codes -> URL -> back to
    /// the same ids, over the real pack. The encoded string is the one
    /// settimes' own `encodePicks` produces for the same picks.
    func testSharePicksRoundTripsOverTheRealPack() throws {
        let pack = try loadLostLands()
        let ids = ["prehistoric-2026-09-18-21:00-levity",
                   "prehistoric-2026-09-18-22:10-excision",
                   "wompy-woods-2026-09-19-00:15-sippy",
                   "wompy-woods-2026-09-19-02:05-riot"]
        let encoded = PicksCodec.encodePicks(ids, in: pack)
        XCTAssertEqual(encoded, "1230q9o.1jksh97.1izp4lt.4yx2gt")

        let url = try XCTUnwrap(PicksCodec.shareURL(for: 261, pickIDs: ids, in: pack))
        XCTAssertEqual(url.absoluteString,
                       "https://settimes.kandiwooks.com/lost-lands/2026/fri?picks=1230q9o.1jksh97.1izp4lt.4yx2gt")

        // The link a phone hands out must import back into the phone.
        let parsed = try XCTUnwrap(PicksCodec.parsePicksInput(url.absoluteString))
        XCTAssertEqual(PicksCodec.decodePicks(parsed, in: pack), .init(ids: ids, dropped: 0))
    }

    /// Every night's share-URL day segment, against settimes' own
    /// `Night.slug` values for this pack (wed/thu/fri/sat/sun).
    func testWeekdaySlugsMatchSettimesNightSlugsForEveryNight() throws {
        let pack = try loadLostLands()
        let nights = Set(pack.sets.map(\.nightDayOfYear)).sorted()
        XCTAssertEqual(nights, [259, 260, 261, 262, 263])
        XCTAssertEqual(nights.map { PicksCodec.weekdaySlug(for: $0, in: pack) },
                       ["wed", "thu", "fri", "sat", "sun"])
    }

    /// No two sets in the real pack share a share code — settimes'
    /// `buildFestival` throws on a collision, so a pack that collides
    /// would break the website, not just this app.
    func testNoShareCodeCollisionsAcrossTheRealPack() throws {
        let pack = try loadLostLands()
        var byCode: [String: String] = [:]
        for set in pack.sets {
            let id = PicksCodec.setID(for: set, in: pack)
            let code = PicksCodec.shortCode(id)
            if let existing = byCode[code], existing != id {
                XCTFail("share-code collision \(code): \(existing) vs \(id)")
            }
            byCode[code] = id
        }
        XCTAssertEqual(Set(pack.sets.map { PicksCodec.setID(for: $0, in: pack) }).count, pack.sets.count)
    }

    /// The exact midnight boundary, which nothing above pins: a set
    /// starting at 00:00 is folded to `startMinute == 1440`, and its
    /// settimes id must carry the NEXT calendar day. The real pack has
    /// two of these ("Secret Takeover" on crater at 00:00 and 01:00,
    /// both billed under the Friday night), so this is live data, not a
    /// constructed edge. Expected strings from settimes' own `setId`/
    /// `shortCode` under node, as above.
    func testASetStartingExactlyAtMidnightTakesTheNextCalendarDay() throws {
        let pack = try loadLostLands()
        let midnight = pack.sets.first { $0.artist == "Secret Takeover" && $0.startMinute == 1440 }
        let oneAM = pack.sets.first { $0.artist == "Secret Takeover" && $0.startMinute == 1500 }
        XCTAssertEqual(PicksCodec.setID(for: try XCTUnwrap(midnight), in: pack),
                       "crater-2026-09-19-00:00-secret-takeover")
        XCTAssertEqual(PicksCodec.shortCode("crater-2026-09-19-00:00-secret-takeover"), "1a307xm")
        XCTAssertEqual(PicksCodec.setID(for: try XCTUnwrap(oneAM), in: pack),
                       "crater-2026-09-19-01:00-secret-takeover")
        XCTAssertEqual(PicksCodec.shortCode("crater-2026-09-19-01:00-secret-takeover"), "16sofuf")
    }
}
