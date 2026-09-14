//
//  CrewDiagnosticsViewModelTests.swift — A02 slice E, task scope item 3:
//  Crew -> Advanced -> "Crew diagnostics". Driven against a small fake
//  `CrewDiagnosticsProviding` — the honest-data rule this file exists
//  to pin is "UNKNOWN never 0 when nothing has run" (task brief), which
//  is a property of the VIEW MODEL's own labels, not of
//  `CrewMembershipEngine`'s counters (already covered by
//  `CrewMembershipEngineTests`).
//
import FireflyModel
import Foundation
import XCTest

@MainActor
private final class FakeDiagnosticsSource: CrewDiagnosticsProviding {
    var crewChannel: CrewChannelIdentity?
    var channelStatus: CrewChannelStatus = .noCrew
    var admissionCounters = CrewAdmissionCounters()
    var lastAdmissionAtMs: UInt64?
}

@MainActor
final class CrewDiagnosticsViewModelTests: XCTestCase {
    private func makeViewModel(source: FakeDiagnosticsSource = FakeDiagnosticsSource(),
                                now: Date = Date(timeIntervalSince1970: 1_780_000_000))
        -> (CrewDiagnosticsViewModel, FakeDiagnosticsSource) {
        (CrewDiagnosticsViewModel(source: source, now: { now }), source)
    }

    // MARK: - "UNKNOWN never 0 when nothing has run"

    func testNoCrew_everyNumberReadsUnknownNeverZero() {
        let (vm, _) = makeViewModel()
        XCTAssertEqual(vm.channelLabel, "No crew set")
        XCTAssertEqual(vm.admittedLabel, "\u{2014}", "never a fabricated 0 before any crew exists")
        XCTAssertEqual(vm.refusedLabel, "\u{2014}")
        XCTAssertTrue(vm.refusalBreakdown.isEmpty)
        XCTAssertEqual(vm.lastAdmissionLabel, "Never")
    }

    /// Once a crew exists, a genuine zero IS shown as "0" — the
    /// distinction is "has this ever run", not "is the number
    /// currently zero".
    func testCrewConfiguredButNothingHasHappenedYet_showsRealZero() {
        let source = FakeDiagnosticsSource()
        source.crewChannel = CrewChannelIdentity(code: "FIRE-4K9M7X", psk: Data(repeating: 1, count: 32))
        source.channelStatus = .resolving
        let (vm, _) = makeViewModel(source: source)
        XCTAssertEqual(vm.channelLabel, "Not resolved yet")
        XCTAssertEqual(vm.admittedLabel, "0", "a crew exists, so zero admissions so far is a real count")
        XCTAssertEqual(vm.refusedLabel, "0")
        XCTAssertEqual(vm.lastAdmissionLabel, "Never", "still honestly 'never', not '0 s ago'")
    }

    func testChannelLabel_everyStatus() {
        let source = FakeDiagnosticsSource()
        source.crewChannel = CrewChannelIdentity(code: "FIRE-4K9M7X", psk: Data(repeating: 1, count: 32))
        let (vm, _) = makeViewModel(source: source)

        source.channelStatus = .resolved(index: 3)
        XCTAssertEqual(vm.channelLabel, "Channel 3")

        source.channelStatus = .notOnCrewChannel
        XCTAssertEqual(vm.channelLabel, "Your puck isn't on this crew's channel")
    }

    func testRefusalBreakdown_onlyNonZeroReasonsPlainLabels() {
        let source = FakeDiagnosticsSource()
        source.crewChannel = CrewChannelIdentity(code: "FIRE-4K9M7X", psk: Data(repeating: 1, count: 32))
        source.channelStatus = .resolved(index: 0)
        source.admissionCounters.refusedHidden = 2
        source.admissionCounters.refusedRosterFull = 1
        // Every other reason stays 0 and must be ABSENT, not padded in.
        let (vm, _) = makeViewModel(source: source)

        let reasons = Set(vm.refusalBreakdown.map(\.reason))
        XCTAssertEqual(reasons, ["hidden", "crew full"])
        XCTAssertEqual(vm.refusalBreakdown.first(where: { $0.reason == "hidden" })?.count, 2)
        XCTAssertEqual(vm.refusalBreakdown.first(where: { $0.reason == "crew full" })?.count, 1)
        XCTAssertEqual(vm.refusedLabel, "3", "the headline total is the sum of every reason")
    }

    func testLastAdmission_honestAgeFromInjectedNow() {
        let source = FakeDiagnosticsSource()
        source.crewChannel = CrewChannelIdentity(code: "FIRE-4K9M7X", psk: Data(repeating: 1, count: 32))
        let now = Date(timeIntervalSince1970: 1_780_000_000)
        source.lastAdmissionAtMs = UInt64((now.addingTimeInterval(-360).timeIntervalSince1970 * 1000).rounded())
        let (vm, _) = makeViewModel(source: source, now: now)
        XCTAssertEqual(vm.lastAdmissionLabel, "6 min ago")
    }
}
