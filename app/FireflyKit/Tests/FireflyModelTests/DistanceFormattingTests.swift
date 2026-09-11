//
//  DistanceFormattingTests.swift — the ONE distance-formatting seam
//  (`DistanceFormatting.swift`) every distance/area string in the app is
//  meant to render through (M2).
//
import FireflyModel
import XCTest

final class DistanceFormattingTests: XCTestCase {

    // MARK: - Formatter table: m/km vs ft/mi thresholds, mirroring the
    // puck's own `ff_fmt_distance` (firmware/core/src/ff_crew.c) —
    // < 1000 (m or ft) shows whole units; at/above shows one decimal
    // km/mi. Every row is picked well clear of a rounding boundary so
    // the expectation is unambiguous.

    private struct Row {
        let meters: Double
        let imperial: Bool
        let expected: String
        let line: UInt

        init(_ meters: Double, imperial: Bool, expected: String, line: UInt = #line) {
            self.meters = meters
            self.imperial = imperial
            self.expected = expected
            self.line = line
        }
    }

    private static let table: [Row] = [
        // Metric: under 1000 m -> whole meters.
        Row(0, imperial: false, expected: "0 m"),
        Row(42, imperial: false, expected: "42 m"),
        Row(999, imperial: false, expected: "999 m"),
        // Metric: at/above 1000 m -> one-decimal km.
        Row(1000, imperial: false, expected: "1.0 km"),
        Row(1500, imperial: false, expected: "1.5 km"),
        Row(5800, imperial: false, expected: "5.8 km"),
        // Imperial: under 1000 ft -> whole feet.
        Row(0, imperial: true, expected: "0 ft"),
        Row(91.44, imperial: true, expected: "300 ft"),   // 91.44 m == 300 ft exactly
        Row(300, imperial: true, expected: "984 ft"),
        // Imperial: at/above 1000 ft -> one-decimal miles.
        Row(310, imperial: true, expected: "0.2 mi"),
        Row(1609.344, imperial: true, expected: "1.0 mi"), // exactly one mile
        Row(5800, imperial: true, expected: "3.6 mi"),
    ]

    func testFormatterTableMirrorsThePuckThresholds() {
        for row in Self.table {
            XCTAssertEqual(DistanceFormatting.distance(meters: row.meters, imperial: row.imperial),
                            row.expected, line: row.line)
        }
    }

    /// The property that actually matters: this is not a second,
    /// hand-rolled implementation of the threshold rules that happens
    /// to agree with the table above today — it is the SAME function
    /// the bridge itself calls (`CrewStore.formatDistance`, which calls
    /// straight into `ff_fmt_distance`). Any input, not just the table
    /// rows, must agree, by construction.
    func testDistanceIsLiterallyCrewStoreFormatDistanceNeverAReimplementation() {
        for meters in stride(from: 0.0, through: 12_000.0, by: 137.0) {
            for imperial in [false, true] {
                XCTAssertEqual(DistanceFormatting.distance(meters: meters, imperial: imperial),
                                CrewStore.formatDistance(meters: Float(meters), imperial: imperial))
            }
        }
    }

    // MARK: - `preference:`/`locale:` overload resolves before formatting

    func testPreferenceOverloadResolvesSystemAgainstLocaleBeforeFormatting() {
        let usLocale = Locale(identifier: "en_US")
        let deLocale = Locale(identifier: "de_DE")
        XCTAssertEqual(DistanceFormatting.distance(meters: 5800, preference: .system, locale: usLocale),
                        "3.6 mi")
        XCTAssertEqual(DistanceFormatting.distance(meters: 5800, preference: .system, locale: deLocale),
                        "5.8 km")
    }

    func testPreferenceOverloadHonorsAnExplicitChoiceOverLocale() {
        XCTAssertEqual(DistanceFormatting.distance(meters: 5800, preference: .imperial,
                                                     locale: Locale(identifier: "de_DE")),
                        "3.6 mi")
        XCTAssertEqual(DistanceFormatting.distance(meters: 5800, preference: .metric,
                                                     locale: Locale(identifier: "en_US")),
                        "5.8 km")
    }

    // MARK: - AREA/"~" honesty marker survives a unit-system conversion
    //
    // issue #47's rule: an approximate-AREA statement is never a bare
    // point distance. The marker must be there regardless of which unit
    // system rendered the number underneath it, and the number itself
    // must be a real conversion of the SAME underlying metres, not two
    // independently-rounded quantities that happen to share a prefix.

    func testAreaDistanceCarriesTheTildeInBothUnitSystems() {
        let metric = DistanceFormatting.areaDistance(meters: 5800, imperial: false)
        let imperial = DistanceFormatting.areaDistance(meters: 5800, imperial: true)
        XCTAssertEqual(metric, "~5.8 km")
        XCTAssertEqual(imperial, "~3.6 mi")
        XCTAssertTrue(metric.hasPrefix("~"))
        XCTAssertTrue(imperial.hasPrefix("~"))
    }

    func testAreaDistanceStaysConsistentWithPlainDistanceMinusThePrefix() {
        for meters in [0.0, 42.0, 999.0, 1000.0, 5800.0] {
            for imperial in [false, true] {
                let plain = DistanceFormatting.distance(meters: meters, imperial: imperial)
                let area = DistanceFormatting.areaDistance(meters: meters, imperial: imperial)
                XCTAssertEqual(area, "~" + plain)
            }
        }
    }

    func testAreaDistancePreferenceOverloadAlsoResolvesSystemFirst() {
        XCTAssertEqual(DistanceFormatting.areaDistance(meters: 5800, preference: .system,
                                                         locale: Locale(identifier: "en_US")),
                        "~3.6 mi")
        XCTAssertEqual(DistanceFormatting.areaDistance(meters: 5800, preference: .system,
                                                         locale: Locale(identifier: "de_DE")),
                        "~5.8 km")
    }
}
