//
//  AlmanacFestpackProviderTests.swift — the provider's state machine:
//  cache-first, a fetch failure keeps whatever pack is already showing,
//  a pack that fails to parse never replaces a good one, the automatic
//  refresh policy (age threshold + throttle), ETag 304 handling,
//  per-festival disk-cache keys, and checksum verification.
//
import XCTest
@testable import FireflyModel

private final class MockFestpackFetcher: FestpackHTTPFetching, @unchecked Sendable {
    enum Behavior {
        case respond(Data, etag: String?)
        case notModified
        case fail
        case failHTTPStatus(Int)
    }

    private let lock = NSLock()
    private var _behavior: Behavior
    private(set) var fetchCount = 0

    init(_ behavior: Behavior) { _behavior = behavior }

    func setBehavior(_ behavior: Behavior) {
        lock.lock(); _behavior = behavior; lock.unlock()
    }

    // `NSLock.lock()`/`unlock()` are `noasync` (Swift 6) — see
    // `LocationProvider.swift`'s own comment on this pattern. The
    // mutation/read happens in this synchronous helper, called from the
    // `async` function below.
    private func recordFetchAndReadBehavior() -> Behavior {
        lock.lock(); defer { lock.unlock() }
        fetchCount += 1
        return _behavior
    }

    func fetch(_ url: URL, ifNoneMatch etag: String?) async throws -> FestpackHTTPResponse? {
        let behavior = recordFetchAndReadBehavior()
        switch behavior {
        case .respond(let data, let etag): return FestpackHTTPResponse(body: data, etag: etag)
        case .notModified: return nil
        case .fail: throw URLError(.notConnectedToInternet)
        case .failHTTPStatus(let code): throw URLError(.init(rawValue: code))
        }
    }
}

private struct MockBundleLoader: FestpackBundleLoading {
    var files: [String: Data] = [:]
    func festpackData(forResource name: String, extension ext: String) -> Data? {
        files["\(name).\(ext)"]
    }
}

final class AlmanacFestpackProviderTests: XCTestCase {
    private var tempDir: URL!

