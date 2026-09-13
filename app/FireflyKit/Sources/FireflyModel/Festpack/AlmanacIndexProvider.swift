//
//  AlmanacIndexProvider.swift — the Settings festival picker's data
//  source (owner ask #2, 2026-09-13: "pick the festival from the
//  app"). Fetches fest-almanac's own index of every pack it publishes:
//
//    https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/index.json
//
//  Format (a CONCURRENT fest-almanac PR adds this; this file is coded
//  against the agreed shape ahead of that PR landing):
//
//    {"schema":"fest-almanac-index/1","generated":<ISO8601>,
//     "packs":[{"slug","year","name","start","end","timezone","path",
//               "updated","sha256"}, ...]}
//
//  Until that PR lands (and forever after, for anyone offline), the
//  picker must never show an empty list — `fetchIndex()` falls back to
//  a bundled index (`firmware/assets/field/fest-almanac-index.json`)
//  containing only Lost Lands 2026, the one festival this app can
//  reach without the network at all.
//
import Foundation

/// One festival the almanac publishes a pack for.
public struct AlmanacIndexPack: Sendable, Equatable, Identifiable {
    /// "<slug>-<year>" — the exact string `SettingsStoring
    /// .festivalNamespace()` produces once this pack is selected, so a
    /// picker row's `id` and the namespace it will switch to are
    /// provably the same value rather than two strings that have to be
    /// kept in sync by convention.
    public var id: String { "\(slug)-\(year)" }
    public let slug: String
    public let year: Int
    public let name: String
    /// Parsed from the index's ISO 8601 `start`/`end` — `nil` only if
    /// the almanac published a string this app's parser cannot read,
    /// which `decode(_:)` treats as a malformed entry (skipped
    /// entirely, never a pack with an invented date).
    public let start: Date
    public let end: Date
    /// The index's own free-text timezone label, if it published one —
    /// display-only, never parsed into a `TimeZone` here (the festpack
    /// itself, once fetched, is the authority on UTC offset — see
    /// `Festpack.timeZone`'s own doc comment on why this schema only
    /// ever carries a fixed offset).
    public let timezone: String?
    /// Relative to the almanac repo's root, e.g.
    /// "packs/lost-lands/2026/festpack.json" — `FestivalPickerViewModel
    /// .select(_:)` turns this into the full raw.githubusercontent.com
    /// URL, never using it as one on its own.
    public let path: String
    public let updated: String?
    /// Hex SHA-256 of the pack file this index entry points at, if the
    /// almanac published one — `AlmanacFestpackProvider` verifies a
    /// freshly fetched pack against this when both are available.
    public let sha256: String?

    public init(slug: String, year: Int, name: String, start: Date, end: Date,
                timezone: String?, path: String, updated: String?, sha256: String?) {
        self.slug = slug
        self.year = year
        self.name = name
        self.start = start
        self.end = end
        self.timezone = timezone
        self.path = path
        self.updated = updated
        self.sha256 = sha256
    }
}

public struct AlmanacIndex: Sendable, Equatable {
    public let generatedAt: Date?
    public let packs: [AlmanacIndexPack]

    public init(generatedAt: Date?, packs: [AlmanacIndexPack]) {
        self.generatedAt = generatedAt
        self.packs = packs
    }

    public static let empty = AlmanacIndex(generatedAt: nil, packs: [])
}

public protocol AlmanacIndexProviding: Sendable {
    /// Never throws, never returns an index with zero packs while a
    /// bundled fallback exists — see this file's own header for the
    /// fallback order. A malformed top-level document (not valid JSON,
    /// or missing a `"packs"` array) is treated as a total failure and
    /// falls through the same way a network error does.
    func fetchIndex() async -> AlmanacIndex
}

/// Demo mode's festival index — mirrors `DemoFestpackProvider`'s own
/// header exactly: "deliberately never touches the network... demo
/// mode must render the SAME scripted world on every run, never
/// something that quietly depends on" whatever fest-almanac happens to
/// publish live that day. Without this, the Settings festival picker
/// under `-FireflyDemo` would show a real, live-fetched list that can
/// change out from under a screenshot script or a demo walkthrough —
/// exactly the non-determinism the demo stack exists to avoid.
public struct DemoAlmanacIndexProvider: AlmanacIndexProviding {
    private let bundleLoader: any FestpackBundleLoading

