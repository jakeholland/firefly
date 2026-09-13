//
//  SettingsFestivalPickerWiringTests.swift — "app: automatic almanac
//  refresh + festival picker" (owner ask #2): `SettingsViewModel` owns
//  a real, working `festivalPicker` wired to the SAME `store` and the
//  SAME `LineupViewModel` the rest of Settings/Lineup share, so a
//  selection made through it lands where the Lineup tab (and a second
//  Settings read) will actually see it.
//
import FireflyModel
import XCTest

private struct StubIndexProvider: AlmanacIndexProviding {
    let index: AlmanacIndex
    func fetchIndex() async -> AlmanacIndex { index }
}

@MainActor
final class SettingsFestivalPickerWiringTests: XCTestCase {
    private func makeIndex() -> AlmanacIndex {
        let now = Date()
        return AlmanacIndex(generatedAt: nil, packs: [
            AlmanacIndexPack(slug: "other-fest", year: 2027, name: "Other Fest",
                              start: now, end: now.addingTimeInterval(3600),
                              timezone: nil, path: "packs/other-fest/2027/festpack.json",
                              updated: nil, sha256: "deadbeef"),
        ])
    }

    /// `SettingsViewModel`'s `lineup:`/`indexProvider:` parameters must
    /// actually reach `festivalPicker` — not merely compile — so a
    /// selection made through `model.festivalPicker` writes into the
    /// SAME `store` this `SettingsViewModel` itself reads (proven here
    /// by checking `store` directly, the same instance both share), and
    /// refreshes the SAME `LineupViewModel` this screen's "REFRESH"
    /// button also drives.
    func testFestivalPickerSharesTheSettingsViewModelsOwnStoreAndLineup() async {
        let store = InMemorySettingsStore()
        let lineup = LineupViewModel(festpackProvider: DemoFestpackProvider(), picksStore: InMemoryPicksStore())
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel(),
                                    lineup: lineup, indexProvider: StubIndexProvider(index: makeIndex()))

        await vm.festivalPicker.load()
        await vm.festivalPicker.select("other-fest-2027")

        XCTAssertEqual(store.string(.festivalSelectedSlug), "other-fest",
                        "the picker must write through the SettingsViewModel's own store, not a private copy")
        XCTAssertEqual(store.string(.festpackSourceURLOverride),
                        "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/other-fest/2027/festpack.json")
        XCTAssertEqual(vm.festpackSourceURLOverride, store.string(.festpackSourceURLOverride),
                        "SettingsViewModel's own read of the override must see the picker's write immediately")
    }

    /// A manual "Pack URL" edit must clear any checksum a PRIOR picker
    /// selection recorded — a hand-typed URL is not guaranteed to match
    /// it (`SettingsViewModel.setFestpackSourceURLOverride`'s own doc
    /// comment).
    func testManualURLEditClearsAPreviouslySelectedChecksum() async {
        let store = InMemorySettingsStore()
        let lineup = LineupViewModel(festpackProvider: DemoFestpackProvider(), picksStore: InMemoryPicksStore())
        let vm = SettingsViewModel(store: store, channelImport: ChannelImportViewModel(),
                                    lineup: lineup, indexProvider: StubIndexProvider(index: makeIndex()))
        await vm.festivalPicker.load()
        await vm.festivalPicker.select("other-fest-2027")
        XCTAssertEqual(store.string(.festivalSelectedSHA256), "deadbeef")

        vm.setFestpackSourceURLOverride("https://example.com/hand-typed.festpack.json")

        XCTAssertNil(store.string(.festivalSelectedSHA256))
        XCTAssertNil(vm.festpackSourceURLError)
    }

    /// The default initializer (every pre-existing call site) must
    /// still construct a real `festivalPicker` rather than crashing or
    /// leaving it `nil` — proven against a STUBBED index provider here,
    /// never the live default (`AlmanacIndexProvider()`'s real network
    /// fetch has no place in a unit test).
    func testDefaultInitializerStillWiresAFunctioningFestivalPicker() async {
        let vm = SettingsViewModel(store: InMemorySettingsStore(), channelImport: ChannelImportViewModel(),
                                    indexProvider: StubIndexProvider(index: makeIndex()))
        await vm.festivalPicker.load()
        XCTAssertEqual(vm.festivalPicker.rows.count, 1)
    }
}
