//
//  CrewPairingStoreTests.swift — M2's persisted crew pairing (docs/specs/
//  A01-companion-app.md, M2: "Crew pairing and colours, driven by
//  `ff_crew`"). Covers: store round-trip, colour-assignment stability
//  (first free index in roster order), restore-before-replay ordering,
//  and the 8-limit message.
//
import FireflyModel
import XCTest

// MARK: - CrewPairingStore (UserDefaults-backed) round-trip

final class CrewPairingStoreTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "CrewPairingStoreTests.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        suiteName = nil
        super.tearDown()
    }

    func testEmptyStoreHasNoRecords() {
        let store = CrewPairingStore(defaults: defaults)
        XCTAssertTrue(store.records().isEmpty)
    }

    func testUpsertRoundTripsNodeIDColorAndNickname() {
        let store = CrewPairingStore(defaults: defaults)
        store.upsert(CrewPairingRecord(nodeID: 42, colorIndex: 3, nickname: "Riley"))
        let records = store.records()
        XCTAssertEqual(records.count, 1)
        XCTAssertEqual(records[0].nodeID, 42)
        XCTAssertEqual(records[0].colorIndex, 3)
        XCTAssertEqual(records[0].nickname, "Riley")
    }

    func testUpsertOnAnExistingNodeUpdatesInPlaceWithoutMovingItInRosterOrder() {
        let store = CrewPairingStore(defaults: defaults)
        store.upsert(CrewPairingRecord(nodeID: 1, colorIndex: 0))
        store.upsert(CrewPairingRecord(nodeID: 2, colorIndex: 1))
        store.upsert(CrewPairingRecord(nodeID: 1, colorIndex: 0, nickname: "Renamed"))
        let records = store.records()
        XCTAssertEqual(records.map(\.nodeID), [1, 2], "roster order survives an in-place update")
        XCTAssertEqual(records[0].nickname, "Renamed")
    }

    func testRemoveDropsExactlyThatRecord() {
        let store = CrewPairingStore(defaults: defaults)
        store.upsert(CrewPairingRecord(nodeID: 1, colorIndex: 0))
        store.upsert(CrewPairingRecord(nodeID: 2, colorIndex: 1))
        store.remove(nodeID: 1)
        XCTAssertEqual(store.records().map(\.nodeID), [2])
    }

    /// A second store instance over the SAME `UserDefaults` sees what
    /// the first one wrote — the actual persistence contract, not just
    /// an in-process cache.
    func testRecordsSurviveAFreshStoreInstanceOverTheSameDefaults() {
        let first = CrewPairingStore(defaults: defaults)
        first.upsert(CrewPairingRecord(nodeID: 7, colorIndex: 2, nickname: "Sam"))

        let second = CrewPairingStore(defaults: defaults)
        let records = second.records()
        XCTAssertEqual(records.count, 1)
        XCTAssertEqual(records[0].nodeID, 7)
        XCTAssertEqual(records[0].colorIndex, 2)
        XCTAssertEqual(records[0].nickname, "Sam")
    }
}

// MARK: - InMemoryCrewPairingStore

final class InMemoryCrewPairingStoreTests: XCTestCase {
    func testRoundTripsAndRosterOrder() {
        let store = InMemoryCrewPairingStore()
        store.upsert(CrewPairingRecord(nodeID: 3, colorIndex: 0))
        store.upsert(CrewPairingRecord(nodeID: 1, colorIndex: 1))
        XCTAssertEqual(store.records().map(\.nodeID), [3, 1], "roster order is FIRST-PAIRED order, not sorted")
    }

    func testDoesNotSurviveAFreshInstance() {
        let first = InMemoryCrewPairingStore()
        first.upsert(CrewPairingRecord(nodeID: 1, colorIndex: 0))
        let second = InMemoryCrewPairingStore()
        XCTAssertTrue(second.records().isEmpty, "the in-memory stand-in is deliberately ephemeral")
    }
}

// MARK: - CrewColorAssignment

final class CrewColorAssignmentTests: XCTestCase {
    func testFirstFreeIndexIsZeroWhenNoneAreUsed() {
        XCTAssertEqual(CrewColorAssignment.nextFreeIndex(usedIndices: []), 0)
    }

    func testFirstFreeIndexSkipsUsedSlots() {
        XCTAssertEqual(CrewColorAssignment.nextFreeIndex(usedIndices: [0, 1, 2]), 3)
    }

    func testFirstFreeIndexFindsAGapRatherThanAppending() {
        // 0 and 2 used, 1 free — "first free", not "next after the max".
        XCTAssertEqual(CrewColorAssignment.nextFreeIndex(usedIndices: [0, 2]), 1)
    }

    func testAllEightUsedReturnsNil() {
        XCTAssertNil(CrewColorAssignment.nextFreeIndex(usedIndices: [0, 1, 2, 3, 4, 5, 6, 7]))
    }
}

// MARK: - CrewPairingRestorer — restore-before-replay ordering

@MainActor
final class CrewPairingRestorerTests: XCTestCase {
    func testRestoreMarksEveryPersistedRecordPairedWithItsPersistedColour() {
        let store = InMemoryCrewPairingStore()
        store.upsert(CrewPairingRecord(nodeID: 11, colorIndex: 4, nickname: "Dana"))
        store.upsert(CrewPairingRecord(nodeID: 22, colorIndex: 1))

        let crew = CrewStore()
        CrewPairingRestorer.restore(from: store, into: crew)

        let now = FireflyClock.nowMillis()
        let dana = crew.member(nodeID: 11, now: now)
        XCTAssertEqual(dana?.paired, true)
        XCTAssertEqual(dana?.colorIndex, 4)
        let other = crew.member(nodeID: 22, now: now)
        XCTAssertEqual(other?.paired, true)
        XCTAssertEqual(other?.colorIndex, 1)
    }

