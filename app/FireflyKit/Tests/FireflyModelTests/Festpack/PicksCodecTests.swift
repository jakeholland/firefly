//
//  PicksCodecTests.swift — byte-compatibility fixtures for
//  `PicksCodec`, ported from settimes' own `src/lib/festival.test.ts`
//  ("share codes" describe block) and `src/lib/festival.ts`'s
//  `buildFestival`/`setId`/`shortCode`.
//
//  The fixture below is a hand-built `Festpack` covering four REAL
//  Lost Lands 2026 schedule entries (github.com/jakeholland/settimes'
//  own `data/lost-lands/2026.json`, the exact rows
//  `festival.test.ts`'s "bills an after-midnight set under the previous
//  night" and "uses the published end..." cases exercise) — Tynan,
//  Levity, Excision and Riot on Friday 2026-09-18/19. Every expected
//  id/code string below was computed INDEPENDENTLY via `node`, running
//  settimes' own `setId`/`shortCode`/`slugifyArtist` against that same
//  JSON (not copy-pasted from a Swift run), so a match here is real
//  evidence of byte compatibility, not a tautology:
//
//    node -e "<setId/shortCode ported inline>" against
//    data/lost-lands/2026.json → uvfs9r, 1230q9o, 1jksh97, 4yx2gt
//
//  Friday 2026-09-18 is day-of-year 261 (2026 is not a leap year);
//  2026-09-19 is 262 — plain Gregorian arithmetic, checked against
//  `datetime.date(2026,9,18).timetuple().tm_yday` in Python.
//
import FireflyModel
import XCTest

final class PicksCodecTests: XCTestCase {
    // MARK: - Fixture: a hand-built Lost Lands 2026 Friday slice

    private let prehistoric = FestpackStage(id: "prehistoric", name: "Prehistoric Stage", colorRGB: 0xFFC66B)
    private let wompyWoods = FestpackStage(id: "wompy-woods", name: "Wompy Woods", colorRGB: 0x4FD8C4)

    private static let fridayDOY = 261 // 2026-09-18
    private static let saturdayDOY = 262 // 2026-09-19

    private func makeSet(id: Int, artist: String, stageID: String, night: Int = fridayDOY,
                         start: Int?, end: Int?, note: String = "") -> FestpackScheduleSet {
        FestpackScheduleSet(id: id, artist: artist, stageID: stageID, nightDayOfYear: night,
                             startMinute: start, endMinute: end, note: note)
    }

    private func makePack(sets: [FestpackScheduleSet]) -> Festpack {
        Festpack(name: "Lost Lands", year: 2026, startDayOfYear: 261, endDayOfYear: 263,
                  utcOffsetMinutes: -240, utcOffsetAssumed: false, originKnown: false, originApproximate: false,
                  stages: [prehistoric, wompyWoods], sets: sets, features: [], landmarks: [], meta: .empty)
    }

    private var tynan: FestpackScheduleSet { makeSet(id: 0, artist: "Tynan", stageID: "prehistoric", start: 14 * 60, end: nil) }
    private var levity: FestpackScheduleSet { makeSet(id: 1, artist: "Levity", stageID: "prehistoric", start: 21 * 60, end: nil) }
    /// Excision 22:10 -> 00:10 the next day: start 1330, end folded
    /// forward one day, 1450 (1440 + 10) — matches `festival.test.ts`'s
    /// own "runs for 120 minutes" assertion (1450 - 1330 == 120).
    private var excision: FestpackScheduleSet {
        makeSet(id: 2, artist: "Excision", stageID: "prehistoric", start: 22 * 60 + 10, end: 1440 + 10, note: "2 hour set")
    }
    /// Riot 02:05 on the calendar day AFTER the Friday night — folded
    /// to `startMinute = 1440 + 125 = 1565` in the SAME night's minute
    /// space, per S05's after-midnight fold.
    private var riot: FestpackScheduleSet { makeSet(id: 3, artist: "Riot", stageID: "wompy-woods", start: 1440 + 2 * 60 + 5, end: nil) }
    /// A synthetic (non-Lost-Lands) TBD set — no published start —
    /// exercising the "tba" branch of `setId`/`setID(for:in:)`.
    private var mysteryB2B: FestpackScheduleSet { makeSet(id: 4, artist: "Mystery B2B", stageID: "prehistoric", start: nil, end: nil) }

