//
//  AlmanacFestpackProviderTests.swift — the provider's state machine:
//  cache-first, a fetch failure keeps whatever pack is already showing,
//  and a pack that fails to parse never replaces a good one.
//
import XCTest
@testable import FireflyModel

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

    func testCacheFirstThenNotModifiedStaysOnCachedPack() async throws {
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
        XCTAssertGreaterThan(try XCTUnwrap(age), 0)
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
        cache.save(json: Data(#"{"festpack":"0.1"}"#.utf8), etag: nil, savedAt: Date())
        // Corrupt the sidecar the way a half-written file or a schema
        // change would.
        try Data("not json at all".utf8)
            .write(to: directory.appendingPathComponent("festpack-cache-meta.json"))

        let entry = try XCTUnwrap(cache.load())
        XCTAssertFalse(entry.json.isEmpty, "the pack itself is still perfectly usable")
        XCTAssertNil(entry.savedAt, "but when it was written is genuinely unknown, not `.distantPast`")
        XCTAssertNil(age(savedAt: entry.savedAt))
    }

    // MARK: - The words a person actually reads

    func testTheUnknownAgeSaysSoRatherThanClaimingFreshness() {
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: nil).statusText, "cached (age unknown)")
        for text in [FestpackSourceState.cached(ageSeconds: nil).statusText] {
            XCTAssertFalse(text.contains("just now"),
                           "an unknown age must never be worded as a fresh one")
        }
    }

    /// The whole ladder, so a three-day-old pack does not read as
    /// "cached (4320 min ago)" — a number nobody parses at a festival.
    func testCachedAgeReadsInUnitsAPersonCanUse() {
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 30).statusText, "cached (just now)")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 600).statusText, "cached (10 min ago)")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 3600 * 5).statusText, "cached (5 hr ago)")
        XCTAssertEqual(FestpackSourceState.cached(ageSeconds: 3600 * 24 * 3).statusText, "cached (3 days ago)")
    }

    func testTheOtherStatesAreUnchanged() {
        XCTAssertEqual(FestpackSourceState.none.statusText, "no pack")
        XCTAssertEqual(FestpackSourceState.bundled.statusText, "bundled copy")
        XCTAssertEqual(FestpackSourceState.fresh.statusText, "fresh")
    }
}
