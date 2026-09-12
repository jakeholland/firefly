//
//  PicksCodec.swift — settimes-compatible pick ids and share codes
//  (docs/specs/A01-companion-app.md, Lineup: "Picks share/import
//  compatible with the set-times website").
//
//  The set-times site (github.com/jakeholland/settimes,
//  src/lib/festival.ts + src/lib/picks.ts) already has a share-link
//  format for exactly this pack schema — a stable per-set id
//  ("stage-day-start-artist"), a short FNV-1a/base36 hash of that id
//  for the URL, and a `?picks=<code>.<code>...` query param. This file
//  is a BYTE-COMPATIBLE Swift port of that scheme, not a new one: a
//  picks link made on the phone must decode on settimes.kandiwooks.com
//  and vice versa, so `FestpackModelTests`' fixtures assert against the
//  SAME expected strings the TypeScript test suite does (computed
//  independently via `node`, not copy-pasted from a Swift run — see
//  that test file's own header).
//
//  What this can NOT losslessly reconstruct: settimes' `setId` is built
//  from the pack's own plain-calendar fields (`stage`, the set's
//  calendar `day`, its raw "HH:MM" `start`), but `fp_parse` (the C
//  core's festpack parser) folds `day`+`night` into ONE `day_doy`
//  (the NIGHT) plus a `start_min` that is >= 1440 for an after-midnight
//  set — see `fp_pack.c`'s `fp_parse_set_daytime` doc comment. That
//  fold is invertible when `startMinute` is known (`day` = the night's
//  date, +1 if `startMinute >= 1440`), which is what `setID(for:in:)`
//  below does. It is NOT invertible for a set with NO published start
//  time: `fp_set_t` only ever stores the NIGHT for such a set, never
//  whether its (irrelevant, since nothing is scheduled by it yet)
//  calendar day would have folded forward — the C core itself has
//  already discarded that distinction by the time this file ever sees
//  the set. `setID(for:in:)` documents the one assumption this forces
//  (day == night for an unknown-start set) rather than silently
//  guessing; the practical effect is a possible share-code MISMATCH
//  against settimes for an after-midnight set whose start time is ALSO
//  unpublished — a doubly-unlikely combination flagged here rather than
//  hidden.
//
import Foundation

public enum PicksCodec {
    /// `PICKS_PARAM` in `settimes/src/lib/picks.ts`.
    public static let picksParam = "picks"

    /// The result of decoding a `?picks=` value: every code that
    /// resolved to a set in THIS pack, plus how many did not (a stale
    /// share link, or one made against a different festival/night) —
    /// mirrors `decodePicks`'s `{ ids, dropped }` return in
    /// `settimes/src/lib/picks.ts` exactly, including its de-duplication
    /// rule (each DISTINCT code counted at most once, empty segments
    /// ignored entirely, never counted as dropped).
    public struct DecodedPicks: Sendable, Equatable {
        public let ids: [String]
        public let dropped: Int
        public init(ids: [String], dropped: Int) {
            self.ids = ids
            self.dropped = dropped
        }
    }

    // MARK: - Stable per-set id (settimes: `setId`)

    /// "stage-day-start-artist", exactly as settimes' `setId(s:
    /// ScheduledSet)` builds it from the pack's plain-calendar fields —
    /// see this file's header comment for the one case (unpublished
    /// start) this can only approximate.
    public static func setID(for set: FestpackScheduleSet, in pack: Festpack) -> String {
        let stage = set.stageID ?? ""
        let day = calendarDayString(for: set, in: pack)
        let start = set.startMinute.map(clockString(fromFoldedMinute:)) ?? "tba"
        return "\(stage)-\(day)-\(start)-\(slugifyArtist(set.artist))"
    }

    /// The calendar ISO date ("2026-09-18") `set` actually starts on —
    /// the night's own date, folded one day forward when the known
    /// start time is >= 1440 (after midnight). Falls back to the
    /// night's own date for an unknown-start set (see header comment).
    private static func calendarDayString(for set: FestpackScheduleSet, in pack: Festpack) -> String {
        let foldedForward = (set.startMinute ?? 0) >= 1440
        let dayOfYear = foldedForward ? set.nightDayOfYear + 1 : set.nightDayOfYear
        guard let date = pack.date(forDayOfYear: dayOfYear), let timeZone = pack.timeZone else { return "" }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let components = calendar.dateComponents([.year, .month, .day], from: date)
        guard let year = components.year, let month = components.month, let day = components.day else { return "" }
        return String(format: "%04d-%02d-%02d", year, month, day)
    }

