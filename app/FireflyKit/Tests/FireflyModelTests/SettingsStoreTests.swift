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

    func testBackgroundConnectDefaultsFalseAndRoundTrips() {
        let store = SettingsStore(defaults: defaults)
        XCTAssertFalse(store.backgroundConnectEnabled)
        store.backgroundConnectEnabled = true
        XCTAssertTrue(store.backgroundConnectEnabled)
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
