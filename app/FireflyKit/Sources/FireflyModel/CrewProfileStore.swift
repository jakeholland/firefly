//
//  CrewProfileStore.swift — Slice B's persisted "which crew is this
//  phone on" state (`docs/specs/A02-crew-join.md`, §2.1 step 3, §3.4,
//  §4.5): the crew code + human name + creation time a Start/Join
//  writes locally BEFORE any radio write, the per-crew hidden-id set,
//  and the pre-crew channel snapshot Leave restores.
//
//  Same shape as `CrewPairingStore.swift` next door: a small typed
//  protocol + an in-memory stand-in (tests, `.stub()`) + a
//  `UserDefaults`-backed real implementation, wired in through
//  `AppDependencies`' append-only convention.
//
import Foundation
import Security

// MARK: - CrewProfile — "which crew is this phone on"

/// The local record a Start/Join writes before any radio write (§2.1
/// step 3) — everything the Crew page and the Start "code" screen need
/// that is NOT re-derivable from the radio's channel table alone: the
/// human name (cosmetic, local — §1.3) and `crewCreatedAt`, the anchor
/// `CrewJoinWatcher`/the stub `CrewMembershipProviding` measures
/// "joined N" from.
public struct CrewProfile: Sendable, Equatable, Codable {
    /// The canonical code, e.g. `"FIRE-4K9M7X"`.
    public let code: String
    /// App-local display only (§1.3) — never written to the radio,
    /// never part of key derivation. Defaults to `"My crew"` at Start,
    /// editable immediately.
    public var humanName: String
    /// Epoch milliseconds — when THIS PHONE started or joined this
    /// crew. Not when the crew was first created by someone else.
    public let createdAtMs: UInt64

    public init(code: String, humanName: String, createdAtMs: UInt64) {
        self.code = code
        self.humanName = humanName
        self.createdAtMs = createdAtMs
    }
}

/// One entry in the "recent crews" list (§3.4: "kept in a local 'recent
/// crews' list (max 4) so coming back is one tap, not a re-scan").
public struct RecentCrew: Sendable, Equatable, Codable {
    public let code: String
    public let humanName: String

    public init(code: String, humanName: String) {
        self.code = code
        self.humanName = humanName
    }
}

public protocol CrewProfileStoring: AnyObject, Sendable {
    func load() -> CrewProfile?
    func save(_ profile: CrewProfile)
    /// Clears the active profile. Does NOT touch the recent-crews list,
    /// the hidden set, or the pre-crew snapshot — those are keyed by
    /// crew code and outlive an active-profile clear (§3.4).
    func clear()

    /// §3.4's "recent crews" list, newest first, max 4. Kept forever
    /// (never auto-pruned beyond that cap) so members/colours/hides
    /// "kept per crew code" (§3.4) have somewhere to be found again.
    func recentCrews() -> [RecentCrew]
    /// Adds/moves `crew` to the front of the recent list, capped at 4 —
    /// called whenever the active profile changes away from a crew that
    /// was real (both a fresh Start/Join's PREVIOUS crew, and Leave's).
    func rememberRecentCrew(_ crew: RecentCrew)
}

/// M1-style stand-in — nothing survives a relaunch. `.stub()`'s default.
public final class InMemoryCrewProfileStore: CrewProfileStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var profile: CrewProfile?
    private var recent: [RecentCrew] = []

    public init() {}

    public func load() -> CrewProfile? { lock.lock(); defer { lock.unlock() }; return profile }
    public func save(_ profile: CrewProfile) { lock.lock(); self.profile = profile; lock.unlock() }
    public func clear() { lock.lock(); profile = nil; lock.unlock() }
    public func recentCrews() -> [RecentCrew] { lock.lock(); defer { lock.unlock() }; return recent }
    public func rememberRecentCrew(_ crew: RecentCrew) {
        lock.lock()
        recent.removeAll { $0.code == crew.code }
        recent.insert(crew, at: 0)
        if recent.count > 4 { recent.removeLast(recent.count - 4) }
        lock.unlock()
    }
}

/// The real, `UserDefaults`-backed store — one JSON value for the
/// active profile, one JSON array for recents, same convention
/// `CrewPairingStore` uses for its own paired-list key.
public final class CrewProfileStore: CrewProfileStoring, @unchecked Sendable {
    private let defaults: UserDefaults
    private let lock = NSLock()
    private static let profileKey = "firefly.crew.profile.v1"
    private static let recentKey = "firefly.crew.recent.v1"

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public func load() -> CrewProfile? {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: Self.profileKey) else { return nil }
        return try? JSONDecoder().decode(CrewProfile.self, from: data)
    }

    public func save(_ profile: CrewProfile) {
        lock.lock(); defer { lock.unlock() }
        guard let data = try? JSONEncoder().encode(profile) else { return }
        defaults.set(data, forKey: Self.profileKey)
    }

    public func clear() {
        lock.lock(); defer { lock.unlock() }
        defaults.removeObject(forKey: Self.profileKey)
    }

    public func recentCrews() -> [RecentCrew] {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: Self.recentKey) else { return [] }
        return (try? JSONDecoder().decode([RecentCrew].self, from: data)) ?? []
    }

    public func rememberRecentCrew(_ crew: RecentCrew) {
        lock.lock(); defer { lock.unlock() }
        var current = (try? JSONDecoder().decode([RecentCrew].self,
                                                   from: defaults.data(forKey: Self.recentKey) ?? Data())) ?? []
        current.removeAll { $0.code == crew.code }
        current.insert(crew, at: 0)
        if current.count > 4 { current.removeLast(current.count - 4) }
        guard let data = try? JSONEncoder().encode(current) else { return }
        defaults.set(data, forKey: Self.recentKey)
    }
}

