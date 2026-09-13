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

    /// Review finding (measured): the festival namespace is not just a
    /// label — it is a field inside this store's own comma-joined,
    /// `"|"`-separated tokens, and its slug half is authored by
    /// fest-almanac, not by this app. Before `SettingsStoring
    /// .sanitizeNamespace`, a slug carrying a `","` wrote the token
    /// `a,b-2026|<base64>`, which splits on that comma and reads back
    /// as NO picks at all — a pick silently destroyed the moment it was
    /// made, with no error on any surface. Mutation check: dropping the
    /// `sanitizeNamespace` call from `festivalNamespace()` fails this
    /// test's round trip.
    func testPicksSurviveAFestivalSlugCarryingATokenSeparator() {
        for hostileSlug in ["a,b", "x|y", "with space", "../escape"] {
            let store = InMemorySettingsStore()
            store.setString(hostileSlug, .festivalSelectedSlug)
            store.setString("2026", .festivalSelectedYear)
            let picks = PicksStore(store: store, namespace: { store.festivalNamespace() })

            picks.setPicked(true, setID: "main-2026-09-18-21:00-excision")

            XCTAssertEqual(picks.pickedSetIDs(), ["main-2026-09-18-21:00-excision"],
                           "slug \(hostileSlug) must round-trip")
            let blob = store.string(.pickedFestivalSetIDs) ?? ""
            XCTAssertEqual(blob.filter { $0 == "|" }.count, 1, "exactly one separator in the token")
            XCTAssertFalse(blob.contains(","), "and no stray token boundary inside the namespace")
        }
    }

    /// The same sanitization has to leave every REAL slug — and the
    /// legacy default — byte-identical, or it would silently
    /// re-namespace existing users' picks.
    func testSanitizationLeavesRealSlugsAndTheLegacyDefaultUnchanged() {
        let store = InMemorySettingsStore()
        XCTAssertEqual(store.festivalNamespace(), PicksStore.legacyNamespace)
        for (slug, expected) in [("lost-lands", "lost-lands-2026"), ("wakaan", "wakaan-2026"),
                                  ("boo-seattle", "boo-seattle-2026"), ("edc-orlando", "edc-orlando-2026")] {
            store.setString(slug, .festivalSelectedSlug)
            store.setString("2026", .festivalSelectedYear)
            XCTAssertEqual(store.festivalNamespace(), expected)
        }
    }
}