    /// A folded minute (0..<1440 for a pre-midnight set, >= 1440 for an
    /// after-midnight one) back to the plain "HH:MM" clock reading on
    /// ITS OWN calendar day — settimes' raw `start` field.
    private static func clockString(fromFoldedMinute minute: Int) -> String {
        let wrapped = ((minute % 1440) + 1440) % 1440
        return String(format: "%02d:%02d", wrapped / 60, wrapped % 60)
    }

    /// settimes' `slugifyArtist`: lowercase, "&" -> "and", every run of
    /// non-`[a-z0-9]` collapsed to one "-", leading/trailing "-" trimmed.
    /// Ported character-by-character (not via `NSRegularExpression`) so
    /// there is no dependency on ICU's notion of "word character"
    /// agreeing with JavaScript's ASCII-only `[a-z0-9]` class.
    public static func slugifyArtist(_ artist: String) -> String {
        collapseNonAlphanumericRuns(artist.lowercased().replacingOccurrences(of: "&", with: "and"))
    }

    /// settimes' `shortCode`: pack.year-scoped 32-bit FNV-1a over the id
    /// string's UTF-16 code units, base36. Every string this is ever
    /// called on (`setID(for:in:)`'s output) is pure ASCII — stage ids,
    /// ISO dates, "HH:MM"/"tba" and a slug are all `[a-z0-9:-]` — so
    /// iterating UTF-8 bytes here is byte-identical to JS's
    /// `charCodeAt` (UTF-16 code units) over the same string.
    public static func shortCode(_ id: String) -> String {
        var hash: UInt32 = 0x811c_9dc5
        for byte in id.utf8 {
            hash ^= UInt32(byte)
            hash = hash &* 0x0100_0193
        }
        return String(hash, radix: 36)
    }

    // MARK: - Share link encode/decode (settimes: `encodePicks`/`decodePicks`)

    /// `.`-joined short codes for every id in `pickIDs` that resolves to
    /// an ACTUAL set in `pack` — an id the pack no longer has (a set
    /// dropped by a refresh) is silently skipped, exactly like
    /// settimes' `encodePicks` skips an id absent from `model.byId`.
    public static func encodePicks<S: Sequence>(_ pickIDs: S, in pack: Festpack) -> String where S.Element == String {
        let validIDs = Set(pack.sets.map { setID(for: $0, in: pack) })
        return pickIDs.filter(validIDs.contains).map(shortCode).joined(separator: ".")
    }

    /// The inverse of `encodePicks`: every code in `encoded` resolved
    /// back to this pack's own set id, plus a count of codes that did
    /// not resolve. `nil`/empty input decodes to `(ids: [], dropped: 0)`
    /// — an ABSENT link is not the same claim as "every code in this
    /// link is stale," which is what `dropped > 0` means.
    public static func decodePicks(_ encoded: String?, in pack: Festpack) -> DecodedPicks {
        guard let encoded, !encoded.isEmpty else { return DecodedPicks(ids: [], dropped: 0) }
        var idByCode: [String: String] = [:]
        for set in pack.sets {
            let id = setID(for: set, in: pack)
            idByCode[shortCode(id)] = id
        }
        var ids: [String] = []
        var dropped = 0
        var seen = Set<Substring>()
        for code in encoded.split(separator: ".", omittingEmptySubsequences: false) {
            guard !code.isEmpty, !seen.contains(code) else { continue }
            seen.insert(code)
            if let id = idByCode[String(code)] {
                ids.append(id)
            } else {
                dropped += 1
            }
        }
        return DecodedPicks(ids: ids, dropped: dropped)
    }

