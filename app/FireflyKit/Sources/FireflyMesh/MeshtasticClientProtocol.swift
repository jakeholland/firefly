//
//  MeshtasticClientProtocol.swift — the seam every view model talks to.
//
//  View models depend on THIS, never on CoreBluetooth, never on a
//  concrete client. That is what lets the whole UI be exercised in unit
//  tests, and what lets the iOS Simulator (which has no Bluetooth at
//  all) run the app at all.
//
import Foundation
import MeshtasticProto

public enum LinkState: Equatable, Sendable {
    case disconnected
    case connecting
    /// Transport up, `want_config` sent, config dump in flight.
    case handshaking
    /// `config_complete_id` matched. Only now is the nodeDB meaningful.
    case ready
    /// M2 — background BLE: an unexpected loss after a session that had
    /// already reached `.ready` once, and the client is trying to get
    /// back — either still waiting on the TRANSPORT to come back up (a
    /// pocket-loss reconnect or a node power-cycle: `BLETransport`'s own
    /// reconnect-on-loss, silently re-arming a pending `central.connect()`
    /// and, past `BLETransport.reconnectFallbackDelay`, falling back to a
    /// scan — see that type's own doc comment), or already back up and
    /// re-running the `want_config` handshake with bounded exponential
    /// backoff (`MeshtasticClient.handshakeRetryDelay(forAttempt:)`).
    /// `attempt` is 1-based: `1` is published the INSTANT the loss is
    /// detected (`consumeTransportEvents`'s `.disconnected` case) — before
    /// the transport has necessarily come back at all — and again for the
    /// first genuinely-repeated attempt if a handshake attempt afterward
    /// itself times out; each is distinct from the plain `.handshaking`
    /// case (the FIRST, never-yet-failed handshake attempt right after an
    /// explicit `connect()`, or the first attempt right after the
    /// transport comes back from a loss). Root-caused 2026-09-11 against
    /// a real power-cycle on the bench
    /// (`FireflyHardwareTests.testReconnectsOnItsOwnAfterFirefly2IsPowerCycled`):
    /// before this, NOTHING was published between `.disconnected` and the
    /// transport eventually reaching `.ready` again — a UI (and this very
    /// test) reading total silence for up to 180s, indistinguishable from
    /// having given up, while `BLETransport` was in fact still honestly
    /// trying. A UI that only ever says "DISCONNECTED" (or silently sits
    /// on a stale `.handshaking`) during a multi-minute reconnect is not
    /// telling the truth about what is actually happening.
    case reconnecting(attempt: Int)
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

/// Finding 2 (first real-radio session): the passive read seam Settings
/// needs so "Region UNKNOWN" / "Channel UNKNOWN" stop being permanent —
/// want_config's own `Config`, `Channel` and self `NodeInfo` frames
/// already carry the node's current owner name, region, modem preset
/// and primary channel; before this type existed, `MeshtasticClient`
/// decoded and then DROPPED every one of those (`.config`/`.channel`
/// were not even matched in `handle(fromRadio:)`'s switch — see the
/// `default: break` that used to catch `.config` there).
///
/// Every field is independently optional — "unset" and "not yet
/// reported by this handshake" are the same honest nil, never a
/// placeholder. Nothing here is a second source of truth for a write:
/// `applyChannelSet`/`setOwner`/`setRegion` still read back and report
/// their OWN authoritative result (`ChannelWriteReport`/
/// `OwnerWriteReport`/`RegionWriteReport`); this snapshot is refreshed
/// FROM that same read-back so a passive reader (Settings) sees the
/// same values an active writer already confirmed, without a second
/// round trip.
public struct NodeConfigSnapshot: Sendable, Equatable {
    /// `User.long_name` / `User.short_name` for OUR OWN connected node
    /// (never a remote one) — the exact fields `setOwner` writes.
    public var ownerLongName: String?
    public var ownerShortName: String?
    public var region: Config.LoRaConfig.RegionCode?
    public var modemPreset: Config.LoRaConfig.ModemPreset?
    /// The PRIMARY channel's name (`Channel.Role.primary`). Meshtastic
    /// ships its own stock preset with an EMPTY name (the modem preset
    /// name, e.g. "LongFast", is implied rather than stored in
    /// `settings.name`) — so `nil` here means "no primary channel
    /// reported yet by this handshake" while `""` means "reported, and
    /// the node genuinely left it blank." A UI wanting a human label for
    /// the blank case says so itself (e.g. "(default channel)" —
    /// `SettingsViewModel.currentChannelName`'s own convention); this
    /// type never guesses one.
    public var primaryChannelName: String?

