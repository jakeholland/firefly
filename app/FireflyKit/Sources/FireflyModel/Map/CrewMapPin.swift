//
//  CrewMapPin.swift — Map tab slice: turns `CrewMember` (Bridge/
//  CrewStore.swift, the real `ff_crew` freshness) into exactly what a
//  map pin is allowed to honestly claim.
//
//  The state table (S09-adjacent honesty rules, this slice's own task
//  brief):
//    - no position at all (`FreshnessCategory.never`, or any member
//      whose `position == nil`) -> NOT DRAWN. There is nothing to
//      honestly anchor a pin to — same "n_pts == 0 -> omit" policy
//      `ff_map_feature_render_kind` applies to festpack features.
//    - `position.asserted` (LOC_MANUAL — typed in, never measured,
//      `CrewStore.PositionMeta`/issue #33) -> `.asserted`, drawn as a
//      SQUARE, regardless of how old the assertion is: elapsed time is
//      a category error for a typed-in position (`FreshnessCategory`'s
//      own doc comment).
//    - degraded precision (`precisionBits` present and below
//      `FF_CREW_POS_PRECISION_MIN_BITS`, issue #47) -> `.imprecise`,
//      drawn as an AREA CIRCLE, never a point — checked before the
//      live/stale/lost split, same priority order `ff_app_map_crew_t`'s
//      own doc comment (S09 Amendments) gives it on the puck.
//    - `.live` -> `.live`, a solid pin.
//    - `.stale` / `.lost` -> `.staleRing` / `.lostRing`, a dashed
//      "last known" ring with a `~`-prefixed age — never silently
//      upgraded to look like a live fix.
//
import FireflyCore
import Foundation

/// How a crew member's position is allowed to render on the map — see
/// this file's header comment for the exact decision table.
public enum CrewMapPinTreatment: Sendable, Equatable {
    /// A solid, filled pin — a live, precise, unasserted fix.
    case live
    /// A dashed "last known" ring, `STALE` freshness.
    case staleRing
    /// A dashed "last known" ring, `LOST` freshness — visually the same
    /// ring as `.staleRing` (S09: "STALE/LOST dashed 'last known'
    /// ring"); kept as a separate case so callers that DO want to say
    /// which (the selected-card text, a test fixture) can.
    case lostRing
    /// A typed-in landmark (LOC_MANUAL) — a SQUARE, never a round pin
    /// (a square reads as "someone said this", not "this was measured").
    case asserted
    /// Precision-degraded — an AREA CIRCLE, never a pin-point claim
    /// (issue #47, this slice's map-specific treatment).
    case imprecise
}

/// One crew member, reduced to exactly what this map is allowed to draw
/// for them.
public struct CrewMapPin: Sendable, Equatable, Identifiable {
    public let id: UInt32
    public let name: String
    public let colorIndex: UInt8
    public let initial: Character?
    public let latitude: Double
    public let longitude: Double
    public let treatment: CrewMapPinTreatment
    /// `~`-prefixed for anything but `.live` (S09: "with a `~` age") —
    /// pre-formatted via `CrewStore.formatAge`, never reimplemented.
    public let ageText: String
    /// nil only when the viewer's own position is unknown — see
    /// `CrewMapPinBuilder.build(from:myPosition:imperial:)`.
    public let distanceMeters: Double?
    public let bearingDegrees: Double?
    /// Only set for `.imprecise` — the approximate cell edge
    /// (`CrewStore.positionPrecisionGridMeters`), meters, for an honest
    /// "~110 m area" style caption. nil otherwise.
    public let precisionGridMeters: Float?

    public init(id: UInt32, name: String, colorIndex: UInt8, initial: Character?, latitude: Double,
                longitude: Double, treatment: CrewMapPinTreatment, ageText: String, distanceMeters: Double?,
                bearingDegrees: Double?, precisionGridMeters: Float?) {
        self.id = id
        self.name = name
        self.colorIndex = colorIndex
        self.initial = initial
        self.latitude = latitude
        self.longitude = longitude
        self.treatment = treatment
        self.ageText = ageText
        self.distanceMeters = distanceMeters
        self.bearingDegrees = bearingDegrees
        self.precisionGridMeters = precisionGridMeters
    }
}

public enum CrewMapPinBuilder {
    /// `ff_crew.h`'s own threshold (issue #47's doc comment): a
    /// precision grid coarser than this many bits is "degraded" — never
    /// rendered as a point. Duplicated here as a named Swift constant
    /// (mirroring the header's OWN literal, the same "transcribed, not
    /// imported" convention `CoreStore.outboxAckTimeoutMs` documents for
    /// `FF_OUTBOX_ACK_TIMEOUT_MS`) because `FF_CREW_POS_PRECISION_MIN_BITS`
    /// is a `ff_crew.h`-private `#define`, not part of the bridge's
    /// public C surface any Swift file already imports a symbol for.
    public static let precisionMinBits: UInt8 = 21

    /// Builds one pin per member with a position at all (see this file's
    /// header comment for the NEVER/omit case) — order matches
    /// `members`'s own order (unspecified per `CrewStore.members`'s own
    /// doc comment; a caller that wants a stable on-screen order sorts
    /// first).
    ///
    /// `myPosition: nil` means the viewer's own fix is unknown —
    /// `distanceMeters`/`bearingDegrees` are honestly `nil` for every
    /// pin in that case, never a distance to a fabricated origin.
    public static func build(from members: [CrewMember], myPosition: GeoCoordinate?,
                              imperial: Bool) -> [CrewMapPin] {
        members.compactMap { member in
            guard let position = member.position else { return nil } // NEVER: nothing to draw
            let treatment = treatment(for: member, position: position)
            let here = GeoCoordinate(latitude: position.latitude, longitude: position.longitude)
            let distance = myPosition.map { GeoBridge.distanceMeters(from: $0, to: here) }
            let bearing = myPosition.map { GeoBridge.bearingDegrees(from: $0, to: here) }
            let ageText = treatment == .asserted ? "ASSERTED" : ageText(for: treatment, ageMs: position.ageMs)
            let grid: Float? = treatment == .imprecise
                ? position.precisionBits.map(CrewStore.positionPrecisionGridMeters(bits:))
                : nil

            return CrewMapPin(id: member.nodeID, name: member.displayName, colorIndex: member.colorIndex,
                               initial: member.initial, latitude: position.latitude, longitude: position.longitude,
                               treatment: treatment, ageText: ageText, distanceMeters: distance,
                               bearingDegrees: bearing, precisionGridMeters: grid)
        }
    }

    private static func treatment(for member: CrewMember, position: CrewMember.Position) -> CrewMapPinTreatment {
        // Priority order matches the S09/ff_app_map_crew_t precedent
        // this file's header comment cites: asserted, then imprecise,
        // then ordinary freshness — an asserted-AND-imprecise position
        // (a typed-in place with stated-but-coarse precision) reads as
        // "someone said this" first, since that is the stronger claim
        // about WHERE the honesty gap actually is.
        if position.asserted { return .asserted }
        if let bits = position.precisionBits, bits < precisionMinBits { return .imprecise }
        switch member.freshness {
        case .live: return .live
        case .stale: return .staleRing
        case .lost: return .lostRing
        case .asserted: return .asserted // defensive: position.asserted already caught this above
        case .never: return .lostRing // defensive: position != nil contradicts .never; never reached in practice
        }
    }

    private static func ageText(for treatment: CrewMapPinTreatment, ageMs: UInt32) -> String {
        let formatted = CrewStore.formatAge(ms: ageMs)
        guard treatment != .live else { return formatted }
        return "~\(formatted)"
    }
}
