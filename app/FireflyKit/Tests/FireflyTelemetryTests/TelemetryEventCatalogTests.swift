//
//  TelemetryEventCatalogTests.swift — A04: pins the exact event names
//  against `docs/specs/A04-telemetry.md`'s own table. A rename here
//  must be a deliberate, reviewed spec change — not a typo that quietly
//  breaks a Firestore query someone already built against the old name.
//
import XCTest
@testable import FireflyTelemetry

final class TelemetryEventCatalogTests: XCTestCase {
    func testBLEEventNames() {
        XCTAssertEqual(TelemetryEventName.bleScanStart, "ble.scan.start")
        XCTAssertEqual(TelemetryEventName.bleScanStop, "ble.scan.stop")
        XCTAssertEqual(TelemetryEventName.bleDiscovered, "ble.discovered")
        XCTAssertEqual(TelemetryEventName.bleConnectAttempt, "ble.connect.attempt")
        XCTAssertEqual(TelemetryEventName.bleConnected, "ble.connected")
        XCTAssertEqual(TelemetryEventName.bleHandshakePhase, "ble.handshake.phase")
        XCTAssertEqual(TelemetryEventName.bleReady, "ble.ready")
        XCTAssertEqual(TelemetryEventName.bleDisconnected, "ble.disconnected")
        XCTAssertEqual(TelemetryEventName.bleLadderScheduled, "ble.ladder.scheduled")
        XCTAssertEqual(TelemetryEventName.bleLadderFired, "ble.ladder.fired")
        XCTAssertEqual(TelemetryEventName.bleRestore, "ble.restore")
        XCTAssertEqual(TelemetryEventName.blePower, "ble.power")
    }

    func testAppEventNames() {
        XCTAssertEqual(TelemetryEventName.appForeground, "app.foreground")
        XCTAssertEqual(TelemetryEventName.appBackground, "app.background")
        XCTAssertEqual(TelemetryEventName.appLaunch, "app.launch")
        XCTAssertEqual(TelemetryEventName.appTerminate, "app.terminate")
    }

    func testNotificationEventNames() {
        XCTAssertEqual(TelemetryEventName.notifPosted, "notif.posted")
        XCTAssertEqual(TelemetryEventName.notifTapped, "notif.tapped")
        XCTAssertEqual(TelemetryEventName.notifAuthorization, "notif.authorization")
    }

    func testCrewAdminEventNames() {
        XCTAssertEqual(TelemetryEventName.adminWrite, "admin.write")
        XCTAssertEqual(TelemetryEventName.crewJoin, "crew.join")
        XCTAssertEqual(TelemetryEventName.crewLeave, "crew.leave")
        XCTAssertEqual(TelemetryEventName.crewStart, "crew.start")
        XCTAssertEqual(TelemetryEventName.crewMemberSeen, "crew.member.seen")
        XCTAssertEqual(TelemetryEventName.crewMemberLost, "crew.member.lost")
    }

    func testRadioPositionAndErrorEventNames() {
        XCTAssertEqual(TelemetryEventName.radioSnapshot, "radio.snapshot")
        XCTAssertEqual(TelemetryEventName.gpsFix, "gps.fix")
        XCTAssertEqual(TelemetryEventName.gpsUplink, "gps.uplink")
        XCTAssertEqual(TelemetryEventName.error, "error")
    }

    func testTriggerValues() {
        XCTAssertEqual(TelemetryTrigger.launch.rawValue, "launch")
        XCTAssertEqual(TelemetryTrigger.auto.rawValue, "auto")
        XCTAssertEqual(TelemetryTrigger.manual.rawValue, "manual")
        XCTAssertEqual(TelemetryTrigger.ladder.rawValue, "ladder")
        XCTAssertEqual(TelemetryTrigger.restore.rawValue, "restore")
    }

    /// Every event name is lowercase, dot-separated, and starts with a
    /// recognised domain — the shape the spec's table promises, not
    /// just the specific strings above.
    func testEveryEventNameFollowsTheDottedDomainConvention() {
        let names = [
            TelemetryEventName.bleScanStart, TelemetryEventName.bleScanStop, TelemetryEventName.bleDiscovered,
            TelemetryEventName.bleConnectAttempt, TelemetryEventName.bleConnected,
            TelemetryEventName.bleHandshakePhase, TelemetryEventName.bleReady, TelemetryEventName.bleDisconnected,
            TelemetryEventName.bleLadderScheduled, TelemetryEventName.bleLadderFired, TelemetryEventName.bleRestore,
            TelemetryEventName.blePower, TelemetryEventName.appForeground, TelemetryEventName.appBackground,
            TelemetryEventName.appLaunch, TelemetryEventName.appTerminate, TelemetryEventName.notifPosted,
            TelemetryEventName.notifTapped, TelemetryEventName.notifAuthorization, TelemetryEventName.adminWrite,
            TelemetryEventName.crewJoin, TelemetryEventName.crewLeave, TelemetryEventName.crewStart,
            TelemetryEventName.crewMemberSeen, TelemetryEventName.crewMemberLost, TelemetryEventName.radioSnapshot,
            TelemetryEventName.gpsFix, TelemetryEventName.gpsUplink, TelemetryEventName.error,
        ]
        let validDomains: Set<Substring> = ["ble", "app", "notif", "admin", "crew", "radio", "gps", "error"]
        for name in names {
            XCTAssertEqual(name, name.lowercased(), "\(name) must be lowercase")
            let domain = name.split(separator: ".", maxSplits: 1).first ?? ""
            XCTAssertTrue(validDomains.contains(domain), "\(name)'s domain \(domain) is not one of \(validDomains)")
        }
    }
}