    public init(ownerLongName: String? = nil, ownerShortName: String? = nil,
                region: Config.LoRaConfig.RegionCode? = nil,
                modemPreset: Config.LoRaConfig.ModemPreset? = nil,
                primaryChannelName: String? = nil) {
        self.ownerLongName = ownerLongName
        self.ownerShortName = ownerShortName
        self.region = region
        self.modemPreset = modemPreset
        self.primaryChannelName = primaryChannelName
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

/// One inbound `TEXT_MESSAGE_APP` packet, decoded off `handle(meshPacket:)`
/// (PR #264 review, BLOCKING item 2 — "nothing decodes and republishes
/// an inbound TEXT_MESSAGE_APP packet"). Carries everything a consumer
/// needs to route and dedup it without reaching back into the client:
/// `from`/`to`/`channel`/`packetID` off the packet itself, `text`
/// decoded from the payload, `rxTime` the same way `NodeDB`'s position
/// path reads it, and the same per-packet RSSI/SNR/hop-path meta
/// `applyRxMeta` computes for the sender's NodeDB entry — reported here
/// too since a message bubble is exactly the other place that meta is
/// worth showing. `RxPath` itself never crosses this boundary (its own
/// doc comment); `direct` is its plain-`Bool?` translation — nil when
/// the hop path could not be established, never "assume DIRECT".
public struct IncomingText: Sendable, Equatable {
    public let from: UInt32
    public let to: UInt32
    public let channel: UInt32
    public let packetID: UInt32
    public let text: String
    public let rxTime: Date?
    public let rssiDbm: Int16?
    public let snrDb: Float?
    public let direct: Bool?

    public init(from: UInt32, to: UInt32, channel: UInt32, packetID: UInt32, text: String,
                rxTime: Date?, rssiDbm: Int16?, snrDb: Float?, direct: Bool?) {
        self.from = from
        self.to = to
        self.channel = channel
        self.packetID = packetID
        self.text = text
        self.rxTime = rxTime
        self.rssiDbm = rssiDbm
        self.snrDb = snrDb
        self.direct = direct
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
    /// One `IncomingText` per inbound `TEXT_MESSAGE_APP` packet this
    /// client decodes — see `IncomingText`'s own doc comment. Echo-dedup
    /// (dropping a self-originated broadcast reflected back with my own
    /// packet id) is deliberately NOT this stream's job: the client has
    /// no durable notion of "packets I sent" once `pendingSends` has
    /// already been pruned or acked, so that guard lives at the one
    /// place that actually knows which packet ids it minted itself
    /// (`InboxViewModel.observe()` / `InMemoryInboxStore.push`).
    func incomingTexts() -> AsyncStream<IncomingText>
    /// One `IncomingPrivate` per inbound packet on Firefly's own
    /// portnum 269, carried as OPAQUE BYTES — see `IncomingPrivate`'s
    /// own doc comment for why the client does not decode it.
    func incomingPrivate() -> AsyncStream<IncomingPrivate>

    /// The connected node's own `num` (`my_info.my_node_num`), or nil
    /// before the handshake has produced one.
    ///
    /// SYNCHRONOUS, and `nonisolated` on the actor that implements it,
    /// for one concrete caller: `PhoneGPSUplink`'s `destinationNodeNum`
    /// is a synchronous `@Sendable () -> UInt32?` closure evaluated per
    /// fix (`LocationProvider.swift`), and an `await` there would put an
    /// actor hop on the cadence check of every GPS reading. Reading this
    /// takes a lock, not an actor hop.
    var connectedNodeNum: UInt32? { get }

    func connect() async throws
    func disconnect() async
    /// Returns the packet id the radio assigned, so the caller can match
    /// a later Routing ack to this message.
    @discardableResult
    func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32
    /// Push a phone GPS fix to `destination` (the connected node itself)
    /// as `POSITION_APP` with `location_source = LOC_EXTERNAL` — see
    /// `ExternalPositionFix`. Returns the packet id, same contract as
    /// `sendText`. Never `want_ack`: a position report is not a message
    /// somebody is waiting on, and an ack for one would be noise.
    @discardableResult
    func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32
    /// Send an already-encoded `ff_proto` frame on portnum 269 — the
    /// FLARE / RALLY / PING / PONG path (S04). `destination` is
    /// `meshBroadcastAddress` for a whole-crew send. The bytes are
    /// opaque here; `FireflyModel`'s `FireflyPacket.encode()` made them.
    @discardableResult
    func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32

    /// M3 — "Channel write-back (admin messages) behind an explicit
    /// confirmation" (docs/specs/A01-companion-app.md, M3). Writes every
    /// channel in `request` (in order), plus the LoRa config it carries
    /// when it carries one, to OUR OWN connected node — never a remote
    /// one, and never a PSK the app minted itself (the owner's decision:
    /// QR/URL import only). Wrapped in `begin_edit_settings`/
    /// `commit_edit_settings` so the firmware saves and reboots once, not
    /// once per channel (Meshtastic-Apple's own
    /// `AccessoryManager+ToRadio.swift`, `beginEditSettings`'s doc
    /// comment, cross-checked against `AdminModule.cpp`). Read back after
    /// the commit and compared against what was sent — honest success is
    /// "the node now reports what was written"; a mismatch, or a node
    /// that never comes back from the reboot a commit triggers, throws
    /// `AdminWriteError` rather than assuming success.
    @discardableResult
    func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport

    /// M3 — `set_owner`, wrapped and read back the same way as
    /// `applyChannelSet`.
    @discardableResult
    func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport

    /// M3 — reads the node's OWN current LoRa config first (so nothing
    /// besides `region` changes — `set_config.lora` replaces the whole
    /// submessage on the wire, not just the field named), writes it back
    /// with only `region` changed, and reads it back the same way as
    /// `applyChannelSet`. `.unset` is Meshtastic's own "radio disabled"
    /// sentinel; this throws `AdminWriteError.regionUnset` rather than
    /// writing it — defense-in-depth inside the client itself, not only
    /// the Settings screen's disabled APPLY button (PR #274 review,
    /// SHOULD-FIX 5).
    @discardableResult
    func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport

    /// M3 (PR #274 review, BLOCKING 1 & 2): the connected node's current
    /// channel table, read LIVE off the radio (never a cache) — one
    /// `Channel` per index `0..<maxChannelSlots` the node reports as
    /// occupied (role `.primary`/`.secondary`; an index the radio reports
    /// `.disabled` for is simply absent here). This is what an "add"
    /// import's free-slot placement, and a "replace" import's untouched-
    /// index disclosure, are planned against
    /// (`ChannelImportResult.makeChannelWritePlan(occupiedIndexes:)`) —
    /// computed BEFORE any write, so the confirmation sheet can state
    /// exactly which slots are free.
    func currentChannelTable() async throws -> [Channel]

    /// Finding 2 — the passive read seam: a fresh, independent,
    /// CURRENT-VALUE stream of the connected node's own config as
    /// want_config (and every subsequent admin write's read-back) fills
    /// it in — same multicast + replay contract as `linkState()`
    /// (`CurrentValueEventHub`, S1 / M1 review follow-up #267): a
    /// Settings screen opened AFTER the handshake already completed
    /// sees the real values immediately, not silence until the next
    /// change.
    func nodeConfigUpdates() -> AsyncStream<NodeConfigSnapshot>
    /// Synchronous snapshot read, same convention as `connectedNodeNum`
    /// (that property's own doc comment) — nil before anything has ever
    /// been reported.
    var connectedNodeConfig: NodeConfigSnapshot? { get }
}

/// Default "reports nothing yet" implementation for finding 2's two new
/// requirements, so every OTHER existing conformer (test mocks in
/// `FireflyModelTests`/`FireflyAppTests` that predate this finding, none
/// of which exercise node-config reads) keeps compiling without change —
/// an honest empty answer is exactly what those mocks already report for
/// everything else they don't model. `StubMeshtasticClient`,
/// `DemoMeshtasticClient` and the real `MeshtasticClient` each override
/// both with real behaviour (PR #282 review, SHOULD-FIX: this doc
/// comment was false for `DemoMeshtasticClient` until this fix — it
/// still silently fell through to this same "reports nothing" default;
/// see `DemoMeshtasticClient.scriptedNodeConfig`).
extension MeshtasticClientProtocol {
    public func nodeConfigUpdates() -> AsyncStream<NodeConfigSnapshot> {
        AsyncStream { $0.finish() }
    }
    public var connectedNodeConfig: NodeConfigSnapshot? { nil }
}

// MARK: - M3: channel/config write-back (admin messages) — shared types

/// Slots the radio keeps. A fixed array with a role per slot, not a list
/// that grows — Meshtastic-Apple's own `Channels.swift:59` constant,
/// cross-checked rather than assumed (PR #274 review, BLOCKING 1 & 2).
public let maxChannelSlots: Int32 = 8

/// One admin write's worth of channels, plus the LoRa config the write
/// must carry with it when the imported URL was a full "replace" (it is
/// nil for an "add" import, which never carries one — `FireflyModel`'s
/// `ChannelURL`/`ChannelSet` is where this is actually built from an
/// imported channel-share URL; `FireflyMesh` never touches a URL).
public struct ChannelWriteRequest: Sendable, Equatable {
    public var channels: [Channel]
    public var loraConfig: Config.LoRaConfig?

    public init(channels: [Channel], loraConfig: Config.LoRaConfig? = nil) {
        self.channels = channels
        self.loraConfig = loraConfig
    }
}

public enum AdminWriteError: Error, Equatable, Sendable {
    /// No connected node to address the admin message to.
    case notConnected
    case encodingFailed
    /// The node never answered a read (request or read-back), or never
    /// came back after the reboot a `commit_edit_settings` triggers.
    case timeout
    /// The write reached the node, but the read-back that followed does
    /// not match what was sent — a clear, honest failure rather than an
    /// assumed success. For `applyChannelSet`'s multi-item read-back the
    /// associated string is a per-item report (which matched, which
    /// didn't — PR #274 review, SHOULD-FIX 7), not just the first
    /// mismatch found; it names when more than one item was involved and
    /// therefore the node may now hold a mix of old and new state.
    case readBackMismatch(String)
    /// `setRegion(.unset)` was called. `.unset` is Meshtastic's own
    /// "radio disabled" sentinel, never something to write on purpose —
    /// PR #274 review, SHOULD-FIX 5.
    case regionUnset
    /// `applyChannelSet`'s SEND phase (before any read-back) failed
    /// partway through a multi-item write — `step` names exactly which
    /// item failed to send (e.g. "channel 1 (Ops)" or "LoRa config");
    /// `underlying` is what actually went wrong. Every item sent before
    /// `step` may already be committed to the node, so the node may be
    /// left holding a mix of old and new state — PR #274 review,
    /// SHOULD-FIX 7.
    case partialApplyFailed(step: String, underlying: String)
}

public struct ChannelWriteReport: Sendable, Equatable {
    /// Read back from the node after the commit — not simply an echo of
    /// what `applyChannelSet` was asked to send.
    public let channels: [Channel]
    public let loraConfig: Config.LoRaConfig?

    public init(channels: [Channel], loraConfig: Config.LoRaConfig?) {
        self.channels = channels
        self.loraConfig = loraConfig
    }
}

public struct OwnerWriteReport: Sendable, Equatable {
    public let longName: String
    public let shortName: String

    public init(longName: String, shortName: String) {
        self.longName = longName
        self.shortName = shortName
    }
}

public struct RegionWriteReport: Sendable, Equatable {
    public let region: Config.LoRaConfig.RegionCode

    public init(region: Config.LoRaConfig.RegionCode) {
        self.region = region
    }
}

/// Broadcast address — `0xFFFFFFFF`, Meshtastic's own.
public let meshBroadcastAddress: UInt32 = 0xFFFF_FFFF

/// The ONE broadcast-vs-direct routing rule every inbound-item path
/// must agree on (PR #271 review, SHOULD-FIX 2) — `InboxViewModel.
/// ingest`'s ordinary-text routing and `AppGraph.pushInboundFeedItem`'s
/// M2 FLARE/RALLY/STATUS routing both call this, so the same `to` value
/// can never land in the CREW thread down one path and a 1:1 thread
/// down the other.
///
/// True only for the wire's actual broadcast address. `to == 0` is
/// protobuf's zero-default for an unset field, not a real broadcast — a
/// real puck should never send it, and treating it as broadcast here
/// would silently paper over a sender bug rather than surface it (the
/// same "decode is strict, no defensive slack" reasoning S04's
/// Amendments section already applies to trailing bytes).
public func isBroadcastDestination(_ to: UInt32) -> Bool {
    to == meshBroadcastAddress
}

/// Milestone-1 stand-in. Reaches `.ready` over whatever transport it is
/// given and records what was sent; it NEVER invents nodes, positions or
/// incoming messages. An empty Radar on a stub client is the honest
/// answer, and it is the same empty Radar a real radio with nothing in
/// range produces.
// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — the hubs
// above are already thread-safe on their own, and every OTHER mutable
// access below goes through `lock`, never unguarded.
public final class StubMeshtasticClient: MeshtasticClientProtocol, @unchecked Sendable {
    // `CurrentValueEventHub`, not `EventHub` (M1 review follow-up,
    // #267): `linkState()` needs current-value semantics so a late
    // subscriber (Thread/Diagnostics opened after `.ready`) sees the
    // real state immediately instead of a stale `.disconnected` — see
    // `CurrentValueEventHub`'s own doc comment.
    private let linkHub = CurrentValueEventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    // Declared but never yielded to — see this class's own header
    // comment ("NEVER invents... incoming messages") and
    // `incomingTexts()`'s protocol doc comment.
    private let incomingTextHub = EventHub<IncomingText>()
    // Same rule: a stub has no mesh, so no FLARE, RALLY or PONG ever
    // arrives on it. Tests that need one inject exact bytes.
    private let incomingPrivateHub = EventHub<IncomingPrivate>()

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
    private var sentPositions: [(ExternalPositionFix, UInt32)] = []
    private var sentPrivate: [(Data, UInt32, Bool)] = []
    private var _connectedNodeNum: UInt32?

    public init(transport: MeshTransport = LoopbackTransport()) {
        self.transport = transport
    }

    public func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    public func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    public func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    public func incomingTexts() -> AsyncStream<IncomingText> { incomingTextHub.subscribe() }
    public func incomingPrivate() -> AsyncStream<IncomingPrivate> { incomingPrivateHub.subscribe() }

    /// nil until a test sets it (`connectedNodeNum = 48_621_524`). The
    /// stub has no `my_info` to learn one from, and inventing one would
    /// make a GPS uplink push to a node that does not exist — exactly
    /// the class of fabrication this type exists to refuse.
    public var connectedNodeNum: UInt32? {
        get { lock.lock(); defer { lock.unlock() }; return _connectedNodeNum }
        set { lock.lock(); defer { lock.unlock() }; _connectedNodeNum = newValue }
    }

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

    /// Records and returns a packet id; sends nothing anywhere, exactly
    /// like `sendText`. A stub has no radio to tell a position to.
    @discardableResult
    public func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        try await transport.send(Data())
        return recordPosition(fix, to: destination)
    }

    /// Records and returns a packet id — no `DeliveryEvent` and no
    /// DELIVERED is fabricated, same as `sendText`.
    @discardableResult
    public func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        try await transport.send(payload)
        return recordPrivate(payload, to: destination, wantAck: wantAck)
    }

    // Non-async on purpose, same rule as `nextOutbox`/`nextPacket`
    // below: an `NSLock` taken across a suspension point is a Swift 6
    // error, so every locked mutation here happens in a synchronous
    // helper called from the async entry point.
    private func recordPosition(_ fix: ExternalPositionFix, to destination: UInt32) -> UInt32 {
        lock.lock()
        sentPositions.append((fix, destination))
        lock.unlock()
        return nextPacket()
    }

    private func recordPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) -> UInt32 {
        lock.lock()
        sentPrivate.append((payload, destination, wantAck))
        lock.unlock()
        return nextPacket()
    }

