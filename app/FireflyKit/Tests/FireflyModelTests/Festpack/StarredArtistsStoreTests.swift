//
//  StarredArtistsStoreTests.swift — persistence + toggle semantics.
//
import FireflyModel
import XCTest

final class StarredArtistsStoreTests: XCTestCase {
    func testStarringPersistsThroughANewStoreInstanceOverTheSameBackingStore() {
        let backing = InMemorySettingsStore()
        let store1 = StarredArtistsStore(store: backing)
        store1.setStarred(true, artist: "Excision")
        store1.setStarred(true, artist: "Sippy, the Duo") // comma in the name, deliberately

        let store2 = StarredArtistsStore(store: backing) // simulates a relaunch over the same disk file
        XCTAssertEqual(store2.starredArtists(), ["Excision", "Sippy, the Duo"])
    }

    func testUnstarringRemovesOnlyThatArtist() {
        let backing = InMemorySettingsStore()
        let store = StarredArtistsStore(store: backing)
        store.setStarred(true, artist: "Excision")
        store.setStarred(true, artist: "Sippy")
        store.setStarred(false, artist: "Excision")
        XCTAssertEqual(store.starredArtists(), ["Sippy"])
    }

    func testToggleFlipsCurrentState() {
        let store = InMemoryStarredArtistsStore()
        XCTAssertFalse(store.isStarred("Excision"))
        store.toggle("Excision")
        XCTAssertTrue(store.isStarred("Excision"))
        store.toggle("Excision")
        XCTAssertFalse(store.isStarred("Excision"))
    }

    func testEmptySetPersistsAsAbsentNotAnEmptyString() {
        let backing = InMemorySettingsStore()
        let store = StarredArtistsStore(store: backing)
        store.setStarred(true, artist: "Excision")
        store.setStarred(false, artist: "Excision")
        XCTAssertNil(backing.string(.starredFestivalArtists))
        XCTAssertTrue(store.starredArtists().isEmpty)
    }
}
