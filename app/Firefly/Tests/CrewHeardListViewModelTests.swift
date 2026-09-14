//
//  CrewHeardListViewModelTests.swift — A02 slice E, task scope item 1:
//  Crew -> Advanced -> "People my puck hears". Driven against small
//  fakes (`CrewHeardListProviding`/`CrewMembershipProviding`) rather
//  than the full `CrewMembershipEngine` + `MeshtasticClient` harness —
//  this view model never talks to either directly, only through the
//  two seams.
//
import FireflyModel
import Foundation
import XCTest

@MainActor
private final class FakeHeardSource: CrewHeardListProviding {
    var untracked: [UntrackedCrewMember] = []
    var hidden: [UInt32] = []
    private(set) var hideCalls: [UInt32] = []
    private(set) var unhideCalls: [UInt32] = []

    func hide(nodeID: UInt32) {
        hideCalls.append(nodeID)
        guard !hidden.contains(nodeID) else { return }
        hidden.append(nodeID)
    }

    func unhide(nodeID: UInt32) {
        unhideCalls.append(nodeID)
        hidden.removeAll { $0 == nodeID }
    }
}

@MainActor
private final class FakeMembershipProvider: CrewMembershipProviding {
    var members: [CrewJoinedMember] = []
    func currentMembers() -> [CrewJoinedMember] { members }
}

@MainActor
final class CrewHeardListViewModelTests: XCTestCase {
    private func makeViewModel(heard: FakeHeardSource = FakeHeardSource(),
                                membership: FakeMembershipProvider = FakeMembershipProvider(),
                                now: Date = Date(timeIntervalSince1970: 1_780_000_000))
        -> (CrewHeardListViewModel, FakeHeardSource, FakeMembershipProvider) {
        let vm = CrewHeardListViewModel(heard: heard, membership: membership, now: { now })
        return (vm, heard, membership)
    }

    func testEmpty_noRowsNoBanner() {
        let (vm, _, _) = makeViewModel()
        XCTAssertTrue(vm.isEmpty)
        XCTAssertTrue(vm.rows.isEmpty)
        XCTAssertNil(vm.overflowBanner, "no banner while the overflow list is empty")
    }

    func testHiddenRow_rendersShortIDAndFixedDetail() {
        let heard = FakeHeardSource()
        heard.hidden = [0x0000_1008]
        let (vm, _, _) = makeViewModel(heard: heard)
        XCTAssertEqual(vm.rows.count, 1)
        let row = vm.rows[0]
        XCTAssertEqual(row.kind, .hidden)
        XCTAssertEqual(row.shortID, "!00001008")
        XCTAssertEqual(row.detail, "hidden from your radar")
    }

    func testOverflowRow_rendersHonestAge_neverFabricatedZero() {
        let heard = FakeHeardSource()
        let now = Date(timeIntervalSince1970: 1_780_000_000)
        heard.untracked = [UntrackedCrewMember(nodeID: 0x0000_1009, firstHeard: now.addingTimeInterval(-600),
                                                lastHeard: now.addingTimeInterval(-120))]
        let (vm, _, _) = makeViewModel(heard: heard, now: now)
        XCTAssertEqual(vm.rows.count, 1)
        let row = vm.rows[0]
        XCTAssertEqual(row.kind, .overflow)
        XCTAssertEqual(row.shortID, "!00001009")
        XCTAssertEqual(row.detail, "heard 2 min ago")
    }

    func testOverflowBanner_singularAndPluralWording() {
        let heard = FakeHeardSource()
        heard.untracked = [UntrackedCrewMember(nodeID: 1, firstHeard: .distantPast, lastHeard: .distantPast)]
        let (vm1, _, _) = makeViewModel(heard: heard)
        XCTAssertEqual(vm1.overflowBanner,
                       "1 more person is on this crew than your puck can track (8 is the limit). " +
                       "Hide someone to make room.")

        heard.untracked.append(UntrackedCrewMember(nodeID: 2, firstHeard: .distantPast, lastHeard: .distantPast))
        let (vm2, _, _) = makeViewModel(heard: heard)
        XCTAssertTrue(vm2.overflowBanner?.hasPrefix("2 more people are") == true)
    }

    /// Ordering: hidden rows first, then overflow oldest-heard first —
    /// the same order `CrewMembershipEngine.noteUntracked` evicts by.
    func testRowOrdering_hiddenFirstThenOverflowOldestFirst() {
        let heard = FakeHeardSource()
        heard.hidden = [50]
        let now = Date(timeIntervalSince1970: 1_780_000_000)
        heard.untracked = [
            UntrackedCrewMember(nodeID: 20, firstHeard: now, lastHeard: now.addingTimeInterval(-30)),
            UntrackedCrewMember(nodeID: 10, firstHeard: now, lastHeard: now.addingTimeInterval(-500)),
        ]
        let (vm, _, _) = makeViewModel(heard: heard, now: now)
        XCTAssertEqual(vm.rows.map(\.id), [50, 10, 20], "hidden first, then overflow sorted by id (10 < 20)")
    }

    func testUnhide_delegatesToSource() {
        let (vm, heard, _) = makeViewModel()
        vm.unhide(nodeID: 42)
        XCTAssertEqual(heard.unhideCalls, [42])
    }

    /// §4.3: "Make room" hides an EXISTING crew member — it must never
    /// touch the overflow person's own id.
    func testMakeRoom_hidesTheChosenExistingMemberNeverTheOverflowPerson() {
        let heard = FakeHeardSource()
        heard.untracked = [UntrackedCrewMember(nodeID: 999, firstHeard: .distantPast, lastHeard: .distantPast)]
        let (vm, fakeHeard, membership) = makeViewModel(heard: heard)
        membership.members = [CrewJoinedMember(id: 7, displayName: "Taylor", colorIndex: 0,
                                                joinedAtMs: nil, heardPresence: .heard)]
        XCTAssertEqual(vm.makeRoomCandidates.map(\.id), [7])

        vm.makeRoom(hiding: 7)
        XCTAssertEqual(fakeHeard.hideCalls, [7], "the EXISTING member is hidden, not the overflow id (999)")
    }

    func testShortID_formatsAsBangHexEight() {
        XCTAssertEqual(CrewHeardListViewModel.shortID(0xFEED_FACE), "!feedface")
        XCTAssertEqual(CrewHeardListViewModel.shortID(1), "!00000001")
    }
}