    public var sentPositionLog: [(ExternalPositionFix, UInt32)] {
        lock.lock(); defer { lock.unlock() }
        return sentPositions
    }

    public var sentPrivateLog: [(Data, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }
        return sentPrivate
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

    // MARK: - M3: channel/config write-back — stub semantics

    private var sentChannelWrites: [ChannelWriteRequest] = []
    private var sentOwnerWrites: [(String, String)] = []
    private var sentRegionWrites: [Config.LoRaConfig.RegionCode] = []

    /// A stub has no firmware to diverge from what it was asked to
    /// write, so its own record IS the read-back — this stays honest
    /// with the rest of the type's "never invents" rule by reporting
    /// back exactly the request, never a hidden extra field. Throws
    /// `.notConnected` under the same rule `sendPosition`'s destination
    /// check would if this type had one — `connectedNodeNum` is only
    /// ever set by a test.
    @discardableResult
    public func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        guard connectedNodeNum != nil else { throw AdminWriteError.notConnected }
        recordChannelWrite(request)
        return ChannelWriteReport(channels: request.channels, loraConfig: request.loraConfig)
    }

    @discardableResult
    public func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        guard connectedNodeNum != nil else { throw AdminWriteError.notConnected }
        recordOwnerWrite(longName, shortName)
        return OwnerWriteReport(longName: longName, shortName: shortName)
    }

