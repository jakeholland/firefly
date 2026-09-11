//
//  AlmanacFestpackProvider.swift — fetches the real festival pack from
//  fest-almanac at runtime (owner's decision: fest-almanac is the
//  single source of truth; the PHONE fetches + caches offline, unlike
//  the puck, which embeds a copy at build time).
//
//  Fallback order, honestly narrated by `sourceState()`:
//    1. Disk cache (Application Support), loaded FIRST, synchronously
//       relative to `refresh()`'s caller — the app never blocks its
//       first frame on the network.
//    2. If no cache: the bundled copy (`firmware/assets/field/
//       lost-lands-2026.festpack.json`), labelled "bundled copy".
//    3. Either way, a background fetch attempt follows. Success updates
//       the cache and `current()`/`sourceState()` to `.fresh`. ANY
//       failure — offline, HTTP error, or `fp_parse` rejecting the
//       response — leaves whatever pack was already showing untouched.
//       A pack that fails to parse is NEVER shown, and NEVER written to
//       the cache (a corrupt fetch must not evict a good cached one).
//
import FireflyMesh
import Foundation

public struct FestpackHTTPResponse: Sendable {
    public let body: Data
    public let etag: String?
    public init(body: Data, etag: String?) {
        self.body = body
        self.etag = etag
    }
}

public protocol FestpackHTTPFetching: Sendable {
    /// `nil` for a 304 Not Modified (the cache is already current);
    /// throws for any transport failure or non-2xx/304 status.
    func fetch(_ url: URL, ifNoneMatch etag: String?) async throws -> FestpackHTTPResponse?
}

public struct URLSessionFestpackFetcher: FestpackHTTPFetching {
    private let session: URLSession

    public init(session: URLSession = .shared) {
        self.session = session
    }

    public func fetch(_ url: URL, ifNoneMatch etag: String?) async throws -> FestpackHTTPResponse? {
        var request = URLRequest(url: url)
        if let etag { request.setValue(etag, forHTTPHeaderField: "If-None-Match") }
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        if http.statusCode == 304 { return nil }
        guard (200..<300).contains(http.statusCode) else { throw URLError(.init(rawValue: http.statusCode)) }
        return FestpackHTTPResponse(body: data, etag: http.value(forHTTPHeaderField: "ETag"))
    }
}

public actor AlmanacFestpackProvider: FestpackProviding {
    /// The owner-specified fest-almanac source: Lost Lands 2026, schema
    /// v0.1.
    public static let defaultURL = URL(
        string: "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/lost-lands/2026/festpack.json")!

    /// `nonisolated`: `CurrentValueEventHub` is its own thread-safe,
    /// `Sendable` type (a lock-guarded class — see its own doc comment),
    /// so `festpackUpdates()` below can hand out a subscription
    /// synchronously, matching `FestpackProviding`'s non-`async`
    /// requirement, without needing actor isolation on this property.
    private nonisolated let hub = CurrentValueEventHub<Festpack>()
    private var pack: Festpack?
    private var state: FestpackSourceState = .none
    private let settings: any SettingsStoring
    private let fetcher: any FestpackHTTPFetching
    private let cache: FestpackDiskCache
    private let bundleLoader: any FestpackBundleLoading
    private let now: @Sendable () -> Date

    public init(settings: any SettingsStoring,
                fetcher: any FestpackHTTPFetching = URLSessionFestpackFetcher(),
                cache: FestpackDiskCache = FestpackDiskCache(),
                bundleLoader: any FestpackBundleLoading = MainBundleFestpackLoader(),
                now: @escaping @Sendable () -> Date = { Date() }) {
        self.settings = settings
        self.fetcher = fetcher
        self.cache = cache
        self.bundleLoader = bundleLoader
        self.now = now
    }

    /// Settings' "Festival data" row override, or the built-in default.
    /// A malformed override string is treated as "not set" — never a
    /// crash, never silently fetching nothing.
    public func sourceURL() -> URL {
        settings.string(.festpackSourceURLOverride).flatMap(URL.init(string:)) ?? Self.defaultURL
    }

    public func current() -> Festpack? { pack }
    public func sourceState() -> FestpackSourceState { state }
    public nonisolated func festpackUpdates() -> AsyncStream<Festpack> { hub.subscribe() }

    /// First call: cache-first (falling back to the bundled copy if
    /// there is no cache), then a background fetch attempt. Every later
    /// call is just the fetch attempt again — the same one Settings'
    /// "refresh" action triggers.
    public func refresh() async {
        if pack == nil {
            loadFromCacheOrBundle()
        }
        await fetchAndPublish()
    }

    private func loadFromCacheOrBundle() {
        if let cached = cache.load(), case .success(let parsed) = FestpackParser.parse(cached.json) {
            pack = parsed
            state = .cached(ageSeconds: max(0, now().timeIntervalSince(cached.savedAt)))
            hub.yield(parsed)
            return
        }
        guard let bundled = bundleLoader.festpackData(forResource: "lost-lands-2026", extension: "festpack.json"),
              case .success(let parsed) = FestpackParser.parse(bundled) else { return }
        pack = parsed
        state = .bundled
        hub.yield(parsed)
    }

    private func fetchAndPublish() async {
        let cachedETag = cache.load()?.etag
        do {
            guard let response = try await fetcher.fetch(sourceURL(), ifNoneMatch: cachedETag) else {
                return // 304 Not Modified: the cache (and whatever it seeded) is already current
            }
            guard case .success(let parsed) = FestpackParser.parse(response.body) else {
                return // never show, and never cache, a pack that failed to parse
            }
            cache.save(json: response.body, etag: response.etag, savedAt: now())
            pack = parsed
            state = .fresh
            hub.yield(parsed)
        } catch {
            // Offline or an HTTP failure: keep whatever pack (cached or
            // bundled) is already showing — never clear it on a failed
            // fetch.
        }
    }
}
