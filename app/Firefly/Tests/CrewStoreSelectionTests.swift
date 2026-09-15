//
//  CrewStoreSelectionTests.swift — the demo-isolation rule
//  `CrewStoreSelection` exists to pin (that file's own header comment):
//  a demo composition must never be handed the real, persistent
//  `CrewSnapshotKeychainStore`/`CrewHiddenStore`.
//
import FireflyMesh
import FireflyModel
import XCTest

final class CrewStoreSelectionTests: XCTestCase {

    // MARK: - Demo composition — always in-memory

    func testDemoCompositionGetsTheInMemorySnapshotStore() {
        let dependencies = AppDependencies.demoBundle().dependencies
        XCTAssertTrue(CrewStoreSelection.snapshotStore(for: dependencies) is InMemoryCrewSnapshotStore,
                      "the demo composition must never touch the real, Keychain-backed pre-crew snapshot")
    }

    func testDemoCompositionGetsTheInMemoryHiddenStore() {
        let dependencies = AppDependencies.demoBundle().dependencies
        XCTAssertTrue(CrewStoreSelection.hiddenStore(for: dependencies) is InMemoryCrewHiddenStore,
                      "the demo composition must never write to the real, UserDefaults-backed hidden-crew store")
    }

    // MARK: - Every other composition — the real, persistent stores, unchanged

    func testStubCompositionStillGetsTheRealSnapshotStore() {
        XCTAssertTrue(CrewStoreSelection.snapshotStore(for: .stub()) is CrewSnapshotKeychainStore,
                      "a non-demo composition must keep the real pre-crew snapshot store")
    }

    func testStubCompositionStillGetsTheRealHiddenStore() {
        XCTAssertTrue(CrewStoreSelection.hiddenStore(for: .stub()) is CrewHiddenStore,
                      "a non-demo composition must keep the real hidden-crew store")
    }
}