    // MARK: - setID / shortCode

    func testSetIDsMatchSettimesStageDayStartArtistFormat() {
        let pack = makePack(sets: [tynan, levity, excision, riot, mysteryB2B])
        XCTAssertEqual(PicksCodec.setID(for: tynan, in: pack), "prehistoric-2026-09-18-14:00-tynan")
        XCTAssertEqual(PicksCodec.setID(for: levity, in: pack), "prehistoric-2026-09-18-21:00-levity")
        XCTAssertEqual(PicksCodec.setID(for: excision, in: pack), "prehistoric-2026-09-18-22:10-excision")
        // The after-midnight fold: billed under Friday night, but its
        // OWN id carries Saturday's calendar date, matching the raw
        // pack's `day: "2026-09-19"` field settimes' `setId` reads.
        XCTAssertEqual(PicksCodec.setID(for: riot, in: pack), "wompy-woods-2026-09-19-02:05-riot")
        XCTAssertEqual(PicksCodec.setID(for: mysteryB2B, in: pack), "prehistoric-2026-09-18-tba-mystery-b2b")
    }

    func testShortCodesMatchSettimesFNV1aBase36ExactlyForRealLostLandsSets() {
        // Computed independently via node against settimes' own
        // shortCode/setId — see this file's header comment.
        XCTAssertEqual(PicksCodec.shortCode("prehistoric-2026-09-18-14:00-tynan"), "uvfs9r")
        XCTAssertEqual(PicksCodec.shortCode("prehistoric-2026-09-18-21:00-levity"), "1230q9o")
        XCTAssertEqual(PicksCodec.shortCode("prehistoric-2026-09-18-22:10-excision"), "1jksh97")
        XCTAssertEqual(PicksCodec.shortCode("wompy-woods-2026-09-19-02:05-riot"), "4yx2gt")
        XCTAssertEqual(PicksCodec.shortCode("prehistoric-2026-09-18-tba-mystery-b2b"), "c21kld")
        // shortCode is a pure function of the string — stable across calls.
        XCTAssertEqual(PicksCodec.shortCode("a"), PicksCodec.shortCode("a"))
    }

    func testSlugifyArtistMatchesSettimesRegexBehavior() {
        XCTAssertEqual(PicksCodec.slugifyArtist("Sullivan King b2b Ray Volpe"), "sullivan-king-b2b-ray-volpe")
        XCTAssertEqual(PicksCodec.slugifyArtist("Sippy, the Duo"), "sippy-the-duo")
        XCTAssertEqual(PicksCodec.slugifyArtist("JKYL & HYDE"), "jkyl-and-hyde")
        XCTAssertEqual(PicksCodec.slugifyArtist("--Weird__Name--"), "weird-name")
    }

    // MARK: - encodePicks / decodePicks (settimes: "share codes")

    func testRoundTripsPicksThroughTheURLFormAndDropsUnknownCodes() {
        let pack = makePack(sets: [tynan, levity, excision, riot])
        let ids = [tynan, levity, excision].map { PicksCodec.setID(for: $0, in: pack) }
        let encoded = PicksCodec.encodePicks(ids, in: pack)
        XCTAssertEqual(encoded, "uvfs9r.1230q9o.1jksh97")

        let result = PicksCodec.decodePicks(encoded + ".nope", in: pack)
        XCTAssertEqual(result.ids, ids)
        XCTAssertEqual(result.dropped, 1)
        XCTAssertEqual(PicksCodec.decodePicks(nil, in: pack), .init(ids: [], dropped: 0))
    }

