//
//  CrewPairingStoreTests.swift — M2's persisted crew pairing (docs/specs/
//  A01-companion-app.md, M2: "Crew pairing and colours, driven by
//  `ff_crew`"). Covers: store round-trip, colour-assignment stability
//  (first free index in roster order), restore-before-replay ordering,
//  the 8-limit message, `unpair()`'s crash-safe write order (persisted
//  record removed before the live flag clears — PR #270 review,
//  SHOULD-FIX 1), and a regression pin for `pair()` staying honest
//  about "full" when the roster's 8 slots are unpaired strangers
//  rather than paired members (PR #270 review, SHOULD-FIX 2 — depends
//  on core PR #268's eviction).
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

/// Wraps an `InMemoryCrewPairingStore` but captures, at the instant
/// `remove(nodeID:)` runs, what `ff_crew`'s live paired flag was still
/// reporting for that node — used by
/// `CrewPairingControllerTests.testUnpairRemovesThePersistedRecordBeforeClearingTheLiveFlag`
/// to prove `CrewPairingController.unpair()` clears the persisted
/// record before the live flag, not after (PR #270 review, SHOULD-FIX
/// 1: the crash-safe order).
private final class OrderSpyCrewPairingStore: CrewPairingStoring, @unchecked Sendable {
    private let wrapped = InMemoryCrewPairingStore()
    var crew: CrewStore!
    private(set) var pairedFlagAtRemoveTime: Bool?

    func records() -> [CrewPairingRecord] { wrapped.records() }
    func upsert(_ record: CrewPairingRecord) { wrapped.upsert(record) }
    func remove(nodeID: UInt32) {
        pairedFlagAtRemoveTime = crew.member(nodeID: nodeID, now: FireflyClock.nowMillis())?.paired
        wrapped.remove(nodeID: nodeID)
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

    /// Crash-safe ordering (PR #270 review, SHOULD-FIX 1): `unpair()`
    /// must remove the persisted record BEFORE it clears `ff_crew`'s
    /// live paired flag. The persisted store is what a relaunch
    /// rebuilds `ff_crew` from (`CrewPairingRestorer.restore`), so a
    /// process kill between the two writes must never leave the
    /// persisted record still saying "paired" for someone the live
    /// flag has already dropped — that's the failure mode that would
    /// silently re-pair a removed member on the next launch. This test
    /// uses a spy store that captures the live flag's value at the
    /// instant `remove` runs: it must still read `true`, proving
    /// `store.remove` executes first.
    func testUnpairRemovesThePersistedRecordBeforeClearingTheLiveFlag() {
        let spyStore = OrderSpyCrewPairingStore()
        let crew = CrewStore()
        spyStore.crew = crew
        let controller = CrewPairingController(crew: crew, store: spyStore)

        controller.pair(nodeID: 1)
        controller.unpair(nodeID: 1)

        XCTAssertEqual(
            spyStore.pairedFlagAtRemoveTime, true,
            "the live flag must still read paired when the persisted record is removed — " +
            "proving store.remove() runs before crew.setPaired(false), the crash-safe order"
        )
        // ...and both halves still end up consistent once unpair() returns.
        XCTAssertEqual(crew.member(nodeID: 1, now: FireflyClock.nowMillis())?.paired, false)
        XCTAssertTrue(spyStore.records().isEmpty)
    }

    /// Regression pin (PR #270 review, SHOULD-FIX 2): before core PR
    /// #268's eviction lands, a roster whose 8 slots are all occupied
    /// by unpaired strangers (heard off `nodeUpdates()`, never paired)
    /// makes `crew.upsert(nodeID:)` fail for a 9th, distinct node even
    /// though 0 members are actually paired — `pair()` would then
    /// return `.full(limit: 8)` and the UI would show "Crew is full"
    /// while genuinely nobody is paired, a real honesty bug on a busy
    /// public mesh. #268's eviction makes `ff_crew_upsert` evict an
    /// unpaired stranger to make room instead, so this must SUCCEED.
    /// This test is expected to start passing only once #268 has
    /// landed and this branch has rebased onto it — that is the point:
    /// CI pins the day the misbehavior described above stops
    /// reproducing.
    func testPairingSucceedsWithEightUnpairedStrangersAlreadyOccupyingTheRoster() {
        let (controller, store, crew) = makeController()
        // 8 strangers heard off the mesh, never paired — exactly what
        // fills a busy public mesh's roster before anyone pairs at all.
        for nodeID in 1...8 {
            crew.upsert(nodeID: UInt32(nodeID))
        }
        XCTAssertEqual(store.records().count, 0, "none of the 8 strangers are paired yet")

        let result = controller.pair(nodeID: 100)

        XCTAssertNotEqual(result, .full(limit: 8),
            "0 paired members must never report the roster as full, even when all 8 slots are held by unpaired strangers")
        guard case .paired = result else {
            return XCTFail("pairing the first-ever member must succeed once #268's eviction is in place, got \(result)")
        }
        XCTAssertEqual(store.records().map(\.nodeID), [100])
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