    public init(bundleLoader: any FestpackBundleLoading = MainBundleFestpackLoader()) {
        self.bundleLoader = bundleLoader
    }

    public func fetchIndex() async -> AlmanacIndex {
        guard let data = bundleLoader.festpackData(forResource: "fest-almanac-index", extension: "json"),
              let index = AlmanacIndexProvider.decode(data) else { return .empty }
        return index
    }
}

public actor AlmanacIndexProvider: AlmanacIndexProviding {
    public static let defaultURL = URL(
        string: "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/packs/index.json")!

    private let fetcher: any FestpackHTTPFetching
    private let bundleLoader: any FestpackBundleLoading

    public init(fetcher: any FestpackHTTPFetching = URLSessionFestpackFetcher(),
                bundleLoader: any FestpackBundleLoading = MainBundleFestpackLoader()) {
        self.fetcher = fetcher
        self.bundleLoader = bundleLoader
    }

    public func fetchIndex() async -> AlmanacIndex {
        if let live = await fetchLiveIndex(), !live.packs.isEmpty {
            return live
        }
        return bundledIndex() ?? .empty
    }

    private func fetchLiveIndex() async -> AlmanacIndex? {
        do {
            guard let response = try await fetcher.fetch(Self.defaultURL, ifNoneMatch: nil) else { return nil }
            return Self.decode(response.body)
        } catch {
            return nil
        }
    }

    private func bundledIndex() -> AlmanacIndex? {
        guard let data = bundleLoader.festpackData(forResource: "fest-almanac-index", extension: "json") else { return nil }
        return Self.decode(data)
    }

    /// `JSONSerialization`, not `Codable`, on purpose: a `Codable`
    /// struct decode fails the WHOLE array the moment one entry has a
    /// wrong-typed field, which is the opposite of "malformed entries
    /// skipped" — this walks the raw object graph so exactly one bad
    /// entry is dropped rather than the entire index.
    static func decode(_ data: Data) -> AlmanacIndex? {
        guard let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return nil }
        // `[Any]`, not `[[String: Any]]`: casting straight to the latter
        // fails the WHOLE array the instant one element is not a
        // dictionary (a stray string or `null` in "packs") — exactly
        // the "one malformed entry skipped" contract this decode must
        // NOT violate. `compactMap` here drops non-dictionary elements;
        // `decodePack` (below) drops dictionaries missing/mistyping a
        // required field.
        guard let rawPacksAny = json["packs"] as? [Any] else { return nil }
        let generated = (json["generated"] as? String).flatMap { ISO8601DateFormatter().date(from: $0) }
        let packs = rawPacksAny.compactMap { $0 as? [String: Any] }.compactMap(decodePack)
        return AlmanacIndex(generatedAt: generated, packs: packs)
    }

    private static func decodePack(_ raw: [String: Any]) -> AlmanacIndexPack? {
        guard let slug = raw["slug"] as? String, !slug.isEmpty,
              let year = raw["year"] as? Int,
              let name = raw["name"] as? String, !name.isEmpty,
              let path = raw["path"] as? String, !path.isEmpty,
              let startString = raw["start"] as? String,
              let endString = raw["end"] as? String else { return nil }
        let formatter = ISO8601DateFormatter()
        // fest-almanac may publish a bare date ("2026-09-18") rather
        // than a full timestamp — accept both rather than dropping
        // every entry the moment the almanac's own format is slightly
        // less precise than a full ISO 8601 instant.
        let dateOnlyFormatter = ISO8601DateFormatter()
        dateOnlyFormatter.formatOptions = [.withFullDate]
        guard let start = formatter.date(from: startString) ?? dateOnlyFormatter.date(from: startString),
              let end = formatter.date(from: endString) ?? dateOnlyFormatter.date(from: endString) else { return nil }
        return AlmanacIndexPack(
            slug: slug, year: year, name: name, start: start, end: end,
            timezone: raw["timezone"] as? String, path: path,
            updated: raw["updated"] as? String, sha256: raw["sha256"] as? String)
    }
}
