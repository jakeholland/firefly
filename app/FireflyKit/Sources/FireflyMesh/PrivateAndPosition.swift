//
//  PrivateAndPosition.swift — the two wire surfaces M1 integration
//  needs that `sendText` does not cover, declared next to the protocol
//  they extend rather than buried in the client:
//
//   1. `ExternalPositionFix` — the phone's own GPS reading, in plain
//      Swift terms, pushed to the connected node as a `POSITION_APP`
//      message with `location_source = LOC_EXTERNAL`
//      (docs/specs/A01-companion-app.md, "Phone GPS -> node"). NOT
//      `AdminMessage.set_fixed_position`: an external fix is a
//      measurement with a time on it, a fixed position is an assertion,
//      and confusing the two would overwrite a bench board's asserted
//      coordinate with a measured one (docs/hardware/heltec-v3.md,
//      "Use 2").
//   2. `IncomingPrivate` — one inbound packet on Firefly's own portnum
//      269 (`FF_PORTNUM`, S04), carried as OPAQUE BYTES. The client
//      never decodes an `ff_proto` frame itself: that is
//      `FireflyModel/Bridge/FireflyPacket.swift`'s job, on the other
//      side of the "C types never leave the bridge" line, exactly as
//      that file's own header says ("`MeshtasticClient` is what
//      actually calls `mc_send_private`/reads inbound private-portnum
//      packets; this type only turns a `Data` payload into or out of an
//      honest Swift value").
//
//  Both are plain value types for the same reason `MeshNodeSnapshot` is
//  one: "protobuf types never leave `FireflyMesh`" (A01, "Data flow").
//
import Foundation

/// One phone GPS reading on its way to the connected node. Every field
/// that can be unknown IS optional — there is no zero-means-absent here
/// either, and nothing downstream may invent a value for a `nil`
/// (`groundSpeed`/`groundTrack` simply go unsent).
///
/// `satsInView` is deliberately ABSENT rather than defaulted: the spec's
/// payload list mentions it, but `CLLocation` does not expose a
/// satellite count on either platform, so there is no honest value to
/// put on the wire and a fabricated 0 would read as a real "0 satellites"
/// to anything that looks. Flagged here rather than silently dropped.
public struct ExternalPositionFix: Sendable, Equatable {
    public let latitude: Double
    public let longitude: Double
    /// Metres, or nil when the fix carries no vertical component.
    public let altitudeMeters: Double?
    /// The moment the fix was TAKEN, not the moment it is sent.
    public let time: Date
    /// Only sent when > 0 (spec's own payload rule).
    public let groundSpeedMetersPerSecond: Double?
    /// Only sent when `0 < value <= 360` (spec's own payload rule); the
    /// caller is responsible for that check, and this encoder re-checks
    /// it rather than trusting it.
    public let groundTrackDegrees: Double?

    public init(latitude: Double, longitude: Double, altitudeMeters: Double?, time: Date,
                groundSpeedMetersPerSecond: Double?, groundTrackDegrees: Double?) {
        self.latitude = latitude
        self.longitude = longitude
        self.altitudeMeters = altitudeMeters
        self.time = time
        self.groundSpeedMetersPerSecond = groundSpeedMetersPerSecond
        self.groundTrackDegrees = groundTrackDegrees
    }
}

/// One inbound packet on Firefly's own private portnum (269). `payload`
/// is the raw `ff_proto` frame — `[ver:1][type:1][body...]` — NOT
/// decoded here; see this file's header.
///
/// Carries the same per-packet meta `IncomingText` does, for the same
/// reason: a PONG's own RSSI is the single fact FIND exists to report,
/// and re-deriving it from the NodeDB entry would silently attribute a
/// relayed packet's reading to the wrong radio.
public struct IncomingPrivate: Sendable, Equatable {
    public let from: UInt32
    public let to: UInt32
    public let channel: UInt32
    public let packetID: UInt32
    public let payload: Data
    public let rxTime: Date?
    public let rssiDbm: Int16?
    public let snrDb: Float?
    public let direct: Bool?

    public init(from: UInt32, to: UInt32, channel: UInt32, packetID: UInt32, payload: Data,
                rxTime: Date?, rssiDbm: Int16?, snrDb: Float?, direct: Bool?) {
        self.from = from
        self.to = to
        self.channel = channel
        self.packetID = packetID
        self.payload = payload
        self.rxTime = rxTime
        self.rssiDbm = rssiDbm
        self.snrDb = snrDb
        self.direct = direct
    }
}

/// Firefly's own Meshtastic portnum (`FF_PORTNUM`, S04). Declared here
/// rather than imported from `FireflyCore` so `FireflyMesh` keeps one
/// obvious place where the wire constant lives; `FireflyModel`'s
/// `fireflyPortNum` reads it straight out of the C header, and
/// `MeshtasticClientTests` pins the two against each other.
public let fireflyPrivatePortNum: UInt32 = 269

/// The BLE node-picker seam (A01's M1 Connect bullet: "node picker (BLE
/// on both platforms)"). Declared as a protocol so the Connect screen
/// depends on a seam rather than on `BLETransport` itself — the same
/// rule every other screen follows — and so a test can drive the picker
/// with no CoreBluetooth anywhere near it. `BLETransport` conforms as
/// it stands; its `scan()`/`stopScanning()`/`setPreferredPeripheral(_:)`
/// are already exactly these three operations.
public protocol NodeScanning: AnyObject, Sendable {
    /// A fresh subscription, multicast via `EventHub` like every other
    /// stream in this app (S1). Starts the scan as a side effect once
    /// the radio is powered on; a radio that never powers on (permission
    /// denied, Bluetooth off) simply yields nothing, never a fabricated
    /// peripheral.
    func scan() async -> AsyncStream<BLEDiscoveredPeripheral>
    func stopScanning() async
    /// The peripheral a subsequent `connect()` should prefer over
    /// whatever else is advertising.
    func setPreferredPeripheral(_ id: UUID?) async
}
