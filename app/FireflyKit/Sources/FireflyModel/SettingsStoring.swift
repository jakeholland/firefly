//
//  SettingsStoring.swift — the persistence seam (docs/specs/
//  A01-companion-app.md, "Persistence", and S5).
//
//  Landed here, un-owned by any single slice, so Settings (slice C),
//  Radar's location-sharing toggle (slice D) and the real transport
//  picker (slices A/F) all read and write through ONE seam instead of
//  each reaching for `UserDefaults` directly. Keychain items (channel
//  PSKs) are deliberately NOT here — the spec is explicit that a key
//  does not belong next to `UserDefaults`-shaped settings.
//
import Foundation

public enum SettingsKey: String, Sendable, CaseIterable {
    case lastPeripheralID
    case bondedPeripheralIDs
    case unitsMetric
    case locationSharingEnabled
    case locationSharingIntervalSeconds
    case transportKind
}

/// Small and typed rather than a raw `UserDefaults` pass-through, so a
/// mistyped key is a compile error, not a silently-`nil` read at a
/// festival. Methods, not `subscript`/property requirements, so a
/// conforming type can stay a plain class without exposing a settable
/// stored property across actor boundaries.
public protocol SettingsStoring: AnyObject, Sendable {
    func string(_ key: SettingsKey) -> String?
    func setString(_ value: String?, _ key: SettingsKey)
    func bool(_ key: SettingsKey) -> Bool
    func setBool(_ value: Bool, _ key: SettingsKey)
    func double(_ key: SettingsKey) -> Double?
    func setDouble(_ value: Double?, _ key: SettingsKey)
}

/// The M1 stand-in and the default for unit tests and the iOS Simulator
/// stub stack: same shape as a real `UserDefaults`-backed store, with
/// nothing that survives a process relaunch — exactly what a test needs
/// to stay hermetic between runs.
public final class InMemorySettingsStore: SettingsStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var strings: [SettingsKey: String] = [:]
    private var bools: [SettingsKey: Bool] = [:]
    private var doubles: [SettingsKey: Double] = [:]

    public init() {}

    public func string(_ key: SettingsKey) -> String? {
        lock.lock(); defer { lock.unlock() }
        return strings[key]
    }

    public func setString(_ value: String?, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        strings[key] = value
    }

    public func bool(_ key: SettingsKey) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return bools[key] ?? false
    }

    public func setBool(_ value: Bool, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        bools[key] = value
    }

    public func double(_ key: SettingsKey) -> Double? {
        lock.lock(); defer { lock.unlock() }
        return doubles[key]
    }

    public func setDouble(_ value: Double?, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        doubles[key] = value
    }
}