    /// Pulls a `picks=` value out of whatever a user pastes into
    /// "Import picks" — a full share URL, a bare query fragment
    /// ("?picks=..."), or just the dot-joined code list on its own.
    /// Byte-for-byte port of settimes' `parsePicksInput`, including its
    /// ordering: a string that parses as an absolute URL (scheme +
    /// host) is judged ONLY on that URL's own `picks` query item, even
    /// if it has none — it never falls through to the bare-code-list
    /// match below (that fallthrough is for genuinely non-URL input).
    /// `Foundation.URL(string:)` is far more lenient than JavaScript's
    /// `new URL(...)` (it happily parses a bare relative reference like
    /// "abc123.def456" that JS's constructor would throw on), so
    /// `scheme != nil && host != nil` stands in for "this is the kind
    /// of string JS's `new URL` would NOT have thrown on."
    public static func parsePicksInput(_ raw: String) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }

        if let url = URL(string: trimmed), url.scheme != nil, url.host != nil {
            let value = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                .queryItems?.first { $0.name == picksParam }?.value
            return (value?.isEmpty == false) ? value : nil
        }

        if let range = trimmed.range(of: "\(picksParam)=") {
            let after = trimmed[range.upperBound...]
            let upToAmpersand = after.split(separator: "&", maxSplits: 1, omittingEmptySubsequences: false).first ?? ""
            return upToAmpersand.isEmpty ? nil : String(upToAmpersand)
        }

        return isBareCodeList(trimmed) ? trimmed : nil
    }

    /// `/^[a-z0-9]+(\.[a-z0-9]+)*$/i` — one or more dot-separated
    /// alphanumeric codes, case-insensitively, and nothing else.
    private static func isBareCodeList(_ s: String) -> Bool {
        let segments = s.split(separator: ".", omittingEmptySubsequences: false)
        guard !segments.isEmpty else { return false }
        return segments.allSatisfy { segment in
            !segment.isEmpty && segment.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) && $0.isASCII }
        }
    }

    // MARK: - Share URL (settimes: `shareUrl`)

    private static let weekdayAbbreviations = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]

    /// The URL segment settimes uses for one of the pack's nights —
    /// its own `Night.slug`, in the common case (no repeated weekday
    /// across the run) a plain lowercase three-letter day name. A
    /// multi-week or >7-night pack that repeats a weekday would need
    /// settimes' ISO-date fallback slug instead; Firefly's own festival
    /// packs never run that long, so that fallback is not implemented
    /// here (an interpretation flagged in the PR, not silently assumed
    /// to never matter elsewhere).
    public static func weekdaySlug(for dayOfYear: Int, in pack: Festpack) -> String {
        guard let date = pack.date(forDayOfYear: dayOfYear), let timeZone = pack.timeZone else {
            return "day\(dayOfYear)"
        }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let weekday = calendar.component(.weekday, from: date) // 1 = Sunday ... 7 = Saturday
        return weekdayAbbreviations[weekday - 1]
    }

    /// settimes' own URL-safe slug for a festival name ("Lost Lands" ->
    /// "lost-lands") — the same collapse-runs-to-one-hyphen rule as
    /// `slugifyArtist`, just without the "&" -> "and" pre-pass (a
    /// festival name is not expected to need it, and settimes' own
    /// `festival.slug` is authored by hand in the pack rather than
    /// derived, so this is this app's own best-effort stand-in for a
    /// field the schema does not actually carry to the phone).
    public static func festivalSlug(_ name: String) -> String {
        collapseNonAlphanumericRuns(name.lowercased())
    }

    /// `https://settimes.kandiwooks.com/<festival-slug>/<year>/<day>?picks=<encoded>`
    /// — the Lineup screen's "Share picks" URL. `nil` only if `night`
    /// cannot be resolved to a date at all (a malformed pack). No
    /// `picks` query item is added when `pickIDs` encodes to nothing,
    /// so an empty-picks share link is a clean night URL, not
    /// `?picks=`.
    public static func shareURL(for night: Int, pickIDs: some Sequence<String>, in pack: Festpack,
                                 host: String = "https://settimes.kandiwooks.com") -> URL? {
        let slug = festivalSlug(pack.name)
        let day = weekdaySlug(for: night, in: pack)
        guard var components = URLComponents(string: "\(host)/\(slug)/\(pack.year)/\(day)") else { return nil }
        let encoded = encodePicks(pickIDs, in: pack)
        if !encoded.isEmpty {
            components.queryItems = [URLQueryItem(name: picksParam, value: encoded)]
        }
        return components.url
    }

    private static func collapseNonAlphanumericRuns(_ s: String) -> String {
        var result = ""
        var lastWasHyphen = false
        for scalar in s.unicodeScalars {
            let isAlphanumericASCII = (scalar.value >= 97 && scalar.value <= 122) || (scalar.value >= 48 && scalar.value <= 57)
            if isAlphanumericASCII {
                result.unicodeScalars.append(scalar)
                lastWasHyphen = false
            } else if !lastWasHyphen {
                result.append("-")
                lastWasHyphen = true
            }
        }
        while result.hasPrefix("-") { result.removeFirst() }
        while result.hasSuffix("-") { result.removeLast() }
        return result
    }
}
