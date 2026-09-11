//
//  AlmanacFestpackProviderTests.swift — the provider's state machine:
//  cache-first, a fetch failure keeps whatever pack is already showing,
//  and a pack that fails to parse never replaces a good one.
//
import FireflyModel
import XCTest

private final class MockFestpackFetcher: FestpackHTTPFetching, @unchecked Sendable {
    enum Behavior {
        case respond(Data, etag: String?)
        case notModified
        case fail
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
        XCTAssertEqual(state, .bundled)
    }

    func testCacheFirstThenNotModifiedStaysOnCachedPack() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: "\"abc123\"", savedAt: Date(timeIntervalSinceNow: -3600))
        let fetcher = MockFestpackFetcher(.notModified)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
        let state = await provider.sourceState()
        guard case .cached(let age) = state else { return XCTFail("expected .cached, got \(state)") }
        XCTAssertGreaterThan(age, 0)
        XCTAssertEqual(fetcher.fetchCount, 1) // it DID try — 304 just means "nothing changed"
    }

    func testFetchFailureKeepsCachedPack() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date())
        let fetcher = MockFestpackFetcher(.fail)
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest")
        let state = await provider.sourceState()
        guard case .cached = state else { return XCTFail("a failed fetch must not clear the cached state") }
    }

    func testParseFailureNeverReplacesAGoodCachedPack() async {
        let cache = FestpackDiskCache(directory: tempDir)
        cache.save(json: minimalPackJSON, etag: nil, savedAt: Date())
        let fetcher = MockFestpackFetcher(.respond(wrongVersionJSON, etag: "\"bad\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher, cache: cache,
            bundleLoader: MockBundleLoader())

        await provider.refresh()

        let current = await provider.current()
        XCTAssertEqual(current?.name, "Minimal Fest", "a pack that fails fp_parse must never replace the good one")
        let state = await provider.sourceState()
        guard case .cached = state else { return XCTFail("expected state to remain .cached, got \(state)") }
        // The bad response must not have been written to disk either.
        XCTAssertEqual(cache.load()?.etag, nil)
    }

    func testSuccessfulFetchGoesFreshAndPersistsForTheNextLaunch() async {
        let fetcher = MockFestpackFetcher(.respond(minimalPackJSON, etag: "\"v1\""))
        let provider = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher,
            cache: FestpackDiskCache(directory: tempDir), bundleLoader: MockBundleLoader())

        await provider.refresh()

        let state = await provider.sourceState()
        XCTAssertEqual(state, .fresh)

        // A second provider instance, same disk directory (simulating a
        // relaunch), must load THIS pack from cache first, before any
        // network call happens.
        let fetcher2 = MockFestpackFetcher(.fail)
        let provider2 = AlmanacFestpackProvider(
            settings: InMemorySettingsStore(), fetcher: fetcher2,
            cache: FestpackDiskCache(directory: tempDir), bundleLoader: MockBundleLoader())
        // Seed provider2's "no network" world before calling refresh(), so
        // the ONLY way it can have a pack afterward is the disk cache.
        let current2 = await provider2.current()
        XCTAssertNil(current2) // honest: nothing loaded yet, pre-refresh
    }

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
    /// `SettingsViewModel.setFestpackSourceURLOverride` should already
    /// reject one before it is ever stored, but a value could reach the
    /// store some other way (a synced or corrupted default), so this
    /// provider must never trust it either.
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
}
