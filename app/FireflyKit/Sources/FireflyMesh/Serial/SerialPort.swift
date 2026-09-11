//
//  SerialPort.swift — POSIX termios I/O over `/dev/cu.*`
//  (docs/specs/A01-companion-app.md, "Serial — macOS only").
//
//  macOS only, deliberately: there is no iOS equivalent (no app can open
//  a `/dev/cu.*` node in the sandbox), and pretending otherwise would
//  just produce a dead code path — the same reasoning the spec states.
//
//  115200 8N1, `CS8|CREAD|CLOCAL`, `VMIN=0 VTIME=1` — a read call
//  returns whatever bytes are ready within ~100ms rather than blocking
//  forever, so a `DispatchSourceRead` on a dedicated queue can drive it
//  without ever blocking the caller's thread. That queue, not
//  `@MainActor`, is where bytes land first — matching the spec's
//  threading model: "the serial read source ... land off the main
//  thread and are funnelled into the client actor".
//
//  Framing is NOT this file's job. `SerialPort` moves raw bytes; the
//  0x94 0xC3 framing that turns them into `FromRadio` protobufs is
//  `StreamFramer`'s (shared, landed already) and is applied one layer
//  up, in `SerialTransport`.
//
#if os(macOS)
import Darwin
import Foundation
import IOKit
import IOKit.serial

/// A discovered `/dev/cu.*` node, IOKit's own picture of it.
public struct SerialPortInfo: Sendable, Equatable {
    public let path: String
    /// Present only for a real USB-serial adapter. `nil` filters OUT
    /// `/dev/cu.debug-console` and `/dev/cu.Bluetooth-Incoming-Port` —
    /// neither is a Meshtastic node, and offering them in a picker is
    /// how someone ends up trying to `want_config` the debug console.
    public let usbSerialNumber: String?

    /// Public so callers — including tests, from a different module —
    /// can construct synthetic values to exercise `isUSBSerialAdapter`
    /// and `availablePorts()`'s exclusion without needing real IOKit
    /// enumeration to produce a Bluetooth-shaped one.
    public init(path: String, usbSerialNumber: String?) {
        self.path = path
        self.usbSerialNumber = usbSerialNumber
    }

    /// True for a real USB-serial adapter's port; false for a
    /// Bluetooth-only or other non-USB `/dev/cu.*` node (no USB serial
    /// number at all — e.g. `/dev/cu.Bluetooth-Incoming-Port` or
    /// `/dev/cu.debug-console`). `availablePorts()` applies this
    /// exclusion itself (see below) rather than leaving it purely to
    /// callers.
    public var isUSBSerialAdapter: Bool {
        usbSerialNumber != nil
    }
}

enum SerialPortError: Error, Equatable, Sendable {
    case openFailed(path: String, errno: Int32)
    case configureFailed(path: String, errno: Int32)
    case notOpen
    case writeFailed(errno: Int32)
}

/// Low-level owner of one open serial file descriptor. `SerialTransport`
/// is the `MeshTransport` conformance built on top of this; this type
/// knows nothing about `ToRadio`/`FromRadio`, `EventHub`, or Meshtastic
/// at all — just bytes in, bytes out, one port.
// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access to `readSource`/`closed` goes through `stateLock`,
// never unguarded; `fd`/`path`/`readQueue` are immutable after `init`.
final class SerialPort: @unchecked Sendable {
    private let fd: Int32
    private let path: String
    private let readQueue: DispatchQueue
    private var readSource: DispatchSourceRead?
    private let stateLock = NSLock()
    private var closed = false

    /// Opens and configures `path` at 115200 8N1. Throws rather than
    /// falling back to a default — a silently misconfigured port is
    /// exactly the "connects fine, receives nothing" failure class the
    /// spec calls out for BLE's subscription race, and serial should
    /// not have its own quiet version of it.
    init(path: String) throws {
        let fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { throw SerialPortError.openFailed(path: path, errno: errno) }

        guard isatty(fd) != 0 else {
            Darwin.close(fd)
            throw SerialPortError.openFailed(path: path, errno: ENOTTY)
        }

        var raw = termios()
        guard tcgetattr(fd, &raw) == 0 else {
            let e = errno
            Darwin.close(fd)
            throw SerialPortError.configureFailed(path: path, errno: e)
        }
        cfmakeraw(&raw)
        cfsetispeed(&raw, speed_t(B115200))
        cfsetospeed(&raw, speed_t(B115200))
        raw.c_cflag |= tcflag_t(CS8 | CREAD | CLOCAL)
        raw.c_cflag &= ~tcflag_t(PARENB | CSTOPB | CRTSCTS)
        withUnsafeMutableBytes(of: &raw.c_cc) { bytes in
            let cc = bytes.bindMemory(to: cc_t.self)
            cc[Int(VMIN)] = 0
            cc[Int(VTIME)] = 1
        }
        guard tcsetattr(fd, TCSANOW, &raw) == 0 else {
            let e = errno
            Darwin.close(fd)
            throw SerialPortError.configureFailed(path: path, errno: e)
        }
        // Drop O_NONBLOCK now that the port is configured: VMIN=0/
        // VTIME=1 already governs read timing (return within ~100ms
        // with whatever is available), and DispatchSourceRead tells us
        // when a read would return something anyway.
        _ = fcntl(fd, F_SETFL, 0)

        self.fd = fd
        self.path = path
        self.readQueue = DispatchQueue(label: "firefly.serial.\(path)")
    }

