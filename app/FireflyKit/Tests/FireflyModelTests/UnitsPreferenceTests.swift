//
//  UnitsPreferenceTests.swift — the tri-state units setting (M2; PR #265's
//  own review flagged the bug this replaces — see `SettingsStoring.swift`'s
//  "Units preference" section and `AppGraph.makeRadarViewModel`'s comment).
//
//  New file rather than additions to `SettingsStoreTests.swift`: that file
//  is not itself shared infra, but several other M2 slices land alongside
//  this one this sprint, and a fresh file confined to exactly this
//  feature's tests is the one least likely to collide with theirs.
//
import FireflyModel
import XCTest

final class UnitsPreferenceTests: XCTestCase {

    // MARK: - Default, before anything is ever written

    func testInMemoryStoreDefaultsToSystem() {
        let store = InMemorySettingsStore()
        XCTAssertEqual(store.unitsPreference(), .system,
                        "a fresh install must not guess metric or imperial")
    }

    func testUserDefaultsBackedStoreDefaultsToSystem() {
        let suiteName = "UnitsPreferenceTests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defer { defaults.removePersistentDomain(forName: suiteName) }
        XCTAssertEqual(SettingsStore(defaults: defaults).unitsPreference(), .system)
    }

    /// A value this build does not recognize (a future case, or on-disk
    /// corruption) must fall back to `.system`, never crash and never
    /// silently coerce to a specific unit.
    func testUnrecognizedStoredValueFallsBackToSystem() {
        let store = InMemorySettingsStore()
        store.setString("furlongs", .unitsPreference)
        XCTAssertEqual(store.unitsPreference(), .system)
    }

    // MARK: - Persistence round-trip (tri-state, not a bool)

    func testInMemoryStoreRoundTripsEveryCase() {
        let store = InMemorySettingsStore()
        for value in UnitsPreference.allCases {
            store.setUnitsPreference(value)
            XCTAssertEqual(store.unitsPreference(), value)
        }
    }

    /// The real property under test for the `UserDefaults`-backed store:
    /// a SECOND instance over the SAME defaults sees what the first
    /// wrote — this is persistence, not an in-process cache.
    func testUserDefaultsBackedStoreRoundTripsAcrossInstances() {
        let suiteName = "UnitsPreferenceTests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defer { defaults.removePersistentDomain(forName: suiteName) }

        SettingsStore(defaults: defaults).setUnitsPreference(.imperial)
        XCTAssertEqual(SettingsStore(defaults: defaults).unitsPreference(), .imperial)

        SettingsStore(defaults: defaults).setUnitsPreference(.metric)
        XCTAssertEqual(SettingsStore(defaults: defaults).unitsPreference(), .metric)
    }

    /// The tri-state's own key (`unitsPreference`) must not collide with
    /// the old bool key (`unitsMetric`) it replaces — both exist in
    /// `SettingsKey` at once (append-only: the old case was never
    /// removed), so this pins that they are genuinely separate slots.
    func testDoesNotCollideWithTheOldBoolKey() {
        let store = InMemorySettingsStore()
        store.setBool(true, .unitsMetric)
        store.setUnitsPreference(.imperial)
        XCTAssertTrue(store.bool(.unitsMetric))
        XCTAssertEqual(store.unitsPreference(), .imperial)
    }

    // MARK: - System-default resolution

    /// en_US: `Locale.MeasurementSystem.us` — the whole reason `.system`
    /// exists rather than defaulting straight to metric.
    func testSystemResolvesImperialForAUSLocale() {
        XCTAssertTrue(UnitsPreference.system.resolvedImperial(locale: Locale(identifier: "en_US")))
    }

    /// en_GB: `.uk` — the UK is otherwise metric, but its own road
    /// distances are miles; `Locale.MeasurementSystem` carries `.uk` as
    /// its own case for exactly this, so it must read imperial too.
    func testSystemResolvesImperialForAUKLocale() {
        XCTAssertTrue(UnitsPreference.system.resolvedImperial(locale: Locale(identifier: "en_GB")))
    }

    func testSystemResolvesMetricForAMetricLocale() {
        XCTAssertFalse(UnitsPreference.system.resolvedImperial(locale: Locale(identifier: "de_DE")))
        XCTAssertFalse(UnitsPreference.system.resolvedImperial(locale: Locale(identifier: "fr_FR")))
    }

    /// An explicit choice must never be overridden by locale, in either
    /// direction — that is the entire point of choosing rather than
    /// leaving it on `.system`.
    func testExplicitChoiceIgnoresLocale() {
        XCTAssertFalse(UnitsPreference.metric.resolvedImperial(locale: Locale(identifier: "en_US")))
        XCTAssertTrue(UnitsPreference.imperial.resolvedImperial(locale: Locale(identifier: "de_DE")))
    }

    func testStoreResolvedImperialConvenienceMatchesPreferenceResolution() {
        let store = InMemorySettingsStore()
        store.setUnitsPreference(.imperial)
        XCTAssertTrue(store.resolvedImperial(locale: Locale(identifier: "de_DE")))

        store.setUnitsPreference(.system)
        XCTAssertTrue(store.resolvedImperial(locale: Locale(identifier: "en_US")))
        XCTAssertFalse(store.resolvedImperial(locale: Locale(identifier: "de_DE")))
    }
}
