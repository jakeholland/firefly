//
//  RadarViewModelTests.swift — Slice D.
//
//  Table-driven over every `radar_mode_t` the app can reach, with fixture
//  values transcribed VERBATIM from
//  `firmware/tests/fixtures/radar_*.json` (the puck's own golden
//  fixtures — read at authoring time, not generated here) so the two
//  products are pinned to the same inputs, plus the chip-text rules, the
//  FIND cadence/cap/stop machinery, and the honesty rules (imprecise ->
//  area text or NEARBY, never a bare point number).
//
import FireflyCore
import FireflyModel
import XCTest

@MainActor
final class RadarViewModelTests: XCTestCase {

    // MARK: - Helpers

    private func makeModel(
        snapshot: RadarSnapshot, hasPairedMembers: Bool = true, selectedNodeID: UInt32? = 42
    ) -> (model: RadarViewModel, radar: MockRadarComputing, find: MockFindSession) {
        let radar = MockRadarComputing()
        radar.nextSnapshot = snapshot
        radar.hasPairedMembers = hasPairedMembers
        radar.selectedNodeID = selectedNodeID
        let find = MockFindSession()
        let model = RadarViewModel(radar: radar, heading: NoHeadingProvider(),
                                    location: UnavailableLocationProvider(), find: find)
        return (model, radar, find)
    }

    private func dot(_ ring: Double, _ initial: Character, _ idx: Int, stale: Bool = false,
                      place: Bool = false, imprecise: Bool = false) -> RadarDot {
        RadarDot(id: idx, ringDegrees: ring, initial: initial, colorIndex: idx,
                 stale: stale, place: place, imprecise: imprecise)
    }

    private func snapshot(
        mode: RadarMode, arrowDegrees: Double = 0, arrowValid: Bool = false, name: String = "",
        distanceText: String = "", distanceImprecise: Bool = false, ageText: String = "", trend: Int = 0,
        bearingDegrees: Double = 0, bearingValid: Bool = false, place: Bool = false, stale: Bool = false,
        heardPresence: HeardPresence = .never, dots: [RadarDot] = [],
        signalTier: SignalTierPresentation = .none, signalHeard: Bool = false, signalViaRelay: Bool = false,
        signalAgeText: String = "", signalDots: [RadarSignalDot] = []
    ) -> RadarSnapshot {
        RadarSnapshot(mode: mode, arrowDegrees: arrowDegrees, arrowValid: arrowValid, name: name,
                      distanceText: distanceText, distanceImprecise: distanceImprecise, ageText: ageText,
                      trend: trend, bearingDegrees: bearingDegrees, bearingValid: bearingValid, place: place,
                      stale: stale, heardPresence: heardPresence, dots: dots, signalTier: signalTier,
                      signalHeard: signalHeard, signalViaRelay: signalViaRelay, signalAgeText: signalAgeText,
                      signalDots: signalDots, clockText: "9:41", battPct: 78, meshOK: true)
    }

    // MARK: - radar_nosel.json

