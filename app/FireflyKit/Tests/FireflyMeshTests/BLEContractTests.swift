//
//  BLEContractTests.swift — the borrowed BLE facts, pinned.
//
//  These constants and rules are Meshtastic's, cross-checked against
//  Meshtastic-Apple and this repo's archived iOS app; see
//  docs/specs/A01-companion-app.md's "Reuse assessment". Pinning them in
//  a test means a well-meaning edit has to argue with a failing build
//  rather than silently break pairing on a bench board.
//
import FireflyMesh
import XCTest

final class BLEContractTests: XCTestCase {

    func testGATTUUIDs() {
        XCTAssertEqual(MeshtasticBLE.serviceUUIDString, "6BA1B218-15A8-461F-9FA8-5DCAE273EAFD")
        XCTAssertEqual(MeshtasticBLE.toRadioUUIDString, "F75C76D2-129E-4DAD-A1DD-7866124401E7")
        XCTAssertEqual(MeshtasticBLE.fromRadioUUIDString, "2C55E69E-4993-11ED-B878-0242AC120002")
        XCTAssertEqual(MeshtasticBLE.fromNumUUIDString, "ED9DA18C-A800-4F66-A670-AA7547E34453")
    }

    /// An empty FROMRADIO read is the ONLY end-of-queue signal there is.
    func testEmptyReadIsTheDrainTerminator() {
        XCTAssertTrue(FromRadioDrainPolicy.isQueueDrained(read: Data()))
        XCTAssertFalse(FromRadioDrainPolicy.isQueueDrained(read: Data([0x00])))
    }

    /// All three drain triggers must stay enumerated. Dropping the
    /// post-write one is the subtle bug: the radio can queue a reply
    /// before the FROMNUM notification lands, and the app then waits
    /// forever for a nudge that already happened.
    func testAllThreeDrainTriggersArePresent() {
        XCTAssertEqual(Set(FromRadioDrainPolicy.Trigger.allCases),
                       [.subscriptionAcknowledged, .fromNumNotification, .toRadioWriteCompleted])
    }
}
