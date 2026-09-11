//
//  StreamFramer.swift — Meshtastic's stream framing, Swift side.
//
//  `0x94 0xC3 [len_hi] [len_lo] [protobuf ...]`, max payload 512 bytes.
//  Used by the SERIAL (macOS) and TCP (meshtasticd / sim) transports.
//  BLE does NOT use it — GATT is already message-framed, one protobuf
//  per characteristic read/write.
//
//  This is the same contract firmware/meshclient's `mc_framer_t`
//  implements in C (docs/specs/S03-meshclient.md, "Framing"), including
//  the two behaviours its AC1 pins: a byte-at-a-time dribble must yield
//  the frame exactly once, and garbage before the magic must resync
//  rather than desync forever.
//
import Foundation

public struct StreamFramer: Sendable {
    public static let magic0: UInt8 = 0x94
    public static let magic1: UInt8 = 0xC3
    /// Matches `MC_MAX_FRAME` on the puck. A longer stated length is
    /// garbage, and is resynced past rather than trusted into a buffer.
    public static let maxPayload = 512

    private enum State: Sendable {
        case wantMagic0
        case wantMagic1
        case wantLenHi
        case wantLenLo(hi: UInt8)
        case wantBody(remaining: Int)
    }

    private var state: State = .wantMagic0
    private var body = Data()

    public init() {}

    /// Feed received bytes; returns every COMPLETE frame payload found,
    /// in order. Partial frames are carried across calls.
    public mutating func feed(_ bytes: Data) -> [Data] {
        var out: [Data] = []
        for byte in bytes {
            switch state {
            case .wantMagic0:
                if byte == Self.magic0 { state = .wantMagic1 }
                // else: garbage, stay hunting for magic — this is the resync.
            case .wantMagic1:
                if byte == Self.magic1 {
                    state = .wantLenHi
                } else if byte == Self.magic0 {
                    // 0x94 0x94 — the second 0x94 may itself start a frame.
                    state = .wantMagic1
                } else {
                    state = .wantMagic0
                }
            case .wantLenHi:
                state = .wantLenLo(hi: byte)
            case .wantLenLo(let hi):
                let len = Int(hi) << 8 | Int(byte)
                if len > Self.maxPayload {
                    // Oversize stated length: never allocate against an
                    // untrusted length. Drop it and resync — matches
                    // `mc_framer_feed`'s `expected > MC_MAX_FRAME` check
                    // in firmware/meshclient/src/mc_framing.c.
                    state = .wantMagic0
                } else if len == 0 {
                    // Degenerate zero-length frame: a VALID frame with
                    // an empty payload, complete immediately — matches
                    // `mc_framer_feed`'s `expected == 0` case exactly
                    // (its own doc comment: "may be 0 for a degenerate
                    // zero-length frame"). Not garbage, not resynced
                    // past.
                    out.append(Data())
                    state = .wantMagic0
                } else {
                    body.removeAll(keepingCapacity: true)
                    body.reserveCapacity(len)
                    state = .wantBody(remaining: len)
                }
            case .wantBody(let remaining):
                body.append(byte)
                if remaining - 1 == 0 {
                    out.append(body)
                    body.removeAll(keepingCapacity: true)
                    state = .wantMagic0
                } else {
                    state = .wantBody(remaining: remaining - 1)
                }
            }
        }
        return out
    }

    /// Wrap a `ToRadio` protobuf for a stream transport. Returns nil
    /// rather than truncating when the payload cannot be framed. An
    /// empty payload IS permitted — matches `mc_frame_encode`, which
    /// only rejects `payload_len > MC_MAX_FRAME`, never a zero length.
    public static func frame(_ payload: Data) -> Data? {
        guard payload.count <= maxPayload else { return nil }
        var out = Data([magic0, magic1, UInt8(payload.count >> 8), UInt8(payload.count & 0xFF)])
        out.append(payload)
        return out
    }
}
