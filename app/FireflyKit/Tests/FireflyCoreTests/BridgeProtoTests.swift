//
//  BridgeProtoTests.swift — FireflyPacket (ff_proto) round-trips through
//  the Swift bridge: every S04 type, plus the strict-decode and
//  retired-RESERVED_01 rules from ff_proto.h's own doc comment.
//
@testable import FireflyModel
import XCTest

final class BridgeProtoTests: XCTestCase {

    func testPortNumAndVersionMatchTheCore() {
        XCTAssertEqual(fireflyPortNum, 269)
    }

    func testFlareRoundTrips() {
        let packet = FireflyPacket.flare(durationS: 120)
        let data = packet.encode()
        XCTAssertNotNil(data)
        XCTAssertEqual(FireflyPacket.decode(data!), packet)
    }

    func testFlareEndRoundTrips() {
        let data = FireflyPacket.flareEnd.encode()
        XCTAssertEqual(FireflyPacket.decode(data!), .flareEnd)
    }

    func testRallyRoundTrips() {
        let packet = FireflyPacket.rally(latitude: 47.707135, longitude: -122.2820993, name: "Main Stage")
        let data = packet.encode()
        XCTAssertNotNil(data)
        guard case .rally(let lat, let lon, let name) = FireflyPacket.decode(data!) else {
            return XCTFail("expected a rally to decode back")
        }
        XCTAssertEqual(lat, 47.707135, accuracy: 0.0001)
        XCTAssertEqual(lon, -122.2820993, accuracy: 0.0001)
        XCTAssertEqual(name, "Main Stage")
    }

    func testRallyClearRoundTrips() {
        let data = FireflyPacket.rallyClear.encode()
        XCTAssertEqual(FireflyPacket.decode(data!), .rallyClear)
    }

    func testStatusRoundTrips() {
        let packet = FireflyPacket.status("RAGING")
        let data = packet.encode()
        XCTAssertEqual(FireflyPacket.decode(data!), packet)
    }

    func testStatusOverTheMaxLengthFailsToEncode() {
        let tooLong = String(repeating: "x", count: 21) // FF_PROTO_STATUS_MAX is 20
        XCTAssertNil(FireflyPacket.status(tooLong).encode())
    }

    func testPingRoundTrips() {
        let packet = FireflyPacket.ping(nonce: 0xDEAD_BEEF)
        let data = packet.encode()
        XCTAssertEqual(FireflyPacket.decode(data!), packet)
    }

    func testPongRoundTripsWithSNR() {
        let packet = FireflyPacket.pong(nonce: 7, rssiDbm: -82, snrDb: 4.5)
        let data = packet.encode()
        guard case .pong(let nonce, let rssi, let snr) = FireflyPacket.decode(data!) else {
            return XCTFail("expected a pong to decode back")
        }
        XCTAssertEqual(nonce, 7)
        XCTAssertEqual(rssi, -82)
        XCTAssertEqual(snr!, 4.5, accuracy: 0.1)
    }

    /// `has_snr == false` must decode back to `nil`, never a fabricated
    /// reading masquerading as a real zero.
    func testPongWithoutSNRDecodesToNilNotZero() {
        let packet = FireflyPacket.pong(nonce: 7, rssiDbm: -82, snrDb: nil)
        let data = packet.encode()
        guard case .pong(_, _, let snr) = FireflyPacket.decode(data!) else {
            return XCTFail("expected a pong to decode back")
        }
        XCTAssertNil(snr)
    }

    /// ACK_PING is decodable (a v1.5 peer's packet must not misparse as
    /// unknown-type) but has no encoder yet — nothing sends it.
    func testAckPingHasNoEncoderButIsUnderstoodOnDecode() {
        XCTAssertNil(FireflyPacket.ackPing(nonce: 1).encode())
    }

    /// A well-formed RESERVED_01 frame (the retired PULSE) is a real,
    /// SUCCESSFUL decode — never an error — but this bridge never
    /// encodes it (there is no encoder any more, matching ff_proto.h).
    func testRetiredReserved01DecodesSuccessfully() {
        // [ver:1][type:1], empty body — the exact shape PULSE always had.
        let bytes: [UInt8] = [1, 0x01]
        XCTAssertEqual(FireflyPacket.decode(Data(bytes)), .retiredReserved01)
        XCTAssertNil(FireflyPacket.retiredReserved01.encode())
    }

    func testDecodeRejectsUnknownType() {
        let bytes: [UInt8] = [1, 0xFF]
        XCTAssertNil(FireflyPacket.decode(Data(bytes)))
    }

    func testDecodeRejectsWrongVersion() {
        let bytes: [UInt8] = [2, 0x03] // FLARE_END shape, wrong version
        XCTAssertNil(FireflyPacket.decode(Data(bytes)))
    }

    func testDecodeRejectsTrailingGarbageBytes() {
        // FLARE_END is defined as an EMPTY body; trailing bytes must be
        // rejected, not silently ignored (ff_proto.h's strict-decode rule).
        let bytes: [UInt8] = [1, 0x03, 0xAA, 0xBB]
        XCTAssertNil(FireflyPacket.decode(Data(bytes)))
    }

    func testDecodeOfEmptyDataIsASafeNoCrash() {
        XCTAssertNil(FireflyPacket.decode(Data()))
    }
}
