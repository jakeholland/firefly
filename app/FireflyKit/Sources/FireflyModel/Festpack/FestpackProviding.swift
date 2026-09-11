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
//  cached (age) / fresh — see that enum's own doc comment.
//
import Foundation

public enum FestpackSourceState: Sendable, Equatable {
    /// Nothing has loaded yet — no cache on disk, no bundled fallback
    /// available, network not yet reached (or already failed).
    case none
    /// The bundled fallback (`firmware/assets/field/...`), used because
    /// there was no disk cache AND no network at load time.
    case bundled
    /// A previously-fetched pack, read from disk. `ageSeconds` is wall-
    /// clock time since it was saved — never invented, always measured
    /// against the same clock `now:` (the provider's injected clock)
    /// reports.
    case cached(ageSeconds: TimeInterval)
    /// Fetched and parsed successfully just now.
    case fresh
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
    /// order.
    func refresh() async
}