    /// The exact scenario `AppGraph.init`'s own comment describes: a
    /// member is restored PAIRED before a want_config-style identity
    /// replay (`setIdentity`, which no longer touches colour at all —
    /// `Bridge/CrewStore.swift`) ever mentions that node again. The
    /// paired flag and the colour must both survive it.
    func testPairedFlagAndColourSurviveAnIdentityReplayAfterRestore() {
        let store = InMemoryCrewPairingStore()
        store.upsert(CrewPairingRecord(nodeID: 99, colorIndex: 5))

        let crew = CrewStore()
        CrewPairingRestorer.restore(from: store, into: crew)

        // A want_config-style replay: the mesh re-announces this node's
        // identity, exactly what `CoreStore.apply(nodeUpdate:)` does on
        // reconnect.
        crew.setIdentity(nodeID: 99, shortName: "SAM", longName: "Sam")

        let member = crew.member(nodeID: 99, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.paired, true, "a replay must never silently unpair a restored member")
        XCTAssertEqual(member?.colorIndex, 5, "a replay must never reassign a restored member's colour")
        XCTAssertEqual(member?.shortName, "SAM")
    }

    func testEmptyStoreRestoresNothing() {
        let crew = CrewStore()
        CrewPairingRestorer.restore(from: InMemoryCrewPairingStore(), into: crew)
        XCTAssertEqual(crew.count, 0)
    }
}

// MARK: - CrewPairingController

@MainActor
final class CrewPairingControllerTests: XCTestCase {
    private func makeController() -> (CrewPairingController, InMemoryCrewPairingStore, CrewStore) {
        let store = InMemoryCrewPairingStore()
        let crew = CrewStore()
        return (CrewPairingController(crew: crew, store: store), store, crew)
    }

    func testPairingANewMemberAssignsTheFirstFreeColourAndPersists() {
        let (controller, store, crew) = makeController()
        let result = controller.pair(nodeID: 1)
        XCTAssertEqual(result, .paired(colorIndex: 0))
        XCTAssertEqual(crew.member(nodeID: 1, now: FireflyClock.nowMillis())?.paired, true)
        XCTAssertEqual(store.records().map(\.nodeID), [1])
    }

    func testColourAssignmentIsStableAcrossARepeatPairCall() {
        let (controller, _, _) = makeController()
        let first = controller.pair(nodeID: 1)
        let second = controller.pair(nodeID: 1) // e.g. the same node paired again after a reconnect
        XCTAssertEqual(first, second, "a repeat pairing must never reassign the member's colour")
    }

    func testTwoMembersGetDistinctColoursNeverACollision() {
        let (controller, _, _) = makeController()
        let a = controller.pair(nodeID: 1)
        let b = controller.pair(nodeID: 2)
        guard case .paired(let colorA) = a, case .paired(let colorB) = b else {
            return XCTFail("both pairings should succeed")
        }
        XCTAssertNotEqual(colorA, colorB)
    }

    /// The honest limit message (M2's own acceptance criterion) — the
    /// 9th distinct node is refused with `.full`, never silently
    /// dropped or force-added past `FF_CREW_MAX`.
    func testTheNinthDistinctMemberIsRefusedWithTheHonestLimit() {
        let (controller, _, _) = makeController()
        for nodeID in 1...8 {
            XCTAssertNotEqual(controller.pair(nodeID: UInt32(nodeID)), .full(limit: 8))
        }
        XCTAssertEqual(controller.pair(nodeID: 9), .full(limit: 8))
        XCTAssertEqual(controller.pairedRecords().count, 8, "the 9th pairing attempt must not have touched the roster")
    }

    func testUnpairRemovesBothTheLiveFlagAndThePersistedRecord() {
        let (controller, store, crew) = makeController()
        controller.pair(nodeID: 1)
        controller.unpair(nodeID: 1)
        XCTAssertEqual(crew.member(nodeID: 1, now: FireflyClock.nowMillis())?.paired, false)
        XCTAssertTrue(store.records().isEmpty)
    }

    /// Unpairing one member frees its colour for a later pairing —
    /// "first free index" has to mean the CURRENT roster, not a
    /// high-water mark.
    func testUnpairingFreesItsColourForTheNextPairing() {
        let (controller, _, _) = makeController()
        controller.pair(nodeID: 1) // color 0
        controller.pair(nodeID: 2) // color 1
        controller.unpair(nodeID: 1)
        XCTAssertEqual(controller.pair(nodeID: 3), .paired(colorIndex: 0))
    }

    func testRenameSetsANicknameThatDoesNotChangeTheMeshIdentity() {
        let (controller, _, crew) = makeController()
        crew.setIdentity(nodeID: 1, shortName: "SAM", longName: "Sam")
        controller.pair(nodeID: 1)
        controller.rename(nodeID: 1, nickname: "Sammy")

        XCTAssertEqual(controller.pairedRecords().first?.nickname, "Sammy")
        let member = crew.member(nodeID: 1, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.longName, "Sam", "a nickname is a local draft — it never overwrites the mesh's own name")
    }

    func testRenameToEmptyStringClearsTheNickname() {
        let (controller, _, _) = makeController()
        controller.pair(nodeID: 1)
        controller.rename(nodeID: 1, nickname: "Sammy")
        controller.rename(nodeID: 1, nickname: "")
        XCTAssertNil(controller.pairedRecords().first?.nickname)
    }

    func testIsFullReflectsThePairedCount() {
        let (controller, _, _) = makeController()
        XCTAssertFalse(controller.isFull)
        for nodeID in 1...8 { controller.pair(nodeID: UInt32(nodeID)) }
        XCTAssertTrue(controller.isFull)
    }
}
