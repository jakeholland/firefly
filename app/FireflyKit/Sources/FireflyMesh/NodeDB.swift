//
//  NodeDB.swift — the Meshtastic node database, phone side.
//
//  In-memory, rebuilt on every want_config handshake (M1;
//  docs/specs/A01-companion-app.md, "NodeDB" — node-database persistence
//  across launches is an explicit scope cut). This file owns the
//  wire-to-Swift conversion for `NodeInfo`, `Position` and per-packet
//  reception metadata, and it is where the spec's three absence rules
//  live — the same three rules the puck's C client enforces
//  (firmware/meshclient/include/mc_client.h, `mc_position_t` /
//  `mc_rx_meta_t` doc comments; firmware/meshclient/src/mc_client.c,
//  `mc_rx_path_from_pkt` / `mc_emit_rx_meta`), reimplemented here over
//  SwiftProtobuf rather than nanopb so the two clients agree about what
//  "unknown" means without sharing code:
//
//    1. `location_source`: absent, LOC_UNSET, and an unrecognised future
//       value all read `.unknown` — never `.internalGPS`.
//    2. RSSI/SNR are per-packet and attributable to a node ONLY when the
//       packet arrived directly; `hop_start == 0` alone is UNKNOWN, not
//       DIRECT — see `rxPath(hopStart:hopLimit:hasDecodedBitfield:viaMqtt:)`.
//    3. `precision_bits`: present only for wire values 1...32; 0 and
//       anything above 32 read absent, and absent is NOT full precision.
//
import Foundation
import MeshtasticProto

/// How a node's RSSI/SNR packet may be attributed to it — the Swift
/// twin of the puck's `mc_rx_path_t` (mc_client.h). Deliberately never
/// crosses into `FireflyModel`; only `MeshNodeSnapshot.hopsAway`
/// (a plain `UInt32?`) does. UNKNOWN is not "assume DIRECT": a hop
/// count that could not be established is not evidence of zero hops.
enum RxPath: Sendable, Equatable {
    case unknown
    case direct
    case indirect
}

/// The phone's in-memory Meshtastic node database. One instance lives
/// inside `MeshtasticClient`; rebuilt from scratch at the start of every
/// handshake.
struct NodeDB: Sendable {
    private var nodes: [UInt32: MeshNodeSnapshot] = [:]

    var all: [MeshNodeSnapshot] { Array(nodes.values) }

    func node(_ num: UInt32) -> MeshNodeSnapshot? { nodes[num] }

    mutating func reset() { nodes.removeAll() }

    /// Apply one `NodeInfo` dump entry (want_config phase 2, or a live
    /// NodeInfo broadcast). Returns the resulting snapshot so the caller
    /// can publish it.
    ///
    /// Deliberately NOT surfaced from this path: `NodeInfo.snr`. It is a
    /// cached "SNR of the last message we heard from this node" with
    /// implicit presence and no reception timestamp of its own — the
    /// same reason `mc_client.c`'s `on_node` path never copies it either
    /// (a want_config replay carries no rx_time, so a cached SNR here
    /// could never feed a freshness check honestly). Live SNR arrives
    /// per-packet via `applyRxMeta` instead.
    @discardableResult
    mutating func apply(nodeInfo info: NodeInfo) -> MeshNodeSnapshot {
        let existing = nodes[info.num]

        let position: NodePosition? = (info.hasPosition && info.position.hasLatitudeI && info.position.hasLongitudeI)
            ? NodeDB.position(from: info.position, rxTime: nil)
            : existing?.position

        let shortName = (info.hasUser && !info.user.shortName.isEmpty) ? info.user.shortName : existing?.shortName
        let longName = (info.hasUser && !info.user.longName.isEmpty) ? info.user.longName : existing?.longName
        let lastHeard = info.lastHeard != 0
            ? Date(timeIntervalSince1970: TimeInterval(info.lastHeard))
            : existing?.lastHeard

        // NodeInfo carries its own hop summary with EXPLICIT presence
        // (has_hops_away), so — unlike a live MeshPacket's hop_start —
        // there is no old-firmware ambiguity to resolve here: absent
        // simply means the nodeDB never recorded one, which is UNKNOWN,
        // never DIRECT.
        let hopsAway: UInt32? = info.viaMqtt ? nil : (info.hasHopsAway ? info.hopsAway : nil)

        let snapshot = MeshNodeSnapshot(
            num: info.num,
            shortName: shortName,
            longName: longName,
            position: position,
            lastHeard: lastHeard,
            rssiDbm: existing?.rssiDbm,
            snrDb: existing?.snrDb,
            hopsAway: hopsAway)
        nodes[info.num] = snapshot
        return snapshot
    }

