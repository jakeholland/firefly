//
//  FestpackProviding.swift — the festival-data seam (owner's
//  instructions: "AlmanacFestpackProvider... FestpackProviding
//  protocol (current Festpack value + festpackUpdates() stream via the
//  existing EventHub pattern)").
//
//  Honest states only: `current()` is `nil` until something has
//  actually loaded (no cache, no bundle, no network yet at first
//  launch) — never a fabricated placeholder pack. `sourceState()` is
//  the Settings "Festival data" row's whole story: no pack / bundled /
//  cached (age) / fetched (age), PLUS whether the most recent refresh
//  attempt actually failed — see `FestpackSourceState`'s own doc
//  comment.
//
import Foundation

/// "app: automatic almanac refresh + festival picker" (owner ask,
/// 2026-09-13) — widened from a bare enum (`.none`/`.bundled`/
/// `.cached(ageSeconds:)`/`.fresh`) into this struct so a provider can
/// report the honest, FULL provenance story an automatic background
/// refresh needs: not just "what pack is showing" but "when did we
/// last even TRY, and did that attempt fail" — a `.cached` state alone
/// cannot distinguish "nothing has tried to refresh in days" from "we
/// just tried and failed, offline". `Equatable` for tests; every field
/// is either optional or has an honest default — never a fabricated
/// value.
public struct FestpackSourceState: Sendable, Equatable {
    /// Where the pack CURRENTLY showing (`FestpackProviding.current()`)
    /// came from. Deliberately not itself named "fresh" — a `.fetched`
    /// pack that is six hours old is not fresh, it is simply the most
    /// recent thing this provider ever downloaded; `ageSeconds` (below)
    /// carries how old, `statusText` renders that honestly.
    public enum Source: Sendable, Equatable {
        /// Nothing has loaded yet — no cache on disk, no bundled
        /// fallback available, network not yet reached (or already
        /// failed) with nothing else to fall back to.
        case none
        /// The bundled fallback (`firmware/assets/field/...`), used
        /// because there was no disk cache AND no network at load time.
        case bundled
        /// A previously-fetched pack, read from disk this launch — the
        /// network was not (successfully) consulted to produce THIS
        /// value, even if a later attempt failed and left it in place.
        case cached
        /// Downloaded and parsed successfully at `savedAt` (a 304 Not
        /// Modified response counts as "fetched" too — the network
        /// confirmed this exact pack is still current, which is the
        /// same thing a 200 with identical bytes would report).
        case fetched
    }

    public let source: Source
    /// When the CURRENT pack was written to disk — `nil` for `.none`/
    /// `.bundled` (neither one has a disk timestamp at all), and also
    /// `nil` for `.cached`/`.fetched` when the disk cache's metadata
    /// sidecar is missing/unreadable. Never a fabricated timestamp.
    public let savedAt: Date?
    /// Elapsed wall-clock time since `savedAt`, measured against the
    /// SAME clock `savedAt` itself was measured against.
    ///
    /// `nil` means the age is genuinely UNKNOWN, and is the honest
    /// answer to two real situations (hardening QA pass, carried
    /// forward from this type's original design):
    ///
    /// 1. The cache's metadata sidecar is missing or corrupt, so there
    ///    is no saved-at time to measure against at all.
    /// 2. The saved-at time is in the FUTURE. This app's premise is a
    ///    phone with no cell service for three days: its clock drifts,
    ///    and iOS corrects it — sometimes BACKWARDS — the moment it sees
    ///    a tower again. A cache written before that correction then
    ///    looks like it was written in the future.
    ///
    /// Both used to be papered over: (1) read as `.distantPast` and (2)
    /// was clamped by a `max(0, …)`, which rendered as **"cached (just
    /// now)"** — a pack of unknown vintage presented as freshly
    /// fetched, which is exactly the fabricated-freshness claim this
    /// project exists not to make.
    public let ageSeconds: TimeInterval?
    /// The last time ANY refresh was ATTEMPTED — a throttled/skipped
    /// auto-refresh does not count, only a real network round trip
    /// (success, 304, parse failure, or transport failure all count).
    /// Drives the 15-minute auto-refresh throttle; not itself shown in
    /// `statusText` (nothing asks "when did you last check", only
    /// "what am I looking at").
    public let lastAttempt: Date?
    /// Non-nil iff the MOST RECENT refresh attempt failed — a short,
    /// honest description ("offline", "http 500", "checksum
    /// mismatch"), never a raw `Error` dump. Cleared the instant a
    /// LATER attempt succeeds (or 304s), so a stale failure is never
    /// shown after a good refresh — `statusText` folds this in as
    /// "refresh failed: <reason> · using <what's actually showing>".
    public let lastError: String?

    public init(source: Source, savedAt: Date?, ageSeconds: TimeInterval?, lastAttempt: Date?, lastError: String?) {
        self.source = source
        self.savedAt = savedAt
        self.ageSeconds = ageSeconds
        self.lastAttempt = lastAttempt
        self.lastError = lastError
    }

    public static let none = FestpackSourceState(source: .none, savedAt: nil, ageSeconds: nil, lastAttempt: nil, lastError: nil)
    public static let bundled = FestpackSourceState(source: .bundled, savedAt: nil, ageSeconds: nil, lastAttempt: nil, lastError: nil)