    /// Start delivering inbound bytes. `onData`/`onError` run on this
    /// port's own dedicated queue — never the caller's thread, and
    /// never `@MainActor` (see file header).
    func startReading(onData: @escaping (Data) -> Void, onError: @escaping (Int32) -> Void) {
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: readQueue)
        source.setEventHandler { [fd] in
            var buffer = [UInt8](repeating: 0, count: 4096)
            let n = buffer.withUnsafeMutableBytes { raw -> Int in
                Darwin.read(fd, raw.baseAddress, raw.count)
            }
            if n > 0 {
                onData(Data(buffer[0..<n]))
            } else if n == 0 {
                onError(0) // EOF: the device went away.
            } else {
                let e = errno
                if e != EAGAIN && e != EWOULDBLOCK {
                    onError(e)
                }
            }
        }
        source.setCancelHandler { [fd] in
            Darwin.close(fd)
        }
        stateLock.lock()
        readSource = source
        stateLock.unlock()
        source.resume()
    }

    /// Blocking write of the full buffer, retrying on `EAGAIN` — the fd
    /// is `O_NONBLOCK` was cleared above, but a partial `write(2)` is
    /// still legal POSIX behaviour and must be looped rather than
    /// assumed away.
    func write(_ data: Data) throws {
        stateLock.lock()
        let isClosed = closed
        stateLock.unlock()
        guard !isClosed else { throw SerialPortError.notOpen }

        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            guard let base = raw.baseAddress, raw.count > 0 else { return }
            var offset = 0
            while offset < raw.count {
                let n = Darwin.write(fd, base.advanced(by: offset), raw.count - offset)
                if n > 0 {
                    offset += n
                } else if n < 0, errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR {
                    continue
                } else {
                    throw SerialPortError.writeFailed(errno: errno)
                }
            }
        }
    }

    /// Idempotent. Cancelling the read source's cancel handler is what
    /// actually closes the fd, so a port that never started reading
    /// closes it directly here instead.
    func close() {
        stateLock.lock()
        guard !closed else { stateLock.unlock(); return }
        closed = true
        let source = readSource
        stateLock.unlock()
        if let source {
            source.cancel()
        } else {
            Darwin.close(fd)
        }
    }

    deinit {
        close()
    }
}

// MARK: - Port enumeration

extension SerialPort {
    /// Every `/dev/cu.*` IOKit knows about that is a real USB-serial
    /// adapter — `/dev/cu.debug-console`, `/dev/cu.Bluetooth-Incoming-Port`,
    /// and any other non-USB node are excluded HERE (via
    /// `SerialPortInfo.isUSBSerialAdapter`), not left for every future
    /// caller (a node picker, `bench_friend.sh`'s default-port guess)
    /// to remember to filter on `usbSerialNumber != nil` themselves —
    /// see the spec: "keep only those with a USB serial number."
    public static func availablePorts() -> [SerialPortInfo] {
        var out: [SerialPortInfo] = []
        let matching = IOServiceMatching(kIOSerialBSDServiceValue)
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return out
        }
        defer { IOObjectRelease(iterator) }

        while case let service = IOIteratorNext(iterator), service != 0 {
            defer { IOObjectRelease(service) }
            guard let calloutProp = IORegistryEntryCreateCFProperty(
                service, kIOCalloutDeviceKey as CFString, kCFAllocatorDefault, 0
            ) else { continue }
            guard let path = calloutProp.takeRetainedValue() as? String else { continue }
            let info = SerialPortInfo(path: path, usbSerialNumber: usbSerialNumber(walkingUpFrom: service))
            if info.isUSBSerialAdapter {
                out.append(info)
            }
        }
        return out
    }

    /// Walks the IOKit registry plane upward from a serial-BSD service
    /// looking for its USB device's serial number. A `/dev/cu.*` entry
    /// itself never carries this property — it lives a few levels up,
    /// on the USB device node — so this climbs the provider chain
    /// rather than reading one entry.
    private static func usbSerialNumber(walkingUpFrom service: io_object_t, maxDepth: Int = 8) -> String? {
        var current = service
        var ownsCurrent = false
        defer { if ownsCurrent { IOObjectRelease(current) } }

        for _ in 0..<maxDepth {
            if let prop = IORegistryEntryCreateCFProperty(
                current, "USB Serial Number" as CFString, kCFAllocatorDefault, 0
            ) {
                return prop.takeRetainedValue() as? String
            }
            var parent: io_registry_entry_t = 0
            let kr = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent)
            if ownsCurrent { IOObjectRelease(current) }
            guard kr == KERN_SUCCESS, parent != 0 else { return nil }
            current = parent
            ownsCurrent = true
        }
        return nil
    }
}
#endif
