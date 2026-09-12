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
/// survives relaunch, same shape as `StarredArtistsStore` (that file's
/// own encode/decode doc comment: comma-joined, each token base64'd
/// first, because a set id embeds a raw "HH:MM" clock reading and — via
/// the artist slug's own separator — hyphens, not because it can
/// contain a comma the way an artist name legitimately can; base64 is
/// simply the one encoding this app already uses for this shape of
/// value, so a second ad hoc scheme is not introduced for no reason).
// `@unchecked Sendable` justified the same way as `EventHub` — every
// mutable access goes through `lock`.
public final class PicksStore: PicksStoring, @unchecked Sendable {
    private let store: any SettingsStoring
    private let lock = NSLock()

    public init(store: any SettingsStoring) {
        self.store = store
    }

    public func pickedSetIDs() -> Set<String> {
        lock.lock(); defer { lock.unlock() }
        return Self.decode(store.string(.pickedFestivalSetIDs))
    }

    public func setPicked(_ picked: Bool, setID: String) {
        lock.lock(); defer { lock.unlock() }
        var current = Self.decode(store.string(.pickedFestivalSetIDs))
        if picked { current.insert(setID) } else { current.remove(setID) }
        store.setString(Self.encode(current), .pickedFestivalSetIDs)
    }

    private static func encode(_ ids: Set<String>) -> String? {
        guard !ids.isEmpty else { return nil }
        return ids.sorted().map { Data($0.utf8).base64EncodedString() }.joined(separator: ",")
    }

    private static func decode(_ raw: String?) -> Set<String> {
        guard let raw, !raw.isEmpty else { return [] }
        return Set(raw.split(separator: ",").compactMap { token -> String? in
            guard let data = Data(base64Encoded: String(token)) else { return nil }
            return String(decoding: data, as: UTF8.self)
        })
    }
}

/// Test/demo stand-in — nothing persists past the process, same
/// "hermetic between runs" contract `InMemorySettingsStore` documents
/// for itself.
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
