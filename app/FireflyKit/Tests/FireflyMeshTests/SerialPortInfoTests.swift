//
//  SerialPortInfoTests.swift — `SerialPortInfo.isUSBSerialAdapter`, the
//  predicate `SerialPort.availablePorts()` uses to exclude Bluetooth and
//  other non-USB `/dev/cu.*` nodes from enumeration.
//
//  No IOKit involved: these are synthetic `SerialPortInfo` values, built
//  with the type's public init, standing in for what IOKit would hand
//  back for a real USB-serial adapter vs. a Bluetooth-only port — this
//  is what makes the filter predicate itself unit-testable independent
//  of real hardware enumeration.
//
#if os(macOS)
import FireflyMesh
import XCTest

final class SerialPortInfoTests: XCTestCase {

    func testUSBSerialAdapterWithASerialNumberIsIncluded() {
        let info = SerialPortInfo(path: "/dev/cu.usbserial-4", usbSerialNumber: "A1B2C3D4")
        XCTAssertTrue(info.isUSBSerialAdapter)
    }

    /// `/dev/cu.Bluetooth-Incoming-Port` — IOKit never attaches a USB
    /// serial number to it, since it isn't a USB device at all. This is
    /// the exact port the review's "excludes Bluetooth ports" checklist
    /// item is about, and `usbSerialNumber: nil` is the faithful
    /// synthetic stand-in for it.
    func testBluetoothPortWithNoSerialNumberIsExcluded() {
        let info = SerialPortInfo(path: "/dev/cu.Bluetooth-Incoming-Port", usbSerialNumber: nil)
        XCTAssertFalse(info.isUSBSerialAdapter)
    }

    /// `/dev/cu.debug-console` — same shape as the Bluetooth port for
    /// this predicate's purposes: no USB serial number, excluded.
    func testDebugConsolePortWithNoSerialNumberIsExcluded() {
        let info = SerialPortInfo(path: "/dev/cu.debug-console", usbSerialNumber: nil)
        XCTAssertFalse(info.isUSBSerialAdapter)
    }

    /// The predicate `availablePorts()` applies is exactly
    /// `usbSerialNumber != nil` — pin that a mixed, synthetic list
    /// filters down to just the USB adapters, the same shape
    /// `availablePorts()`'s own loop performs against real IOKit
    /// results.
    func testFilteringAMixedListKeepsOnlyUSBSerialAdapters() {
        let ports = [
            SerialPortInfo(path: "/dev/cu.usbserial-4", usbSerialNumber: "A1B2C3D4"),
            SerialPortInfo(path: "/dev/cu.Bluetooth-Incoming-Port", usbSerialNumber: nil),
            SerialPortInfo(path: "/dev/cu.usbserial-0001", usbSerialNumber: "5E6F7A8B"),
            SerialPortInfo(path: "/dev/cu.debug-console", usbSerialNumber: nil),
        ]
        let filtered = ports.filter(\.isUSBSerialAdapter)
        XCTAssertEqual(filtered.map(\.path), ["/dev/cu.usbserial-4", "/dev/cu.usbserial-0001"])
    }

    func testEquatableComparesBothFields() {
        let a = SerialPortInfo(path: "/dev/cu.usbserial-4", usbSerialNumber: "A1B2C3D4")
        let b = SerialPortInfo(path: "/dev/cu.usbserial-4", usbSerialNumber: "A1B2C3D4")
        let c = SerialPortInfo(path: "/dev/cu.usbserial-4", usbSerialNumber: "different")
        XCTAssertEqual(a, b)
        XCTAssertNotEqual(a, c)
    }
}
#endif
