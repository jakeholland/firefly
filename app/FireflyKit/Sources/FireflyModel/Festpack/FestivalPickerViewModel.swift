//
//  FestivalPickerViewModel.swift — Settings' "Festival data" picker
//  (owner ask #2, 2026-09-13: "pick the festival from the app").
//
//  Lists every festival `AlmanacIndexProvider` knows about — live or
//  the bundled fallback, see that type's own header — sorted by start
//  date, with the currently-selected one and the one happening RIGHT
//  NOW each marked. Selecting a row writes the SAME three settings
//  `AlmanacFestpackProvider`/`PicksStore` already read
//  (`festpackSourceURLOverride`, `festivalSelectedSlug`/`Year`/
//  `SHA256` — `SettingsStoring.festivalNamespace()`'s own doc comment)
//  and then triggers a real refresh through the shared `LineupViewModel`
//  — never a second, parallel refresh path that could show a different
//  answer than the Lineup tab.
//
import Foundation
import Observation

@MainActor
@Observable
public final class FestivalPickerViewModel {
    public struct Row: Sendable, Identifiable, Equatable {
        public let id: String
        public let name: String
        public let year: Int
        public let start: Date
        /// The festival whose `[start, end]` contains "now" — at most
        /// one row is ever marked current.
        public let isCurrent: Bool
        public let isSelected: Bool
    }

    private let indexProvider: any AlmanacIndexProviding
    private let settings: any SettingsStoring
    private let lineup: LineupViewModel
    private let now: @Sendable () -> Date
    /// The full index entries behind `rows`, keyed by `AlmanacIndexPack
    /// .id` — `select(_:)` needs the entry's `path`/`sha256`, which
    /// `Row` deliberately does not carry (a view has no business
    /// building a raw.githubusercontent.com URL itself).
    private var packsByID: [String: AlmanacIndexPack] = [:]

    public private(set) var rows: [Row] = []
    public private(set) var isLoading = false
    /// Set only when a LIVE fetch fails AND the bundled fallback also
    /// has nothing (should not happen in practice — the bundled index
    /// always carries Lost Lands 2026 — but honest rather than silently
    /// showing an unexplained empty list).
    public private(set) var loadError: String?

    public init(indexProvider: any AlmanacIndexProviding, settings: any SettingsStoring,
                lineup: LineupViewModel, now: @escaping @Sendable () -> Date = { Date() }) {
        self.indexProvider = indexProvider
        self.settings = settings
        self.lineup = lineup
        self.now = now
    }

    /// The row currently selected — `SettingsStoring.festivalNamespace()`
    /// itself, so "which row is checked" and "which festival the rest
    /// of the app is actually pointed at" can never disagree.
    public var selectedID: String { settings.festivalNamespace() }

    public func load() async {
        isLoading = true
        let index = await indexProvider.fetchIndex()
        let nowDate = now()
        let selected = selectedID
        packsByID = Dictionary(uniqueKeysWithValues: index.packs.map { ($0.id, $0) })
        rows = Self.ordered(index.packs, now: nowDate)
            .map { pack in
                Row(id: pack.id, name: pack.name, year: pack.year, start: pack.start,
                    isCurrent: pack.start <= nowDate && nowDate <= pack.end,
                    isSelected: pack.id == selected)
            }
        loadError = index.packs.isEmpty ? "No festivals available — check your connection." : nil
        isLoading = false
    }

    /// Review fix — the owner ask is "pick the festival you are going
    /// to", so the list leads with the ones that are still ahead:
    /// anything not yet over (current first, then upcoming, soonest
    /// first), and only then the ones already finished, most recent
    /// first. A plain ascending sort by `start` put Bass Canyon
    /// (August) at the top of a list opened at Lost Lands in
    /// September, and pushed every festival the user could actually
    /// still attend below the fold. Past festivals are still SHOWN —
    /// last year's picks and lineup are real data, not something to
    /// hide — just not first. Pure and `static` so the ordering is
    /// testable with no index provider, settings or lineup.
    static func ordered(_ packs: [AlmanacIndexPack], now: Date) -> [AlmanacIndexPack] {
        let upcoming = packs.filter { $0.end >= now }.sorted { $0.start < $1.start }
        let past = packs.filter { $0.end < now }.sorted { $0.start > $1.start }
        return upcoming + past
    }

    /// Writes the selection to settings (URL, slug, year, checksum —
    /// ONE call so the four can never land only partially) and triggers
    /// a real refresh via the shared `LineupViewModel`, then reloads
    /// `rows` so the new selection's checkmark shows immediately. A
    /// `packID` this picker does not currently know about (a stale row
    /// from a previous `load()`) is a no-op rather than writing a
    /// half-formed selection.
    public func select(_ packID: String) async {
        guard let pack = packsByID[packID] else { return }
        let urlString = "https://raw.githubusercontent.com/jakeholland/fest-almanac/main/\(pack.path)"
        guard case .use(let normalized) = FestpackSourceURLValidator.validate(urlString) else { return }
        settings.setString(pack.slug, .festivalSelectedSlug)
        settings.setString(String(pack.year), .festivalSelectedYear)
        settings.setString(normalized, .festpackSourceURLOverride)
        settings.setString(pack.sha256, .festivalSelectedSHA256)
        await lineup.refresh()
        await load()
    }
}