    @discardableResult
    public func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        guard region != .unset else { throw AdminWriteError.regionUnset }
        guard connectedNodeNum != nil else { throw AdminWriteError.notConnected }
        recordRegionWrite(region)
        return RegionWriteReport(region: region)
    }

    /// Test-injected only — a stub has no radio to read a channel table
    /// off. Empty (nothing occupied) unless a test sets `channelTable`,
    /// same "never invents" rule every other field here follows.
    private var _channelTable: [Channel] = []
    public var channelTable: [Channel] {
        get { lock.lock(); defer { lock.unlock() }; return _channelTable }
        set { lock.lock(); defer { lock.unlock() }; _channelTable = newValue }
    }

    public func currentChannelTable() async throws -> [Channel] {
        guard connectedNodeNum != nil else { throw AdminWriteError.notConnected }
        return channelTable
    }

    // Non-async on purpose — same NSLock-across-a-suspension-point
    // convention as `nextOutbox`/`nextPacket` above: an `NSLock` taken
    // inline inside an `async` function is a Swift 6 error, so every
    // locked mutation happens in a synchronous helper called from the
    // async entry point.
    private func recordChannelWrite(_ request: ChannelWriteRequest) {
        lock.lock(); sentChannelWrites.append(request); lock.unlock()
    }
    private func recordOwnerWrite(_ longName: String, _ shortName: String) {
        lock.lock(); sentOwnerWrites.append((longName, shortName)); lock.unlock()
    }
    private func recordRegionWrite(_ region: Config.LoRaConfig.RegionCode) {
        lock.lock(); sentRegionWrites.append(region); lock.unlock()
    }

    public var sentChannelWriteLog: [ChannelWriteRequest] {
        lock.lock(); defer { lock.unlock() }
        return sentChannelWrites
    }
    public var sentOwnerWriteLog: [(String, String)] {
        lock.lock(); defer { lock.unlock() }
        return sentOwnerWrites
    }
    public var sentRegionWriteLog: [Config.LoRaConfig.RegionCode] {
        lock.lock(); defer { lock.unlock() }
        return sentRegionWrites
    }

    // MARK: - Finding 2: node config passive-read seam — stub semantics

    private let nodeConfigHub = CurrentValueEventHub<NodeConfigSnapshot>()
    private var _nodeConfig: NodeConfigSnapshot?

    /// Test-injected only, same "never invents" rule as `channelTable`
    /// above — a stub has no want_config to parse this from. Setting it
    /// publishes to `nodeConfigUpdates()` too, so a test can exercise
    /// both the synchronous read and the stream from one call.
    public var nodeConfig: NodeConfigSnapshot? {
        get { lock.lock(); defer { lock.unlock() }; return _nodeConfig }
        set {
            lock.lock(); _nodeConfig = newValue; lock.unlock()
            if let newValue { nodeConfigHub.yield(newValue) }
        }
    }

    public func nodeConfigUpdates() -> AsyncStream<NodeConfigSnapshot> { nodeConfigHub.subscribe() }
    public var connectedNodeConfig: NodeConfigSnapshot? { nodeConfig }
}
