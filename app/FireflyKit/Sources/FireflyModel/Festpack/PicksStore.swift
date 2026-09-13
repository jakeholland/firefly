//
//  PicksStore.swift — the Lineup screen's persisted "My picks" list
//  (docs/specs/A01-companion-app.md, Lineup: "Picks persistence via the
//  existing persistence layer... keyed by the festpack set id so they
//  survive festpack refreshes").
//
//  Superseded `StarredArtistsStore`: that store keyed a pick off ARTIST
//  NAME, which cannot tell apart two sets by the same artist (a back-
//  to-back set, or the same headliner across two nights). Keying off
//  `PicksCodec.setID(for:in:)` — the same stable "stage-day-start-
//  artist" id settimes.kandiwooks.com uses for its own share links
//  (`PicksCodec.swift`'s header comment) — fixes that AND is exactly
//  the id a share/import round-trip already needs, so there is one id
//  space for both jobs rather than two that could disagree. It is
//  still NOT `FestpackScheduleSet.id` (that struct's own doc comment:
//  a pack-relative array index, stable only within one loaded pack) —
//  it is a value derived from the set's own content, so it survives a
//  festpack re-fetch that reorders or reformats the same schedule.
//
//  MIGRATION (starred -> picked): an existing `starredFestivalArtists`
//  value is DELIBERATELY NOT migrated into `pickedFestivalSetIDs`. The
//  two key spaces answer different questions — "I want to catch this
//  artist (whenever they play)" vs "I am going to THIS set" — so an
//  automatic conversion would have to invent the missing half.
//
//  PER-FESTIVAL NAMESPACING ("app: automatic almanac refresh + festival
//  picker", owner ask #2): a set id is stable only WITHIN one festpack
//  schema (see `PicksCodec`'s own header) — it says nothing about WHICH
//  festival that pack belongs to, so two different festivals could
//  coin the same id (however unlikely) and, more importantly, a pick
//  made while Lost Lands 2026 was selected must never silently show up
//  as a pick under some OTHER festival the Settings picker later
//  switches to. Every persisted token therefore carries an explicit
//  namespace prefix — `"<slug>-<year>|<base64(setID)>"` — read/written
//  through the SAME `SettingsKey.pickedFestivalSetIDs` blob rather than
//  a new per-festival settings key, so the one existing key still
//  holds every pick ever made, across every festival, and `pickedSetIDs
//  ()`/`setPicked(_:setID:)` simply filter/target the CURRENT namespace
//  (`namespace()`, sourced from `SettingsStoring.festivalNamespace()`).
//
//  MIGRATION (un-namespaced -> legacy-namespaced): every pick persisted
//  before this feature shipped is a plain base64 token with no `"|"`
//  separator at all — the only festival this app has ever pointed at
//  is Lost Lands 2026, so `migrateAndDecode(store:)` reassigns any such
//  legacy token to `legacyNamespace` ("lost-lands-2026", the same
//  string `SettingsStoring.festivalNamespace()` resolves to when no
//  festival has ever been explicitly picked) and PERSISTS the migrated
//  form back on the very next read — a real, one-time migration, not
//  merely an interpretive shim re-run on every call.
//
import Foundation

public protocol PicksStoring: AnyObject, Sendable {
    func pickedSetIDs() -> Set<String>
    func setPicked(_ picked: Bool, setID: String)
}

extension PicksStoring {
    public func isPicked(_ setID: String) -> Bool { pickedSetIDs().contains(setID) }
    public func toggle(_ setID: String) { setPicked(!isPicked(setID), setID: setID) }
}

/// `SettingsStoring`-backed (`SettingsKey.pickedFestivalSetIDs`) —
/// survives relaunch. Encoding per-token: `"<namespace>|<base64(setID)
/// >"` for a namespaced pick, or a bare `base64(setID)` for a LEGACY
/// (pre-namespacing) pick — see this file's own header for the
/// migration rule and why base64 was already this app's encoding for
/// this shape of value (a set id embeds a raw "HH:MM" clock reading and
/// hyphens; base64 sidesteps that without a second ad hoc escaping
/// scheme).
// `@unchecked Sendable` justified the same way as `EventHub` — every
// mutable access goes through `lock`.
public final class PicksStore: PicksStoring, @unchecked Sendable {
    /// The only real festival this app pointed at before per-festival
    /// namespacing existed — every legacy (un-namespaced) pick migrates
    /// here, and it is also `SettingsStoring.festivalNamespace()`'s own
    /// default when no festival has ever been explicitly selected.
    public static let legacyNamespace = "lost-lands-2026"