    override func setUp() {
        super.setUp()
        tempDir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: tempDir)
        super.tearDown()
    }

    private var minimalPackJSON: Data {
        Data("""
        {"festpack":"0.1","festival":{"name":"Minimal Fest","year":2027,"start":"2027-07-01","end":"2027-07-02"},
         "stages":[{"id":"a","name":"A Stage","color":"#00ff00"}],
         "schedule":[{"artist":"Solo Act","stage":"a","day":"2027-07-01","start":"20:00","end":"21:00"}]}
        """.utf8)
    }

    private var wrongVersionJSON: Data {
        Data(#"{"festpack":"9.9"}"#.utf8)
    }

    // MARK: - Fallback order

    func testNoCacheAndNoNetworkFallsBackToBundled() async {
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher,
            cache: FestpackDiskCache(directory: tempDir),
            bundleLoader: MockBundleLoader(files: ["lost-lands-2026.festpack.json": minimalPackJSON]))

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
        let state = await provider.sourceState()
        XCTAssertEqual(state.source, .bundled)
    }

    func testCacheFirstThenNotModifiedStaysOnCachedPackAndBumpsFreshness() async throws {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"abc123\"", savedAt: Date(timeIntervalSinceNow: -3600), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.notModified)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
        let state = await provider.sourceState()
        // 304 confirms the pack is current AS OF NOW — the network was
        // consulted and agreed, so this reads as `.fetched`, not merely
        // `.cached` (this file's own header on `fetchAndPublish()`).
        XCTAssertEqual(state.source, .fetched)
        XCTAssertEqual(try XCTUnwrap(state.ageSeconds), 0, accuracy: 1)
        XCTAssertEqual(fetcher.fetchCount, 1) // it DID try — 304 just means "nothing changed"
    }

    func testFetchFailureKeepsCachedPackAndRecordsTheFailureHonestly() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date(), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
        let state = await provider.sourceState()
        XCTAssertEqual(state.source, .cached, "a failed fetch must not clear the cached state")
        XCTAssertEqual(state.lastError, "offline")
        XCTAssertNotNil(state.lastAttempt)
        XCTAssertTrue(state.statusText.contains("refresh failed: offline"))
        XCTAssertTrue(state.statusText.contains("using cache from"))
    }

    func testHTTPStatusFailureIsDescribedByItsStatusCode() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date(), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.failHTTPStatus(500))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let state = await provider.sourceState()
        XCTAssertEqual(state.lastError, "http 500")
    }

    func testAFailureThenASuccessClearsTheError() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date(), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())
        await provider.refresh()
        let firstAttempt = await provider.sourceState()
        XCTAssertNotNil(firstAttempt.lastError)

        fetcher.setBehavior(.respond(minimalPackJSON, etag: "\"v2\""))
        await provider.refresh()

        let state = await provider.sourceState()
        XCTAssertNil(state.lastError, "a later success must clear a previous failure, never show it stale")
    }

    func testParseFailureNeverReplacesAGoodCachedPack() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date(), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.respond(wrongVersionJSON, etag: "\"bad\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest", "a pack that fails fp_parse must never replace the good one")
        let state = await provider.sourceState()
        XCTAssertEqual(state.source, .cached, "expected state to remain .cached")
        XCTAssertEqual(state.lastError, "malformed pack")
        // The bad response must not have been written to disk either.
        XCTAssertEqual(cache.load(key: "lost-lands-2026")?.etag, nil)
    }

    func testSuccessfulFetchGoesFetchedAndPersistsForTheNextLaunch() async {
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v1\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher,
            cache: FestpackDiskCache(directory: tempDir), bundleLoader: MockBundleLoader())

        await provider.refresh()

        let state = await provider.sourceState()
        XCTAssertEqual(state.source, .fetched)

        // A second provider instance, same disk directory (simulating a
        // relaunch), must load THIS pack from cache — eagerly, on the
        // very FIRST `current()` read, with no network call at all
        // (`fetcher2` fails outright) and no `refresh()`/`refreshIfNeeded()`
        // call of its own.
        //
        // "app: Map subscribes to festpack updates" (2026-09-13):
        // `current()` used to be a passive read of whatever
        // `refresh()`/`refreshIfNeeded()` had last loaded into `pack` —
        // `nil` here otherwise, even with a perfectly good disk cache
        // sitting unread. That gap is exactly the shape of the Map
        // forever-spinner bug (`MapViewModel.observe()` read `current()`
        // exactly once, before `AppGraph.start()`'s detached
        // `refreshIfNeeded()` task had necessarily run). Mutation check:
        // reverting `current()` to a bare `pack` read (no
        // `ensureCacheOrBundleLoaded()` call) fails this test — `current2`
        // comes back `nil` instead of the cached pack.
        let fetcher2 = MockFestpackFetcher(.fail)
        let provider2 = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher2,
            cache: FestpackDiskCache(directory: tempDir), bundleLoader: MockBundleLoader())
        let current2 = await provider2.current()
        XCTAssertEqual(current2?.name, "Minimal Fest",
                       "current() must eagerly load the disk cache, never stay nil merely because no refresh() has run yet")
    }

    // MARK: - Settings URL override

    func testSettingsURLOverrideIsUsedWhenValid() async {
        let settings = InMemorySettingsStore()
        settings.setString("https://example.com/custom.festpack.json", .festpackSourceURLOverride)
        let provider = AlmanacFestpackProvider(settings: settings)
        let url = await provider.sourceURL()
        XCTAssertEqual(url.absoluteString, "https://example.com/custom.festpack.json")
    }

    func testMalformedSettingsURLFallsBackToDefault() async {
        let settings = InMemorySettingsStore()
        // The empty string is the one input `URL(string:)` reliably
        // refuses (modern Foundation percent-encodes almost anything
        // else into SOME URL rather than failing) — exercising the
        // "override present but unusable" fallback path honestly.
        settings.setString("", .festpackSourceURLOverride)
        let provider = AlmanacFestpackProvider(settings: settings)
        let url = await provider.sourceURL()
        XCTAssertEqual(url, AlmanacFestpackProvider.defaultURL)
    }

    /// BLOCKING review finding 1: `sourceURL()` is the last line of
    /// defense against a non-https override reaching the network —
    /// `FestpackSourceURLValidator`/`SettingsViewModel
    /// .setFestpackSourceURLOverride` should already reject one before
    /// it is ever stored, but a value could reach the store some other
    /// way (a synced or corrupted default), so this provider must never
    /// trust it either.
    func testHTTPSettingsURLFallsBackToDefault() async {
        let settings = InMemorySettingsStore()
        settings.setString("http://example.com/custom.festpack.json", .festpackSourceURLOverride)
        let provider = AlmanacFestpackProvider(settings: settings)
        let url = await provider.sourceURL()
        XCTAssertEqual(url, AlmanacFestpackProvider.defaultURL, "a plaintext http:// override must never be fetched")
    }

    func testFileSchemeSettingsURLFallsBackToDefault() async {
        let settings = InMemorySettingsStore()
        settings.setString("file:///etc/passwd", .festpackSourceURLOverride)
        let provider = AlmanacFestpackProvider(settings: settings)
        let url = await provider.sourceURL()
        XCTAssertEqual(url, AlmanacFestpackProvider.defaultURL)
    }

    func testGarbageSettingsURLFallsBackToDefault() async {
        let settings = InMemorySettingsStore()
        settings.setString("not a url at all", .festpackSourceURLOverride)
        let provider = AlmanacFestpackProvider(settings: settings)
        let url = await provider.sourceURL()
        XCTAssertEqual(url, AlmanacFestpackProvider.defaultURL)
    }

    // MARK: - Automatic refresh policy ("app: automatic almanac refresh")

    func testRefreshIfNeededFetchesWhenNothingIsCached() async {
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v1\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher,
            cache: FestpackDiskCache(directory: tempDir), bundleLoader: MockBundleLoader())

        await provider.refreshIfNeeded()

        let state = await provider.sourceState()
        XCTAssertEqual(fetcher.fetchCount, 1)
        XCTAssertEqual(state.source, .fetched)
    }

    func testRefreshIfNeededSkipsTheNetworkWhenTheCacheIsFreshEnough() async {
        let cache = FestpackDiskCache(directory: tempDir)
        // Well inside the 6-hour staleness threshold.
        cache.save(json: minimalPackJSON, etag: "\"abc\"", savedAt: Date(timeIntervalSinceNow: -60), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refreshIfNeeded()

        let current = await provider.current()
        let state = await provider.sourceState()
        XCTAssertEqual(fetcher.fetchCount, 0, "a fresh-enough cache must not trigger a network call at all")
        XCTAssertEqual(current?.name, "Minimal Fest")
        XCTAssertNil(state.lastError)
    }

    func testRefreshIfNeededFetchesWhenTheCacheIsOlderThanSixHours() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"abc\"",
                   savedAt: Date(timeIntervalSinceNow: -(6 * 3600 + 60)), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v2\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refreshIfNeeded()

        XCTAssertEqual(fetcher.fetchCount, 1, "a cache older than the staleness threshold must trigger a fetch")
    }

    func testRefreshIfNeededNeverFetchesMoreThanOncePerFifteenMinutesEvenWhenStale() async {
        let cache = FestpackDiskCache(directory: tempDir)
        // Old enough to be stale...
        cache.save(json: minimalPackJSON, etag: "\"abc\"",
                   savedAt: Date(timeIntervalSinceNow: -(7 * 3600)), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refreshIfNeeded() // ...first call attempts (and fails offline).
        XCTAssertEqual(fetcher.fetchCount, 1)

        fetcher.setBehavior(.respond(minimalPackJSON, etag: "\"v2\""))
        await provider.refreshIfNeeded() // called again immediately — must be throttled.

        XCTAssertEqual(fetcher.fetchCount, 1, "a second attempt inside the 15-minute throttle window must be skipped")
    }

    /// Test-only, lock-protected clock override — `AlmanacFestpackProvider`
    /// is an `actor`; a plain captured `var Date` is the exact race
    /// Swift 6 is right to flag, same reasoning `LocationProviderTests`
    /// '`LockedTestClock` already states for `PhoneGPSUplink`.
    private final class LockedTestClock: @unchecked Sendable {
        private let lock = NSLock()
        private var value: Date
        init(_ initial: Date) { value = initial }
        func get() -> Date { lock.lock(); defer { lock.unlock() }; return value }
        func advance(by interval: TimeInterval) {
            lock.lock(); value = value.addingTimeInterval(interval); lock.unlock()
        }
    }

    func testRefreshIfNeededIgnoresTheThrottleAfterFifteenMinutesHavePassed() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"abc\"",
                   savedAt: Date(timeIntervalSinceNow: -(7 * 3600)), key: "lost-lands-2026")
        let now = LockedTestClock(Date())
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader(),
            now: { now.get() })

        await provider.refreshIfNeeded()
        XCTAssertEqual(fetcher.fetchCount, 1)

        now.advance(by: 16 * 60) // past the 15-minute throttle
        fetcher.setBehavior(.respond(minimalPackJSON, etag: "\"v2\""))
        await provider.refreshIfNeeded()

        XCTAssertEqual(fetcher.fetchCount, 2, "once the throttle window passes, a still-stale cache must be retried")
    }

    /// `refresh()` (the manual REFRESH button / pull-to-refresh) is
    /// NEVER throttled, unlike `refreshIfNeeded()` — even immediately
    /// after an automatic attempt.
    func testManualRefreshIsNeverThrottled() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"abc\"", savedAt: Date(), key: "lost-lands-2026")
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v2\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refresh()
        await provider.refresh()

        XCTAssertEqual(fetcher.fetchCount, 2, "the manual path must always hit the network, throttle or not")
    }

    // MARK: - Per-festival disk-cache keys

    func testSwitchingTheSelectedFestivalSwitchesTheDiskCacheKey() async {
        let settings = InMemorySettingsStore()
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date(), key: "lost-lands-2026")
        let otherPackJSON = Data("""
        {"festpack":"0.1","festival":{"name":"Other Fest","year":2027,"start":"2027-08-01","end":"2027-08-02"},
         "stages":[],"schedule":[]}
        """.utf8)
        cache.save(json: otherPackJSON, etag: nil, savedAt: Date(), key: "other-fest-2027")
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(settings: settings, fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refresh()
        let first = await provider.current()
        XCTAssertEqual(first?.name, "Minimal Fest", "default namespace is lost-lands-2026")

        settings.setString("other-fest", .festivalSelectedSlug)
        settings.setString("2027", .festivalSelectedYear)
        await provider.refresh()
        let second = await provider.current()
        XCTAssertEqual(second?.name, "Other Fest", "switching festival must switch the disk-cache key")

        // Switching BACK must be instant (no network needed) — the
        // original festival's own cache file is untouched.
        settings.setString(nil, .festivalSelectedSlug)
        settings.setString(nil, .festivalSelectedYear)
        await provider.refresh()
        let third = await provider.current()
        XCTAssertEqual(third?.name, "Minimal Fest")
    }

    func testANonDefaultFestivalWithNoCacheAndNoBundleHasNoPackUntilAFetchSucceeds() async {
        let settings = InMemorySettingsStore()
        settings.setString("other-fest", .festivalSelectedSlug)
        settings.setString("2027", .festivalSelectedYear)
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: settings, fetcher: fetcher, cache: FestpackDiskCache(directory: tempDir),
            // The bundle only ever has "lost-lands-2026" in it — see
            // `loadFromCacheOrBundle()`'s own doc comment.
            bundleLoader: MockBundleLoader(files: ["lost-lands-2026.festpack.json": minimalPackJSON]))

        await provider.refresh()

        let current = await provider.current()
        let state = await provider.sourceState()
        XCTAssertNil(current, "no bundled fallback exists for a non-default festival")
        XCTAssertEqual(state.source, .none)
    }

    // MARK: - Checksum verification ("app: ... festival picker", owner ask #2)

    func testMatchingChecksumAcceptsTheFetchedPack() async {
        let settings = InMemorySettingsStore()
        settings.setString(sha256Hex(minimalPackJSON), .festivalSelectedSHA256)
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v1\""))
        let provider = AlmanacFestpackProvider(
            settings: settings, fetcher: fetcher, cache: FestpackDiskCache(directory: tempDir),
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        let state = await provider.sourceState()
        XCTAssertEqual(current?.name, "Minimal Fest")
        XCTAssertNil(state.lastError)
    }

    func testMismatchedChecksumRejectsTheFetchAndKeepsTheOldPack() async {
        let settings = InMemorySettingsStore()
        settings.setString(String(repeating: "0", count: 64), .festivalSelectedSHA256)
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"old\"", savedAt: Date(), key: "lost-lands-2026")
        let differentPackJSON = Data("""
        {"festpack":"0.1","festival":{"name":"Wrong Bytes","year":2027,"start":"2027-07-01","end":"2027-07-02"},
         "stages":[],"schedule":[]}
        """.utf8)
        let fetcher = MockFestpackFetcher(.respond(differentPackJSON, etag: "\"new\""))
        let provider = AlmanacFestpackProvider(settings: settings, fetcher: fetcher, cache: cache, bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest", "a checksum mismatch must never replace the old pack")
        let state = await provider.sourceState()
        XCTAssertEqual(state.lastError, "checksum mismatch")
        // The mismatched bytes must not have been written to disk either.
        XCTAssertEqual(cache.load(key: "lost-lands-2026")?.etag, "\"old\"")
    }

    func testChecksumIsCaseInsensitive() async {
        let settings = InMemorySettingsStore()
        settings.setString(sha256Hex(minimalPackJSON).uppercased(), .festivalSelectedSHA256)
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v1\""))
        let provider = AlmanacFestpackProvider(
            settings: settings, fetcher: fetcher, cache: FestpackDiskCache(directory: tempDir),
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
    }

    private func sha256Hex(_ data: Data) -> String {
        AlmanacFestpackProvider.sha256Hex(data)
    }
    // MARK: - Festival switching (review findings)

    /// Two different festivals behind two different URLs, so a switch
    /// is a genuine change of pack rather than the same bytes twice.
    private func festivalPackJSON(name: String, artist: String) -> Data {
        Data("""
        {"festpack":"0.1","festival":{"name":"\(name)","year":2026,"start":"2026-07-01","end":"2026-07-02"},
         "stages":[{"id":"a","name":"A Stage","color":"#00ff00"}],
         "schedule":[{"artist":"\(artist)","stage":"a","day":"2026-07-01","start":"20:00","end":"21:00"}]}
        """.utf8)
    }

    private static let lostLandsURL =
        "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/lost-lands/2026/festpack.json"
    private static let wakaanURL =
        "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/wakaan/2026/festpack.json"

    private func select(_ slug: String, url: String, in store: InMemorySettingsStore) {
        store.setString(slug, .festivalSelectedSlug)
        store.setString("2026", .festivalSelectedYear)
        store.setString(url, .festpackSourceURLOverride)
    }

    /// Review finding (BLOCKING, measured): switching the Settings
    /// picker to a festival with no disk cache while offline left
    /// `LineupViewModel.festpack` holding the PREVIOUS festival's pack
    /// — a full schedule, now/next strip and settimes share URL for
    /// Lost Lands rendered under a Settings screen saying Wakaan is
    /// selected and `sourceState` saying "refresh failed: offline · no
    /// pack cached". Mutation check: reverting the `festpack = nil`
    /// branch in `LineupViewModel.refresh()` fails this test on the
    /// `XCTAssertNil` below.
    func testSwitchingToAnUncachedFestivalWhileOfflineClearsTheLineupRatherThanShowingTheOldPack() async {
        let store = InMemorySettingsStore()
        let fetcher = MockFestpackFetcher(.respond(festivalPackJSON(name: "Lost Lands", artist: "Excision"), etag: nil))
        let provider = AlmanacFestpackProvider(settings: store, fetcher: fetcher,
                                                cache: FestpackDiskCache(directory: tempDir),
                                                bundleLoader: MockBundleLoader())
        let lineup = await LineupViewModel(festpackProvider: provider, picksStore: InMemoryPicksStore())
        select("lost-lands", url: Self.lostLandsURL, in: store)
        await lineup.refresh()
        let loadedName = await lineup.festpack?.name
        XCTAssertEqual(loadedName, "Lost Lands")

        select("wakaan", url: Self.wakaanURL, in: store)
        fetcher.setBehavior(.fail)
        await lineup.refresh()

        let providerPack = await provider.current()
        XCTAssertNil(providerPack, "the provider itself honestly has no pack for the newly selected festival")
        let shownPack = await lineup.festpack
        XCTAssertNil(shownPack, "the Lineup must not keep rendering the previous festival's schedule")
        let night = await lineup.selectedNightDayOfYear
        XCTAssertNil(night)
        let state = await lineup.sourceState
        XCTAssertEqual(state.statusText, "refresh failed: offline · no pack cached")
    }

    /// Same finding, the stream half: `festpackUpdates()` is a
    /// current-value hub, so a subscriber arriving after the switch was
    /// replayed the old festival's pack even though `current()` was
    /// `nil`. Mutation check: replacing `hub.yield(nil)` with a no-op
    /// (or with `hub.clearCurrent()`, the OLD fix — the whole point of
    /// this test's own update, app: Map subscribes to festpack updates,
    /// 2026-09-13) fails this test, either by re-asserting "Lost Lands"
    /// as the honest current value or by never delivering the honest
    /// `nil` at all.
    func testFestpackUpdatesStopsReplayingThePreviousFestivalOncePackIsUnknown() async {
        let store = InMemorySettingsStore()
        let fetcher = MockFestpackFetcher(.respond(festivalPackJSON(name: "Lost Lands", artist: "Excision"), etag: nil))
        let provider = AlmanacFestpackProvider(settings: store, fetcher: fetcher,
                                                cache: FestpackDiskCache(directory: tempDir),
                                                bundleLoader: MockBundleLoader())
        select("lost-lands", url: Self.lostLandsURL, in: store)
        await provider.refresh()

        select("wakaan", url: Self.wakaanURL, in: store)
        fetcher.setBehavior(.fail)
        await provider.refresh()

        // Subscribe NOW — while the provider honestly has no pack —
        // then let the next successful fetch publish. The FIRST element
        // this subscriber sees must be the honest `nil` (never a
        // replayed Lost Lands); the first NON-nil element must be the
        // NEW festival. No sleep: the `for await` returns as soon as
        // each real yield lands.
        let stream = provider.festpackUpdates()
        fetcher.setBehavior(.respond(festivalPackJSON(name: "Wakaan", artist: "Liquid Stranger"), etag: nil))
        await provider.refresh()
        var sawHonestNilFirst = false
        var firstRealName: String?
        for await pack in stream {
            if let pack {
                firstRealName = pack.name
                break
            }
            sawHonestNilFirst = true
        }
        XCTAssertTrue(sawHonestNilFirst, "the hub must actively tell an already-subscribed reader the pack is gone")
        XCTAssertEqual(firstRealName, "Wakaan", "the hub must not re-assert a pack the provider no longer has")
    }

    /// Review finding (BLOCKING, measured): picks are namespaced per
    /// festival in the STORE correctly, but `LineupViewModel` cached
    /// `pickedSetIDs` at `init` and only re-read it after a local
    /// toggle — so Lost Lands -> Wakaan -> Lost Lands showed the wrong
    /// pick set on both switches. Mutation check: removing the
    /// `pickedSetIDs = picksStore.pickedSetIDs()` line from
    /// `LineupViewModel.apply(_:)` fails this test on the Wakaan leg.
    func testSwitchingFestivalsShowsEachFestivalsOwnPicks() async {
        let store = InMemorySettingsStore()
        let fetcher = MockFestpackFetcher(.respond(festivalPackJSON(name: "Lost Lands", artist: "Excision"), etag: nil))
        let provider = AlmanacFestpackProvider(settings: store, fetcher: fetcher,
                                                cache: FestpackDiskCache(directory: tempDir),
                                                bundleLoader: MockBundleLoader())
        let picks = PicksStore(store: store, namespace: { store.festivalNamespace() })
        let lineup = await LineupViewModel(festpackProvider: provider, picksStore: picks)

        select("lost-lands", url: Self.lostLandsURL, in: store)
        await lineup.refresh()
        let lostLandsSet = await lineup.festpack!.sets[0]
        await lineup.togglePick(lostLandsSet)
        let lostLandsPicks = await lineup.pickedSetIDs
        XCTAssertEqual(lostLandsPicks.count, 1)

        select("wakaan", url: Self.wakaanURL, in: store)
        fetcher.setBehavior(.respond(festivalPackJSON(name: "Wakaan", artist: "Liquid Stranger"), etag: nil))
        await lineup.refresh()
        let onWakaan = await lineup.pickedSetIDs
        XCTAssertEqual(onWakaan, [], "Wakaan has its own, empty picks namespace")

        select("lost-lands", url: Self.lostLandsURL, in: store)
        fetcher.setBehavior(.respond(festivalPackJSON(name: "Lost Lands", artist: "Excision"), etag: nil))
        await lineup.refresh()
        let backOnLostLands = await lineup.pickedSetIDs
        XCTAssertEqual(backOnLostLands, lostLandsPicks, "the Lost Lands pick comes back on the way back")
    }
}


