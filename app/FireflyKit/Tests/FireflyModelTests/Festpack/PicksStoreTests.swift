//
//  PicksStoreTests.swift — persistence + toggle semantics.
//
import FireflyModel
import XCTest

final class PicksStoreTests: XCTestCase {
    func testPickingPersistsThroughANewStoreInstanceOverTheSameBackingStore() {
        let backing = InMemorySettingsStore()
        let store1 = PicksStore(store: backing)
        store1.setPicked(true, setID: "prehistoric-2026-09-18-22:10-excision")
        store1.setPicked(true, setID: "wompy-woods-2026-09-19-02:05-riot") // colon in the id, deliberately

        let store2 = PicksStore(store: backing) // simulates a relaunch over the same disk file
        XCTAssertEqual(store2.pickedSetIDs(), ["prehistoric-2026-09-18-22:10-excision", "wompy-woods-2026-09-19-02:05-riot"])
    }

    func testUnpickingRemovesOnlyThatSet() {
        let backing = InMemorySettingsStore()
        let store = PicksStore(store: backing)
        store.setPicked(true, setID: "a-2026-09-18-20:00-one")
        store.setPicked(true, setID: "a-2026-09-18-21:00-two")
        store.setPicked(false, setID: "a-2026-09-18-20:00-one")
        XCTAssertEqual(store.pickedSetIDs(), ["a-2026-09-18-21:00-two"])
    }

    func testToggleFlipsCurrentState() {
        let store = InMemoryPicksStore()
        XCTAssertFalse(store.isPicked("a-2026-09-18-20:00-one"))
        store.toggle("a-2026-09-18-20:00-one")
        XCTAssertTrue(store.isPicked("a-2026-09-18-20:00-one"))
        store.toggle("a-2026-09-18-20:00-one")
        XCTAssertFalse(store.isPicked("a-2026-09-18-20:00-one"))
    }

    func testEmptySetPersistsAsAbsentNotAnEmptyString() {
        let backing = InMemorySettingsStore()
        let store = PicksStore(store: backing)
        store.setPicked(true, setID: "a-2026-09-18-20:00-one")
        store.setPicked(false, setID: "a-2026-09-18-20:00-one")
        XCTAssertNil(backing.string(.pickedFestivalSetIDs))
        XCTAssertTrue(store.pickedSetIDs().isEmpty)
    }

    // MARK: - Per-festival namespacing ("app: automatic almanac refresh
    // + festival picker", owner ask #2)

    /// Test-only, lock-protected mutable-namespace box — `namespace:`
    /// is a `@Sendable () -> String` closure, so a plain captured `var`
    /// (what this used to be) is the exact race Swift 6 is right to
    /// flag, same reasoning `LocationProviderTests`' `LockedTestClock`
    /// already states for `PhoneGPSUplink`'s `now:`.
    private final class LockedNamespaceBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value: String
        init(_ initial: String) { value = initial }
        func get() -> String { lock.lock(); defer { lock.unlock() }; return value }
        func set(_ newValue: String) { lock.lock(); value = newValue; lock.unlock() }
    }

    func testPicksMadeUnderOneFestivalDoNotShowUnderAnother() {
        let backing = InMemorySettingsStore()
        let namespace = LockedNamespaceBox("lost-lands-2026")
        let store = PicksStore(store: backing, namespace: { namespace.get() })

        store.setPicked(true, setID: "a-2026-09-18-20:00-one")
        XCTAssertEqual(store.pickedSetIDs(), ["a-2026-09-18-20:00-one"])

        namespace.set("other-fest-2027")
        XCTAssertTrue(store.pickedSetIDs().isEmpty, "a different festival's picks must start empty")
        store.setPicked(true, setID: "b-2027-08-01-10:00-two")
        XCTAssertEqual(store.pickedSetIDs(), ["b-2027-08-01-10:00-two"])

        namespace.set("lost-lands-2026")
        XCTAssertEqual(store.pickedSetIDs(), ["a-2026-09-18-20:00-one"],
                       "switching back must show exactly what was picked there, untouched by the other festival")
    }

    func testTwoStoresOverTheSameBackingRespectEachOthersNamespace() {
        let backing = InMemorySettingsStore()
        let lostLands = PicksStore(store: backing, namespace: { "lost-lands-2026" })
        let otherFest = PicksStore(store: backing, namespace: { "other-fest-2027" })

        lostLands.setPicked(true, setID: "a-2026-09-18-20:00-one")
        otherFest.setPicked(true, setID: "b-2027-08-01-10:00-two")

        XCTAssertEqual(lostLands.pickedSetIDs(), ["a-2026-09-18-20:00-one"])
        XCTAssertEqual(otherFest.pickedSetIDs(), ["b-2027-08-01-10:00-two"])

        otherFest.setPicked(false, setID: "b-2027-08-01-10:00-two")
        XCTAssertEqual(lostLands.pickedSetIDs(), ["a-2026-09-18-20:00-one"],
                       "removing one festival's pick must never touch another's")
    }

    /// MIGRATION: a pick persisted before per-festival namespacing
    /// shipped is a bare base64 token with no `"|"` separator — it must
    /// land under `PicksStore.legacyNamespace` ("lost-lands-2026") the
    /// first time it is read, and the migrated form must be PERSISTED
    /// back, not merely reinterpreted on every call.
    func testLegacyUnNamespacedPicksMigrateToLostLands2026() {
        let backing = InMemorySettingsStore()
        let legacyToken = Data("a-2026-09-18-20:00-one".utf8).base64EncodedString()
        backing.setString(legacyToken, .pickedFestivalSetIDs)

        let lostLands = PicksStore(store: backing, namespace: { "lost-lands-2026" })
        XCTAssertEqual(lostLands.pickedSetIDs(), ["a-2026-09-18-20:00-one"])

        // The migration must have PERSISTED — the raw stored string is
        // no longer the bare legacy token.
        let migratedRaw = backing.string(.pickedFestivalSetIDs)
        XCTAssertEqual(migratedRaw, "lost-lands-2026|\(legacyToken)")

        // And it must never leak into some OTHER festival's namespace.
        let otherFest = PicksStore(store: backing, namespace: { "other-fest-2027" })
        XCTAssertTrue(otherFest.pickedSetIDs().isEmpty)
    }

    func testMigrationCoexistsWithAlreadyNamespacedPicks() {
        let backing = InMemorySettingsStore()
        let legacyToken = Data("legacy-set".utf8).base64EncodedString()
        let namespacedToken = "other-fest-2027|" + Data("new-set".utf8).base64EncodedString()
        backing.setString("\(legacyToken),\(namespacedToken)", .pickedFestivalSetIDs)

        let lostLands = PicksStore(store: backing, namespace: { "lost-lands-2026" })
        XCTAssertEqual(lostLands.pickedSetIDs(), ["legacy-set"])
        let otherFest = PicksStore(store: backing, namespace: { "other-fest-2027" })
        XCTAssertEqual(otherFest.pickedSetIDs(), ["new-set"])
    }
}