    func testNoselShowsNoArrowAndAsksForASelection() {
        let s = snapshot(mode: .nosel, arrowValid: false, dots: [])
        let (model, radar, _) = makeModel(snapshot: s, hasPairedMembers: false, selectedNodeID: nil)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "SELECT A FRIEND")
        XCTAssertFalse(model.snapshot.arrowValid)
        XCTAssertNil(model.theirPositionLine)
        XCTAssertNil(model.findTargetNodeID)
        _ = radar // silence unused warning if any
    }

    // MARK: - radar_nofix.json

    func testNofixShowsRadioOnlyAndLooksForTheSelection() {
        let s = snapshot(mode: .nofix, arrowValid: false, name: "DANA", distanceText: "", ageText: "6 MIN")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "NO FIX \u{00B7} RADIO ONLY")
        XCTAssertEqual(model.subheadline, "Looking for DANA")
        XCTAssertEqual(model.primaryReadoutText, "", "NOFIX has nothing geometric to show as a big number")
        // A true last-known age for the selection's OWN fix is still
        // honestly reportable even though nothing geometric follows from
        // it (S06: NOFIX still carries a real age_str).
        XCTAssertEqual(model.theirPositionLine,
                       "DANA's last known position: their puck GPS, 6 MIN ago (your distance unknown — no fix of your own)")
    }

    // MARK: - radar_nohdg.json / radar_nohdg_stale.json

    func testNohdgShowsBearingHintNoArrowNoLastSeenChip() {
        let s = snapshot(mode: .nohdg, arrowValid: false, name: "Taylor", distanceText: "492 ft",
                          ageText: "8 SEC", bearingDegrees: 180, bearingValid: true, place: false, stale: false)
        let (model, radar, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "NO COMPASS")
        XCTAssertFalse(model.snapshot.arrowValid, "macOS (no magnetometer) must never show a stuck/fabricated arrow")
        XCTAssertEqual(model.bearingHintText, "BEARING 180\u{00B0} \u{00B7} S")
        XCTAssertEqual(model.primaryReadoutText, "492 ft")
        XCTAssertNil(model.subheadline, "NOHDG never grows a second chip beyond NO COMPASS + the bearing hint")
        // A01's Slice D acceptance list: "a test that macOS (no
        // magnetometer) renders NOHDG rather than a stuck arrow." Pin
        // that NoHeadingProvider's nil actually reaches
        // RadarComputing.compute(headingDegrees:) as nil, not just that
        // the mode resolves to NOHDG incidentally.
        guard let heading = radar.lastHeadingDegrees else {
            return XCTFail("compute(headingDegrees:) was never called")
        }
        XCTAssertNil(heading, "NoHeadingProvider must feed a nil heading into RadarComputing, never a stuck angle")
    }

    func testNohdgStaleDoesNotGrowASecondChip() {
        let s = snapshot(mode: .nohdg, name: "Taylor", distanceText: "492 ft", ageText: "12 MIN",
                          bearingDegrees: 180, bearingValid: true, place: false, stale: true)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        // Same chip text as the fresh case — freshness affects rim
        // styling only, never the mode's own chip (S06's 2026-09-05
        // amendment: "does NOT grow a second 'LAST SEEN'-style chip").
        XCTAssertEqual(model.chipText, "NO COMPASS")
        XCTAssertTrue(model.snapshot.stale)
    }

    // MARK: - radar_live.json

    func testLiveShowsNameDistanceAgeAndSolidArrow() {
        let s = snapshot(mode: .live, arrowDegrees: 42, arrowValid: true, name: "DANA",
                          distanceText: "320 m", ageText: "8 SEC",
                          dots: [dot(42, "D", 0), dot(128, "R", 1), dot(205, "M", 2, stale: true), dot(301, "J", 3)])
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "LIVE")
        XCTAssertEqual(model.primaryReadoutText, "320 m")
        XCTAssertEqual(model.theirPositionLine, "DANA's position: their puck GPS, 8 SEC ago")
        XCTAssertEqual(model.snapshot.dots.count, 4)
    }

    // MARK: - radar_stale.json

    func testStaleShowsLastSeenChip() {
        let s = snapshot(mode: .stale, arrowDegrees: 42, arrowValid: true, name: "DANA",
                          distanceText: "320 m", ageText: "4 MIN")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "LAST SEEN 4 MIN")
    }

    // MARK: - radar_lost.json (a real, aged fix)

    func testLostWithARealFixShowsLastSeenAndTildeDistance() {
        let s = snapshot(mode: .lost, arrowDegrees: 200, arrowValid: true, name: "DANA",
                          distanceText: "1.1 km", ageText: "42 MIN")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "LAST SEEN 42 MIN")
        XCTAssertEqual(model.primaryReadoutText, "~1.1 km",
                       "LOST must read as distinctly less trustworthy than STALE, never a plain number")
        XCTAssertEqual(model.theirPositionLine, "DANA's position: their puck GPS, last seen 42 MIN ago")
    }

    // MARK: - radar_never.json / radar_heard_no_fix.json (RENDERER CONTRACT: mode alone does not distinguish)

    func testLostNeverFixedAndGenuinelySilentShowsNoFixYet() {
        let s = snapshot(mode: .lost, arrowValid: false, name: "JAMIE", distanceText: "", ageText: "",
                          heardPresence: .never)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "NO FIX YET")
        XCTAssertEqual(model.subheadline, "Waiting for their first GPS fix")
        XCTAssertNil(model.theirPositionLine, "never fixed: nothing honest to claim as a position")
    }

    func testLostNeverFixedButHeardRecentlyShowsNearNoFix() {
        // radar_heard_no_fix.json is byte-identical to radar_never.json
        // except heard_presence — the exact case the 2026-09-07
        // presence-heard-vs-position amendment exists for: a friend the
        // radio is still hearing from must not read the same as one it
        // has never heard from at all.
        let s = snapshot(mode: .lost, arrowValid: false, name: "JAMIE", distanceText: "", ageText: "",
                          heardPresence: .heard)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "NEAR, NO FIX")
        XCTAssertEqual(model.subheadline, "Heard recently, no GPS fix yet")
    }

    func testLostStalePresenceAlsoReadsAsNearNoFix() {
        let s = snapshot(mode: .lost, name: "JAMIE", ageText: "", heardPresence: .stale)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "NEAR, NO FIX")
    }

    // MARK: - radar_close.json

    func testCloseShowsPulseHeadlineAndWarmerTrend() {
        let s = snapshot(mode: .close, name: "Dana", distanceText: "15 m", ageText: "3 SEC", trend: 1)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "CLOSE RANGE")
        XCTAssertEqual(model.primaryReadoutText, "15 m")
        XCTAssertTrue(model.showsTrendChip)
        XCTAssertEqual(model.trendLabel, "WARMER")
    }

    func testCloseWithDegradedPrecisionShowsNearbyNeverAFabricatedNumber() {
        let s = snapshot(mode: .close, name: "Dana", distanceText: "~5.8 km", distanceImprecise: true,
                          ageText: "3 SEC", trend: 0)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        // "shows NEARBY instead of a fabricated big number" (S29) — the
        // multi-km area text must never sit next to pulsing close-range
        // rings, which would directly contradict them.
        XCTAssertEqual(model.primaryReadoutText, "NEARBY")
        XCTAssertFalse(model.primaryReadoutText.contains("km"))
    }

    // MARK: - radar_place.json

    func testPlaceShowsFixedPositionAndNeverAnAge() {
        let s = snapshot(mode: .place, arrowDegrees: 118, arrowValid: true, name: "CAMP BASE",
                          distanceText: "610 m", ageText: "")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "FIXED POSITION")
        XCTAssertNotEqual(model.chipText, "LIVE", "an assertion is never rendered as a fresh measurement")
        XCTAssertEqual(model.primaryReadoutText, "610 m")
        XCTAssertEqual(model.theirPositionLine, "CAMP BASE's position: fixed position, asserted (no age given)")
    }

    /// S06 issue #47's amendment names the suffix explicitly on all of
    /// LIVE/STALE/PLACE/LOST. A landmark can be asserted at
    /// degraded precision same as a measured fix (`has_precision_bits`
    /// applies to any position), so PLACE must caveat exactly like the
    /// other three modes rather than silently reading as a confident
    /// point next to a multi-kilometer area estimate.
    func testPlaceWithImprecisePositionAppendsTheAreaSuffixToTheChip() {
        let s = snapshot(mode: .place, arrowDegrees: 118, arrowValid: true, name: "CAMP BASE",
                          distanceText: "~5.8 km", distanceImprecise: true, ageText: "")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "FIXED POSITION - AREA")
    }

    // MARK: - radar_imprecise.json (issue #47 — honesty rule: imprecise never renders a point distance)

    func testImpreciseLiveShowsAreaSuffixAndDimmedApproximateDistance() {
        let s = snapshot(mode: .live, arrowDegrees: 42, arrowValid: true, name: "DANA",
                          distanceText: "~5.8 km", distanceImprecise: true, ageText: "8 SEC")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "LIVE - AREA")
        XCTAssertEqual(model.primaryReadoutText, "~5.8 km")
        XCTAssertTrue(model.snapshot.distanceImprecise)
    }

    /// The core honesty rule as a single mechanical assertion, table-driven
    /// over every mode that can carry `distanceImprecise`: whenever it is
    /// true, the readout must be an area/approximate statement (contains
    /// "~" or "AREA") or CLOSE's "NEARBY" substitution — never a bare
    /// point-looking number.
    func testDistanceImpreciseNeverRendersAsAConfidentPoint() {
        let cases: [(RadarMode, String)] = [
            (.live, "~5.8 km"), (.stale, "~5.8 km"), (.lost, "~5.8 km"), (.place, "~5.8 km"),
        ]
        for (mode, distanceText) in cases {
            let s = snapshot(mode: mode, arrowValid: true, name: "DANA", distanceText: distanceText,
                              distanceImprecise: true, ageText: mode == .place ? "" : "8 SEC")
            let (model, _, _) = makeModel(snapshot: s)
            model.observe(); defer { model.stopObserving() }
            let readout = model.primaryReadoutText
            XCTAssertTrue(readout.hasPrefix("~"), "\(mode): \(readout)")
            let chip = model.chipText
            XCTAssertTrue(chip.contains("AREA"), "\(mode) chip should caveat imprecision: \(chip)")
        }
        let closeSnapshot = snapshot(mode: .close, name: "Dana", distanceText: "~5.8 km",
                                      distanceImprecise: true, ageText: "3 SEC")
        let (closeModel, _, _) = makeModel(snapshot: closeSnapshot)
        closeModel.observe(); defer { closeModel.stopObserving() }
        XCTAssertEqual(closeModel.primaryReadoutText, "NEARBY")
    }

    // MARK: - radar_signal_strong.json

    func testSignalStrongShowsTierChipAndHeardAgeNoArrow() {
        let s = snapshot(mode: .signal, arrowValid: false, name: "DANA", trend: 1,
                          signalTier: .strong, signalHeard: true, signalViaRelay: false, signalAgeText: "8 SEC",
                          signalDots: [
                              RadarSignalDot(id: 0, initial: "R", colorIndex: 1, tier: .weak, viaRelay: false),
                              RadarSignalDot(id: 1, initial: "M", colorIndex: 2, tier: .none, viaRelay: true),
                          ])
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "STRONG SIGNAL")
        XCTAssertEqual(model.subheadline, "heard 8 SEC ago")
        XCTAssertFalse(model.snapshot.arrowValid, "no ghost arrow when my own position is what's missing")
        XCTAssertTrue(model.showsTrendChip)
        XCTAssertEqual(model.snapshot.signalDots.count, 2)
        XCTAssertEqual(model.primaryReadoutText, "", "SIGNAL's non-ghost sub-case has no distance to show")
    }

    // MARK: - radar_signal_faint_relay.json

    func testSignalViaRelayShowsRelayChipNoTierNoTrend() {
        let s = snapshot(mode: .signal, name: "Taylor", trend: 0,
                          signalTier: .none, signalHeard: true, signalViaRelay: true, signalAgeText: "2 MIN")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "VIA RELAY")
        XCTAssertEqual(model.subheadline, "heard 2 MIN ago")
        XCTAssertFalse(model.showsTrendChip, "a trend on top of no direct reading would be a fabricated refinement of nothing")
    }

    // MARK: - radar_signal_lastknown.json (the ghost sub-case)

    func testSignalGhostShowsLastKnownArrowAndDistinctSignalLine() {
        let s = snapshot(mode: .signal, arrowDegrees: 205, arrowValid: true, name: "DANA",
                          distanceText: "1.1 km", ageText: "42 MIN", trend: -1,
                          bearingDegrees: 205, bearingValid: true,
                          signalTier: .good, signalHeard: true, signalViaRelay: false, signalAgeText: "3 MIN")
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        XCTAssertEqual(model.chipText, "GOOD SIGNAL")
        XCTAssertTrue(model.snapshot.arrowValid, "a real, if old, fix must still ghost-arrow")
        XCTAssertEqual(model.trendLabel, "COLDER")
        XCTAssertEqual(model.subheadline, "heard 3 MIN ago\nLAST KNOWN 42 MIN ago, ~1.1 km SSW")
        // Two INDEPENDENT axes, never conflated: the position (GPS, old)
        // and the radio evidence (current, direct/relay + age).
        XCTAssertEqual(model.theirPositionLine,
                       "DANA's last known position: their puck GPS, 42 MIN ago, SSW")
        XCTAssertEqual(model.theirSignalLine, "DANA's radio: good signal, direct, heard 3 MIN ago")
    }

    // MARK: - Compass point (wraps ff_geo_compass_point directly — never reimplemented)

    func testCompassPointMatchesTheCoresOwnSixteenPointNaming() {
        XCTAssertEqual(CompassPoint.name(forBearingDegrees: 0), "N")
        XCTAssertEqual(CompassPoint.name(forBearingDegrees: 90), "E")
        XCTAssertEqual(CompassPoint.name(forBearingDegrees: 180), "S")
        XCTAssertEqual(CompassPoint.name(forBearingDegrees: 270), "W")
        XCTAssertEqual(CompassPoint.name(forBearingDegrees: 205), "SSW")
    }

    // MARK: - Colorblind-safe crew palette (S17), pinned against ff_theme.h — same anti-drift discipline as ThemeTests

    private var themeHeader: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // FireflyModelTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // FireflyKit
            .deletingLastPathComponent()   // app
            .deletingLastPathComponent()   // <repo root>
            .appending(path: "firmware/app/theme/ff_theme.h")
    }

    private func hexDefines() throws -> [String: UInt32] {
        let text = try String(contentsOf: themeHeader, encoding: .utf8)
        var out: [String: UInt32] = [:]
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard line.hasPrefix("#define ") else { continue }
            let parts = line.dropFirst("#define ".count).split(separator: " ", omittingEmptySubsequences: true)
            guard parts.count >= 2, parts[1].hasPrefix("0x") else { continue }
            guard let value = UInt32(parts[1].dropFirst(2), radix: 16) else { continue }
            out[String(parts[0])] = value
        }
        return out
    }

    func testColorblindCrewPaletteMatchesTheHeaderInOrder() throws {
        let d = try hexDefines()
        let expected = [
            "FF_THEME_CREW_CB_ORANGE", "FF_THEME_CREW_CB_SKYBLUE", "FF_THEME_CREW_CB_GREEN",
            "FF_THEME_CREW_CB_YELLOW", "FF_THEME_CREW_CB_BLUE", "FF_THEME_CREW_CB_VERMILLION",
            "FF_THEME_CREW_CB_PURPLE", "FF_THEME_CREW_CB_MAUVE",
        ].map { d[$0] }
        XCTAssertEqual(expected, RadarCrewPalette.colorblind.map { Optional($0) })
    }

    func testCrewColorWrapsAndSelectsThePaletteExplicitly() {
        XCTAssertEqual(RadarCrewPalette.hex(index: 0, colorblind: false), FireflyTheme.crew[0])
        XCTAssertEqual(RadarCrewPalette.hex(index: 8, colorblind: false), FireflyTheme.crew[0])
        XCTAssertEqual(RadarCrewPalette.hex(index: 0, colorblind: true), RadarCrewPalette.colorblind[0])
        XCTAssertEqual(RadarCrewPalette.hex(index: 9, colorblind: true), RadarCrewPalette.colorblind[1])
    }

    // MARK: - Selection cycling

    func testCycleSelectionCallsThroughAndRecomputes() {
        let s = snapshot(mode: .live, name: "DANA")
        let (model, radar, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        let before = radar.computeCallCount
        model.cycleSelection()
        XCTAssertEqual(radar.cycleSelectionCallCount, 1)
        XCTAssertGreaterThan(radar.computeCallCount, before)
    }

    // MARK: - "your position" line (always present, never blank)

    func testMyPositionLineNamesSourceAndAgeEvenWhenUnavailable() {
        let s = snapshot(mode: .nosel)
        let (model, _, _) = makeModel(snapshot: s)
        model.observe(); defer { model.stopObserving() }
        // NoHeadingProvider/UnavailableLocationProvider: both halves
        // honestly unavailable, never a fabricated placeholder.
        XCTAssertEqual(model.myPositionLine, "your position: no fix; heading: unavailable")
    }

    // MARK: - FIND (S29 PR2): cadence, cap, cancel-on-new-target, stop

    func testFindSendsFirstPingImmediatelyThenRespectsTheTenSecondFloor() {
        let find = MockFindSession()
        var now = Date(timeIntervalSince1970: 1000)
        find.start(targetNodeID: 7, now: now)
        XCTAssertTrue(find.tick(now: now), "the very first tick after start always sends immediately")
        XCTAssertFalse(find.tick(now: now.addingTimeInterval(1)), "a caller ticking faster than 10s must never send twice inside the window")
        now = now.addingTimeInterval(FindSessionConstants.pingIntervalSeconds)
        XCTAssertTrue(find.tick(now: now))
        XCTAssertEqual(find.pingCount, 2)
    }

    func testFindStopsAtThirtyPings() {
        let find = MockFindSession()
        var now = Date(timeIntervalSince1970: 2000)
        find.start(targetNodeID: 7, now: now)
        for _ in 0..<FindSessionConstants.maxPings {
            _ = find.tick(now: now)
            now = now.addingTimeInterval(FindSessionConstants.pingIntervalSeconds)
        }
        XCTAssertEqual(find.pingCount, FindSessionConstants.maxPings)
        // The cap is checked BEFORE the rate-limit floor on the NEXT
        // tick (mirrors `ff_find_tick`'s own doc comment) — so the
        // session is still nominally active right after sending its
        // 30th ping; the very next tick is the one that notices the cap
        // and stops without sending a 31st.
        XCTAssertTrue(find.isActive)
        XCTAssertFalse(find.tick(now: now), "no 31st ping once the cap is reached")
        XCTAssertFalse(find.isActive, "auto-stops once the cap-checking tick runs")
    }

    func testFindStopsAtFiveMinutesEvenWithFewerThanThirtyPings() {
        let find = MockFindSession()
        let start = Date(timeIntervalSince1970: 3000)
        find.start(targetNodeID: 7, now: start)
        _ = find.tick(now: start)
        // An irregular tick loop that never reaches 30 sends must not be
        // able to out-wait the wall-clock cap.
        XCTAssertFalse(find.tick(now: start.addingTimeInterval(FindSessionConstants.sessionMaxSeconds)))
        XCTAssertFalse(find.isActive)
    }

    func testFindNewTargetCancelsThePriorSessionOutright() {
        let find = MockFindSession()
        let now = Date(timeIntervalSince1970: 4000)
        find.start(targetNodeID: 1, now: now)
        _ = find.tick(now: now)
        XCTAssertEqual(find.pingCount, 1)
        find.start(targetNodeID: 2, now: now) // single active session — no queuing
        XCTAssertEqual(find.pingCount, 0, "a new target silently replaces the prior session")
        XCTAssertEqual(find.targetNodeID, 2)
    }

    func testFindStopViaViewModelCancelsOnFaceLeave() {
        let s = snapshot(mode: .signal, signalTier: .good, signalHeard: true)
        let (model, _, find) = makeModel(snapshot: s, selectedNodeID: 99)
        model.observe()
        model.startFindOnSelection()
        XCTAssertTrue(find.isActive)
        XCTAssertEqual(find.targetNodeID, 99)
        model.stopObserving() // .onDisappear calls this, which also stops FIND
        XCTAssertFalse(find.isActive)
    }

    func testFindDoesNothingWithoutASelection() {
        let s = snapshot(mode: .nosel)
        let (model, _, find) = makeModel(snapshot: s, selectedNodeID: nil)
        model.observe(); defer { model.stopObserving() }
        model.startFindOnSelection()
        XCTAssertFalse(find.isActive, "never silently pings node 0")
        XCTAssertNil(model.findTargetNodeID)
    }

    // MARK: - FIND trend-crossing haptics (radar_find_active.json's own numbers)

    func testFindWarmerCrossingFiresExactlyOncePerCrossing() {
        let find = MockFindSession()
        let now = Date(timeIntervalSince1970: 5000)
        find.start(targetNodeID: 1, now: now)
        // Steady baseline, then a >=3 dB improvement.
        for rssi: Int16 in [-90, -90, -90] { XCTAssertEqual(find.recordPong(fromNodeID: 1, rssiDbm: rssi, hasSNR: false, snrDb: 0, now: now), .none) }
        var verdicts: [FindHaptic] = []
        for rssi: Int16 in [-80, -80, -80] {
            verdicts.append(find.recordPong(fromNodeID: 1, rssiDbm: rssi, hasSNR: false, snrDb: 0, now: now))
        }
        // A trend is computable only once a FULL 6-sample window exists
        // (3 baseline + 3 new) — the crossing therefore fires on the
        // THIRD "-80" sample, the first call where both 3-sample halves
        // are populated, not on the first one to cross the threshold.
        XCTAssertEqual(verdicts, [.none, .none, .warmer], "fires once per CROSSING, not once per sample above threshold")
        // A later call that still reads warmer must not re-fire.
        XCTAssertEqual(find.recordPong(fromNodeID: 1, rssiDbm: -80, hasSNR: false, snrDb: 0, now: now), .none)
    }

    func testFindPongFromAnotherNodeIsIgnored() {
        let find = MockFindSession()
        let now = Date(timeIntervalSince1970: 6000)
        find.start(targetNodeID: 1, now: now)
        XCTAssertEqual(find.recordPong(fromNodeID: 99, rssiDbm: -60, hasSNR: false, snrDb: 0, now: now), .none)
    }

    func testHandlePongAppendsAReplyWithTheRightTierAndFiresHaptics() {
        let s = snapshot(mode: .signal, signalTier: .good, signalHeard: true)
        let (model, _, _) = makeModel(snapshot: s, selectedNodeID: 3_494_928_410)
        model.observe(); defer { model.stopObserving() }
        model.startFindOnSelection()
        model.handlePong(fromNodeID: 3_494_928_410, rssiDbm: -72, hasSNR: true, snrDb: -2.5)
        XCTAssertEqual(model.findReplies.count, 1)
        XCTAssertEqual(model.findReplies[0].rssiOfUs, -72)
        XCTAssertEqual(model.findReplies[0].tier, SignalTierPresentation.tier(rssiDbm: -72))
    }

    // MARK: - Honesty: no signal label may contain a number or a unit (A01_AC7, mirrored for the FIND/SIGNAL chip vocabulary)

    func testNoSignalChipTextContainsADigit() {
        for tier: SignalTierPresentation in [.strong, .good, .weak, .faint, .none] {
            let s = snapshot(mode: .signal, name: "X", signalTier: tier, signalHeard: true, signalViaRelay: tier == .none)
            let (model, _, _) = makeModel(snapshot: s)
            model.observe(); defer { model.stopObserving() }
            let chip = model.chipText
            XCTAssertFalse(chip.contains(where: \.isNumber), "signal chip must never contain a number: \(chip)")
        }
    }

    func testSignalRingDisclaimerIsExplicit() {
        XCTAssertEqual(RadarViewModel.signalRingDisclaimer, "Ring is signal order, not direction")
    }
}
