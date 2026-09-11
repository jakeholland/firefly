//
//  MeshtasticClient.swift — the real Meshtastic client: framing,
//  the two-phase want_config handshake, nodeDB updates, and routing-ack
//  → delivery-state translation.
//
//  This is the Swift twin of the puck's `mc_client.c`
//  (firmware/meshclient), reimplemented against SwiftProtobuf +
//  AsyncStream rather than nanopb + `tick()` — see
//  docs/specs/A01-companion-app.md, "Why firmware/meshclient is not
//  linked in too". The wire format is shared exactly, through the
//  pinned protobufs (`MeshtasticProto`, `ProtobufPinTests`); this file
//  is where the wire *behavior* — handshake sequencing, ack semantics,
//  reboot handling — is re-derived against the same spec so the two
//  clients stay honest against each other.
//
import Foundation
import MeshtasticProto

/// Meshtastic's own sentinels for `ToRadio.want_config_id` — NOT
/// arbitrary correlation nonces. Confirmed two ways (docs/specs/
/// A01-companion-app.md, "Handshake"): Meshtastic-Apple's
/// `AccessoryManager.swift:150-151` defines exactly these two constants
/// and dispatches `config_complete_id` on them by name at lines
/// 1188/1199 ("Unknown nonce completed" for anything else); this repo's
/// own archived app (`git show 8b0967f:
/// Firefly/Core/Models/MeshtasticClient.swift:53-56,121`) documents
/// 69420 as "the Meshtastic firmware constant" for the same split.
public enum MeshtasticConfigNonce {
    /// Phase A: my_info, metadata, channels, config, module_config.
    public static let onlyConfig: UInt32 = 69420
    /// Phase B: the node database dump.
    public static let onlyNodeDB: UInt32 = 69421
}

public enum MeshtasticClientError: Error, Equatable, Sendable {
    /// A want_config phase's `config_complete_id` never arrived in time.
    /// The associated value is the sentinel nonce that timed out
    /// (`MeshtasticConfigNonce.onlyConfig` or `.onlyNodeDB`).
    case handshakeTimeout(phase: UInt32)
    case encodingFailed
    /// M2: `connect()` is not reentrant. `AppGraph` now auto-connects to
    /// the remembered peripheral at launch (`AppGraph.start()`'s own doc
    /// comment) at the same time the Connect screen's CONNECT button can
    /// call `connect()` by hand — without this guard, two overlapping
    /// calls would each build their own `receiveTask`, leaking one and
    /// double-consuming `transport.events()`. Thrown by the SECOND
    /// overlapping call; the first runs to completion normally.
    case alreadyConnecting
}

/// The seam between the handshake-retry loop
/// (`MeshtasticClient.handleTransportReconnected()`) and however it
/// actually waits out the delay between attempts. `handshakeRetryDelay(
/// forAttempt:)` itself stays a PURE function of `Duration` in, `Duration`
/// out — `testHandshakeRetryDelayDoublesAndCaps`/
/// `testHandshakeRetryDelayWorksAtSubSecondPrecision` exercise that real
/// math directly, no clock involved. This protocol is only about what
/// happens to the `Duration` it returns: production actually waits it
/// out (`SystemHandshakeRetryClock`); a test that only cares about the
/// retry LOOP's own behaviour — attempt counts, the `.reconnecting`/
/// `.failed` events it publishes — can inject something that resolves
/// near-instantly instead, so that behaviour is exercised deterministically
/// rather than by dialling `handshakeRetryBaseDelay` down to a handful of
/// milliseconds and then still waiting out that many milliseconds of REAL
/// wall-clock time per attempt. That distinction is exactly what CI run
/// 34606690299 found: `ClientReconnectTests
/// .testHandshakeFailsHonestlyOnceEveryBoundedRetryIsSpent` drove the
/// real backoff with real (if small) `Task.sleep`s, and a loaded runner's
/// cooperative-thread-pool contention inflated those small sleeps (and
/// the polling loops waiting on their effects) well past the test's own
/// timeout even though the nominal, uncontended total was under 100ms.
public protocol HandshakeRetryClock: Sendable {
    func sleep(for duration: Duration) async throws
}

/// Production default — an actual wall-clock wait for the actual
/// computed backoff delay.
public struct SystemHandshakeRetryClock: HandshakeRetryClock {
    public init() {}
    public func sleep(for duration: Duration) async throws {
        try await Task.sleep(for: duration)
    }
}

