//
//  MeshtasticClientProtocol.swift — the seam every view model talks to.
//
//  View models depend on THIS, never on CoreBluetooth, never on a
//  concrete client. That is what lets the whole UI be exercised in unit
//  tests, and what lets the iOS Simulator (which has no Bluetooth at
//  all) run the app at all.
//
import Foundation

public enum LinkState: Equatable, Sendable {
    case disconnected
    case connecting
    /// Transport up, `want_config` sent, config dump in flight.
    case handshaking
    /// `config_complete_id` matched. Only now is the nodeDB meaningful.
    case ready
    case failed(String)
}

/// A node the radio has told us about. Every field that can be unknown
/// IS optional — no sentinels, no zero-means-absent (the rule
/// docs/specs/S03-meshclient.md AC9/AC10/AC11 pin for the C client).
public struct MeshNodeSnapshot: Sendable, Equatable, Identifiable {
    public var id: UInt32 { num }
    public let num: UInt32
    public let shortName: String?
    public let longName: String?
    /// nil = the radio never reported a position for this node.
    public let position: NodePosition?
    /// nil = never heard.
    public let lastHeard: Date?
    /// Attributable to `num` ONLY when the packet arrived directly;
    /// a relayed packet's RSSI belongs to the relay.
    public let rssiDbm: Int16?
    public let snrDb: Float?
    public let hopsAway: UInt32?

    public init(num: UInt32, shortName: String?, longName: String?, position: NodePosition?,
                lastHeard: Date?, rssiDbm: Int16?, snrDb: Float?, hopsAway: UInt32?) {
        self.num = num
        self.shortName = shortName
        self.longName = longName
        self.position = position
        self.lastHeard = lastHeard
        self.rssiDbm = rssiDbm
        self.snrDb = snrDb
        self.hopsAway = hopsAway
    }
}

public struct NodePosition: Sendable, Equatable {
    public enum Source: Sendable, Equatable {
        /// The sender said nothing. Never render this as a GPS fix.
        case unknown
        /// Somebody typed it in. Asserted, not measured — freshness is a
        /// category error for it (docs/hardware/heltec-v3.md, issue #33).
        case manual
        case internalGPS
        case externalGPS
    }
    public let latitude: Double
    public let longitude: Double
    public let time: Date?
    public let source: Source
    /// nil = the sender did not state precision. NOT "full precision":
    /// the default public channel truncates to a ~5.8 km grid and this
    /// field is the only wire-level tell (issue #47).
    public let precisionBits: UInt8?

    public init(latitude: Double, longitude: Double, time: Date?, source: Source, precisionBits: UInt8?) {
        self.latitude = latitude
        self.longitude = longitude
        self.time = time
        self.source = source
        self.precisionBits = precisionBits
    }
}

public protocol MeshtasticClientProtocol: AnyObject, Sendable {
    /// A fresh, independent stream for the caller. Multicast via
    /// `EventHub` (docs/specs/A01-companion-app.md, S1): a view model
    /// AND `CoreStore` (and, for `linkState`, Diagnostics too) each need
    /// their own subscription, so this is a method, not a stored
    /// `AsyncStream` property — a second `for await` on one shared
    /// instance would compete with the first for elements rather than
    /// getting its own copy. Every stream is `.bufferingNewest(4096)`.
    func linkState() -> AsyncStream<LinkState>
    func nodeUpdates() -> AsyncStream<MeshNodeSnapshot>
    /// One `DeliveryEvent` per WAITING/SENT/DELIVERED/NO_ACK(NAK)/DROPPED
    /// transition this client observes. The ack-TIMEOUT half of NO_ACK is
    /// deliberately not reported here — it has no per-message key to
    /// give, and is instead `CoreStore.tick(nowMs:)`'s job, mirroring
    /// `ff_feed_expire_pending_acks` being a tick sweep on the puck too.
    func deliveryUpdates() -> AsyncStream<DeliveryEvent>

    func connect() async throws
    func disconnect() async
    /// Returns the packet id the radio assigned, so the caller can match
    /// a later Routing ack to this message.
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32
}

/// Broadcast address — `0xFFFFFFFF`, Meshtastic's own.
public let meshBroadcastAddress: UInt32 = 0xFFFF_FFFF

/// Milestone-1 stand-in. Reaches `.ready` over whatever transport it is
/// given and records what was sent; it NEVER invents nodes, positions or
/// incoming messages. An empty Radar on a stub client is the honest
/// answer, and it is the same empty Radar a real radio with nothing in
/// range produces.
public final class StubMeshtasticClient: MeshtasticClientProtocol, @unchecked Sendable {
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()

    private let transport: MeshTransport
    private let lock = NSLock()
    // Two INDEPENDENT counters, deliberately never sharing a value space
    // — a stand-in for `shell_next_outbox_id` (the retry-queue key,
    // assigned before a send is even attempted) and the radio's own
    // packet id (known only once the transport accepts the send). Same
    // two-key split `ff_feed.h`/`ff_shell.c` use; keeping them numerically
    // distinct here is what makes a test that accidentally aliases them
    // (PR #261 review, finding 1) impossible to write by coincidence.
    private var nextOutboxID: UInt32 = 1
    private var nextPacketID: UInt32 = 1_000_001
    private var sentTexts: [(String, UInt32, Bool)] = []

    public init(transport: MeshTransport = LoopbackTransport()) {
        self.transport = transport
    }

    public func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    public func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    public func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }

    public func connect() async throws {
        linkHub.yield(.connecting)
        try await transport.connect()
        linkHub.yield(.handshaking)
        linkHub.yield(.ready)
    }

    public func disconnect() async {
        await transport.disconnect()
        linkHub.yield(.disconnected)
    }

    @discardableResult
    public func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        let outboxID = nextOutbox(text: text, destination: destination, wantAck: wantAck)
        deliveryHub.yield(.waiting(outboxID: OutboxID(outboxID)))
        try await transport.send(Data(text.utf8))
        let packetID = nextPacket()
        deliveryHub.yield(.sent(outboxID: OutboxID(outboxID), packetID: PacketID(packetID), wantAck: wantAck))
        // No DELIVERED is fabricated. A stub has no mesh to ack it, and
        // a broadcast would never be acked even by a real one.
        return packetID
    }

    // Non-async on purpose — see LoopbackTransport.record(_:).
    private func nextOutbox(text: String, destination: UInt32, wantAck: Bool) -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        let i = nextOutboxID
        nextOutboxID &+= 1
        sentTexts.append((text, destination, wantAck))
        return i
    }

    private func nextPacket() -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        let i = nextPacketID
        nextPacketID &+= 1
        return i
    }

    public var sentTextLog: [(String, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }
        return sentTexts
    }
}
