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
    // Appended for M2 (PR #265's own review flagged this): a NEW case
    // rather than reusing `unitsMetric`, whose stored TYPE (bool) is
    // exactly the bug — `SettingsStoring.bool` cannot tell "chose
    // imperial" from "never written", so a fresh install would have to
    // guess metric-or-imperial from an unset default. `unitsMetric`
    // itself is left in place, untouched, rather than removed: this
    // file is shared infra several other M2 slices also touch this
    // sprint, and removing a case is not an append. See this file's
    // bottom section ("Units preference") for the tri-state that
    // replaces it in practice.
    case unitsPreference
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
public final class InMemorySettingsStore: FireflyExtraSettingsStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var strings: [SettingsKey: String] = [:]
    private var bools: [SettingsKey: Bool] = [:]
    private var doubles: [SettingsKey: Double] = [:]
    private var extraBools: [String: Bool] = [:]
    private var extraStrings: [String: String] = [:]

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

    // MARK: - FireflyExtraSettingsStoring
    //
    // The same four Settings/Diagnostics preferences `SettingsStore`
    // persists (`FireflyExtraSettingsKey`), held in memory — so a test
    // or the iOS Simulator gets one store with the whole surface on it,
    // rather than two stores that can disagree.

    public var colorblindPaletteEnabled: Bool {
        get { extraBool("colorblindPaletteEnabled") }
        set { setExtraBool(newValue, "colorblindPaletteEnabled") }
    }

    public var backgroundConnectEnabled: Bool {
        get { extraBool("backgroundConnectEnabled") }
        set { setExtraBool(newValue, "backgroundConnectEnabled") }
    }

    public var nodeLongNamePreference: String? {
        get { extraString("nodeLongNamePreference") }
        set { setExtraString(newValue, "nodeLongNamePreference") }
    }

    public var nodeShortNamePreference: String? {
        get { extraString("nodeShortNamePreference") }
        set { setExtraString(newValue, "nodeShortNamePreference") }
    }

    private func extraBool(_ key: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return extraBools[key] ?? false
    }

    private func setExtraBool(_ value: Bool, _ key: String) {
        lock.lock(); defer { lock.unlock() }
        extraBools[key] = value
    }

    private func extraString(_ key: String) -> String? {
        lock.lock(); defer { lock.unlock() }
        return extraStrings[key]
    }

    private func setExtraString(_ value: String?, _ key: String) {
        lock.lock(); defer { lock.unlock() }
        extraStrings[key] = value
    }
}

// MARK: - Units preference (tri-state; M2)
//
// Appended as one contiguous hunk rather than folded into the sections
// above — `SettingsStoring` is shared infra several other M2 slices
// also touch this sprint, so a diff confined to the end of the file is
// the one least likely to collide with theirs.
//
// The bug this replaces (PR #265's own review, and `AppGraph
// .makeRadarViewModel`'s doc comment before this PR): `SettingsKey
// .unitsMetric` is a BOOL, and `SettingsStoring.bool` has no way to
// distinguish "the user picked imperial" from "nobody has ever written
// this key" — both read `false`. A bool default of `false` therefore
// made every fresh install METRIC, silently, regardless of where the
// phone actually is, and nothing ever wrote the key besides tests. A
// tri-state fixes this at the type level: `.system` (the real default)
// is not itself a unit, so a fresh install renders the Radar's very
// first frame in whatever unit its OWN locale implies, never a guess
// baked into this file.

/// A user's distance-unit choice. `.metric`/`.imperial` are explicit
/// overrides; `.system` — the default for a fresh install, and the only
/// value ever read before something writes this key — defers to the
/// phone's own region rather than asserting a unit on its own
/// (`resolvedImperial(locale:)`).
public enum UnitsPreference: String, Sendable, CaseIterable {
    case system
    case metric
    case imperial

    /// Resolves this preference to an actual metric(`false`)/
    /// imperial(`true`) choice. `.metric`/`.imperial` pass straight
    /// through; `.system` follows `locale.measurementSystem` — `.us`
    /// AND `.uk` read imperial (the UK's own road distances are miles
    /// despite the UK being otherwise metric; `Locale.MeasurementSystem`
    /// carries `.uk` as its own case for exactly this reason), every
    /// other region metric.
    public func resolvedImperial(locale: Locale = .current) -> Bool {
        switch self {
        case .metric: return false
        case .imperial: return true
        case .system:
            switch locale.measurementSystem {
            case .us, .uk: return true
            default: return false
            }
        }
    }
}

extension SettingsStoring {
    /// The stored tri-state preference — `.system` when nothing has
    /// been written yet (a fresh install) or when a stored value this
    /// build no longer recognizes, never a silently-wrong guess at a
    /// physical unit. Built on `string(_:)`, one of the six methods
    /// every `SettingsStoring` conformer already implements, so this is
    /// a pure protocol-extension default — no conformer (`SettingsStore`,
    /// `InMemorySettingsStore`, any test double) needs a single line
    /// changed to pick it up.
    public func unitsPreference() -> UnitsPreference {
        string(.unitsPreference).flatMap(UnitsPreference.init(rawValue:)) ?? .system
    }

    public func setUnitsPreference(_ value: UnitsPreference) {
        setString(value.rawValue, .unitsPreference)
    }

    /// Convenience: `unitsPreference().resolvedImperial(locale:)`. The
    /// one call every distance-rendering seam in the app (Radar today;
    /// Inbox/Thread RALLY and Diagnostics once they render a distance)
    /// makes to decide metric vs. imperial — so there is exactly one
    /// place that resolution logic lives, never re-derived per call
    /// site.
    public func resolvedImperial(locale: Locale = .current) -> Bool {
        unitsPreference().resolvedImperial(locale: locale)
    }
}
