//
//  StarredArtistsStore.swift — the Lineup screen's persisted star list.
//
//  Starring keys off ARTIST NAME, not a pack-relative set index or a
//  (stage, day, time) tuple: those all can change (or vanish entirely)
//  across a pack re-fetch, but "I want to catch this artist" survives
//  one — see `Festpack.swift`'s `FestpackScheduleSet.id` doc comment for
//  why that index is deliberately NOT the persistence key.
//
import Foundation

public protocol StarredArtistsStoring: AnyObject, Sendable {
    func starredArtists() -> Set<String>
    func setStarred(_ starred: Bool, artist: String)
}

extension StarredArtistsStoring {
    public func isStarred(_ artist: String) -> Bool { starredArtists().contains(artist) }
    public func toggle(_ artist: String) { setStarred(!isStarred(artist), artist: artist) }
}

/// `SettingsStoring`-backed (`SettingsKey.starredFestivalArtists`) —
/// survives relaunch, shared with everything else that reads through
/// the same store (Settings, `SettingsStore`/`InMemorySettingsStore`).
// `@unchecked Sendable` justified the same way as `EventHub` (that
// file's own comment) — every mutable access goes through `lock`.
public final class StarredArtistsStore: StarredArtistsStoring, @unchecked Sendable {
    private let store: any SettingsStoring
    private let lock = NSLock()

    public init(store: any SettingsStoring) {
        self.store = store
    }

    public func starredArtists() -> Set<String> {
        lock.lock(); defer { lock.unlock() }
        return Self.decode(store.string(.starredFestivalArtists))
    }

    public func setStarred(_ starred: Bool, artist: String) {
        lock.lock(); defer { lock.unlock() }
        var current = Self.decode(store.string(.starredFestivalArtists))
        if starred { current.insert(artist) } else { current.remove(artist) }
        store.setString(Self.encode(current), .starredFestivalArtists)
    }

    /// Comma-joined, each artist name base64-encoded first — same
    /// "typed persistence, not a raw pass-through" spirit as
    /// `SettingsKey.bondedPeripheralIDs` (`AppDependencies.live()`'s own
    /// `parsePeripheralIDs`), except base64 here (rather than a bare
    /// comma-join) because an ARTIST NAME, unlike a UUID, can itself
    /// legally contain a comma.
    private static func encode(_ artists: Set<String>) -> String? {
        guard !artists.isEmpty else { return nil }
        return artists.sorted().map { Data($0.utf8).base64EncodedString() }.joined(separator: ",")
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
public final class InMemoryStarredArtistsStore: StarredArtistsStoring, @unchecked Sendable {
    private let lock = NSLock()
    private var artists: Set<String>

    public init(_ initial: Set<String> = []) {
        artists = initial
    }

    public func starredArtists() -> Set<String> {
        lock.lock(); defer { lock.unlock() }
        return artists
    }

    public func setStarred(_ starred: Bool, artist: String) {
        lock.lock(); defer { lock.unlock() }
        if starred { artists.insert(artist) } else { artists.remove(artist) }
    }
}