/// The real Meshtastic client: drives `MeshTransport`, runs the
/// handshake, maintains `NodeDB`, and turns routing acks into
/// `DeliveryState` transitions. An actor — CoreBluetooth delegate
/// callbacks, a serial read source and a TCP receive loop all land off
/// the main thread (docs/specs/A01-companion-app.md, "Threading model"),
/// and funnel into this single isolation domain rather than the
/// `@MainActor` the C core bridge uses.
public actor MeshtasticClient: MeshtasticClientProtocol {
    /// Diagnostics for the client-level connect/handshake path — added
    /// to close exactly the visibility gap that let "the live macOS app
    /// stays NOT CONNECTED forever with no client-level log at all"
    /// (docs/specs/A01-companion-app.md; the app: fix live connect path
    /// investigation) go undiagnosed: `BLETransport` already logs every
    /// CoreBluetooth delegate step unconditionally
    /// (`BLETransport.log(_:)`'s own doc comment — "cheap enough to
    /// leave in permanently"), but this actor — everything ABOVE the
    /// transport seam: `connect()`'s own two awaits, the want_config
    /// handshake, every `FromRadio` this client decodes, and every
    /// `LinkState` this client ever published — had none at all. Same
    /// discipline as `BLETransport.log(_:)`: a raw
    /// `FileHandle.standardError.write`, not `print()`, so a line is
    /// never lost to stdout's full block-buffering once
    /// `xcodebuild test`/`open --stderr <file>` turns it into a pipe.
    /// On by default (`FIREFLY_VERBOSE_LOG`/`-FireflyVerboseLog` gate
    /// the FromRadio-per-message line specifically, the one line noisy
    /// enough during a node-dump to be worth silencing in an ordinary
    /// run) — every other line here is exactly as cheap as `connect()`
    /// itself, at most a handful of calls per connection attempt.
    static func verboseLoggingEnabled(arguments: [String] = CommandLine.arguments,
                                       environment: [String: String] = ProcessInfo.processInfo.environment) -> Bool {
        arguments.contains("-FireflyVerboseLog") || environment["FIREFLY_VERBOSE_LOG"] == "1"
    }
    private static func log(_ message: String) {
        let line = "[MeshtasticClient] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }
    private static func verboseLog(_ message: @autoclosure () -> String) {
        guard verboseLoggingEnabled() else { return }
        log(message())
    }

    private let transport: MeshTransport
    // `CurrentValueEventHub` (M1 review follow-up, #267) — see
    // `CurrentValueEventHub`'s own doc comment.
    private let linkHub = CurrentValueEventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let incomingTextHub = EventHub<IncomingText>()
    private let incomingPrivateHub = EventHub<IncomingPrivate>()

    private var nodeDB = NodeDB()
    private var framer = StreamFramer()

    /// The actor-isolated value every internal read uses. Its `didSet`
    /// mirrors it into `nodeNumBox` for the one caller that has to read
    /// it with no actor hop — see `connectedNodeNum`.
    private var myNodeNum: UInt32? {
        didSet { nodeNumBox.value = myNodeNum }
    }
    /// A lock-protected mirror of `myNodeNum`, written ONLY by that
    /// property's `didSet`. Not a second source of truth: nothing else
    /// writes it, and it always holds whatever the actor stored last.
    private let nodeNumBox = LockedValue<UInt32?>(nil)
    /// Extra, non-protocol state — useful to `FireflyHardwareTests`
    /// (which holds a concrete `MeshtasticClient`, not just the
    /// protocol existential) and to a future Diagnostics screen. Not
    /// part of `MeshtasticClientProtocol`: nothing above that seam may
    /// depend on it in M1.
    public private(set) var firmwareVersion: String?
    public private(set) var isFirmwareBelowSupportedFloor = false
    public private(set) var channelNames: Set<String> = []

    private var receiveTask: Task<Void, Never>?
    private var heartbeatTask: Task<Void, Never>?
    private var hasCompletedInitialConnect = false
    private var lastRxAt: Date?
    /// Guards `connect()` against a second, overlapping call — see
    /// `MeshtasticClientError.alreadyConnecting`'s own doc comment.
    private var isConnectAttemptInFlight = false
    /// The handshake-retry-with-backoff loop a transport reconnect (or a
    /// `FromRadio.rebooted` frame) starts (`handleTransportReconnected()`,
    /// via `restartReconnectTask()`). Tracked so a NEW transport `.ready`,
    /// a `.rebooted` frame — including one arriving mid-retry (PR #272
    /// review, BLOCKING item 1) — or a `.disconnected` mid-retry can
    /// cancel any retry already in flight rather than letting two
    /// overlapping loops both retry the handshake — the client-level
    /// analog of "no duplicate CBCentralManager": at most one handshake
    /// attempt is ever outstanding, no matter which of those three
    /// triggers fires it.
    private var reconnectTask: Task<Void, Never>?

    /// Which want_config phase is currently outstanding, if any —
    /// diagnostic only (a future Diagnostics screen); no longer load-bearing
    /// for correctness (see `configCompleteHub`, below).
    public private(set) var pendingConfigPhase: UInt32?
    /// `configCompleteID` events, fanned out via `EventHub` rather than a
    /// single stored `CheckedContinuation`: a continuation set up INSIDE
    /// a task-group child task (as an earlier version of this file did)
    /// races the receive loop — nothing guarantees the child task runs
    /// and registers itself before an already-injected/arrived
    /// `config_complete_id` is dispatched, and a signal that arrives
    /// with no continuation yet registered is lost forever, hanging the
    /// handshake until the real timeout. Subscribing to an `EventHub`
    /// happens synchronously, before the want_config write goes out
    /// (S1's own "subscribe before you can miss something" rule), and
    /// its `.bufferingNewest` `AsyncStream` holds a value published
    /// before its consumer starts iterating — so this ordering issue
    /// cannot recur.
    private let configCompleteHub = EventHub<UInt32>()

    /// M3 — one `AdminMessage` response per inbound `ADMIN_APP` packet,
    /// paired with the `Data.request_id` it answers (the same
    /// request/response correlation `routingApp` already uses —
    /// `handle(routingAck:requestID:)`'s own doc comment). Subscribed
    /// BEFORE the request that asks for it goes out, same S1 ordering
    /// rule `configCompleteHub` follows.
    private let adminResponseHub = EventHub<(requestID: UInt32, message: AdminMessage)>()
    /// How long a single admin read (a `get_*_request`, including the
    /// read-back after a write) waits for its response, and how long
    /// `applyChannelSet`/`setOwner`/`setRegion` wait for the link to
    /// reach `.ready` again after a `commit_edit_settings` — real
    /// firmware disables Bluetooth and reboots at commit (Meshtastic-
    /// Apple's own `commitEditSettings` doc comment), so the read-back
    /// routinely has to outlive a real disconnect/reboot/reconnect
    /// cycle. Injectable, same convention as `configPhaseTimeout`.
    private let adminResponseTimeout: Duration
    /// NIT 10 (PR #274 review) — the pause between `beginEditSettings`'s
    /// two copies. Injectable so a test can drive the whole
    /// begin/set/commit sequence in milliseconds rather than waiting out
    /// the production default, same convention every other timing knob
    /// on this type follows.
    private let beginEditSettingsRetryDelay: Duration

    private struct PendingSend {
        let isBroadcast: Bool
        let wantAck: Bool
        let sentAt: Date
    }
    private var pendingSends: [UInt32: PendingSend] = [:]
    /// Bound (SHOULD-FIX 4, PR #264 review): nothing ever pruned a
    /// `pendingSends` entry whose 5-minute render-time NO_ACK window had
    /// already elapsed, so a long session accumulated dead entries
    /// indefinitely, and a very-late routing ack (a mesh retransmit
    /// arriving after that window) could silently re-promote an
    /// already-shown NO_ACK message back to DELIVERED. Swept
    /// opportunistically on every new send — this dict is bounded by how
    /// many `want_ack` DMs are in flight, not by session length, so this
    /// is cheap — rather than on a timer: a timer that fires while the
    /// app is suspended is a timer that lies, the same rule
    /// `noAckWindow` itself follows. The extra minute past `noAckWindow`
    /// is slack so a genuine ack racing the render-time window's own
    /// boundary is never pruned out from under it.
    private static let pendingSendsMaxAge: TimeInterval = MeshtasticClient.noAckWindow + 60
    private var packetIDCounter: UInt32
    /// Distinct from `packetIDCounter` (PR #264 review, BLOCKING item 1
    /// — the exact bug class PR #261 already found and fixed on the
    /// sibling slice-B branch): minted BEFORE any I/O, so `.waiting`
    /// carries an id nothing about the radio or the write can fail to
    /// produce, and never shares a value space with the packet id the
    /// radio only assigns once a write actually succeeds.
    private var outboxIDCounter: UInt32 = 1

    /// Handshake timeouts, injectable so unit tests can exercise a real
    /// timeout without a 30s/120s wait. Defaults are the spec's own:
    /// 30s for phase A (config), 120s for phase B (the node database).
    private let configPhaseTimeout: Duration
    private let nodeDBPhaseTimeout: Duration
    /// Stream-transport heartbeat cadence (serial/TCP only — BLE gets
    /// its liveness from the link itself). 15s send interval, treated as
    /// dead if nothing at all has been received for one interval plus a
    /// 5s grace.
    private let heartbeatInterval: Duration
    private let heartbeatGrace: Duration
    /// M2 — bounded exponential backoff for the want_config handshake
    /// retry that follows a transport reconnect (docs/specs/
    /// A01-companion-app.md M2: "reconnecting after the node is power
    /// cycled" — firmware mid-boot may not answer want_config on the
    /// first try even though the BLE link itself is back up). Injectable,
    /// same convention as the two phase timeouts above, so a test can
    /// exercise the whole bounded loop in milliseconds rather than
    /// minutes. Defaults: up to 6 attempts, 2s/4s/8s/16s/32s between them,
    /// capped at 60s — battery-conscious (well over the "no timers under
    /// 30s" floor between retries) and bounded (never an infinite hot
    /// loop against a node that is truly gone).
    private let handshakeRetryLimit: Int
    private let handshakeRetryBaseDelay: Duration
    private let handshakeRetryMaxDelay: Duration
    /// What actually waits out `handshakeRetryDelay(forAttempt:)` between
    /// attempts — see `HandshakeRetryClock`'s own doc comment. Defaults to
    /// a real wall-clock wait; a test exercising the retry LOOP itself
    /// (as opposed to the pure backoff-table math) injects a near-instant
    /// one instead.
    private let handshakeRetryClock: HandshakeRetryClock

    public init(
        transport: MeshTransport,
        configPhaseTimeout: Duration = .seconds(30),
        nodeDBPhaseTimeout: Duration = .seconds(120),
        heartbeatInterval: Duration = .seconds(15),
        heartbeatGrace: Duration = .seconds(5),
        handshakeRetryLimit: Int = 6,
        handshakeRetryBaseDelay: Duration = .seconds(2),
        handshakeRetryMaxDelay: Duration = .seconds(60),
        handshakeRetryClock: HandshakeRetryClock = SystemHandshakeRetryClock(),
        adminResponseTimeout: Duration = .seconds(30),
        beginEditSettingsRetryDelay: Duration = .milliseconds(150)
    ) {
        self.transport = transport
        self.configPhaseTimeout = configPhaseTimeout
        self.nodeDBPhaseTimeout = nodeDBPhaseTimeout
        self.heartbeatInterval = heartbeatInterval
        self.heartbeatGrace = heartbeatGrace
        self.handshakeRetryLimit = handshakeRetryLimit
        self.handshakeRetryBaseDelay = handshakeRetryBaseDelay
        self.handshakeRetryMaxDelay = handshakeRetryMaxDelay
        self.handshakeRetryClock = handshakeRetryClock
        self.adminResponseTimeout = adminResponseTimeout
        self.beginEditSettingsRetryDelay = beginEditSettingsRetryDelay
        // Seeded, not started at 1: two client lifetimes that both start
        // packet ids at 1 collide on every id until the higher session's
        // send count is exceeded, against Meshtastic's short per-(from,
        // id) packet history (mc_seed_packet_ids's own doc comment,
        // mc_client.h). 0 is never a valid id (Meshtastic's "unset"
        // convention), so the seed range excludes it.
        self.packetIDCounter = UInt32.random(in: 1...UInt32.max)
    }

    // MARK: - MeshtasticClientProtocol

    // `nonisolated`: `MeshtasticClientProtocol`'s three stream accessors
    // are synchronous requirements (S1 — a caller must be able to grab
    // its `AsyncStream` without an `await` racing whatever it is about
    // to trigger). Safe here because each hub is an immutable `let` of
    // an `@unchecked Sendable` class that does its own locking
    // (`EventHub`), so reading it from outside this actor's isolation
    // touches no actor-isolated state.
    public nonisolated func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    public nonisolated func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    public nonisolated func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    public nonisolated func incomingTexts() -> AsyncStream<IncomingText> { incomingTextHub.subscribe() }
    public nonisolated func incomingPrivate() -> AsyncStream<IncomingPrivate> { incomingPrivateHub.subscribe() }

    /// `nonisolated` for the same reason the stream accessors are: the
    /// one caller (`PhoneGPSUplink`'s synchronous `destinationNodeNum`
    /// closure) cannot `await`. Safe because `nodeNumBox` is an
    /// immutable `let` of a lock-protected class.
    public nonisolated var connectedNodeNum: UInt32? { nodeNumBox.value }

    /// The ONE place `linkHub.yield(_:)` is called from here on — every
    /// transition this client ever publishes is worth a log line, and
    /// routing them all through one method is what makes that true by
    /// construction rather than by remembering to add a line at every
    /// call site (the exact gap that made the live graph's `.ready`
    /// silently going missing indistinguishable from a hang: nothing
    /// said which transitions, if any, actually fired).
    private func publish(_ state: LinkState) {
        Self.log("linkState -> \(state)")
        linkHub.yield(state)
    }

    public func connect() async throws {
        Self.log("connect() called (isConnectAttemptInFlight=\(isConnectAttemptInFlight))")
        // Reentrancy guard — see `MeshtasticClientError.alreadyConnecting`'s
        // own doc comment. Checked and set BEFORE `resetSessionState()`
        // touches anything, so a caller that loses the race never tears
        // down the FIRST call's in-flight `receiveTask`/transport session.
        guard !isConnectAttemptInFlight else {
            Self.log("connect() throwing .alreadyConnecting — a connect attempt is already in flight")
            throw MeshtasticClientError.alreadyConnecting
        }
        isConnectAttemptInFlight = true
        defer { isConnectAttemptInFlight = false }

        resetSessionState()

        // Subscribe to the transport's events BEFORE calling connect():
        // `events()` is multicast via EventHub (S1) and does not replay,
        // so a subscription registered after `.ready` is published would
        // simply miss it.
        let events = transport.events()
        receiveTask = Task { [weak self] in
            await self?.consumeTransportEvents(events)
        }

        publish(.connecting)
        do {
            // For BLE this does not return until the FROMNUM
            // subscription is ACKed (MeshtasticBLE.swift,
            // FromRadioDrainPolicy) — only then is it safe to send
            // want_config.
            Self.log("connect(): awaiting transport.connect()")
            try await transport.connect()
            Self.log("connect(): transport.connect() returned successfully")
        } catch {
            Self.log("connect(): transport.connect() threw \(error)")
            publish(.failed(String(describing: error)))
            throw error
        }

        publish(.handshaking)
        do {
            Self.log("connect(): awaiting performHandshake()")
            try await performHandshake()
            Self.log("connect(): performHandshake() returned successfully")
        } catch {
            Self.log("connect(): performHandshake() threw \(error)")
            publish(.failed(String(describing: error)))
            throw error
        }

        hasCompletedInitialConnect = true
        startHeartbeatLoopIfNeeded()
        publish(.ready)
        Self.log("connect() completed — link is .ready")
    }

    public func disconnect() async {
        Self.log("disconnect() called")
        heartbeatTask?.cancel(); heartbeatTask = nil
        reconnectTask?.cancel(); reconnectTask = nil
        receiveTask?.cancel(); receiveTask = nil
        hasCompletedInitialConnect = false
        // PR #265 review, should-fix: a disconnect must clear who we
        // were connected to. Before this, `myNodeNum`/`connectedNodeNum`
        // kept the LAST session's value after `disconnect()` returned —
        // `resetSessionState()` (called at the TOP of `connect()`)
        // cleared it on the way back IN, but nothing cleared it on the
        // way out, so `connectedNodeNum` briefly lied about there being
        // a connected node at all between a disconnect and the next
        // connect attempt. `PhoneGPSUplink.destinationNodeNum` and
        // Diagnostics both read this synchronously, so that window was
        // real, not theoretical.
        myNodeNum = nil
        await transport.disconnect()
        publish(.disconnected)
    }

    @discardableResult
    public func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        let isBroadcast = destination == meshBroadcastAddress
        // Nothing acks a broadcast — the mesh gives it no delivery
        // receipt at all, so want_ack is never set for one regardless of
        // what the caller asked for.
        let effectiveWantAck = isBroadcast ? false : wantAck

        // Minted BEFORE any I/O, and from a counter that never shares a
        // value space with `packetIDCounter` (PR #264 review, BLOCKING
        // item 1 — see `outboxIDCounter`'s own doc comment). `.waiting`
        // is published against THIS id, never the packet id, which does
        // not exist yet and may never (an encode or write failure below
        // means it never will).
        let outboxID = nextOutboxID()
        deliveryHub.yield(.waiting(outboxID: OutboxID(outboxID)))

        var data = DataMessage()
        data.portnum = .textMessageApp
        data.payload = Data(text.utf8)

        let id = nextPacketID()
        var packet = MeshPacket()
        packet.id = id
        packet.to = destination
        packet.wantAck = effectiveWantAck
        packet.decoded = data

        var toRadio = ToRadio()
        toRadio.packet = packet

        guard let bytes = try? toRadio.serializedData() else {
            // SHOULD-FIX 5: an orphaned WAITING event that nobody ever
            // resolves is exactly as dishonest as the thrown error alone
            // — a local encode/write failure now gets an explicit
            // `.dropped`, not just a thrown `Error` the caller may not
            // translate into any UI state at all.
            deliveryHub.yield(.dropped(outboxID: OutboxID(outboxID)))
            throw MeshtasticClientError.encodingFailed
        }
        do {
            try await writeToRadio(bytes)
        } catch {
            deliveryHub.yield(.dropped(outboxID: OutboxID(outboxID)))
            throw error
        }

        pendingSends[id] = PendingSend(isBroadcast: isBroadcast, wantAck: effectiveWantAck, sentAt: Date())
        prunePendingSends()
        deliveryHub.yield(.sent(outboxID: OutboxID(outboxID), packetID: PacketID(id), wantAck: effectiveWantAck))
        return id
    }

    /// `POSITION_APP` + `LOC_EXTERNAL` — the phone-GPS uplink
    /// (docs/specs/A01-companion-app.md, "Phone GPS -> node"). NOT an
    /// admin message, and never `set_fixed_position`: an external fix is
    /// a measurement with a time on it.
    ///
    /// No `DeliveryEvent` is published for this at all. The delivery
    /// vocabulary (WAITING/SENT/DELIVERED/NO ACK) belongs to messages a
    /// person is waiting on; attaching it to a 30-second position
    /// heartbeat would flood the Inbox's own outbox bookkeeping with
    /// rows nothing renders.
    @discardableResult
    public func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        var position = Position()
        position.latitudeI = Int32((fix.latitude * 1e7).rounded())
        position.longitudeI = Int32((fix.longitude * 1e7).rounded())
        position.time = UInt32(max(0, fix.time.timeIntervalSince1970))
        position.locationSource = .locExternal
        if let altitude = fix.altitudeMeters {
            position.altitude = Int32(altitude.rounded())
        }
        // Both are "only when the value means something" fields, per the
        // spec's payload rule — an absent speed is not 0 m/s, and an
        // absent course is not due north.
        if let speed = fix.groundSpeedMetersPerSecond, speed > 0 {
            position.groundSpeed = UInt32(speed.rounded())
        }
        if let track = fix.groundTrackDegrees, track > 0, track <= 360 {
            position.groundTrack = UInt32(track.rounded())
        }
        // `sats_in_view` and `precision_bits` are deliberately unset:
        // CoreLocation reports no satellite count, and precision is the
        // CHANNEL's setting (`position_precision`), asserted by the node
        // itself — claiming either here would be inventing wire data.

        guard let payload = try? position.serializedData() else {
            throw MeshtasticClientError.encodingFailed
        }
        return try await sendData(payload, portnum: .positionApp, to: destination, wantAck: false)
    }

    /// Portnum 269, Firefly's own (S04) — `payload` is an already
    /// encoded `ff_proto` frame and stays opaque here.
    ///
    /// Like `sendPosition`, this publishes no `DeliveryEvent`: FLARE and
    /// the FIND pings are fire-and-forget by design
    /// (`ThreadViewModel.sendFlare`'s own doc comment — "never enters
    /// the outbox"), so there is no outbox row for a WAITING/SENT pair
    /// to attach to.
    @discardableResult
    public func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        // Nothing acks a broadcast — same rule `sendText` applies.
        let effectiveWantAck = (destination == meshBroadcastAddress) ? false : wantAck
        let portnum = PortNum(rawValue: Int(fireflyPrivatePortNum)) ?? .privateApp
        return try await sendData(payload, portnum: portnum, to: destination, wantAck: effectiveWantAck)
    }

    /// The one packet-minting/writing path `sendPosition` and
    /// `sendPrivate` share. Deliberately NOT shared with `sendText`:
    /// that one additionally mints an outbox id, publishes
    /// WAITING/SENT/DROPPED and registers a `pendingSends` entry so a
    /// routing ack can find it — none of which applies here.
    private func sendData(_ payload: Data, portnum: PortNum, to destination: UInt32,
                          wantAck: Bool) async throws -> UInt32 {
        var data = DataMessage()
        data.portnum = portnum
        data.payload = payload

        let id = nextPacketID()
        var packet = MeshPacket()
        packet.id = id
        packet.to = destination
        packet.wantAck = wantAck
        packet.decoded = data

        var toRadio = ToRadio()
        toRadio.packet = packet

        guard let bytes = try? toRadio.serializedData() else {
            throw MeshtasticClientError.encodingFailed
        }
        try await writeToRadio(bytes)
        return id
    }

    // MARK: - M3: channel/config write-back (admin messages)
    //
    // Every write below follows the same shape: begin_edit_settings,
    // the write(s), commit_edit_settings (best-effort — see its own doc
    // comment), wait for the link to be `.ready` again (a commit reboots
    // the node and drops the link — Meshtastic-Apple's own
    // `commitEditSettings` doc comment, cross-checked against
    // `AdminModule.cpp`), then a read-back compared against what was
    // sent. "Honest success" is exactly that comparison; anything else
    // — including a node that never comes back — throws `AdminWriteError`
    // rather than assuming the write took.
    //
    // Every admin message here addresses OUR OWN connected node
    // (`to == from == myNodeNum`) — never a remote one, matching
    // Meshtastic-Apple's own local-admin convention
    // (`AccessoryManager+ToRadio.swift`'s `saveChannelSet`/`saveUser`:
    // `meshPacket.to = deviceNum; meshPacket.from = deviceNum`, no
    // `sessionPasskey` — that field is only for a REMOTE node's admin
    // channel).

    @discardableResult
    public func applyChannelSet(_ request: ChannelWriteRequest) async throws -> ChannelWriteReport {
        let me = try requireConnectedNode()

        try await beginEditSettings(to: me)
        do {
            for channel in request.channels {
                var admin = AdminMessage()
                admin.setChannel = channel
                do {
                    try await sendAdminWrite(admin, to: me)
                } catch {
                    // SHOULD-FIX 7 (PR #274 review): name exactly which item failed to send —
                    // every channel before this one in `request.channels` may already be
                    // committed to the node, so the caller must be told this write is not a
                    // clean all-or-nothing failure.
                    let name = channel.settings.name.isEmpty ? "(default)" : channel.settings.name
                    throw AdminWriteError.partialApplyFailed(
                        step: "channel \(channel.index) (\(name))", underlying: String(describing: error))
                }
            }
            if let lora = request.loraConfig {
                var admin = AdminMessage()
                var config = Config()
                config.lora = lora
                admin.setConfig = config
                do {
                    try await sendAdminWrite(admin, to: me)
                } catch {
                    throw AdminWriteError.partialApplyFailed(step: "LoRa config", underlying: String(describing: error))
                }
            }
        } catch {
            await commitEditSettingsBestEffort(to: me)
            throw error
        }
        await commitEditSettingsBestEffort(to: me)
        try await waitForReadyAfterCommit()

        // SHOULD-FIX 7 (PR #274 review): every item is read back and
        // compared — never bail at the first mismatch — so a partial
        // apply is reported per item rather than hiding whichever items
        // came after the first failure.
        var readChannels: [Channel] = []
        var mismatches: [String] = []
        for channel in request.channels {
            let got = try await requestChannel(index: channel.index, from: me)
            if got == channel {
                readChannels.append(got)
            } else {
                let name = channel.settings.name.isEmpty ? "(default)" : channel.settings.name
                mismatches.append("channel \(channel.index) (\(name))")
            }
        }
        var readLora: Config.LoRaConfig?
        if let lora = request.loraConfig {
            let got = try await requestLoRaConfig(from: me)
            if got == lora {
                readLora = got
            } else {
                mismatches.append("LoRa config")
            }
        }
        guard mismatches.isEmpty else {
            let totalItems = request.channels.count + (request.loraConfig != nil ? 1 : 0)
            let matchedCount = totalItems - mismatches.count
            let partialNote = totalItems > 1
                ? " (\(matchedCount) of \(totalItems) item(s) matched — the node may be partially configured)"
                : ""
            throw AdminWriteError.readBackMismatch(
                "did not read back as written: \(mismatches.joined(separator: ", "))\(partialNote)")
        }
        return ChannelWriteReport(channels: readChannels, loraConfig: readLora)
    }

    @discardableResult
    public func setOwner(longName: String, shortName: String) async throws -> OwnerWriteReport {
        let me = try requireConnectedNode()

        var owner = User()
        owner.longName = longName
        owner.shortName = shortName
        var admin = AdminMessage()
        admin.setOwner = owner

        try await beginEditSettings(to: me)
        do {
            try await sendAdminWrite(admin, to: me)
        } catch {
            await commitEditSettingsBestEffort(to: me)
            throw error
        }
        await commitEditSettingsBestEffort(to: me)
        try await waitForReadyAfterCommit()

        let got = try await requestOwner(from: me)
        guard got.longName == longName, got.shortName == shortName else {
            throw AdminWriteError.readBackMismatch("owner name did not read back as written")
        }
        return OwnerWriteReport(longName: got.longName, shortName: got.shortName)
    }

    @discardableResult
    public func setRegion(_ region: Config.LoRaConfig.RegionCode) async throws -> RegionWriteReport {
        // SHOULD-FIX 5 (PR #274 review): defense-in-depth. `.unset` is
        // Meshtastic's own "radio disabled" sentinel; before this the only
        // guard was the Settings screen disabling its APPLY button, so any
        // other caller (or a future UI bug) could still reach the radio
        // with it. Checked before anything else, including the connection
        // check below — this is an invalid CALL regardless of link state.
        guard region != .unset else { throw AdminWriteError.regionUnset }
        let me = try requireConnectedNode()

        // Read the CURRENT LoRa config first: `set_config.lora` replaces
        // the whole submessage on the wire, not just `region` — sending
        // a bare `Config.LoRaConfig()` with only `region` set would
        // silently reset bandwidth/spread-factor/tx-power/etc to their
        // zero defaults. This is the one write in this file that reads
        // before it writes for exactly that reason.
        let current = try await requestLoRaConfig(from: me)
        var lora = current
        lora.region = region

        var admin = AdminMessage()
        var config = Config()
        config.lora = lora
        admin.setConfig = config

        try await beginEditSettings(to: me)
        do {
            try await sendAdminWrite(admin, to: me)
        } catch {
            await commitEditSettingsBestEffort(to: me)
            throw error
        }
        await commitEditSettingsBestEffort(to: me)
        try await waitForReadyAfterCommit()

        let got = try await requestLoRaConfig(from: me)
        guard got.region == region else {
            throw AdminWriteError.readBackMismatch("region did not read back as written")
        }
        return RegionWriteReport(region: got.region)
    }

    // MARK: - M3: admin write/read plumbing

    private func requireConnectedNode() throws -> UInt32 {
        guard let me = myNodeNum else { throw AdminWriteError.notConnected }
        return me
    }

    /// NIT 10 (PR #274 review): sent TWICE on purpose, matching
    /// Meshtastic-Apple's `DeviceProfileImporter.run()`
    /// (`Meshtastic/Import/DeviceProfileImporter.swift:127-138`):
    /// `begin_edit_settings` is idempotent server-side
    /// (`AdminModule.cpp` only sets `hasOpenEditTransaction = true`) and
    /// the firmware never acks it, so a single dropped copy is
    /// undetectable from here and silently downgrades the whole write to
    /// untransacted — every subsequent `set_*` then saves to flash and
    /// reboots on its own instead of batching under one commit. A second
    /// copy costs one packet and removes that single point of failure;
    /// Meshtastic-Apple's own comment cites this as an observed-on-
    /// hardware failure mode, not a theoretical one.
    private func beginEditSettings(to dest: UInt32) async throws {
        var admin = AdminMessage()
        admin.beginEditSettings = true
        try await sendAdminWrite(admin, to: dest)
        try? await Task.sleep(for: beginEditSettingsRetryDelay)
        try await sendAdminWrite(admin, to: dest)
    }

    /// Best-effort on purpose: a real `commit_edit_settings` disables
    /// Bluetooth as the first step of the commit and reboots the node
    /// (Meshtastic-Apple's own `commitEditSettings` doc comment), so the
    /// write that carries it can throw (transport gone mid-write) even
    /// though the node accepted the commit. Swallowed here — the
    /// read-back after `waitForReadyAfterCommit()` is what actually
    /// decides success or failure for the caller, never this write's own
    /// local error. Still called on every path (including the error path
    /// of the writes above): an edit transaction left open defers every
    /// subsequent write from ANY client until something commits it
    /// (`beginEditSettings`'s own citation of `AdminModule.cpp`), so an
    /// abandoned transaction must never be left behind.
    private func commitEditSettingsBestEffort(to dest: UInt32) async {
        var admin = AdminMessage()
        admin.commitEditSettings = true
        _ = try? await sendAdminWrite(admin, to: dest)
    }

    /// Fire-and-forget admin write: `want_ack` true (mirrors Meshtastic-
    /// Apple's own admin sends — `MeshPacket.Priority.reliable`), no
    /// `want_response` (this is a WRITE, not a `get_*_request` question —
    /// `mc_client.c`'s own `mc_send_data_packet_ex` doc comment: only a
    /// `get_*_request` sets that bit).
    @discardableResult
    private func sendAdminWrite(_ admin: AdminMessage, to dest: UInt32) async throws -> UInt32 {
        guard let payload = try? admin.serializedData() else {
            throw AdminWriteError.encodingFailed
        }
        var data = DataMessage()
        data.portnum = .adminApp
        data.payload = payload

        let id = nextPacketID()
        var packet = MeshPacket()
        packet.id = id
        packet.to = dest
        packet.from = dest
        packet.wantAck = true
        packet.priority = .reliable
        packet.decoded = data

        var toRadio = ToRadio()
        toRadio.packet = packet
        guard let bytes = try? toRadio.serializedData() else {
            throw AdminWriteError.encodingFailed
        }
        try await writeToRadio(bytes)
        return id
    }

    private func requestChannel(index: Int32, from dest: UInt32) async throws -> Channel {
        var admin = AdminMessage()
        admin.getChannelRequest = UInt32(index)
        let response = try await sendAdminRequest(admin, to: dest)
        guard case .getChannelResponse(let channel) = response.payloadVariant else {
            throw AdminWriteError.readBackMismatch("no channel response for index \(index)")
        }
        return channel
    }

    private func requestLoRaConfig(from dest: UInt32) async throws -> Config.LoRaConfig {
        var admin = AdminMessage()
        admin.getConfigRequest = .loraConfig
        let response = try await sendAdminRequest(admin, to: dest)
        guard case .getConfigResponse(let config) = response.payloadVariant,
              case .lora(let lora)? = config.payloadVariant else {
            throw AdminWriteError.readBackMismatch("no LoRa config response")
        }
        return lora
    }

    private func requestOwner(from dest: UInt32) async throws -> User {
        var admin = AdminMessage()
        admin.getOwnerRequest = true
        let response = try await sendAdminRequest(admin, to: dest)
        guard case .getOwnerResponse(let user) = response.payloadVariant else {
            throw AdminWriteError.readBackMismatch("no owner response")
        }
        return user
    }

    /// The one place a `get_*_request` actually goes out and its
    /// response is awaited. `Data.want_response = true` is REQUIRED for
    /// a real `AdminModule` to answer AT ALL —
    /// `firmware/meshclient/src/mc_client.c`'s own citation of
    /// `AdminModule::handleGetOwner` (`v2.7.26.54e0d8d0`,
    /// `src/modules/AdminModule.cpp`): "AdminModule only builds and
    /// sends a get_owner_response when the INCOMING request's
    /// Data.want_response bit is set" — a bench finding (2026-09-06)
    /// against a real puck, not a guess, and the same mechanism a real
    /// AdminModule uses for every `get_*_request`, not just
    /// `get_owner_request`.
    ///
    /// NIT 9 (PR #274 review): `want_ack = true` and `priority =
    /// .reliable`, matching Meshtastic-Apple's own admin READS, not just
    /// its writes — `AccessoryManager+ToRadio.swift`'s
    /// `requestLoRaConfig` sets both on a `get_config_request` the same
    /// way `saveLoRaConfig` does on the write. Without this a lost read
    /// REQUEST (as opposed to a lost response) had no mesh-level retry,
    /// only this method's own 30s timeout-then-fail.
    private func sendAdminRequest(_ admin: AdminMessage, to dest: UInt32) async throws -> AdminMessage {
        guard let payload = try? admin.serializedData() else {
            throw AdminWriteError.encodingFailed
        }
        var data = DataMessage()
        data.portnum = .adminApp
        data.payload = payload
        data.wantResponse = true

        let id = nextPacketID()
        var packet = MeshPacket()
        packet.id = id
        packet.to = dest
        packet.from = dest
        packet.wantAck = true
        packet.priority = .reliable
        packet.decoded = data

        var toRadio = ToRadio()
        toRadio.packet = packet

        // Subscribe BEFORE writing — the same S1 ordering rule
        // `requestConfig` follows for `configCompleteHub`.
        let responses = adminResponseHub.subscribe()
        guard let bytes = try? toRadio.serializedData() else {
            throw AdminWriteError.encodingFailed
        }
        try await writeToRadio(bytes)

        let timeout = adminResponseTimeout
        return try await withThrowingTaskGroup(of: AdminMessage.self) { group in
            group.addTask {
                for await (requestID, message) in responses where requestID == id {
                    return message
                }
                throw AdminWriteError.timeout
            }
            group.addTask {
                try await Task.sleep(for: timeout)
                throw AdminWriteError.timeout
            }
            guard let result = try await group.next() else {
                throw AdminWriteError.timeout
            }
            group.cancelAll()
            return result
        }
    }

    /// A `commit_edit_settings` reboots the node and drops the link
    /// (this section's own header comment). If the link is ALREADY
    /// `.ready` — nothing actually disconnected (a loopback transport in
    /// a test, or 2.8's live-apply path for some config types) —
    /// `linkHub`'s current-value replay resolves this immediately.
    /// Otherwise this waits out the disconnect/reconnect M2's own
    /// background-BLE retry loop already drives, up to `timeout`. A
    /// terminal `.failed` is treated the same as a timeout: the node did
    /// not come back, so there is nothing honest left to read back from.
    private func waitForReadyAfterCommit() async throws {
        let states = linkHub.subscribe()
        let timeout = adminResponseTimeout
        try await withThrowingTaskGroup(of: Void.self) { group in
            group.addTask {
                for await state in states {
                    switch state {
                    case .ready: return
                    case .failed: throw AdminWriteError.timeout
                    default: continue
                    }
                }
                throw AdminWriteError.timeout
            }
            group.addTask {
                try await Task.sleep(for: timeout)
                throw AdminWriteError.timeout
            }
            try await group.next()
            group.cancelAll()
        }
    }

    // MARK: - M3: read-only admin queries (concrete-type only — used by
    // `HardwareTests`' gated write-back suite to read the CURRENT state
    // before writing it back unchanged, and available to a future
    // Diagnostics screen). These call the SAME `request*` helpers
    // `applyChannelSet`/`setOwner`/`setRegion` use for their own
    // read-back — never a second, possibly-divergent implementation.

    public func currentChannel(index: Int32) async throws -> Channel {
        try await requestChannel(index: index, from: try requireConnectedNode())
    }

    public func currentLoRaConfig() async throws -> Config.LoRaConfig {
        try await requestLoRaConfig(from: try requireConnectedNode())
    }

    public func currentOwner() async throws -> User {
        try await requestOwner(from: try requireConnectedNode())
    }

    /// M3 (PR #274 review, BLOCKING 1 & 2) — the connected node's current
    /// channel table, read LIVE index by index (`0..<maxChannelSlots`)
    /// with the SAME `requestChannel` helper `applyChannelSet`'s own
    /// read-back uses, never a second implementation. An index the node
    /// reports `.disabled` for is simply omitted — "occupied" means
    /// "role is primary or secondary right now," matching
    /// `ChannelWritePlan`'s own vocabulary.
    public func currentChannelTable() async throws -> [Channel] {
        let me = try requireConnectedNode()
        var table: [Channel] = []
        for index in Int32(0)..<maxChannelSlots {
            let channel = try await requestChannel(index: index, from: me)
            if channel.role != .disabled {
                table.append(channel)
            }
        }
        return table
    }

    // MARK: - Extra, non-protocol accessors (concrete-type only)

    public func nodeSnapshot(_ num: UInt32) -> MeshNodeSnapshot? { nodeDB.node(num) }
    public var allNodeSnapshots: [MeshNodeSnapshot] { nodeDB.all }
    public var currentMyNodeNum: UInt32? { myNodeNum }

    // MARK: - Render-time delivery state

    /// docs/specs/A01-companion-app.md, "Routing ACK → delivery state":
    /// the 5-minute NO_ACK window is derived AT RENDER TIME from the
    /// message's own timestamp, never by an internal timer — "a timer
    /// that fires while the app is suspended is a timer that lies". This
    /// is the pure function a Thread view model calls at draw time; the
    /// client itself never spontaneously emits `.noAck`.
    public static let noAckWindow: TimeInterval = 5 * 60

    public static func renderedDeliveryState(
        base: DeliveryState, wantAck: Bool, isBroadcast: Bool, sentAt: Date, now: Date = Date()
    ) -> DeliveryState {
        guard base == .sent, wantAck, !isBroadcast else { return base }
        return now.timeIntervalSince(sentAt) >= noAckWindow ? .noAck : base
    }

    // MARK: - Handshake

    private func performHandshake() async throws {
        Self.log("performHandshake() starting")
        // Step 2: a Heartbeat with its OWN random nonce (never 1, which
        // firmware may special-case) — a keepalive value, NOT part of
        // the want_config mechanism below (conflating the two is the
        // mistake an earlier spec draft made; see MeshtasticConfigNonce).
        try await sendHeartbeat()

        // Step 3: phase A.
        try await requestConfig(nonce: MeshtasticConfigNonce.onlyConfig, timeout: configPhaseTimeout)

        // Step 4: phase B. Never re-sent while already in flight — this
        // function is only ever invoked once per handshake attempt, and
        // `connect()`/`handleTransportReconnected()` are the only
        // callers, both of which run at most one handshake at a time.
        try await requestConfig(nonce: MeshtasticConfigNonce.onlyNodeDB, timeout: nodeDBPhaseTimeout)

        // Step 5: firmware floor check, observed rather than enforced —
        // M1 says so plainly (`isFirmwareBelowSupportedFloor`) rather
        // than failing mysteriously later; it does not hard-fail the
        // connection.
        if let version = firmwareVersion {
            isFirmwareBelowSupportedFloor = MeshtasticClient.isVersion(version, below: MeshtasticClient.minimumSupportedFirmware)
        }
        Self.log("performHandshake() done — firmwareVersion=\(firmwareVersion ?? "nil") myNodeNum=\(myNodeNum.map(String.init) ?? "nil")")
        // Step 6 (.ready) is published by the caller once this returns.
    }

    private func requestConfig(nonce: UInt32, timeout: Duration) async throws {
        pendingConfigPhase = nonce
        defer { if pendingConfigPhase == nonce { pendingConfigPhase = nil } }

        // Subscribe BEFORE writing want_config — see `configCompleteHub`'s
        // doc comment for why this ordering is load-bearing, not
        // stylistic.
        let completions = configCompleteHub.subscribe()

        var toRadio = ToRadio()
        toRadio.wantConfigID = nonce
        guard let bytes = try? toRadio.serializedData() else {
            Self.log("requestConfig(nonce: \(nonce)): encoding ToRadio failed")
            throw MeshtasticClientError.encodingFailed
        }
        Self.log("requestConfig(nonce: \(nonce)): sending want_config, timeout=\(timeout)")
        try await writeToRadio(bytes)

        do {
            try await withThrowingTaskGroup(of: Void.self) { group in
                group.addTask {
                    for await id in completions where id == nonce { return }
                }
                group.addTask {
                    try await Task.sleep(for: timeout)
                    throw MeshtasticClientError.handshakeTimeout(phase: nonce)
                }
                try await group.next()
                group.cancelAll()
            }
        } catch {
            Self.log("requestConfig(nonce: \(nonce)): did not complete — \(error)")
            throw error
        }
        Self.log("requestConfig(nonce: \(nonce)): config_complete_id matched")
    }

    private func sendHeartbeat() async throws {
        var heartbeat = Heartbeat()
        heartbeat.nonce = UInt32.random(in: 2...UInt32.max)
        var toRadio = ToRadio()
        toRadio.heartbeat = heartbeat
        guard let bytes = try? toRadio.serializedData() else {
            throw MeshtasticClientError.encodingFailed
        }
        try await writeToRadio(bytes)
    }

    private func startHeartbeatLoopIfNeeded() {
        guard transport.kind == .stream else { return }
        heartbeatTask?.cancel()
        heartbeatTask = Task { [weak self, heartbeatInterval, heartbeatGrace] in
            while !Task.isCancelled {
                try? await Task.sleep(for: heartbeatInterval)
                guard let self, !Task.isCancelled else { return }
                await self.heartbeatTick(grace: heartbeatGrace)
            }
        }
    }

    private func heartbeatTick(grace: Duration) async {
        if let lastRxAt, Date().timeIntervalSince(lastRxAt) > heartbeatIntervalSeconds() + graceSeconds(grace) {
            publish(.failed("no traffic since \(lastRxAt) — stream transport presumed dead"))
            await disconnect()
            return
        }
        try? await sendHeartbeat()
    }

    private func heartbeatIntervalSeconds() -> TimeInterval {
        Double(heartbeatInterval.components.seconds) + Double(heartbeatInterval.components.attoseconds) / 1e18
    }
    private func graceSeconds(_ d: Duration) -> TimeInterval {
        Double(d.components.seconds) + Double(d.components.attoseconds) / 1e18
    }

    // MARK: - Reboot / reconnect

    /// M2 — background BLE: the transport reconnected on its own (a
    /// pocket-loss reconnect or a node power-cycle,
    /// `BLETransport.handleDisconnected`'s own reconnect-on-loss). A
    /// fresh handshake means a fresh session either way — the radio
    /// resends the full config and node dump, and stale `pendingSends`
    /// reference packet ids the new session knows nothing about — so
    /// `nodeDB`/`pendingSends` are reset exactly ONCE here, before the
    /// retry loop, never per attempt (the node dump is rebuilt once per
    /// reconnect, not once per handshake attempt within it).
    ///
    /// The handshake itself is retried with bounded exponential backoff
    /// (`handshakeRetryLimit` attempts, `handshakeRetryDelay(forAttempt:)`
    /// between them): a node mid-boot after a power cycle may not answer
    /// `want_config` on the very first try even though the BLE link is
    /// already back up (`configPhaseTimeout`/`nodeDBPhaseTimeout` firing
    /// is exactly that case, not a fabricated failure). `.reconnecting
    /// (attempt:)` is published between attempts so the UI can say so
    /// honestly rather than sitting on a silent `.handshaking` for
    /// minutes; `.failed` only once every attempt in the bound is spent.
    private func handleTransportReconnected() async {
        Self.log("handleTransportReconnected() starting the bounded handshake-retry loop")
        nodeDB.reset()
        pendingSends.removeAll()

        var attempt = 0
        while true {
            attempt += 1
            publish(attempt == 1 ? .handshaking : .reconnecting(attempt: attempt))
            do {
                try await performHandshake()
                startHeartbeatLoopIfNeeded()
                publish(.ready)
                return
            } catch {
                guard !Task.isCancelled else {
                    Self.log("handleTransportReconnected(): attempt \(attempt) cancelled")
                    return
                }
                Self.log("handleTransportReconnected(): attempt \(attempt) threw \(error)")
                guard attempt < handshakeRetryLimit else {
                    publish(.failed(String(describing: error)))
                    return
                }
                let delay = Self.handshakeRetryDelay(
                    forAttempt: attempt, base: handshakeRetryBaseDelay, cap: handshakeRetryMaxDelay)
                try? await handshakeRetryClock.sleep(for: delay)
                if Task.isCancelled { return }
            }
        }
    }

    /// Pure, and static so it is testable with no actor and no real
    /// sleeps: doubles `base` after every failed attempt, capped at
    /// `cap`. `attempt` is the 1-based attempt that JUST failed — the
    /// delay returned is how long to wait before the NEXT one.
    ///
    /// Works in fractional seconds via BOTH `Duration` components
    /// (`.seconds` and `.attoseconds`) — same conversion
    /// `heartbeatIntervalSeconds()`/`graceSeconds(_:)` already use below
    /// — rather than `.components.seconds` alone: a sub-second `base`
    /// (every test in `ClientReconnectTests` uses one, to run in
    /// milliseconds rather than minutes) would otherwise truncate to
    /// `0`, silently discarding the whole backoff.
    public static func handshakeRetryDelay(forAttempt attempt: Int, base: Duration, cap: Duration) -> Duration {
        guard attempt > 0 else { return base }
        // `pow(2, attempt - 1)` as a `Double`, not `Int`/`<<`, so a large
        // attempt count (this loop is bounded, but the formula itself
        // should not overflow if that bound is ever raised) saturates
        // toward `.infinity` rather than wrapping negative.
        let multiplier = pow(2.0, Double(attempt - 1))
        let baseSeconds = Double(base.components.seconds) + Double(base.components.attoseconds) / 1e18
        let capSeconds = Double(cap.components.seconds) + Double(cap.components.attoseconds) / 1e18
        return .seconds(min(baseSeconds * multiplier, capSeconds))
    }

    /// The ONE place a handshake-retry loop is (re)started — cancels
    /// whichever `reconnectTask` is currently outstanding (a no-op if
    /// none is) before replacing it, so at most one
    /// `handleTransportReconnected()` is ever running at a time no
    /// matter which of the two triggers fires it: the transport reaching
    /// `.ready` a second time (`consumeTransportEvents`), or a
    /// `FromRadio.rebooted` frame arriving — including one that lands
    /// WHILE an existing retry is already mid-backoff (PR #272 review,
    /// BLOCKING item 1). `handleTransportReconnected()` reissues BOTH
    /// want_config phases from scratch, same two sentinels — never fresh
    /// ones — same as it always has.
    private func restartReconnectTask() {
        reconnectTask?.cancel()
        reconnectTask = Task { [weak self] in
            await self?.handleTransportReconnected()
        }
    }

    private func resetSessionState() {
        heartbeatTask?.cancel(); heartbeatTask = nil
        receiveTask?.cancel(); receiveTask = nil
        nodeDB.reset()
        pendingSends.removeAll()
        myNodeNum = nil
        firmwareVersion = nil
        isFirmwareBelowSupportedFloor = false
        channelNames.removeAll()
        framer = StreamFramer()
        lastRxAt = nil
        hasCompletedInitialConnect = false
    }

    // MARK: - Wire I/O

    private func writeToRadio(_ payload: Data) async throws {
        switch transport.kind {
        case .message:
            try await transport.send(payload)
        case .stream:
            guard let framed = StreamFramer.frame(payload) else {
                throw MeshtasticClientError.encodingFailed
            }
            try await transport.send(framed)
        }
    }

    private func consumeTransportEvents(_ events: AsyncStream<TransportEvent>) async {
        for await event in events {
            Self.log("consumeTransportEvents: received \(Self.describe(event)) (hasCompletedInitialConnect=\(hasCompletedInitialConnect))")
            switch event {
            case .connecting:
                break // the client publishes its own .connecting from connect()
            case .ready:
                // The transport reaching .ready a SECOND time (after the
                // initial connect()'s own await already resolved) means
                // it reconnected on its own — redo the handshake.
                //
                // Spawned as its OWN task, not awaited inline: this
                // `for await` loop is the ONLY reader of `events`, so
                // blocking it here (a bounded backoff loop can sleep for
                // up to a minute between attempts) would stall processing
                // of whatever the transport sends next — including the
                // very `.disconnected`/`.ready` pair a second, faster
                // reconnect would produce. Cancelling any retry already
                // in flight before starting a new one is what keeps at
                // most one handshake attempt outstanding at a time (the
                // client-level "no duplicate" guarantee — see
                // `reconnectTask`'s own doc comment).
                if hasCompletedInitialConnect {
                    restartReconnectTask()
                }
            case .received(let data):
                ingest(data)
            case .disconnected(let reason):
                Self.log("consumeTransportEvents: .disconnected(reason: \(reason ?? "nil")) — publishing .disconnected")
                heartbeatTask?.cancel(); heartbeatTask = nil
                reconnectTask?.cancel(); reconnectTask = nil
                publish(.disconnected)
            }
        }
        Self.log("consumeTransportEvents: transport event stream ended")
    }

    /// Log-only — never dumps raw payload bytes (`.received`'s own
    /// `Data`, which a bare `String(describing:)` would otherwise print
    /// as a byte array and flood the log during a node-dump).
    private static func describe(_ event: TransportEvent) -> String {
        switch event {
        case .connecting: return ".connecting"
        case .ready: return ".ready"
        case .received(let data): return ".received(\(data.count) bytes)"
        case .disconnected(let reason): return ".disconnected(reason: \(reason ?? "nil"))"
        }
    }

    private func ingest(_ data: Data) {
        switch transport.kind {
        case .message:
            if let fr = try? FromRadio(serializedBytes: data) {
                lastRxAt = Date()
                handle(fromRadio: fr)
            } else {
                Self.log("ingest(): \(data.count)B off a .message transport did not decode as FromRadio — dropped")
            }
            // else: malformed bytes off a message transport — dropped
            // silently rather than tearing the link down over one frame.
        case .stream:
            for frame in framer.feed(data) {
                if let fr = try? FromRadio(serializedBytes: frame) {
                    lastRxAt = Date()
                    handle(fromRadio: fr)
                } else {
                    Self.log("ingest(): \(frame.count)B stream frame did not decode as FromRadio — dropped")
                }
            }
        }
    }

    // MARK: - FromRadio dispatch

    private func handle(fromRadio fr: FromRadio) {
        // `Mirror`'s enum case label, not a bespoke `switch` over every
        // `FromRadio.OneOf_PayloadVariant` case just to name it — this
        // is diagnostic-only text, never a decode decision, so the
        // cheaper reflection is the honest tool for the job. Gated by
        // `verboseLog` (never `log`): a node-dump reconnect is hundreds
        // of `.nodeInfo` frames, one line each, and that volume is
        // exactly what `FIREFLY_VERBOSE_LOG`/`-FireflyVerboseLog` exists
        // to opt into rather than force on every run.
        Self.verboseLog("handle(fromRadio:): decoded kind=\(Mirror(reflecting: fr.payloadVariant as Any).children.first?.label ?? String(describing: fr.payloadVariant))")
        switch fr.payloadVariant {
        case .rebooted:
            // BLOCKING 1 (PR #272 review): this used to be an untracked
            // `Task { handleRebooted() }` that awaited
            // `handleTransportReconnected()` INLINE — a second, fully
            // concurrent handshake-retry loop whenever `.rebooted`
            // arrived while a transport-`.ready`-triggered `reconnectTask`
            // was already mid-retry (both would independently
            // `nodeDB.reset()`/`pendingSends.removeAll()` and both send
            // `want_config`). Routed through the SAME cancel-and-replace
            // `restartReconnectTask()` the `.ready` case above uses:
            // exactly one handshake attempt is ever outstanding,
            // regardless of which event triggers the retry.
            Self.log("handle(fromRadio:): .rebooted — restarting the handshake-retry loop")
            heartbeatTask?.cancel(); heartbeatTask = nil
            restartReconnectTask()

        case .myInfo(let info):
            Self.log("handle(fromRadio:): .myInfo myNodeNum=\(info.myNodeNum)")
            myNodeNum = info.myNodeNum

        case .nodeInfo(let info):
            let snapshot = nodeDB.apply(nodeInfo: info)
            nodeHub.yield(snapshot)

        case .metadata(let meta):
            firmwareVersion = meta.firmwareVersion

        case .channel(let ch):
            if ch.hasSettings {
                channelNames.insert(ch.settings.name)
            }

        case .configCompleteID(let id):
            // Always published; `requestConfig`'s waiter filters for the
            // ONE nonce it asked for (`where id == nonce`) and ignores
            // everything else — a config_complete_id matching neither
            // sentinel, or arriving with nobody waiting at all, is
            // silently dropped rather than treated as an error, per
            // mc_client.c's own `config_complete_id` branch.
            Self.log("handle(fromRadio:): .configCompleteID(\(id))")
            configCompleteHub.yield(id)

        case .packet(let pkt):
            handle(meshPacket: pkt)

        default:
            break
        }
    }

    private func handle(meshPacket pkt: MeshPacket) {
        // rx-meta first — mirrors mc_client.c's guarantee that
        // on_rx_meta fires before any payload event, for any packet
        // naming a sender (from == 0 means "sender unknown", nobody to
        // attribute a reading to).
        if pkt.from != 0 {
            applyRxMeta(for: pkt)
        }

        guard case .decoded(let data) = pkt.payloadVariant else {
            return // encrypted — no keys, out of decode scope for M1
        }

        switch data.portnum {
        case .positionApp:
            guard let pb = try? Position(serializedBytes: data.payload) else { return }
            let rxTime: Date? = pkt.hasRxTime ? Date(timeIntervalSince1970: TimeInterval(pkt.rxTime)) : nil
            if let snapshot = nodeDB.apply(position: pb, from: pkt.from, rxTime: rxTime) {
                nodeHub.yield(snapshot)
            }

        case .routingApp:
            guard let routing = try? Routing(serializedBytes: data.payload) else { return }
            handle(routingAck: routing, requestID: data.requestID)

        case .adminApp:
            // M3 — the only AdminMessage traffic this client decodes is a
            // response to a `get_*_request` THIS client sent (`data.
            // requestID` is proto3's `Data.request_id`, the field
            // AdminModule echoes the request's packet id into — the
            // exact correlation `handle(routingAck:requestID:)` already
            // uses for `routingApp`). A malformed payload is dropped
            // silently, same discipline as every other decode here.
            guard let admin = try? AdminMessage(serializedBytes: data.payload) else { return }
            adminResponseHub.yield((requestID: data.requestID, message: admin))

        case .textMessageApp:
            // PR #264 review, BLOCKING item 2: this was `default: break`
            // with "text/telemetry/etc. have no consumer... yet" — the
            // one self-acknowledged gap that left slice E's Inbox with
            // no path for an inbound message to ever reach it. Decode
            // failure (not valid UTF-8) reads absent, same `try?`/
            // `guard let` discipline as every other decode in this file
            // — no force-unwraps, no crash on a malformed payload.
            guard let text = String(data: data.payload, encoding: .utf8) else { return }
            let rxTime: Date? = pkt.hasRxTime ? Date(timeIntervalSince1970: TimeInterval(pkt.rxTime)) : nil
            let meta = rxMeta(for: pkt)
            incomingTextHub.yield(IncomingText(
                from: pkt.from, to: pkt.to, channel: pkt.channel, packetID: pkt.id, text: text,
                rxTime: rxTime, rssiDbm: meta.rssiDbm, snrDb: meta.snrDb, direct: meta.direct))

        default:
            // Firefly's own portnum (269) is not a case in the generated
            // `PortNum` enum at all — it arrives as `.UNRECOGNIZED(269)`
            // (`WireFormatTests.testFireflyPortnumSurvivesAsUnrecognized`
            // pins that), so it is matched on `rawValue` here rather
            // than by case. The frame itself stays OPAQUE: decoding it
            // is `FireflyPacket`/`ff_proto`'s job on the other side of
            // the bridge boundary (`IncomingPrivate`'s doc comment).
            guard data.portnum.rawValue == Int(fireflyPrivatePortNum) else {
                break // out of decode scope for M1 (telemetry/etc. have
                       // no consumer through MeshtasticClientProtocol)
            }
            let rxTime: Date? = pkt.hasRxTime ? Date(timeIntervalSince1970: TimeInterval(pkt.rxTime)) : nil
            let meta = rxMeta(for: pkt)
            incomingPrivateHub.yield(IncomingPrivate(
                from: pkt.from, to: pkt.to, channel: pkt.channel, packetID: pkt.id, payload: data.payload,
                rxTime: rxTime, rssiDbm: meta.rssiDbm, snrDb: meta.snrDb, direct: meta.direct))
        }
    }

    /// The plausibility-gated RSSI/SNR/hop-path triple, pulled out of
    /// `applyRxMeta(for:)` so `handle(meshPacket:)`'s `.textMessageApp`
    /// case can report the SAME per-packet meta on `IncomingText`
    /// without duplicating the gating rules (or drifting from them).
    /// Pure — no NodeDB mutation, no hub yield.
    private func rxMeta(for pkt: MeshPacket) -> (rssiDbm: Int16?, snrDb: Float?, path: RxPath, direct: Bool?) {
        let hasDecodedBitfield: Bool = {
            if case .decoded(let d) = pkt.payloadVariant { return d.hasBitfield }
            return false
        }()

        // Plausibility-gated exactly like mc_client.c's mc_emit_rx_meta:
        // a value outside a radio's physically possible range is not a
        // measurement, reported absent rather than clamped or passed
        // through.
        let rssi: Int16? = (pkt.hasRxRssi && pkt.rxRssi >= -512 && pkt.rxRssi <= 512) ? Int16(pkt.rxRssi) : nil

        // rx_snr has proto3 IMPLICIT presence: exactly 0.0 is
        // byte-identical to absent, so it reads unknown rather than a
        // real 0.0 dB reading (under-claim, never fabricate). NaN needs
        // its own test (`v == v` is false only for NaN) since it would
        // otherwise pass a bare `!= 0.0` check while poisoning every
        // downstream comparison.
        let snr: Float? = {
            let v = pkt.rxSnr
            guard v == v, v != 0.0, v >= -128.0, v <= 128.0 else { return nil }
            return v
        }()

        let path = NodeDB.rxPath(hopStart: pkt.hopStart, hopLimit: pkt.hopLimit,
                                  hasDecodedBitfield: hasDecodedBitfield, viaMqtt: pkt.viaMqtt)
        // `RxPath` itself never crosses into `FireflyModel` (its own doc
        // comment) — `direct` is the plain-`Bool?` translation
        // `IncomingText` actually carries. `.unknown` stays nil: a hop
        // count that could not be established is not evidence of zero
        // hops, same rule `NodeDB.rxPath` documents.
        let direct: Bool? = { switch path { case .direct: return true; case .indirect: return false; case .unknown: return nil } }()

        return (rssi, snr, path, direct)
    }

    private func applyRxMeta(for pkt: MeshPacket) {
        let meta = rxMeta(for: pkt)
        if let snapshot = nodeDB.applyRxMeta(from: pkt.from, rssiDbm: meta.rssiDbm, snrDb: meta.snrDb, path: meta.path) {
            nodeHub.yield(snapshot)
        }
    }

    private func handle(routingAck routing: Routing, requestID: UInt32) {
        guard let pending = pendingSends.removeValue(forKey: requestID), pending.wantAck, !pending.isBroadcast else {
            // Not ours, not want_ack'd, or a broadcast: broadcasts never
            // get promoted past SENT — "delivered to the mesh" is not
            // delivery to a person.
            return
        }

        // `request_id` reports NONE (ok) or a specific reason (NAK). An
        // ABSENT error_reason variant also reads NONE/ok — proto3
        // implicit presence makes it byte-identical to an explicit NONE,
        // same as mc_client.c's on_routing_ack semantics.
        let ok: Bool
        if case .errorReason(let reason) = routing.variant {
            ok = (reason == .none)
        } else {
            ok = true
        }

        // `.delivered`/`.noAck` — NEVER `.dropped` (PR #264 review,
        // BLOCKING item 1's partitioning): `.dropped` is reserved for an
        // outbox-full eviction or a send the transport refused outright,
        // neither of which has a packet id yet. A routing NAK answers a
        // packet id that WAS accepted and sent; the honest terminal
        // state for that is NO ACK, mirroring `ff_shell.c`'s own
        // `ff_feed_set_ack_by_packet_id(..., ok: false)` call for a NAK.
        deliveryHub.yield(ok ? .delivered(packetID: PacketID(requestID)) : .noAck(packetID: PacketID(requestID)))
    }

    /// SHOULD-FIX 4 — see `pendingSendsMaxAge`'s own doc comment. Called
    /// after every new `sendText` insertion; a no-op most of the time
    /// (cheap age scan over a dict that in practice holds a handful of
    /// in-flight `want_ack` DMs).
    private func prunePendingSends(now: Date = Date()) {
        guard !pendingSends.isEmpty else { return }
        let cutoff = now.addingTimeInterval(-Self.pendingSendsMaxAge)
        pendingSends = pendingSends.filter { $0.value.sentAt >= cutoff }
    }

    // MARK: - Packet / outbox ids

    private func nextOutboxID() -> UInt32 {
        let id = outboxIDCounter
        let next = outboxIDCounter &+ 1
        // Never let the local-only outbox id space collide with `0`
        // either — no functional requirement forces this (unlike the
        // packet id, it never crosses the wire), but keeping the same
        // "0 is never a valid id" convention avoids a footgun for any
        // future caller that assumes it, same as `nextPacketID()`.
        outboxIDCounter = next == 0 ? 1 : next
        return id
    }

    private func nextPacketID() -> UInt32 {
        let id = packetIDCounter
        let next = packetIDCounter &+ 1
        // 0 is never a valid Meshtastic packet id (the wire protocol's
        // own "unset" convention) — skip it on wraparound.
        packetIDCounter = next == 0 ? 1 : next
        return id
    }

    // MARK: - Firmware floor

    static let minimumSupportedFirmware = "2.7.26"

    /// Dotted-version compare, tolerant of a non-numeric suffix on any
    /// component (a build metadata tag, say) — only the leading digits
    /// of each component are compared, and a missing trailing component
    /// on either side reads as 0.
    static func isVersion(_ version: String, below floor: String) -> Bool {
        func parts(_ s: String) -> [Int] {
            s.split(separator: ".").map { component -> Int in
                let digits = component.prefix { $0.isNumber }
                return Int(digits) ?? 0
            }
        }
        let v = parts(version)
        let f = parts(floor)
        for i in 0..<max(v.count, f.count) {
            let a = i < v.count ? v[i] : 0
            let b = i < f.count ? f[i] : 0
            if a != b { return a < b }
        }
        return false
    }
}
