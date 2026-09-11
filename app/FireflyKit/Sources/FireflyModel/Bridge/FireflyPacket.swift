//
//  FireflyPacket.swift — the Swift-safe wrapper over
//  `firmware/core/ff_proto` (docs/specs/A01-companion-app.md, slice B):
//  encode/decode of Firefly's own app-layer packets, riding Meshtastic
//  as opaque bytes on private portnum 269.
//
//  Pure encode/decode, no transport — matches ff_proto.h's own
//  "extraction-grade, includable by anything" framing. `MeshtasticClient`
//  (slice A) is what actually calls `mc_send_private`/reads inbound
//  private-portnum packets; this type only turns a `Data` payload into
//  or out of an honest Swift value.
//
import FireflyCore
import Foundation

/// The Meshtastic portnum Firefly's own protocol rides on
/// (`FF_PORTNUM`).
public let fireflyPortNum: UInt32 = UInt32(FF_PORTNUM)

public enum FireflyPacket: Sendable, Equatable {
    case flare(durationS: UInt16)
    case flareEnd
    case rally(latitude: Double, longitude: Double, name: String)
    case rallyClear
    case status(String)
    /// Reserved for delivery UX (v1.5) — decodable, but there is
    /// deliberately no encoder yet (ff_proto.h's own note).
    case ackPing(nonce: UInt32)
    case ping(nonce: UInt32)
    case pong(nonce: UInt32, rssiDbm: Int16, snrDb: Float?)
    /// A well-formed frame from an old build that no longer carries any
    /// content (the retired PULSE type, `FF_PROTO_TYPE_RESERVED_01`) —
    /// a real, successful decode, never an error (ff_proto.h's own
    /// "RESERVED_01" section).
    case retiredReserved01

    /// `[ver:1][type:1][body...]`, or nil if this case has no encoder
    /// (`.ackPing`, `.retiredReserved01`) or the core's own encoder
    /// rejected it (a too-long rally name/status string).
    public func encode() -> Data? {
        var buf = [UInt8](repeating: 0, count: Int(FF_PROTO_MAX_PAYLOAD))
        let n: Int32
        switch self {
        case .flare(let durationS):
            n = ff_proto_encode_flare(&buf, buf.count, durationS)
        case .flareEnd:
            n = ff_proto_encode_flare_end(&buf, buf.count)
        case .rally(let latitude, let longitude, let name):
            n = name.withCString { cName in
                ff_proto_encode_rally(&buf, buf.count, ff_latlon_t(lat: latitude, lon: longitude), cName)
            }
        case .rallyClear:
            n = ff_proto_encode_rally_clear(&buf, buf.count)
        case .status(let text):
            n = text.withCString { cText in ff_proto_encode_status(&buf, buf.count, cText) }
        case .ackPing:
            return nil
        case .ping(let nonce):
            n = ff_proto_encode_ping(&buf, buf.count, nonce)
        case .pong(let nonce, let rssiDbm, let snrDb):
            n = ff_proto_encode_pong(&buf, buf.count, nonce, rssiDbm, snrDb != nil, Int16((snrDb ?? 0) * 10))
        case .retiredReserved01:
            return nil
        }
        guard n > 0 else { return nil }
        return Data(buf.prefix(Int(n)))
    }

    /// Strict decode — see ff_proto.h's own doc comment for the exact
    /// rejection rules (bad version, unknown type, a body length that
    /// isn't exactly right for its type). Returns nil for anything
    /// `ff_proto_decode` itself rejects.
    public static func decode(_ data: Data) -> FireflyPacket? {
        var msg = ff_proto_msg_t()
        let type: Int32 = [UInt8](data).withUnsafeBufferPointer { buf in
            ff_proto_decode(buf.baseAddress, buf.count, &msg)
        }
        switch type {
        case Int32(FF_PROTO_TYPE_RESERVED_01.rawValue):
            return .retiredReserved01
        case Int32(FF_PROTO_TYPE_FLARE.rawValue):
            return .flare(durationS: msg.body.flare.dur_s)
        case Int32(FF_PROTO_TYPE_FLARE_END.rawValue):
            return .flareEnd
        case Int32(FF_PROTO_TYPE_RALLY.rawValue):
            return .rally(latitude: msg.body.rally.pos.lat, longitude: msg.body.rally.pos.lon,
                          name: FixedCString.decode(msg.body.rally.name))
        case Int32(FF_PROTO_TYPE_RALLY_CLEAR.rawValue):
            return .rallyClear
        case Int32(FF_PROTO_TYPE_STATUS.rawValue):
            return .status(FixedCString.decode(msg.body.status.text))
        case Int32(FF_PROTO_TYPE_ACK_PING.rawValue):
            return .ackPing(nonce: msg.body.ack_ping.nonce)
        case Int32(FF_PROTO_TYPE_PING.rawValue):
            return .ping(nonce: msg.body.ping.nonce)
        case Int32(FF_PROTO_TYPE_PONG.rawValue):
            let pong = msg.body.pong
            return .pong(nonce: pong.nonce, rssiDbm: pong.rssi_dbm,
                        snrDb: pong.has_snr ? Float(pong.snr_x10) / 10 : nil)
        default:
            return nil
        }
    }
}
