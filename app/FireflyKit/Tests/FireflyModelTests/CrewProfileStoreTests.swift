//
//  CrewProfileStoreTests.swift — the `UserDefaults`-backed
//  `CrewProfileStore`/`CrewHiddenStore` round trip
//  (`docs/specs/A02-crew-join.md`, §2.1 step 3, §4.5). The Keychain-
//  backed `CrewSnapshotKeychainStore` is exercised only via
//  `CrewControllerTests`' in-memory stand-in (`CrewControllerTests`,
//  app target) — a bare `swift test` process is not a signed, entitled
//  app bundle, so a real Keychain call there is exactly the kind of
//  environment-dependent flake this suite avoids.
//
import Foundation
import XCTest
@testable import FireflyModel

final class CrewProfileStoreTests: XCTestCase {
    private func makeDefaults() -> UserDefaults {
        let suiteName = "CrewProfileStoreTests.\(UUID().uuidString)"
        addTeardownBlock { UserDefaults().removePersistentDomain(forName: suiteName) }
        return UserDefaults(suiteName: suiteName)!
    }

    func testProfileRoundTripsAcrossInstances() {
        let defaults = makeDefaults()
        let store1 = CrewProfileStore(defaults: defaults)
        XCTAssertNil(store1.load())

        let profile = CrewProfile(code: "FIRE-4K9M7X", humanName: "Camp Firefly", createdAtMs: 1_700_000_000_000)
        store1.save(profile)

        let store2 = CrewProfileStore(defaults: defaults)
        XCTAssertEqual(store2.load(), profile)

        store2.clear()
        XCTAssertNil(store1.load())
    }

    func testRecentCrewsCapsAtFourNewestFirst() {
        let defaults = makeDefaults()
        let store = CrewProfileStore(defaults: defaults)
        for index in 0..<5 {
            store.rememberRecentCrew(RecentCrew(code: "FIRE-00000\(index)", humanName: "Crew \(index)"))
        }
        let recents = store.recentCrews()
        XCTAssertEqual(recents.count, 4)
        XCTAssertEqual(recents.first?.humanName, "Crew 4", "newest first")
        XCTAssertFalse(recents.contains { $0.humanName == "Crew 0" }, "oldest evicted past the cap of 4")
    }

    func testHiddenSetIsKeyedPerCrewCodeAndRoundTrips() {
        let defaults = makeDefaults()
        let store1 = CrewHiddenStore(defaults: defaults)
        XCTAssertEqual(store1.hiddenIDs(forCrew: "FIRE-4K9M7X"), [])

        store1.setHiddenIDs([111, 222], forCrew: "FIRE-4K9M7X")
        store1.setHiddenIDs([333], forCrew: "FIRE-000000")

        let store2 = CrewHiddenStore(defaults: defaults)
        XCTAssertEqual(store2.hiddenIDs(forCrew: "FIRE-4K9M7X"), [111, 222])
        XCTAssertEqual(store2.hiddenIDs(forCrew: "FIRE-000000"), [333], "a different crew code is a different hide list")
    }
}
