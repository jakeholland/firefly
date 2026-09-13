//
//  AlmanacFestpackProvider.swift — fetches the real festival pack from
//  fest-almanac at runtime (owner's decision: fest-almanac is the
//  single source of truth; the PHONE fetches + caches offline, unlike
//  the puck, which embeds a copy at build time).
//
//  Fallback order, honestly narrated by `sourceState()`:
//    1. Disk cache (Application Support), loaded FIRST, synchronously
//       relative to `refresh()`/`refreshIfNeeded()`'s caller — the app
//       never blocks its first frame on the network. Keyed per festival
//       (`SettingsStoring.festivalNamespace()`, "<slug>-<year>") so
//       switching the Settings festival picker back to a previously
//       loaded festival is instant — see `FestpackDiskCache`'s own
//       header.
//    2. If no cache: the bundled copy — ONLY for the built-in default
//       festival (Lost Lands 2026, `firmware/assets/field/
//       lost-lands-2026.festpack.json`); no other festival ships an
//       offline fallback, so this step is a no-op for anything else,
//       labelled "bundled copy" only when it actually applies.
//    3. Either way, a fetch attempt follows: unconditionally for
//       `refresh()` (the manual REFRESH button and pull-to-refresh),
//       or subject to the auto-refresh policy for `refreshIfNeeded()`
//       (`AppGraph.start()`/foreground — see that method's own doc
//       comment for the exact thresholds). Success updates the cache
//       and `current()`/`sourceState()` to `.fetched`. ANY failure —
//       offline, HTTP error, a checksum mismatch, or `fp_parse`
//       rejecting the response — leaves whatever pack was already
//       showing untouched, and is recorded honestly in `sourceState()
//       .lastError` rather than swallowed.
//
import CryptoKit
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
    /// v0.1. Also the built-in default for `SettingsKey
    /// .festivalSelectedSlug`/`.festivalSelectedYear` (unset = this
    /// festival — `SettingsStoring.festivalNamespace()`).
    public static let defaultURL = URL(
        string: "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/lost-lands/2026/festpack.json")!

    /// "app: automatic almanac refresh" (owner ask #1, 2026-09-13):
    /// `refreshIfNeeded()` attempts a network fetch when the showing
    /// pack is missing or at least this old.
    public static let autoRefreshStaleAfter: TimeInterval = 6 * 60 * 60
    /// Never more than one attempt per this interval, regardless of
    /// staleness — a flapping connection (airplane mode toggled,
    /// elevator Wi-Fi) must not turn every foreground resume into a
    /// fetch attempt.
    public static let autoRefreshMinInterval: TimeInterval = 15 * 60

    /// `nonisolated`: `CurrentValueEventHub` is its own thread-safe,
    /// `Sendable` type (a lock-guarded class — see its own doc comment),
    /// so `festpackUpdates()` below can hand out a subscription
    /// synchronously, matching `FestpackProviding`'s non-`async`
    /// requirement, without needing actor isolation on this property.
    private nonisolated let hub = CurrentValueEventHub<Festpack>()
    private var pack: Festpack?
    /// Raw provenance fields — `sourceState()` composes these, PLUS a
    /// freshly measured `ageSeconds` (never baked in at write time),
    /// into the struct callers actually read. See `FestpackSourceState`
    /// 's own doc comment for why each field exists.
    private var currentSource: FestpackSourceState.Source = .none
    private var currentSavedAt: Date?
    private var lastAttemptAt: Date?
    private var lastErrorMessage: String?
    /// The festival namespace (`SettingsStoring.festivalNamespace()`)
    /// this provider was last operating against — compared on every
    /// `refresh()`/`refreshIfNeeded()` call so a festival switch made
    /// through the Settings picker (which only writes settings; it does
    /// not reach into this actor directly) is picked up the next time
    /// either method runs, the same way `sourceURL()` already re-reads
    /// settings on every call rather than capturing it once at `init`.
    private var lastFestivalKey: String?
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
    /// crash, never silently fetching nothing. https-only: this URL
    /// feeds the whole cache-then-parse pipeline, so a non-https scheme
    /// (plaintext `http://`, `file://`, or anything else) is rejected
    /// exactly the same way an unparseable string already is, even
    /// though `FestpackSourceURLValidator`/`SettingsViewModel`/
    /// `FestivalPickerViewModel` should never let one reach the store in
    /// the first place — this is the last line of defense against a
    /// synced/corrupted settings value.
    public func sourceURL() -> URL {
        guard let raw = settings.string(.festpackSourceURLOverride),
              let url = URL(string: raw), url.scheme?.lowercased() == "https" else {
            return Self.defaultURL
        }
        return url
    }

    /// The festival namespace this provider is currently pointed at —
    /// `SettingsStoring.festivalNamespace()`, "<slug>-<year>", also the
    /// disk-cache key and the bundled-resource base name (see
    /// `loadFromCacheOrBundle()`).
    private func festivalKey() -> String { settings.festivalNamespace() }

    /// The Settings festival picker's expected checksum for the
    /// CURRENTLY selected festival, if the almanac index carried one —
    /// `SettingsKey.festivalSelectedSHA256`. `nil` whenever there is
    /// nothing to verify against (no festival ever picked, or the index
    /// entry had no `sha256`, or the field was hand-edited via the
    /// manual "Pack URL" override — see `SettingsViewModel
    /// .setFestpackSourceURLOverride`'s own doc comment on why a manual
    /// edit clears this).
    private func expectedSHA256() -> String? {
        settings.string(.festivalSelectedSHA256)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
    }

    /// `nil` when the age cannot be known — see
    /// `FestpackSourceState.ageSeconds`'s own doc comment. A NEGATIVE
    /// interval is not clamped to zero: the old `max(0, …)` turned "this
    /// phone's clock moved backwards since the cache was written" into
    /// "cached (just now)", presenting a pack of unknown vintage as
    /// freshly fetched. Pure and `static` so it is testable with no
    /// provider, no cache and no network.
    static func cacheAge(savedAt: Date?, now: Date) -> TimeInterval? {
        guard let savedAt else { return nil }
        let age = now.timeIntervalSince(savedAt)
        guard age.isFinite, age >= 0 else { return nil }
        return age
    }

    /// A short, honest description of a fetch failure — "offline" for
    /// the connectivity-shaped `URLError`s, "http NNN" for the
    /// non-2xx/304 statuses `URLSessionFestpackFetcher` re-throws as a
    /// `URLError` carrying the HTTP status as its raw code (that type's
    /// own `fetch(_:ifNoneMatch:)` doc comment), and a generic fallback
    /// for anything else. Never a raw `Error.localizedDescription` dump
    /// — this string is read by a person at a festival, not a debugger.
    static func describeFetchFailure(_ error: Error) -> String {
        guard let urlError = error as? URLError else { return "network error" }
        let offlineCodes: Set<Int> = [
            URLError.notConnectedToInternet.rawValue,
            URLError.networkConnectionLost.rawValue,
            URLError.timedOut.rawValue,
            URLError.cannotConnectToHost.rawValue,
            URLError.cannotFindHost.rawValue,
            URLError.dnsLookupFailed.rawValue,
            URLError.internationalRoamingOff.rawValue,
            URLError.dataNotAllowed.rawValue,
        ]
        if offlineCodes.contains(urlError.code.rawValue) { return "offline" }
        if (100...599).contains(urlError.code.rawValue) { return "http \(urlError.code.rawValue)" }
        return "network error"
    }

    static func sha256Hex(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    public func current() -> Festpack? { pack }
    public nonisolated func festpackUpdates() -> AsyncStream<Festpack> { hub.subscribe() }

    public func sourceState() -> FestpackSourceState {
        FestpackSourceState(source: currentSource,
                             savedAt: currentSavedAt,
                             ageSeconds: Self.cacheAge(savedAt: currentSavedAt, now: now()),
                             lastAttempt: lastAttemptAt,
                             lastError: lastErrorMessage)
    }

    /// First call: cache-first (falling back to the bundled copy if
    /// there is no cache), then an UNCONDITIONAL background fetch
    /// attempt. Every later call is just the fetch attempt again — the
    /// same one Settings' "REFRESH" button and Lineup's pull-to-refresh
    /// trigger. Never throttled — see `refreshIfNeeded()` for the
    /// throttled, automatic counterpart.
    public func refresh() async {
        reloadIfFestivalChanged()
        if pack == nil {
            loadFromCacheOrBundle()
        }
        await fetchAndPublish()
    }

    /// "app: automatic almanac refresh" (owner ask #1) — what
    /// `AppGraph.start()`/`handleScenePhaseChange(.foreground)` call.
    /// Loads cache/bundle first exactly like `refresh()` when nothing
    /// is loaded yet, but only attempts the network when the showing
    /// pack is missing/unknown-age or older than
    /// `autoRefreshStaleAfter`, AND at least `autoRefreshMinInterval`
    /// has passed since the last attempt (whether that attempt
    /// succeeded, 304'd, or failed).
    public func refreshIfNeeded() async {
        reloadIfFestivalChanged()
        if pack == nil {
            loadFromCacheOrBundle()
        }
        guard shouldAttemptAutoRefresh() else { return }
        await fetchAndPublish()
    }

    private func shouldAttemptAutoRefresh() -> Bool {
        if let lastAttemptAt, now().timeIntervalSince(lastAttemptAt) < Self.autoRefreshMinInterval {
            return false
        }
        guard let age = Self.cacheAge(savedAt: currentSavedAt, now: now()) else { return true }
        return age > Self.autoRefreshStaleAfter
    }

    /// A Settings festival-picker selection only ever writes settings
    /// (`FestivalPickerViewModel.select(_:)`) — it has no reference to
    /// this actor to reset directly. So both `refresh()` and
    /// `refreshIfNeeded()` re-read `festivalKey()` on every call
    /// (`sourceURL()`'s own existing convention) and, if it changed
    /// since the last operation, drop the in-memory pack/state and let
    /// `loadFromCacheOrBundle()` reload from THAT festival's own cache —
    /// instant if it was ever loaded before this process launched, per
    /// `FestpackDiskCache`'s own per-key header comment.
    private func reloadIfFestivalChanged() {
        let key = festivalKey()
        guard key != lastFestivalKey else { return }
        lastFestivalKey = key
        pack = nil
        currentSource = .none
        currentSavedAt = nil
        lastAttemptAt = nil
        lastErrorMessage = nil
        // Review fix: the replay buffer has to go back to "unknown"
        // too. Dropping `pack` alone left `festpackUpdates()` still
        // handing the PREVIOUS festival's pack to the next subscriber
        // while `current()`/`sourceState()` honestly reported `.none` —
        // two surfaces of this one provider disagreeing about which
        // festival is loaded, which is exactly the confidently-wrong
        // screen CLAUDE.md's honest-data rule forbids. A later
        // `loadFromCacheOrBundle()`/`fetchAndPublish()` re-yields as
        // soon as there is something true to say.
        hub.clearCurrent()
    }

    private func loadFromCacheOrBundle() {
        let key = festivalKey()
        if let cached = cache.load(key: key), case .success(let parsed) = FestpackParser.parse(cached.json) {
            pack = parsed
            currentSource = .cached
            currentSavedAt = cached.savedAt
            hub.yield(parsed)
            return
        }
        // Only the built-in default festival ships an offline fallback
        // (the bundled resource is literally named after it) — any
        // other festival simply has nothing to show until a fetch
        // succeeds, which `loadFromCacheOrBundle()` leaves as the
        // honest `.none` state rather than inventing a bundled copy
        // that does not exist.
        guard let bundled = bundleLoader.festpackData(forResource: key, extension: "festpack.json"),
              case .success(let parsed) = FestpackParser.parse(bundled) else { return }
        pack = parsed
        currentSource = .bundled
        currentSavedAt = nil
        hub.yield(parsed)
    }

    private func fetchAndPublish() async {
        let key = festivalKey()
        lastAttemptAt = now()
        let cachedETag = cache.load(key: key)?.etag
        do {
            guard let response = try await fetcher.fetch(sourceURL(), ifNoneMatch: cachedETag) else {
                // 304 Not Modified: the network just confirmed this
                // exact pack is still current. Bump the disk sidecar
                // (not the bytes) so the freshness clock resets — an
                // hour-old pack the server just re-validated is not
                // honestly "an hour old" any more, it is confirmed
                // current as of right now.
                lastErrorMessage = nil
                if let entry = cache.load(key: key) {
                    cache.touch(etag: entry.etag, savedAt: now(), key: key)
                    currentSource = .fetched
                    currentSavedAt = now()
                }
                return
            }
            guard case .success(let parsed) = FestpackParser.parse(response.body) else {
                lastErrorMessage = "malformed pack"
                return // never show, and never cache, a pack that failed to parse
            }
            if let expected = expectedSHA256(), !expected.isEmpty {
                let actual = Self.sha256Hex(response.body)
                guard actual == expected else {
                    // "verify sha256 of a fetched pack against the index
                    // when both are available (mismatch -> keep the old
                    // pack, show the error honestly)" — owner ask #2.
                    // Never cached, never parsed into `pack`, exactly
                    // like a parse failure just above.
                    lastErrorMessage = "checksum mismatch"
                    return
                }
            }
            cache.save(json: response.body, etag: response.etag, savedAt: now(), key: key)
            pack = parsed
            currentSource = .fetched
            currentSavedAt = now()
            lastErrorMessage = nil
            hub.yield(parsed)
        } catch {
            // Offline or an HTTP failure: keep whatever pack (cached or
            // bundled) is already showing — never clear it on a failed
            // fetch. Recorded honestly rather than swallowed.
            lastErrorMessage = Self.describeFetchFailure(error)
        }
    }
}
