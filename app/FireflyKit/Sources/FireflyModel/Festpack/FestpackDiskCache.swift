//
//  FestpackDiskCache.swift — the raw-JSON-on-disk half of
//  `AlmanacFestpackProvider` (owner's instructions: "caches the raw
//  JSON on disk (Application Support)... loads the cache first at
//  launch").
//
//  Deliberately caches the RAW bytes, not a decoded `Festpack` — a
//  future app version's parser is the one thing allowed to read an
//  old cache differently; re-parsing on every load keeps that true
//  instead of freezing today's decode into the cache format.
//
//  PER-PACK KEYING ("app: automatic almanac refresh + festival
//  picker", owner ask #2): every call takes a `key` — the festival
//  namespace (`SettingsStoring.festivalNamespace()`, "<slug>-<year>")
//  — and reads/writes that pack's OWN pair of files. Switching the
//  Settings festival picker back to a previously-selected festival is
//  then "instant": its cache is still on disk under its own key, never
//  overwritten by whichever OTHER festival was selected in between.
//  `key` is sanitized to a filesystem-safe token so an almanac slug
//  cannot smuggle a path separator into a cache filename.
//
import Foundation

public struct FestpackCacheEntry: Sendable, Equatable {
    public let json: Data
    public let etag: String?
    /// `nil` when the metadata sidecar is missing or unreadable — the
    /// JSON is still perfectly usable, but WHEN it was written is then
    /// genuinely unknown. Hardening QA pass: this used to read
    /// `.distantPast`, a made-up timestamp that downstream age math
    /// cannot tell apart from a real one.
    public let savedAt: Date?
}

/// `@unchecked Sendable` justified the same way as `EventHub`
/// (`EventHub.swift`'s own comment) — every access to the files this
/// owns goes through `lock`.
public final class FestpackDiskCache: @unchecked Sendable {
    private let directory: URL
    private let lock = NSLock()

    /// `directory` is injectable so tests never touch the real
    /// `Application Support` directory — production call sites should
    /// leave it `nil` and get the real one.
    public init(directory: URL? = nil) {
        self.directory = directory ?? Self.defaultDirectory()
    }

    private static func defaultDirectory() -> URL {
        let base = (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true))
            ?? FileManager.default.temporaryDirectory
        return base.appendingPathComponent("Festpack", isDirectory: true)
    }

    /// Filesystem-safe form of a festival namespace key — letters,
    /// digits, `-`/`_` pass through; anything else (a path separator
    /// smuggled through a malformed almanac slug, say) becomes `_`,
    /// same defensive posture `AlmanacFestpackProvider.sourceURL()`
    /// already takes toward untrusted almanac-sourced strings.
    private static func sanitize(_ key: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let cleaned = String(key.unicodeScalars.map { allowed.contains($0) ? Character($0) : "_" })
        return cleaned.isEmpty ? "default" : cleaned
    }

    private func jsonURL(key: String) -> URL {
        directory.appendingPathComponent("festpack-cache-\(Self.sanitize(key)).json")
    }
    private func metaURL(key: String) -> URL {
        directory.appendingPathComponent("festpack-cache-\(Self.sanitize(key))-meta.json")
    }

    public func load(key: String) -> FestpackCacheEntry? {
        lock.lock(); defer { lock.unlock() }
        guard let json = try? Data(contentsOf: jsonURL(key: key)) else { return nil }
        let meta = (try? Data(contentsOf: metaURL(key: key))).flatMap { try? JSONDecoder().decode(CacheMeta.self, from: $0) }
        return FestpackCacheEntry(json: json, etag: meta?.etag, savedAt: meta?.savedAt)
    }

    public func save(json: Data, etag: String?, savedAt: Date, key: String) {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? json.write(to: jsonURL(key: key), options: .atomic)
        if let data = try? JSONEncoder().encode(CacheMeta(etag: etag, savedAt: savedAt)) {
            try? data.write(to: metaURL(key: key), options: .atomic)
        }
    }

    /// Rewrites only the metadata sidecar (`etag`/`savedAt`), leaving
    /// the cached JSON bytes untouched — what a 304 Not Modified
    /// response means: the network confirmed this pack is STILL
    /// current as of right now, with nothing new to write. A no-op if
    /// there is no cached JSON to own this metadata (should not happen
    /// — a 304 implies an `If-None-Match` was sent, which implies a
    /// cached etag existed — but defensive rather than fabricating a
    /// JSON file that was never fetched).
    public func touch(etag: String?, savedAt: Date, key: String) {
        lock.lock(); defer { lock.unlock() }
        guard FileManager.default.fileExists(atPath: jsonURL(key: key).path) else { return }
        if let data = try? JSONEncoder().encode(CacheMeta(etag: etag, savedAt: savedAt)) {
            try? data.write(to: metaURL(key: key), options: .atomic)
        }
    }

    /// Test-only teardown — production never deletes its own cache
    /// outside a fresh `refresh()` overwriting it.
    public func clear(key: String) {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.removeItem(at: jsonURL(key: key))
        try? FileManager.default.removeItem(at: metaURL(key: key))
    }

    private struct CacheMeta: Codable {
        let etag: String?
        let savedAt: Date
    }
}