// MARK: - Hidden set (§4.5)

/// Per-crew-code hidden id set — "stored per crew code
/// (`firefly.crew.hidden.<code>.v1`), so leaving and rejoining a crew
/// restores the hides you had" (§4.5).
public protocol CrewHiddenStoring: AnyObject, Sendable {
    func hiddenIDs(forCrew code: String) -> Set<UInt32>
    func setHiddenIDs(_ ids: Set<UInt32>, forCrew code: String)
}

public final class InMemoryCrewHiddenStore: CrewHiddenStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var byCode: [String: Set<UInt32>] = [:]

    public init() {}

    public func hiddenIDs(forCrew code: String) -> Set<UInt32> {
        lock.lock(); defer { lock.unlock() }
        return byCode[code] ?? []
    }

    public func setHiddenIDs(_ ids: Set<UInt32>, forCrew code: String) {
        lock.lock(); byCode[code] = ids; lock.unlock()
    }
}

public final class CrewHiddenStore: CrewHiddenStoring, @unchecked Sendable {
    private let defaults: UserDefaults
    private let lock = NSLock()

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    private static func key(_ code: String) -> String { "firefly.crew.hidden.\(code).v1" }

    public func hiddenIDs(forCrew code: String) -> Set<UInt32> {
        lock.lock(); defer { lock.unlock() }
        guard let data = defaults.data(forKey: Self.key(code)) else { return [] }
        return Set((try? JSONDecoder().decode([UInt32].self, from: data)) ?? [])
    }

    public func setHiddenIDs(_ ids: Set<UInt32>, forCrew code: String) {
        lock.lock(); defer { lock.unlock() }
        guard let data = try? JSONEncoder().encode(Array(ids)) else { return }
        defaults.set(data, forKey: Self.key(code))
    }
}

// MARK: - Pre-crew snapshot (§2.1 step 4, §3.4)

/// The radio's index-0 channel exactly as it was before the FIRST crew
/// write this phone ever makes — captured once, never overwritten by a
/// later Firefly crew channel (§2.1 step 4), and what Leave restores
/// when present (§3.4).
public struct CrewPreCrewSnapshot: Sendable, Equatable, Codable {
    public let name: String
    public let psk: Data
    public let positionPrecision: UInt32

    public init(name: String, psk: Data, positionPrecision: UInt32) {
        self.name = name
        self.psk = psk
        self.positionPrecision = positionPrecision
    }

    /// §3.4's fallback when this phone has no snapshot: the stock
    /// Meshtastic default primary — empty name, the documented
    /// single-byte "default key" shorthand, precision 0 (a public
    /// default channel must never inherit the crew's exact-location
    /// setting).
    public static let stockDefault = CrewPreCrewSnapshot(name: "", psk: Data([0x01]), positionPrecision: 0)
}

public protocol CrewSnapshotStoring: AnyObject, Sendable {
    func load() -> CrewPreCrewSnapshot?
    /// Saves ONLY if nothing is already stored — §2.1 step 4's "once
    /// only, and never overwritten by a Firefly crew channel".
    func saveIfAbsent(_ snapshot: CrewPreCrewSnapshot)
}

public final class InMemoryCrewSnapshotStore: CrewSnapshotStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var snapshot: CrewPreCrewSnapshot?

    public init() {}

    public func load() -> CrewPreCrewSnapshot? { lock.lock(); defer { lock.unlock() }; return snapshot }
    public func saveIfAbsent(_ snapshot: CrewPreCrewSnapshot) {
        lock.lock(); defer { lock.unlock() }
        guard self.snapshot == nil else { return }
        self.snapshot = snapshot
    }
}

/// The real, Keychain-backed store — `firefly.crew.preCrewPrimary.v1`
/// (§2.1 step 4). A plain JSON blob under one generic-password item
/// (`kSecClassGenericPassword`); the PSK it carries is exactly as
/// sensitive as any other crew PSK already sitting in this app's
/// `UserDefaults`-backed settings, so Keychain here is about surviving
/// an app-data reset / reinstall more cleanly than about a stronger
/// trust boundary.
public final class CrewSnapshotKeychainStore: CrewSnapshotStoring, @unchecked Sendable {
    private static let account = "firefly.crew.preCrewPrimary.v1"
    private static let service = "com.firefly.crew"
    private let lock = NSLock()

    public init() {}

    public func load() -> CrewPreCrewSnapshot? {
        lock.lock(); defer { lock.unlock() }
        return loadLocked()
    }

    /// Caller already holds `lock` — `saveIfAbsent` needs this to check
    /// "already stored?" without re-entering `NSLock` (not reentrant).
    private func loadLocked() -> CrewPreCrewSnapshot? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: Self.account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return nil }
        return try? JSONDecoder().decode(CrewPreCrewSnapshot.self, from: data)
    }

    public func saveIfAbsent(_ snapshot: CrewPreCrewSnapshot) {
        lock.lock(); defer { lock.unlock() }
        guard loadLocked() == nil, let data = try? JSONEncoder().encode(snapshot) else { return }
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: Self.account,
        ]
        SecItemDelete(query as CFDictionary) // idempotent — clears any stale item before adding
        var addQuery = query
        addQuery[kSecValueData as String] = data
        addQuery[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(addQuery as CFDictionary, nil)
    }
}
