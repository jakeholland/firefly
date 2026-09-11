//
//  WireFormatTests.swift — the generated types actually round-trip.
//
import MeshtasticProto
import XCTest

final class WireFormatTests: XCTestCase {

    /// A `want_config` `ToRadio` is the very first thing the app puts on
    /// the wire; if this does not serialize we have nothing.
    func testWantConfigToRadioRoundTrips() throws {
        var toRadio = MeshtasticProto.ToRadio()
        toRadio.wantConfigID = 0x1234_5678
        let bytes = try toRadio.serializedData()
        XCTAssertFalse(bytes.isEmpty)

        let decoded = try MeshtasticProto.ToRadio(serializedBytes: bytes)
        guard case .wantConfigID(let nonce)? = decoded.payloadVariant else {
            return XCTFail("payloadVariant was not wantConfigID")
        }
        XCTAssertEqual(nonce, 0x1234_5678)
    }

    /// Positions travel as fixed-point 1e-7 degrees. Getting the scale
    /// wrong puts the crew in the wrong hemisphere, quietly.
    func testPositionScaling() throws {
        var position = MeshtasticProto.Position()
        position.latitudeI = Int32(39.766_0 * 1e7)
        position.longitudeI = Int32(-82.472_0 * 1e7)
        position.precisionBits = 32
        position.locationSource = .locExternal

        let decoded = try MeshtasticProto.Position(serializedBytes: try position.serializedData())
        XCTAssertEqual(Double(decoded.latitudeI) * 1e-7, 39.766, accuracy: 1e-5)
        XCTAssertEqual(Double(decoded.longitudeI) * 1e-7, -82.472, accuracy: 1e-5)
        XCTAssertEqual(decoded.precisionBits, 32)
        XCTAssertEqual(decoded.locationSource, .locExternal)
    }

    /// Firefly's own packets ride a private portnum with a version byte
    /// (docs/specs/S04-firefly-protocol.md). 269 must be expressible as
    /// a `PortNum` the generated enum will carry unchanged.
    func testFireflyPortnumSurvivesAsUnrecognized() throws {
        var data = MeshtasticProto.DataMessage()
        data.portnum = MeshtasticProto.PortNum(rawValue: 269) ?? .unknownApp
        data.payload = Data([0x01, 0x02])
        let decoded = try MeshtasticProto.DataMessage(serializedBytes: try data.serializedData())
        XCTAssertEqual(decoded.portnum.rawValue, 269)
        XCTAssertEqual(decoded.payload, Data([0x01, 0x02]))
    }
}
