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
}
