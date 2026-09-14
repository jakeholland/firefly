//
//  CrewLink.swift — the `firefly://crew` deep link (`docs/specs/
//  A02-crew-join.md`, §1.8, AC5): what the Start screen's QR encodes,
//  what Share Link sends, and what `onOpenURL`/the Join scanner parses
//  back.
//
//  `firefly://crew?v=1&code=FIRE-4K9M7X&name=Camp%20Firefly` — parameter
//  order is FIXED (`v`, `code`, `name`) so the payload is byte-
//  reproducible across platforms and pinned by
//  `docs/specs/fixtures/A02-crew-codes.json`. `encode` therefore builds
//  the query string by hand rather than through `URLComponents`, whose
//  own query-item ordering/encoding is not guaranteed to match a fixture
//  byte for byte.
//
import Foundation

public enum CrewLinkError: Error, Equatable, Sendable {
    /// Not a `firefly://crew…` URL at all.
    case notACrewLink
    /// `v` was present but not `1` — "made by a newer version" (§1.8).
    case unsupportedVersion(String)
    /// No `code` parameter.
    case missingCode
    /// A `code` parameter that `CrewCode.parse` rejects.
    case invalidCode(CrewCodeError)
}

/// The deep link's decoded payload — a code, always; a human name,
/// optionally (§1.3: the human name is app-local display only, never
/// part of the key derivation).
public struct CrewLink: Sendable, Equatable {
    public static let currentVersion = 1
    /// AC5: "a `name` longer than 24 decoded characters (clamped, not
    /// rejected)".
    public static let maxNameLength = 24

    public let code: CrewCode
    public let name: String?

    public init(code: CrewCode, name: String?) {
        self.code = code
        self.name = name.map { String($0.prefix(CrewLink.maxNameLength)) }
    }

    /// Byte-exact against vector 1's `deep_link` field. Percent-encodes
    /// `name` as UTF-8 (space → `%20`, never `+` — this is a URL query
    /// component, not `application/x-www-form-urlencoded`).
    public static func encode(code: CrewCode, name: String?) -> String {
        var link = "firefly://crew?v=\(currentVersion)&code=\(code.canonical)"
        if let name, !name.isEmpty {
            let clamped = String(name.prefix(maxNameLength))
            let encoded = clamped.addingPercentEncoding(withAllowedCharacters: .crewLinkQueryValue) ?? clamped
            link += "&name=\(encoded)"
        }
        return link
    }

    /// The inverse of `encode`. Order-independent (a real URL a phone
    /// hands back from `onOpenURL` is not guaranteed to preserve the
    /// query order `encode` wrote), but every failure is a typed,
    /// specific error — never a partial parse (AC5).
    public static func parse(_ raw: String) throws -> CrewLink {
        guard let components = URLComponents(string: raw),
              (components.scheme ?? "").lowercased() == "firefly",
              (components.host ?? "").lowercased() == "crew" else {
            throw CrewLinkError.notACrewLink
        }
        let items = components.queryItems ?? []
        func value(_ name: String) -> String? { items.first { $0.name == name }?.value }

        let version = value("v") ?? ""
        guard version == String(currentVersion) else {
            throw CrewLinkError.unsupportedVersion(version)
        }
        guard let codeText = value("code"), !codeText.isEmpty else {
            throw CrewLinkError.missingCode
        }
        do {
            let code = try CrewCode.parse(codeText)
            return CrewLink(code: code, name: value("name"))
        } catch let error as CrewCodeError {
            throw CrewLinkError.invalidCode(error)
        }
    }
}

extension CharacterSet {
    /// Alphanumerics only, so anything else in a human-typed crew name —
    /// spaces included — is percent-encoded. Deliberately narrower than
    /// `.urlQueryAllowed` (which leaves space unencoded, wrong for a
    /// query VALUE) and than leaving `&`/`=` unescaped (which would
    /// corrupt the fixed `v`/`code`/`name` ordering `encode` relies on).
    static let crewLinkQueryValue = CharacterSet.alphanumerics
}

// MARK: - Scan payload classification (§3.1)

/// What a scanned/pasted string in the Join flow turns out to be — the
/// three shapes §3.1 enumerates, in the order it lists them. Pure
/// classification; `CrewJoinViewModel` decides what to DO with each
/// case.
public enum CrewScanPayload: Sendable, Equatable {
    /// 1. `firefly://crew?…`.
    case crewLink(CrewLink)
    /// 2. A bare code, any spelling §1.2 accepts, with no name.
    case bareCode(CrewCode)
    /// 3. A `meshtastic.org/e/#…` / `meshtastic://e/#…` channel link —
    /// handed to the EXISTING import flow (§3.1's own "That's a
    /// Meshtastic channel link, not a Firefly crew code" wording).
    case meshtasticChannelLink(String)
    /// Anything else — "That's not a Firefly crew code." (§3.1). The
    /// camera keeps running; nothing is dismissed.
    case unrecognized

    /// Tries each shape in §3.1's own order: a Firefly deep link first,
    /// then a bare code, then a Meshtastic channel URL, else
    /// `.unrecognized`.
    public static func classify(_ raw: String) -> CrewScanPayload {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let link = try? CrewLink.parse(trimmed) {
            return .crewLink(link)
        }
        if let code = try? CrewCode.parse(trimmed) {
            return .bareCode(code)
        }
        let lowered = trimmed.lowercased()
        if lowered.hasPrefix("https://meshtastic.org/e/") || lowered.hasPrefix("http://meshtastic.org/e/") ||
            lowered.hasPrefix("meshtastic://e/") {
            return .meshtasticChannelLink(trimmed)
        }
        return .unrecognized
    }
}
