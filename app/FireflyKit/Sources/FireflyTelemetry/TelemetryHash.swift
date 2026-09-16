//
//  TelemetryHash.swift — A04: node-id hashing for `crew.member.seen`/
//  `crew.member.lost`'s `id_hash` attribute.
//
//  A Meshtastic node number identifies a real person's radio; the crew
//  CODE and message TEXT are never recorded at all (there is no
//  attribute key for either — see `TelemetryAttributeAllowlist`), and a
//  node id is hashed rather than recorded raw so a diagnostics export
//  can be read for CONNECTIVITY patterns ("this id_hash keeps dropping
//  off") without being a durable, reversible log of who was on a given
//  puck.
//
import CryptoKit
import Foundation

public enum TelemetryHash {
    /// SHA-256, truncated to 16 hex characters (64 bits) — plenty to
    /// tell two node ids apart across one field weekend's worth of
    /// events, short enough that a JSON-lines line stays readable.
    /// Deterministic and salt-free BY DESIGN: the same node number must
    /// hash to the same `id_hash` across the whole recording (and across
    /// a relaunch, since nothing here is ever un-hashed) so "how often
    /// does this member drop" is answerable from the log at all.
    public static func nodeID(_ nodeNum: UInt32) -> String {
        var bytes = nodeNum.bigEndian
        let data = withUnsafeBytes(of: &bytes) { Data($0) }
        let digest = SHA256.hash(data: data)
        return digest.compactMap { String(format: "%02x", $0) }.joined().prefix(16).description
    }
}