// MARK: - Hardening QA pass: cache freshness must not be invented

/// The festival premise is a phone with no cell service for three days.
/// Its clock drifts, and iOS corrects it — sometimes BACKWARDS — the
/// moment it sees a tower again, which makes a cache written before the
/// correction look like it was written in the future.
///
/// That case used to be clamped by a `max(0, …)` and rendered as
/// **"cached (just now)"**: a pack of entirely unknown vintage
/// presented as freshly fetched. The missing-metadata case was papered
/// over the same way, as `.distantPast`.
final class FestpackCacheFreshnessTests: XCTestCase {

    private func age(savedAt: Date?, now: Date = Date(timeIntervalSince1970: 1_790_000_000)) -> TimeInterval? {
        AlmanacFestpackProvider.cacheAge(savedAt: savedAt, now: now)
    }

    func testANormalCacheAgeIsMeasuredNormally() throws {
        let now = Date(timeIntervalSince1970: 1_790_000_000)
        XCTAssertEqual(try XCTUnwrap(age(savedAt: now.addingTimeInterval(-3600))), 3600, accuracy: 0.001)
        XCTAssertEqual(try XCTUnwrap(age(savedAt: now)), 0, accuracy: 0.001)
    }

    func testAFutureSavedAtIsUnknownAgeNotZero() {
        let now = Date(timeIntervalSince1970: 1_790_000_000)
        XCTAssertNil(age(savedAt: now.addingTimeInterval(1)),
                     "one second into the future is already a clock this app cannot measure against")
        XCTAssertNil(age(savedAt: now.addingTimeInterval(86_400 * 3)),
                     "a three-day clock correction must never render as 'just now'")
    }

