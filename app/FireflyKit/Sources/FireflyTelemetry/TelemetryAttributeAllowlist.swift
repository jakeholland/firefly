//
//  TelemetryAttributeAllowlist.swift — A04: the second line of defense
//  behind "never record raw coordinates, the crew code, or message
//  text" — enforced at the TYPE level everywhere it can be (`gps.fix`
//  only ever carries a bucket string; nothing in this package has a
//  `Double` latitude/longitude to pass), and enforced HERE, structurally,
//  for the case a type-level guarantee cannot reach: a future call site
//  that reaches for a plausible-looking key like "lat"/"lon"/"text"/
//  "message"/"code" by hand.
//
//  `TelemetryRecorder.record(_:)` calls `TelemetryAttributeAllowlist
//  .strip(_:)` on every event's attributes before it ever reaches disk
//  or a sink — so a call site that violates the allowlist loses the
//  offending key rather than the whole event, and
//  `TelemetryAttributeAllowlistTests`/`NoCoordinatesNoTextGuardTests`
//  pin exactly which keys are forbidden and why.
//
import Foundation

public enum TelemetryAttributeAllowlist {
    /// Keys that must NEVER appear on any event, however a future call
    /// site spells them — deliberately over-inclusive (common
    /// abbreviations and both cases) rather than an exact match on one
    /// spelling.
    public static let forbiddenKeys: Set<String> = [
        "lat", "latitude", "lon", "lng", "longitude", "coordinate", "coordinates",
        "position", "gps_lat", "gps_lon", "gps_latitude", "gps_longitude",
        "text", "body", "message", "message_text", "content",
        "code", "crew_code", "join_code", "invite_code",
        "node_id", "node_num", "from", "to", // raw node identifiers — id_hash only
    ]

    /// Removes every forbidden key from `attributes`, case-insensitively
    /// on the KEY only (values are never inspected — an event that wants
    /// to say `outcome: "joined"` is fine; an event that wants to say
    /// `code: "FIRE-4K9M7X"` never reaches disk with that key at all,
    /// whatever it typed the key as).
    public static func strip(_ attributes: [String: TelemetryValue]) -> [String: TelemetryValue] {
        attributes.filter { !forbiddenKeys.contains($0.key.lowercased()) }
    }

    /// `true` iff `attributes` contains no forbidden key — what the
    /// guard tests assert directly against a hand-built "someone made a
    /// mistake" event, independent of `strip(_:)` actually being wired
    /// into the recording path (belt AND braces: one test proves the
    /// predicate is right, another proves `TelemetryRecorder` calls it).
    public static func isClean(_ attributes: [String: TelemetryValue]) -> Bool {
        attributes.keys.allSatisfy { !forbiddenKeys.contains($0.lowercased()) }
    }
}