    /// Apply a live `Position` (POSITION_APP) packet. Returns `nil` when
    /// the position carries no fix at all (both lat/lon fields absent) —
    /// a legitimate "no GPS fix yet" broadcast, not corruption, mirroring
    /// `mc_client.c`'s own silent-drop for the same condition.
    @discardableResult
    mutating func apply(position pb: Position, from num: UInt32, rxTime: Date?) -> MeshNodeSnapshot? {
        guard pb.hasLatitudeI, pb.hasLongitudeI else { return nil }
        let existing = nodes[num]
        let pos = NodeDB.position(from: pb, rxTime: rxTime)
        let snapshot = MeshNodeSnapshot(
            num: num,
            shortName: existing?.shortName,
            longName: existing?.longName,
            position: pos,
            lastHeard: rxTime ?? existing?.lastHeard,
            rssiDbm: existing?.rssiDbm,
            snrDb: existing?.snrDb,
            hopsAway: existing?.hopsAway)
        nodes[num] = snapshot
        return snapshot
    }

    /// Apply per-packet reception metadata (RSSI/SNR/hop path), measured
    /// by OUR radio, for any packet naming `from`. Only `.direct` licenses
    /// attributing rssi/snr to the node — see `RxPath`. Returns `nil` (and
    /// does nothing) when the node has no slot yet: there is nobody to
    /// attribute the reading to until a `NodeInfo`/`Position` has given
    /// this `num` an identity, matching `mc_client.c`'s comment that
    /// `on_rx_meta` firing does not imply any consumer state exists yet.
    @discardableResult
    mutating func applyRxMeta(from num: UInt32, rssiDbm: Int16?, snrDb: Float?, path: RxPath) -> MeshNodeSnapshot? {
        guard let existing = nodes[num] else { return nil }
        guard path == .direct, (rssiDbm != nil || snrDb != nil) else { return nil }
        let snapshot = MeshNodeSnapshot(
            num: num,
            shortName: existing.shortName,
            longName: existing.longName,
            position: existing.position,
            lastHeard: existing.lastHeard,
            rssiDbm: rssiDbm ?? existing.rssiDbm,
            snrDb: snrDb ?? existing.snrDb,
            hopsAway: existing.hopsAway)
        nodes[num] = snapshot
        return snapshot
    }

    // MARK: - Wire conversion (the three absence rules)

    static func position(from pb: Position, rxTime: Date?) -> NodePosition {
        let lat = Double(pb.latitudeI) * 1e-7
        let lon = Double(pb.longitudeI) * 1e-7
        let source = NodeDB.locationSource(pb.locationSource)
        let precisionBits = NodeDB.precisionBits(pb.precisionBits)
        // `time` (the sender's own GPS fix timestamp) wins when present;
        // a want_config NodeInfo replay never carries `rxTime` (the
        // caller passes `nil`), matching mc_client.c's replay path,
        // which hardcodes `has_rx_time = false` for the same reason.
        let time: Date? = pb.time != 0 ? Date(timeIntervalSince1970: TimeInterval(pb.time)) : rxTime
        return NodePosition(latitude: lat, longitude: lon, time: time, source: source, precisionBits: precisionBits)
    }

    /// Rule 1: absent (implicit-presence zero), `.locUnset`, and
    /// `.UNRECOGNIZED` all read `.unknown` — never `.internalGPS`. MANUAL
    /// is asserted, not measured, and is the one case that must never be
    /// confused with a stale measurement.
    static func locationSource(_ wire: Position.LocSource) -> NodePosition.Source {
        switch wire {
        case .locUnset: return .unknown
        case .locManual: return .manual
        case .locInternal: return .internalGPS
        case .locExternal: return .externalGPS
        case .UNRECOGNIZED: return .unknown
        }
    }

    /// Rule 3: present only for wire values 1...32. `0` (byte-identical
    /// to absent under proto3 implicit presence, and also Meshtastic's
    /// own "position disabled on this channel" value) and anything above
    /// 32 (not a precision of a 32-bit coordinate — untrusted RF garbage)
    /// both read absent, reported rather than clamped.
    static func precisionBits(_ wire: UInt32) -> UInt8? {
        guard wire >= 1, wire <= 32 else { return nil }
        return UInt8(wire)
    }

    /// Rule 2, the hop half — ported verbatim from `mc_client.c`'s
    /// `mc_rx_path_from_pkt`: `hop_start == 0` is UNKNOWN unless the
    /// sender's decoded `bitfield` is present AND `hop_limit == 0` (the
    /// vendored protobuf's own documented tell that this sender runs
    /// firmware new enough to actually populate `hop_start`; older
    /// firmware never sets it, so a bare zero there proves nothing). A
    /// `hop_limit` that exceeds `hop_start` is malformed and also reads
    /// UNKNOWN — the asymmetry (everything not positively established
    /// lands on UNKNOWN) is deliberate: a false DIRECT would silently
    /// misattribute a relay's signal strength to a distant friend.
    static func rxPath(hopStart: UInt32, hopLimit: UInt32, hasDecodedBitfield: Bool, viaMqtt: Bool) -> RxPath {
        if viaMqtt { return .indirect }
        if hopStart > 0 {
            if hopLimit > hopStart { return .unknown }
            return hopLimit == hopStart ? .direct : .indirect
        }
        if hasDecodedBitfield && hopLimit == 0 { return .direct }
        return .unknown
    }
}