    func testAMissingSavedAtIsUnknownAgeNotAncient() {
        XCTAssertNil(age(savedAt: nil))
    }

    func testANonFiniteSavedAtIsUnknownAgeNotATrap() {
        XCTAssertNil(age(savedAt: Date(timeIntervalSince1970: .nan)))
        XCTAssertNil(age(savedAt: .distantFuture))
    }

    /// A cache whose metadata sidecar was never written (or was
    /// corrupted) still yields its JSON — losing the age must not lose
    /// the pack.
    func testACacheWithNoMetadataStillLoadsItsJSONButWithNoSavedAt() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("festpack-freshness-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let cache = FestpackDiskCache(directory: directory)
        cache.save(json: Data(#"{"festpack":"0.1"}"#.utf8), etag: nil, savedAt: Date(), key: "lost-lands-2026")
        // Corrupt the sidecar the way a half-written file or a schema
        // change would.
        try Data("not json at all".utf8)
            .write(to: directory.appendingPathComponent("festpack-cache-lost-lands-2026-meta.json"))

        let entry = try XCTUnwrap(cache.load(key: "lost-lands-2026"))
        XCTAssertFalse(entry.json.isEmpty, "the pack itself is still perfectly usable")
        XCTAssertNil(entry.savedAt, "but when it was written is genuinely unknown, not `.distantPast`")
        XCTAssertNil(age(savedAt: entry.savedAt))
    }

    // MARK: - The words a person actually reads

    func testTheUnknownAgeSaysSoRatherThanClaimingFreshness() {
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: nil).statusText, "cached (age unknown)")
    }

    /// The whole ladder, so a three-day-old pack does not read as
    /// "cached (4320 min ago)" — a number nobody parses at a festival.
    func testCachedAgeReadsInUnitsAPersonCanUse() {
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 30).statusText, "cached just now")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 600).statusText, "cached 10 min ago")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 3600 * 5).statusText, "cached 5 h ago")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 3600 * 24 * 3).statusText, "cached 3 d ago")
    }

    func testFetchedAgeReadsTheSameLadder() {
        XCTAssertEqual(FestpackSourceState.fetched(ageSeconds: 3600 * 2).statusText, "fetched 2 h ago")
    }

    func testTheOtherStatesAreUnchanged() {
        XCTAssertEqual(FestpackSourceState.none.statusText, "no pack")
        XCTAssertEqual(FestpackSourceState.bundled.statusText, "bundled copy")
    }

    func testAFailedRefreshIsNamedHonestlyAlongsideWhatIsStillShowing() {
        let state = FestpackSourceState(source: .cached, savedAt: nil, ageSeconds: 3600 * 6, lastAttempt: Date(), lastError: "offline")
        XCTAssertEqual(state.statusText, "refresh failed: offline · using cache from 6 h ago")
    }

}