    private let store: any SettingsStoring
    private let lock = NSLock()
    /// Resolves to the CURRENT festival's namespace on every call — not
    /// captured once at `init`, the same "re-read every time" discipline
    /// `AlmanacFestpackProvider.sourceURL()` already follows, since the
    /// Settings festival picker can change this at any point in the
    /// process's lifetime.
    private let namespace: @Sendable () -> String

    public init(store: any SettingsStoring, namespace: @escaping @Sendable () -> String = { PicksStore.legacyNamespace }) {
        self.store = store
        self.namespace = namespace
    }

    public func pickedSetIDs() -> Set<String> {
        lock.lock(); defer { lock.unlock() }
        return Self.migrateAndDecode(store: store)[namespace()] ?? []
    }

    public func setPicked(_ picked: Bool, setID: String) {
        lock.lock(); defer { lock.unlock() }
        var byNamespace = Self.migrateAndDecode(store: store)
        var current = byNamespace[namespace()] ?? []
        if picked { current.insert(setID) } else { current.remove(setID) }
        byNamespace[namespace()] = current
        store.setString(Self.encode(byNamespace), .pickedFestivalSetIDs)
    }

    /// Reads the raw stored blob, migrating any legacy (un-namespaced)
    /// tokens into `legacyNamespace` and persisting the migrated form
    /// back — safe to call on every read, a no-op once nothing legacy
    /// remains (the common case: either there was never a legacy value,
    /// or a previous call already migrated it).
    private static func migrateAndDecode(store: any SettingsStoring) -> [String: Set<String>] {
        let (decoded, hadLegacy) = decode(store.string(.pickedFestivalSetIDs))
        if hadLegacy {
            store.setString(encode(decoded), .pickedFestivalSetIDs)
        }
        return decoded
    }

    private static func encode(_ byNamespace: [String: Set<String>]) -> String? {
        let tokens = byNamespace
            .flatMap { namespace, ids in ids.map { "\(namespace)|\(Data($0.utf8).base64EncodedString())" } }
            .sorted()
        return tokens.isEmpty ? nil : tokens.joined(separator: ",")
    }

    /// Returns every namespace's picks, PLUS whether any LEGACY
    /// (un-namespaced) token was seen — the caller uses that to decide
    /// whether to persist the migrated form.
    private static func decode(_ raw: String?) -> (byNamespace: [String: Set<String>], hadLegacy: Bool) {
        guard let raw, !raw.isEmpty else { return ([:], false) }
        var byNamespace: [String: Set<String>] = [:]
        var hadLegacy = false
        for rawToken in raw.split(separator: ",") {
            let token = String(rawToken)
            if let separatorIndex = token.firstIndex(of: "|") {
                let namespace = String(token[token.startIndex..<separatorIndex])
                let encoded = String(token[token.index(after: separatorIndex)...])
                guard let data = Data(base64Encoded: encoded) else { continue }
                byNamespace[namespace, default: []].insert(String(decoding: data, as: UTF8.self))
            } else {
                // Legacy, pre-namespacing token — no `"|"` at all.
                guard let data = Data(base64Encoded: token) else { continue }
                byNamespace[legacyNamespace, default: []].insert(String(decoding: data, as: UTF8.self))
                hadLegacy = true
            }
        }
        return (byNamespace, hadLegacy)
    }
}

/// Test/demo stand-in — nothing persists past the process, same
/// "hermetic between runs" contract `InMemorySettingsStore` documents
/// for itself. Deliberately NOT namespaced — a test that needs
/// namespacing behaviour exercises the real `PicksStore` against an
/// `InMemorySettingsStore`, same as `PicksStoreTests` does; this type
/// stays the simple single-bucket double every OTHER test already
/// expects.
public final class InMemoryPicksStore: PicksStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var ids: Set<String>

    public init(_ initial: Set<String> = []) {
        ids = initial
    }

    public func pickedSetIDs() -> Set<String> {
        lock.lock(); defer { lock.unlock() }
        return ids
    }

    public func setPicked(_ picked: Bool, setID: String) {
        lock.lock(); defer { lock.unlock() }
        if picked { ids.insert(setID) } else { ids.remove(setID) }
    }
}