    func testReportsALinkWhoseCodesHaveAllGoneStale() {
        let pack = makePack(sets: [tynan])
        XCTAssertEqual(PicksCodec.decodePicks("bogus1.bogus2", in: pack), .init(ids: [], dropped: 2))
        // An empty param is not a stale link; it must not report dropped.
        XCTAssertEqual(PicksCodec.decodePicks("", in: pack), .init(ids: [], dropped: 0))
    }

    func testCountsDroppedCodesOnceEachIgnoringEmptyStringsAndRepeats() {
        let pack = makePack(sets: [tynan])
        let code = PicksCodec.shortCode(PicksCodec.setID(for: tynan, in: pack))
        let result = PicksCodec.decodePicks("\(code).bogus1.bogus1.bogus2..\(code)", in: pack)
        XCTAssertEqual(result.ids, [PicksCodec.setID(for: tynan, in: pack)])
        XCTAssertEqual(result.dropped, 2)
    }

    func testEncodePicksSkipsAnIDNoLongerInThePack() {
        let pack = makePack(sets: [tynan]) // levity's id is not in this pack
        let encoded = PicksCodec.encodePicks(["prehistoric-2026-09-18-21:00-levity",
                                               PicksCodec.setID(for: tynan, in: pack)], in: pack)
        XCTAssertEqual(encoded, "uvfs9r")
    }

    // MARK: - parsePicksInput (settimes: "pulls pick codes out of anything a user might paste")

    func testPullsPickCodesOutOfAnythingAUserMightPaste() {
        let pack = makePack(sets: [tynan, levity])
        let encoded = PicksCodec.encodePicks([tynan, levity].map { PicksCodec.setID(for: $0, in: pack) }, in: pack)

        XCTAssertEqual(PicksCodec.parsePicksInput("https://settimes.kandiwooks.com/lost-lands/2026/fri?picks=\(encoded)"), encoded)
        // Extra params either side must not be swallowed.
        XCTAssertEqual(PicksCodec.parsePicksInput("https://x.test/a?now=2026-09-18T21:40&picks=\(encoded)&z=1"), encoded)
        // A bare code list, pasted without the link around it.
        XCTAssertEqual(PicksCodec.parsePicksInput("  \(encoded)  "), encoded)
        // A bare query fragment, which is what a half-selected copy produces.
        XCTAssertEqual(PicksCodec.parsePicksInput("?picks=\(encoded)"), encoded)
        // Nothing pick-shaped.
        XCTAssertNil(PicksCodec.parsePicksInput(""))
        XCTAssertNil(PicksCodec.parsePicksInput("https://settimes.kandiwooks.com/lost-lands/2026/fri"))
        XCTAssertNil(PicksCodec.parsePicksInput("just some words"))
    }

    // MARK: - shareURL

    func testShareURLMatchesTheSettimesLinkShapeForTheGivenNight() {
        let pack = makePack(sets: [tynan, levity])
        let url = PicksCodec.shareURL(for: Self.fridayDOY, pickIDs: [tynan, levity].map { PicksCodec.setID(for: $0, in: pack) },
                                       in: pack)
        XCTAssertEqual(url?.absoluteString, "https://settimes.kandiwooks.com/lost-lands/2026/fri?picks=uvfs9r.1230q9o")
    }

    func testShareURLWithNoPicksHasNoQueryItem() {
        let pack = makePack(sets: [tynan])
        let url = PicksCodec.shareURL(for: Self.fridayDOY, pickIDs: [], in: pack)
        XCTAssertEqual(url?.absoluteString, "https://settimes.kandiwooks.com/lost-lands/2026/fri")
    }

    func testWeekdaySlugMatchesTheRealCalendarWeekday() {
        let pack = makePack(sets: [tynan, riot])
        XCTAssertEqual(PicksCodec.weekdaySlug(for: Self.fridayDOY, in: pack), "fri")
        XCTAssertEqual(PicksCodec.weekdaySlug(for: Self.saturdayDOY, in: pack), "sat")
    }
}
