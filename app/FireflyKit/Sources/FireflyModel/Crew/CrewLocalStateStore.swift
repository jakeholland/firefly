//
//  CrewLocalStateStore.swift — the two per-crew-code lists A02 keeps on
//  the phone: the hide list (§4.5) and the join log (§2.3).
//
//  Both are keyed by CREW CODE, not stored globally, because §4.5 says
//  so out loud: *"leaving and rejoining a crew restores the hides you
//  had."* A crew's local state is crew-local.
//
//  Shaped exactly like `CrewPairingStoring` next door — a protocol with
//  methods (never a settable array property), an in-memory stand-in for
//  tests, and one `UserDefaults`-backed implementation storing JSON
//  under one namespaced key per crew. That symmetry is deliberate: this
//  is the same kind of small, durable, app-owned list, and a second
//  shape for it would be a second thing to reason about.
//
import Foundation

/// Persistence for the hide list and the join log.
///
/// `crewCode` is the canonical code (`CrewChannelIdentity.code`), or
/// `CrewLocalState.noCrewCode` for an install that has no crew code yet
/// — §4.6's migration state, where a user can still hide a
/// manually-paired member and that hide must still survive a relaunch.
public protocol CrewLocalStateStoring: AnyObject, Sendable {
    func hiddenIDs(crewCode: String) -> [UInt32]
    func setHiddenIDs(_ ids: [UInt32], crewCode: String)
    func joinEvents(crewCode: String) -> [CrewJoinEvent]
    func setJoinEvents(_ events: [CrewJoinEvent], crewCode: String)
}

public enum CrewLocalState {
    /// The namespace used before any crew code exists. Not a code and
    /// never rendered as one — it is simply where a pre-A02 install's
    /// hides live so they are not lost the moment the user starts or
    /// joins a crew.
    public static let noCrewCode = "none"

    static func hiddenKey(_ crewCode: String) -> String { "firefly.crew.hidden.\(crewCode).v1" }
    static func joinedKey(_ crewCode: String) -> String { "firefly.crew.joined.\(crewCode).v1" }
}

/// Test-only stand-in, same role `InMemoryCrewPairingStore` plays for
/// the paired list: real shape, nothing that survives a relaunch.
// `@unchecked Sendable` on the same terms as every other store in this
// module: every mutable access goes through `lock`, never unguarded.
public final class InMemoryCrewLocalStateStore: CrewLocalStateStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var hidden: [String: [UInt32]] = [:]
    private var joined: [String: [CrewJoinEvent]] = [:]

    public init() {}

    public func hiddenIDs(crewCode: String) -> [UInt32] {
        lock.lock(); defer { lock.unlock() }
        return hidden[crewCode] ?? []
    }

    public func setHiddenIDs(_ ids: [UInt32], crewCode: String) {
        lock.lock(); defer { lock.unlock() }
        hidden[crewCode] = ids
    }

    public func joinEvents(crewCode: String) -> [CrewJoinEvent] {
        lock.lock(); defer { lock.unlock() }
        return joined[crewCode] ?? []
    }

    public func setJoinEvents(_ events: [CrewJoinEvent], crewCode: String) {
        lock.lock(); defer { lock.unlock() }
        joined[crewCode] = events
    }
}

/// The real, `UserDefaults`-backed store — one JSON blob per crew code
/// per list, under `firefly.crew.hidden.<code>.v1` /
/// `firefly.crew.joined.<code>.v1` (the key §4.5 names).
public final class CrewLocalStateStore: CrewLocalStateStoring, @unchecked Sendable {
    private let defaults: UserDefaults
    private let lock = NSLock()

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func hiddenIDs(crewCode: String) -> [UInt32] {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: CrewLocalState.hiddenKey(crewCode)) else { return [] }
        return (try? JSONDecoder().decode([UInt32].self, from: data)) ?? []
    }

    public func setHiddenIDs(_ ids: [UInt32], crewCode: String) {
        lock.lock(); defer { lock.unlock() }
        guard let data = try? JSONEncoder().encode(ids) else { return }
        defaults.set(data, forKey: CrewLocalState.hiddenKey(crewCode))
    }

    public func joinEvents(crewCode: String) -> [CrewJoinEvent] {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: CrewLocalState.joinedKey(crewCode)) else { return [] }
        return (try? JSONDecoder().decode([CrewJoinEvent].self, from: data)) ?? []
    }

    public func setJoinEvents(_ events: [CrewJoinEvent], crewCode: String) {
        lock.lock(); defer { lock.unlock() }
        guard let data = try? JSONEncoder().encode(events) else { return }
        defaults.set(data, forKey: CrewLocalState.joinedKey(crewCode))
    }
}
