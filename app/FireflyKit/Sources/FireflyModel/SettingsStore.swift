//
//  SettingsStore.swift — the real, UserDefaults-backed SettingsStoring
//  implementation (docs/specs/A01-companion-app.md, Slice C).
//
//  SettingsStoring itself (the six shared keys) is landed, frozen infra
//  (S5) this slice depends on rather than owns — see SettingsStoring.swift.
//  This file is the thing the spec actually asks slice C to add: a real
//  backing store for that protocol, plus a small number of ADDITIONAL
//  typed preferences the Settings/Diagnostics screen needs that the six
//  shared keys don't cover (colorblind palette, "stay connected in
//  background", a locally drafted node name). Those extra keys are
//  declared HERE, not added to `SettingsKey` in SettingsStoring.swift:
//  that file is shared infra other slices already depend on by its
//  current shape, and growing it out from under them is exactly the
//  kind of shared-file edit this PR's slicing rule (S6) reserves for a
//  declared, append-only hunk in one of the four named files — and
//  Settings/Diagnostics is the only consumer of these four anyway.
//
import Foundation

/// Preferences only Settings/Diagnostics reads or writes — see the
/// header above for why these are not `SettingsKey` cases.
public enum FireflyExtraSettingsKey: String, Sendable, CaseIterable {
    case colorblindPaletteEnabled
    case backgroundConnectEnabled
    case nodeLongNamePreference
    case nodeShortNamePreference
}

/// Real backing store for `SettingsStoring`. `UserDefaults`-backed,
/// same six methods as `InMemorySettingsStore` (the M1 stand-in, landed
/// with the protocol) — so swapping one for the other changes nothing
/// above this seam. Every key is namespaced `firefly.settings.` so this
/// store can share a `UserDefaults` suite (or `.standard`) without
/// colliding with anything else that reads or writes it.
public final class SettingsStore: SettingsStoring, @unchecked Sendable {
    private let defaults: UserDefaults
    private let lock = NSLock()
    private static let prefix = "firefly.settings."

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    private func namespaced(_ key: String) -> String { Self.prefix + key }

    // MARK: - SettingsStoring

    public func string(_ key: SettingsKey) -> String? {
        lock.lock(); defer { lock.unlock() }
        return defaults.string(forKey: namespaced(key.rawValue))
    }

    public func setString(_ value: String?, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        let k = namespaced(key.rawValue)
        if let value { defaults.set(value, forKey: k) } else { defaults.removeObject(forKey: k) }
    }

    public func bool(_ key: SettingsKey) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return defaults.bool(forKey: namespaced(key.rawValue))
    }

    public func setBool(_ value: Bool, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        defaults.set(value, forKey: namespaced(key.rawValue))
    }

    /// `nil` when never set — distinct from `0`, per `SettingsStoring`'s
    /// own contract, so a caller does not read an unset interval as
    /// "zero seconds" and treat that as a real (absurd) cadence.
    public func double(_ key: SettingsKey) -> Double? {
        lock.lock(); defer { lock.unlock() }
        let k = namespaced(key.rawValue)
        guard defaults.object(forKey: k) != nil else { return nil }
        return defaults.double(forKey: k)
    }

    public func setDouble(_ value: Double?, _ key: SettingsKey) {
        lock.lock(); defer { lock.unlock() }
        let k = namespaced(key.rawValue)
        if let value { defaults.set(value, forKey: k) } else { defaults.removeObject(forKey: k) }
    }

    // MARK: - Extra, Settings/Diagnostics-only preferences (see header)

    public var colorblindPaletteEnabled: Bool {
        get { rawBool(.colorblindPaletteEnabled) }
        set { setRawBool(newValue, .colorblindPaletteEnabled) }
    }

    public var backgroundConnectEnabled: Bool {
        get { rawBool(.backgroundConnectEnabled) }
        set { setRawBool(newValue, .backgroundConnectEnabled) }
    }

    /// A LOCAL DRAFT only, never sent anywhere: M1 has no admin-message
    /// path to actually rename the connected node (that is M3-earliest
    /// territory, the same "Channel editing" scope cut applies here) —
    /// the Settings screen labels it as a draft rather than implying it
    /// takes effect.
    public var nodeLongNamePreference: String? {
        get { rawString(.nodeLongNamePreference) }
        set { setRawString(newValue, .nodeLongNamePreference) }
    }

    public var nodeShortNamePreference: String? {
        get { rawString(.nodeShortNamePreference) }
        set { setRawString(newValue, .nodeShortNamePreference) }
    }

    private func rawBool(_ key: FireflyExtraSettingsKey) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return defaults.bool(forKey: namespaced(key.rawValue))
    }

    private func setRawBool(_ value: Bool, _ key: FireflyExtraSettingsKey) {
        lock.lock(); defer { lock.unlock() }
        defaults.set(value, forKey: namespaced(key.rawValue))
    }

    private func rawString(_ key: FireflyExtraSettingsKey) -> String? {
        lock.lock(); defer { lock.unlock() }
        return defaults.string(forKey: namespaced(key.rawValue))
    }

    private func setRawString(_ value: String?, _ key: FireflyExtraSettingsKey) {
        lock.lock(); defer { lock.unlock() }
        let k = namespaced(key.rawValue)
        if let value { defaults.set(value, forKey: k) } else { defaults.removeObject(forKey: k) }
    }
}
