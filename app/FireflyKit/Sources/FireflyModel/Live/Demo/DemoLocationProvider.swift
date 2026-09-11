//
//  DemoLocationProvider.swift — the phone's own scripted GPS fix for
//  demo mode (`docs/specs/S20-demo-mode.md`'s honesty rule, ported to
//  the app: seeded, not faked — this really is what `fixes()` reports,
//  the same seam `PhoneGPSUplink`/`RadarViewModel` read in the live
//  graph).
//
//  Deliberately NOT `EventHub`-backed like the client's own streams:
//  `EventHub.yield` only reaches subscribers that already exist
//  (`EventHub.swift`'s own doc comment — "a subscriber that arrives
//  after this call does not see it"), and this provider has to survive
//  being read by a screen that appears AFTER `DemoRunner` has already
//  set the fix once (e.g. the Radar tab, opened well after `start()`).
//  So every new `fixes()`/`headings()` subscriber gets hand ed the
//  CURRENT value immediately, then future updates as they happen —
//  "replay latest", not "multicast only from now on".
//
import Foundation

// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access below goes through `lock`, never unguarded.
public final class DemoLocationProvider: LocationProviding, @unchecked Sendable {
    public let authorization: LocationAuthorization = .whenInUse

    private let lock = NSLock()
    private var current: LocationFix?
    private var nextID = 0
    private var continuations: [Int: AsyncStream<LocationFix?>.Continuation] = [:]

    public init(initialFix: LocationFix?) {
        self.current = initialFix
    }

    public func requestWhenInUseAuthorization() async {}
    public func requestAlwaysAuthorization() async {}

    public func fixes() -> AsyncStream<LocationFix?> {
        lock.lock()
        let id = nextID
        nextID += 1
        let value = current
        lock.unlock()
        return AsyncStream(bufferingPolicy: .bufferingNewest(4)) { continuation in
            continuation.yield(value)
            lock.lock()
            continuations[id] = continuation
            lock.unlock()
            continuation.onTermination = { [weak self] _ in self?.remove(id) }
        }
    }

    /// `DemoRunner`'s own control surface — `nil` is how the "no-GPS
    /// signal view" screenshot puts the phone into RADAR_SIGNAL
    /// honestly: not a special mode flag anywhere, just what
    /// `fixes()` actually reports, exactly like a real phone that lost
    /// its fix.
    public func setFix(_ fix: LocationFix?) {
        lock.lock()
        current = fix
        let subs = Array(continuations.values)
        lock.unlock()
        for continuation in subs { continuation.yield(fix) }
    }

    private func remove(_ id: Int) {
        lock.lock(); continuations.removeValue(forKey: id); lock.unlock()
    }
}
