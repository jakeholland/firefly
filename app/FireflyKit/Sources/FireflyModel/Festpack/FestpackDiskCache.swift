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
import Foundation

public struct FestpackCacheEntry: Sendable, Equatable {
    public let json: Data
    public let etag: String?
    public let savedAt: Date
}

/// `@unchecked Sendable` justified the same way as `EventHub`
/// (`EventHub.swift`'s own comment) — every access to the two files this
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

    private var jsonURL: URL { directory.appendingPathComponent("festpack-cache.json") }
    private var metaURL: URL { directory.appendingPathComponent("festpack-cache-meta.json") }

    public func load() -> FestpackCacheEntry? {
        lock.lock(); defer { lock.unlock() }
        guard let json = try? Data(contentsOf: jsonURL) else { return nil }
        let meta = (try? Data(contentsOf: metaURL)).flatMap { try? JSONDecoder().decode(CacheMeta.self, from: $0) }
        return FestpackCacheEntry(json: json, etag: meta?.etag, savedAt: meta?.savedAt ?? .distantPast)
    }

    public func save(json: Data, etag: String?, savedAt: Date) {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try? json.write(to: jsonURL, options: .atomic)
        if let data = try? JSONEncoder().encode(CacheMeta(etag: etag, savedAt: savedAt)) {
            try? data.write(to: metaURL, options: .atomic)
        }
    }

    /// Test-only teardown — production never deletes its own cache
    /// outside a fresh `refresh()` overwriting it.
    public func clear() {
        lock.lock(); defer { lock.unlock() }
        try? FileManager.default.removeItem(at: jsonURL)
        try? FileManager.default.removeItem(at: metaURL)
    }

    private struct CacheMeta: Codable {
        let etag: String?
        let savedAt: Date
    }
}
