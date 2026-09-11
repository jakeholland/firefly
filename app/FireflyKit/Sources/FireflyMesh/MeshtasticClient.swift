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
}

/// The real Meshtastic client: drives `MeshTransport`, runs the
/// handshake, maintains `NodeDB`, and turns routing acks into
/// `DeliveryState` transitions. An actor — CoreBluetooth delegate
/// callbacks, a serial read source and a TCP receive loop all land off
/// the main thread (docs/specs/A01-companion-app.md, "Threading model"),
/// and funnel into this single isolation domain rather than the
/// `@MainActor` the C core bridge uses.
public actor MeshtasticClient: MeshtasticClientProtocol {
    private let transport: MeshTransport
    private let linkHub = EventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let incomingTextHub = EventHub<IncomingText>()

    private var nodeDB = NodeDB()
    private var framer = StreamFramer()

    private var myNodeNum: UInt32?
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

    public init(
        transport: MeshTransport,
        configPhaseTimeout: Duration = .seconds(30),
        nodeDBPhaseTimeout: Duration = .seconds(120),
        heartbeatInterval: Duration = .seconds(15),
        heartbeatGrace: Duration = .seconds(5)
    ) {
        self.transport = transport
        self.configPhaseTimeout = configPhaseTimeout
        self.nodeDBPhaseTimeout = nodeDBPhaseTimeout
        self.heartbeatInterval = heartbeatInterval
        self.heartbeatGrace = heartbeatGrace
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

    public func connect() async throws {
        resetSessionState()

        // Subscribe to the transport's events BEFORE calling connect():
        // `events()` is multicast via EventHub (S1) and does not replay,
        // so a subscription registered after `.ready` is published would
        // simply miss it.
        let events = transport.events()
        receiveTask = Task { [weak self] in
            await self?.consumeTransportEvents(events)
        }

        linkHub.yield(.connecting)
        do {
            // For BLE this does not return until the FROMNUM
            // subscription is ACKed (MeshtasticBLE.swift,
            // FromRadioDrainPolicy) — only then is it safe to send
            // want_config.
            try await transport.connect()
        } catch {
            linkHub.yield(.failed(String(describing: error)))
            throw error
        }

        linkHub.yield(.handshaking)
        do {
            try await performHandshake()
        } catch {
            linkHub.yield(.failed(String(describing: error)))
            throw error
        }

        hasCompletedInitialConnect = true
        startHeartbeatLoopIfNeeded()
        linkHub.yield(.ready)
    }

    public func disconnect() async {
        heartbeatTask?.cancel(); heartbeatTask = nil
        receiveTask?.cancel(); receiveTask = nil
        hasCompletedInitialConnect = false
        await transport.disconnect()
        linkHub.yield(.disconnected)
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
            throw MeshtasticClientError.encodingFailed
        }
        try await writeToRadio(bytes)

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
            linkHub.yield(.failed("no traffic since \(lastRxAt) — stream transport presumed dead"))
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

    private func handleTransportReconnected() async {
        linkHub.yield(.handshaking)
        // A fresh handshake means a fresh session, whether it was
        // triggered by `FromRadio.rebooted` or by the transport
        // reconnecting on its own: the radio resends the full config and
        // node dump either way, and stale `pendingSends` reference
        // packet ids the new session knows nothing about.
        nodeDB.reset()
        pendingSends.removeAll()
        do {
            try await performHandshake()
            startHeartbeatLoopIfNeeded()
            linkHub.yield(.ready)
        } catch {
            linkHub.yield(.failed(String(describing: error)))
        }
    }

    /// `FromRadio.rebooted` is an immediate session loss, not something
    /// to discover via a silence timeout (docs/specs/A01-companion-app.md,
    /// "Handshake"; the same lesson `mc_client.c`'s
    /// `meshtastic_FromRadio_rebooted_tag` handling encodes for the
    /// puck). Reissues BOTH want_config phases from scratch, same two
    /// sentinels — never fresh ones.
    private func handleRebooted() async {
        heartbeatTask?.cancel(); heartbeatTask = nil
        await handleTransportReconnected()
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
            switch event {
            case .connecting:
                break // the client publishes its own .connecting from connect()
            case .ready:
                // The transport reaching .ready a SECOND time (after the
                // initial connect()'s own await already resolved) means
                // it reconnected on its own — redo the handshake.
                if hasCompletedInitialConnect {
                    await handleTransportReconnected()
                }
            case .received(let data):
                ingest(data)
            case .disconnected:
                heartbeatTask?.cancel(); heartbeatTask = nil
                linkHub.yield(.disconnected)
            }
        }
    }

    private func ingest(_ data: Data) {
        switch transport.kind {
        case .message:
            if let fr = try? FromRadio(serializedBytes: data) {
                lastRxAt = Date()
                handle(fromRadio: fr)
            }
            // else: malformed bytes off a message transport — dropped
            // silently rather than tearing the link down over one frame.
        case .stream:
            for frame in framer.feed(data) {
                if let fr = try? FromRadio(serializedBytes: frame) {
                    lastRxAt = Date()
                    handle(fromRadio: fr)
                }
            }
        }
    }

    // MARK: - FromRadio dispatch

    private func handle(fromRadio fr: FromRadio) {
        switch fr.payloadVariant {
        case .rebooted:
            Task { [weak self] in await self?.handleRebooted() }

        case .myInfo(let info):
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
            break // out of decode scope for M1 (telemetry/etc. have no
                   // consumer through MeshtasticClientProtocol yet)
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