    /// Test/preview convenience — production always goes through the
    /// full initializer above (via a provider's own state, which also
    /// carries `lastAttempt`/`lastError`).
    public static func cached(ageSeconds: TimeInterval?) -> FestpackSourceState {
        FestpackSourceState(source: .cached, savedAt: nil, ageSeconds: ageSeconds, lastAttempt: nil, lastError: nil)
    }

    /// Test/preview convenience, see `cached(ageSeconds:)` above.
    public static func fetched(ageSeconds: TimeInterval? = 0) -> FestpackSourceState {
        FestpackSourceState(source: .fetched, savedAt: nil, ageSeconds: ageSeconds, lastAttempt: nil, lastError: nil)
    }

    /// The Settings "Festival data" row's and the Lineup header's
    /// shared wording. In the model, not in either view, so the string
    /// is testable and the two screens cannot drift apart — the same
    /// rule `ConnectViewModel.statusLabel` follows (A01, "MVVM
    /// conventions" item 5). Examples: "fetched 2 h ago", "cached (age
    /// unknown)", "bundled copy", "refresh failed: offline · using
    /// cache from 6 h ago".
    public var statusText: String {
        let base: String
        switch source {
        case .none: base = "no pack"
        case .bundled: base = "bundled copy"
        case .cached: base = "cached \(Self.ageDescription(ageSeconds))"
        case .fetched: base = "fetched \(Self.ageDescription(ageSeconds))"
        }
        guard let lastError else { return base }
        switch source {
        case .none: return "refresh failed: \(lastError) · no pack cached"
        case .bundled: return "refresh failed: \(lastError) · using bundled copy"
        case .cached, .fetched: return "refresh failed: \(lastError) · using cache from \(Self.ageDescription(ageSeconds))"
        }
    }

    /// "2 h ago" / "(age unknown)" / "just now" — the same ladder this
    /// type always used, in units a person reading a phone at 2am can
    /// parse (never raw minutes into the thousands).
    private static func ageDescription(_ ageSeconds: TimeInterval?) -> String {
        guard let age = ageSeconds else { return "(age unknown)" }
        let minutes = Int(age / 60)
        if minutes < 1 { return "just now" }
        if minutes < 60 { return "\(minutes) min ago" }
        let hours = minutes / 60
        if hours < 48 { return "\(hours) h ago" }
        return "\(hours / 24) d ago"
    }
}

/// `FestpackProviding`'s one seam onto "the app bundle" — real callers
/// use `Bundle.main` (works correctly even for code that lives in the
/// `FireflyModel` SwiftPM target, since `Bundle.main` resolves to
/// whatever process is actually running, i.e. `Firefly.app`); a bare
/// `swift test` process has no such bundle, so tests inject a loader
/// that reads the real `firmware/assets/*` files straight off disk —
/// see `FestpackProviderTests`.
public protocol FestpackBundleLoading: Sendable {
    func festpackData(forResource name: String, extension ext: String) -> Data?
}

public struct MainBundleFestpackLoader: FestpackBundleLoading {
    public init() {}
    public func festpackData(forResource name: String, extension ext: String) -> Data? {
        guard let url = Bundle.main.url(forResource: name, withExtension: ext) else { return nil }
        return try? Data(contentsOf: url)
    }
}

/// The phone fetches the pack at runtime and caches it offline (unlike
/// the puck, which embeds one at build time — owner's decision). Every
/// conformer is `Sendable`; `AlmanacFestpackProvider`/`DemoFestpackProvider`
/// are `actor`s so their mutable pack/state can be read from any isolation
/// domain without a lock.
public protocol FestpackProviding: Sendable {
    /// The current pack, or `nil` if nothing has loaded yet.
    func current() async -> Festpack?
    func sourceState() async -> FestpackSourceState
    /// Multicast (`EventHub`/`CurrentValueEventHub` pattern, see
    /// `FireflyMesh/EventHub.swift`) — a late subscriber is replayed the
    /// current pack (if any) immediately, then every pack this provider
    /// successfully loads after that, in order. A failed fetch or a pack
    /// that failed to parse never yields here — see `refresh()`.
    func festpackUpdates() -> AsyncStream<Festpack>
    /// Fetches (if this provider has a network source) and/or loads
    /// whatever is available. On first call, this is also "start":
    /// cache-first, then a background fetch attempt. A parse failure or
    /// fetch failure NEVER replaces an already-good `current()` pack —
    /// see each conformer's own doc comment for its exact fallback
    /// order. UNCONDITIONAL — always attempts a fetch (subject only to
    /// ETag-conditional GET). This is what the Settings REFRESH button
    /// and the Lineup pull-to-refresh call; it is never throttled.
    func refresh() async
    /// "app: automatic almanac refresh" (owner ask #1, 2026-09-13) —
    /// what `AppGraph.start()`/`handleScenePhaseChange(.foreground)`
    /// call instead of `refresh()`. Loads cache/bundle first exactly
    /// like `refresh()` does when nothing is loaded yet, but only
    /// attempts a network fetch when BOTH: the current pack is missing
    /// or older than the provider's own staleness threshold, AND at
    /// least the provider's own minimum interval has passed since the
    /// last attempt (throttle) — see `AlmanacFestpackProvider`'s own
    /// doc comment for the exact numbers. `DemoFestpackProvider` has no
    /// network source at all, so this is identical to `refresh()` there
    /// (already idempotent once loaded).
    func refreshIfNeeded() async
}
