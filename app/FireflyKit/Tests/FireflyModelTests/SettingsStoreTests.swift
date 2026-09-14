//
//  SettingsStoreTests.swift — settings persistence (docs/specs/
//  A01-companion-app.md, Slice C "Must add": "settings persistence").
//
//  Each test gets its own ephemeral `UserDefaults` suite so runs never
//  see another test's (or a real launch's) leftover values — the same
//  hermeticity `InMemorySettingsStore` gives tests for free, applied
//  here because this store is deliberately NOT in-memory.
//
import FireflyModel
import XCTest

final class SettingsStoreTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "SettingsStoreTests.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        suiteName = nil
        super.tearDown()
    }

    func testUnsetStringReadsNil() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertNil(store.string(.lastPeripheralID))
    }

    func testStringRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        store.setString("AA:BB:CC", .lastPeripheralID)
        XCTAssertEqual(store.string(.lastPeripheralID), "AA:BB:CC")
    }

    func testClearingAStringRemovesIt() {
        let store = SettingsStore(defaults: defaults)
        store.setString("AA:BB:CC", .lastPeripheralID)
        store.setString(nil, .lastPeripheralID)
        XCTAssertNil(store.string(.lastPeripheralID))
    }

    func testBoolDefaultsFalse() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertFalse(store.bool(.locationSharingEnabled))
    }

    func testBoolRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        store.setBool(true, .locationSharingEnabled)
        XCTAssertTrue(store.bool(.locationSharingEnabled))
    }

    /// `nil` (never set) must stay distinguishable from an explicit
    /// `0` — the same rule `SettingsStoring`'s own doc comment states,
    /// and the reason `double(_:)` is `Double?` rather than `Double`.
    func testUnsetDoubleReadsNilNotZero() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertNil(store.double(.locationSharingIntervalSeconds))
    }

    func testDoubleRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        store.setDouble(30.0, .locationSharingIntervalSeconds)
        XCTAssertEqual(store.double(.locationSharingIntervalSeconds), 30.0)
    }

    func testDoubleCanBeExplicitlyZero() {
        let store = SettingsStore(defaults: defaults)
        store.setDouble(0.0, .locationSharingIntervalSeconds)
        XCTAssertEqual(store.double(.locationSharingIntervalSeconds), 0.0)
    }

    /// A second `SettingsStore` over the SAME `UserDefaults` sees what
    /// the first one wrote — the actual property under test: this is a
    /// real backing store, not a mock that resets every launch.
    func testValuesSurviveAFreshStoreInstanceOverTheSameDefaults() {
        SettingsStore(defaults: defaults).setBool(true, .unitsMetric)
        let reopened = SettingsStore(defaults: defaults)
        XCTAssertTrue(reopened.bool(.unitsMetric))
    }

    // MARK: - Extra, Settings/Diagnostics-only preferences

    func testColorblindPaletteDefaultsFalseAndRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertFalse(store.colorblindPaletteEnabled)
        store.colorblindPaletteEnabled = true
        XCTAssertTrue(store.colorblindPaletteEnabled)
    }

    /// A03_AC9 — **this test's name is the product decision.** It used
    /// to be `testBackgroundConnectDefaultsFalseAndRoundTrips`, and it
    /// was pinning the single highest-cost default in the app: with
    /// nothing persisted, `UserDefaults.bool` read `false`, so
    /// `AppGraph.handleScenePhaseChange(.background)` disconnected the
    /// radio and cancelled the notification subscription the moment the
    /// screen locked. Out of the box, Firefly went deaf in a pocket —
    /// which is the one place this product is for (A03 §3.3, audit
    /// 2.4.18). Inverted rather than deleted, so the change is visible
    /// in the diff of a test whose name states the decision.
    func testA03_AC9_BackgroundConnectDefaultsTrueAndRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertTrue(store.backgroundConnectEnabled, "nothing persisted means ON (A03 §3.3)")
        store.backgroundConnectEnabled = true
        XCTAssertTrue(store.backgroundConnectEnabled)
    }

    /// A03_AC9, the other half — and the migration rule. Someone who has
    /// explicitly turned this OFF keeps it off across the upgrade that
    /// flips the default: the setter always writes an explicit value, so
    /// their `false` is on disk and the three-state read returns it. A
    /// default change must not overrule a choice somebody made.
    func testA03_AC9_AnExplicitFalseSurvivesTheDefaultFlip() {
        let store = SettingsStore(defaults: defaults)
        store.backgroundConnectEnabled = false
        XCTAssertFalse(store.backgroundConnectEnabled)
        // A second store over the same defaults IS the next launch.
        let nextLaunch = SettingsStore(defaults: defaults)
        XCTAssertFalse(nextLaunch.backgroundConnectEnabled,
                        "an explicit OFF is a choice; the new default must not overrule it")
    }

    /// A03_AC9, the migration proved against the DISK rather than
    /// against our own setter (REVIEW FIX, PR #310).
    ///
    /// `testA03_AC9_AnExplicitFalseSurvivesTheDefaultFlip` above writes
    /// through `SettingsStore`'s own setter, so it holds whatever key
    /// that setter happens to use today — it would pass unchanged if the
    /// key were renamed, while every real upgrade silently flipped to
    /// ON. The migration claim is about a value written by a PREVIOUS
    /// BUILD, so the only honest way to state it is to put that value
    /// into `UserDefaults` by its literal key and read it back through
    /// the new three-state getter.
    ///
    /// The literal is deliberately spelled out rather than composed from
    /// `FireflyExtraSettingsKey` + the private prefix: a test that
    /// derives the key from the same source the code does cannot detect
    /// the key changing, which is the whole failure this test exists for.
    func testA03_AC9_AnOffWrittenByAPreviousBuildIsStillOffAtTheRawKey() {
        let key = "firefly.settings.backgroundConnectEnabled"
        // Exactly what a build before A03 left behind for someone who
        // turned the toggle off.
        defaults.set(false, forKey: key)

        let store = SettingsStore(defaults: defaults)
        XCTAssertFalse(store.backgroundConnectEnabled,
                        "an OFF written by an older build must survive the default flip")

        // ...and the absence of that key — a fresh install, or an
        // upgrade from a build that never wrote it — is the ON default.
        defaults.removeObject(forKey: key)
        XCTAssertNil(defaults.object(forKey: key), "the key really is unset")
        XCTAssertTrue(SettingsStore(defaults: defaults).backgroundConnectEnabled)

        // An explicit TRUE at the raw key reads true as well, so the
        // three-state read is genuinely three-state and not "anything
        // present means false".
        defaults.set(true, forKey: key)
        XCTAssertTrue(SettingsStore(defaults: defaults).backgroundConnectEnabled)
    }

    /// The stand-in store must agree with the real one about the
    /// default, or every `.stub()`/demo composition exercises a
    /// lifecycle path no real install takes.
    func testA03_AC9_InMemoryStoreAgreesAboutTheDefault() {
        let store = InMemorySettingsStore()
        XCTAssertTrue(store.backgroundConnectEnabled)
        store.backgroundConnectEnabled = false
        XCTAssertFalse(store.backgroundConnectEnabled)
    }

    func testNodeNameDraftsRoundTripAndClear() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertNil(store.nodeLongNamePreference)
        store.nodeLongNamePreference = "Firefly One"
        store.nodeShortNamePreference = "FF1"
        XCTAssertEqual(store.nodeLongNamePreference, "Firefly One")
        XCTAssertEqual(store.nodeShortNamePreference, "FF1")
        store.nodeLongNamePreference = nil
        XCTAssertNil(store.nodeLongNamePreference)
    }

    /// The extra keys share the same `UserDefaults` as the six
    /// `SettingsKey` ones but are namespaced separately (`rawValue`s
    /// come from a different enum) — one must never shadow the other.
    func testExtraKeysDoNotCollideWithSettingsKeys() {
        let store = SettingsStore(defaults: defaults)
        store.setBool(true, .unitsMetric)
        store.backgroundConnectEnabled = false
        XCTAssertTrue(store.bool(.unitsMetric))
        XCTAssertFalse(store.backgroundConnectEnabled)
    }
}
