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
    /// A04 (docs/specs/A04-telemetry.md) — "Share diagnostics" on the
    /// Diagnostics screen. Appended last, same append-only convention
    /// as every case above it.
    case shareDiagnosticsEnabled
}

/// `SettingsStoring` plus the four Settings/Diagnostics-only
/// preferences declared in this file — the seam the composition root
/// actually hands around.
///
/// It exists because `SettingsViewModel` needs those four AND the six
/// shared keys from the SAME instance: before integration it took a
/// CONCRETE `SettingsStore()` of its own (that file's own comment said
/// so, and flagged the consequence — "a Radar-screen read of
/// `dependencies.store.bool(.locationSharingEnabled)` will not see what
/// Settings wrote"). Refining the protocol rather than widening
/// `SettingsKey` keeps slice C's original judgement intact: these four
/// are Settings' own, and nothing else reads them.
public protocol FireflyExtraSettingsStoring: SettingsStoring {
    var colorblindPaletteEnabled: Bool { get set }
    var backgroundConnectEnabled: Bool { get set }
    /// A LOCAL DRAFT only — see `SettingsStore`'s own doc comment.
    var nodeLongNamePreference: String? { get set }
    var nodeShortNamePreference: String? { get set }
    /// A04 — "Share diagnostics". Default ON in DEBUG/TestFlight (a
    /// build only the field-test crew and Jake ever run), unset (and
    /// therefore OFF) in a plain App Store build — see
    /// `SettingsStore.shareDiagnosticsEnabled`'s own doc comment for the
    /// three-state read this follows, same shape as
    /// `backgroundConnectEnabled`.
    var shareDiagnosticsEnabled: Bool { get set }
}

/// Real backing store for `SettingsStoring`. `UserDefaults`-backed,
/// same six methods as `InMemorySettingsStore` (the M1 stand-in, landed
/// with the protocol) — so swapping one for the other changes nothing
/// above this seam. Every key is namespaced `firefly.settings.` so this
/// store can share a `UserDefaults` suite (or `.standard`) without
/// colliding with anything else that reads or writes it.
// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access goes through `lock`, never unguarded.
public final class SettingsStore: FireflyExtraSettingsStoring, @unchecked Sendable {
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

    /// A03 §3.3 — **defaults ON**, and this is the highest-value single
    /// line in that spec. A three-state read, not `UserDefaults.bool`:
    ///
    /// * nothing persisted (a fresh install, or an upgrade from a build
    ///   that never wrote this key) -> `true`;
    /// * an explicitly written value -> exactly that value.
    ///
    /// That second clause IS the migration: the setter below always
    /// writes explicitly, so anyone who has ever turned this OFF has
    /// `false` on disk and keeps it. Nobody's choice is overridden; only
    /// the absence of a choice changes meaning.
    ///
    /// Why the default flips: with `UserDefaults.bool`'s `false`,
    /// `AppGraph.handleScenePhaseChange(.background)` disconnects the
    /// radio and cancels the notification subscription the moment the
    /// screen locks — so out of the box the app goes deaf in a pocket,
    /// which is the one place this product is for. The battery half of
    /// the justification is A03 §4.2: in the steady connected state this
    /// costs a 0 % scan duty cycle, and the expensive case it used to
    /// imply (the unbounded rediscovery scan, audit 2.2.6) is bounded to
    /// 3.3 % by the §3.6 ladder that lands in this same slice.
    public var backgroundConnectEnabled: Bool {
        get { rawBool(.backgroundConnectEnabled, default: true) }
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

    /// A04 — same three-state read as `backgroundConnectEnabled` right
    /// above: nothing persisted -> `Self.defaultShareDiagnosticsEnabled()`;
    /// an explicit write -> exactly that value, forever, whichever way
    /// the build-config default later moves. "Share diagnostics" sends
    /// connection diagnostics — never message text, never exact
    /// location (`TelemetryAttributeAllowlist`) — when this phone has
    /// signal; OFF means record locally only, no upload, ever.
    public var shareDiagnosticsEnabled: Bool {
        get { rawBool(.shareDiagnosticsEnabled, default: Self.defaultShareDiagnosticsEnabled()) }
        set { setRawBool(newValue, .shareDiagnosticsEnabled) }
    }

    /// `true` in a DEBUG build, or a TestFlight build (an
    /// `.app-store-sandboxed` receipt — Apple's own signal for "this
    /// build came through TestFlight, not the App Store"); `false`
    /// otherwise. The field-test crew and Jake are the only people who
    /// ever run either of those; a plain App Store install defaults to
    /// OFF and asks nobody's phone to upload anything without an
    /// explicit tap.
    static func defaultShareDiagnosticsEnabled(bundle: Bundle = .main) -> Bool {
        #if DEBUG
        return true
        #else
        return isTestFlightReceipt(bundle: bundle)
        #endif
    }

    /// Apple's own documented TestFlight signal: the receipt URL's last
    /// path component is `"sandboxReceipt"` for a build installed via
    /// TestFlight, `"receipt"` for one installed from the App Store, and
    /// `nil` for a build with no receipt at all (a plain `xcodebuild`
    /// debug run, which `#if DEBUG` already covers above — this helper
    /// exists for the non-DEBUG branch only, but is unconditional and
    /// pure so `SettingsStoreTests` can pin it directly).
    static func isTestFlightReceipt(bundle: Bundle) -> Bool {
        bundle.appStoreReceiptURL?.lastPathComponent == "sandboxReceipt"
    }

    /// `default:` is what makes "never set" distinguishable from "set to
    /// false" — the same `object(forKey:) != nil` probe `double(_:)`
    /// above already uses for its own `nil`-when-unset contract, rather
    /// than a second, different way of asking the same question.
    private func rawBool(_ key: FireflyExtraSettingsKey, default fallback: Bool = false) -> Bool {
        lock.lock(); defer { lock.unlock() }
        let k = namespaced(key.rawValue)
        guard defaults.object(forKey: k) != nil else { return fallback }
        return defaults.bool(forKey: k)
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
