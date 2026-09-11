//
//  SignalTierTests.swift — a signal reading is never a distance.
//
//  docs/specs/S29-radio-only.md's honesty rule, made enforceable. RSSI
//  is attenuation, not range: a body between two antennas costs more dB
//  than fifty metres of open field does. Any label carrying a unit would
//  be presenting a measurement the hardware never took.
//
import FireflyModel
import XCTest

final class SignalTierTests: XCTestCase {

    func testTiersComeFromThePucksOwnThresholds() {
        XCTAssertEqual(SignalTierPresentation.tier(rssiDbm: -55), .strong)
        XCTAssertEqual(SignalTierPresentation.tier(rssiDbm: -80), .good)   // boundary is GOOD
        XCTAssertEqual(SignalTierPresentation.tier(rssiDbm: -96), .weak)
        XCTAssertEqual(SignalTierPresentation.tier(rssiDbm: -130), .faint)
    }

    /// The enforcement, not a comment: no tier label may read as a
    /// distance.
    func testNoTierLabelCanBeReadAsADistance() {
        let banned = ["m", "M", "ft", "FT", "km", "KM", "yd", "mi", "METRE", "METER", "FEET"]
        for tier in SignalTierPresentation.allCases {
            let label = tier.label
            XCTAssertFalse(label.contains(where: \.isNumber), "\(label) contains a number")
            for unit in banned {
                XCTAssertFalse(label.split(separator: " ").contains(Substring(unit)),
                               "\(label) reads as a distance")
            }
        }
    }

    func testBarFillIsOrderedAndBounded() {
        let fills = [SignalTierPresentation.none, .faint, .weak, .good, .strong].map(\.barFill)
        XCTAssertEqual(fills, fills.sorted())
        XCTAssertEqual(fills.first, 0.0)
        XCTAssertEqual(fills.last, 1.0)
    }
}
