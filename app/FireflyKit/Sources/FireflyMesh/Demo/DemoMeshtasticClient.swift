//
//  DemoMeshtasticClient.swift — S20's honesty rule, ported to the app:
//  "demo data is REAL state seeded through the REAL core APIs... it is
//  not faked freshness or a canned splash" (docs/specs/
//  S20-demo-mode.md). This type is the ONLY thing that is fictional —
//  everything downstream of it (CoreStore, `ff_crew`/`ff_feed`, every
//  view model) is the exact same object code the live graph runs. The
//  iOS Simulator has no Bluetooth at all (`AppDependencies.swift`'s own
//  `.current()` comment), so this is what lets it show every screen
//  with data that behaves like the real thing instead of an empty
//  Simulator or a set of static screenshots.
//
//  Structurally this is `StubMeshtasticClient` with a script: same
//  hubs, same "never invents outside what it is explicitly told to
//  yield" discipline, plus a scripted node dump, a scripted incoming
//  text, and a scripted ack outcome per DM so a demo thread can show
//  WAITING -> SENT -> DELIVERED and WAITING -> SENT -> NO ACK without
//  a human tapping anything. `DemoRunner` (FireflyModel) is what
//  decides WHEN each of these fires and what the fictional crew looks
//  like (`DemoWorld`) — this type only knows how to play a script, not
//  what Firefly Fields is.
//
import Foundation

/// One scripted response to a `sendText`/`sendPrivate` that set
/// `wantAck`. `.delivered`/`.noAck` fire a `DeliveryEvent` a short,
/// screenshot-friendly delay after SENT; `.none` leaves the message at
/// SENT forever, same as `StubMeshtasticClient`'s honest "no mesh to
/// ack it" default.
public enum DemoAckOutcome: Sendable, Equatable {
    case delivered
    case noAck
    case none
}

/// A scripted, non-empty answer to a FIND ping (`FireflyPacket.ping`,
/// portnum 269) — the "Find mode" screenshot needs a PONG to arrive so
/// FIND's trend/haptic path has something to report. `DemoRunner`
/// builds the actual `ff_proto` bytes (it can import `FireflyModel`;
/// this module, `FireflyMesh`, may not) and hands them to
/// `injectIncomingPrivate` — `onSendPrivate` is only the hook that
/// tells it a PING just went out.
public final class DemoMeshtasticClient: MeshtasticClientProtocol, @unchecked Sendable {
    // `CurrentValueEventHub` (M1 review follow-up, #267) — see
    // `MeshtasticClientProtocol.swift`'s `StubMeshtasticClient` and
    // `CurrentValueEventHub`'s own doc comment. Without this, navigating
    // to Thread/Diagnostics after demo `connect()` already reached
    // `.ready` showed a stale "NODE NOT CONNECTED" banner.
    private let linkHub = CurrentValueEventHub<LinkState>()
    private let nodeHub = EventHub<MeshNodeSnapshot>()
    private let deliveryHub = EventHub<DeliveryEvent>()
    private let incomingTextHub = EventHub<IncomingText>()
    private let incomingPrivateHub = EventHub<IncomingPrivate>()

    private let lock = NSLock()
    private let myNodeNum: UInt32
    private let scriptedNodes: [MeshNodeSnapshot]
    /// Milliseconds — kept short (not `ff_shell`'s real ack-timeout
    /// window) so a screenshot/video script never has to wait out a
    /// real mesh's timing to see DELIVERED/NO ACK land.
    private let ackDelayMs: UInt64

    private var nextOutboxID: UInt32 = 1
    private var nextPacketID: UInt32 = 1
    private var _connectedNodeNum: UInt32?
    /// FIFO, consumed once per `wantAck: true` send — see
    /// `DemoAckOutcome`. Repeats the LAST entry once exhausted, rather
    /// than falling back to a hidden default, so a test can see exactly
    /// how many scripted outcomes it configured.
    private var ackOutcomes: [DemoAckOutcome]
    private var sentTexts: [(String, UInt32, Bool)] = []
    private var sentPositions: [(ExternalPositionFix, UInt32)] = []
    private var sentPrivate: [(Data, UInt32, Bool)] = []

    /// Called synchronously from `sendPrivate`, before the transport
    /// "write" — `DemoRunner` uses this to notice a FIND ping went out
    /// and schedule a scripted PONG. Never consulted for anything else;
    /// a real client has no such hook at all.
    public var onSendPrivate: (@Sendable (Data, UInt32, Bool) -> Void)?

    public init(myNodeNum: UInt32, nodes: [MeshNodeSnapshot], ackOutcomes: [DemoAckOutcome] = [.delivered, .noAck],
                ackDelayMs: UInt64 = 900) {
        self.myNodeNum = myNodeNum
        self.scriptedNodes = nodes
        self.ackOutcomes = ackOutcomes
        self.ackDelayMs = ackDelayMs
    }

    public func linkState() -> AsyncStream<LinkState> { linkHub.subscribe() }
    public func nodeUpdates() -> AsyncStream<MeshNodeSnapshot> { nodeHub.subscribe() }
    public func deliveryUpdates() -> AsyncStream<DeliveryEvent> { deliveryHub.subscribe() }
    public func incomingTexts() -> AsyncStream<IncomingText> { incomingTextHub.subscribe() }
    public func incomingPrivate() -> AsyncStream<IncomingPrivate> { incomingPrivateHub.subscribe() }

    public var connectedNodeNum: UInt32? {
        get { lock.lock(); defer { lock.unlock() }; return _connectedNodeNum }
        set { lock.lock(); defer { lock.unlock() }; _connectedNodeNum = newValue }
    }

    /// Plays connecting -> handshaking -> [the scripted nodeDB dump] ->
    /// ready, with the same two-phase shape `MeshtasticClient`'s real
    /// handshake has (`MeshtasticConfigNonce`'s own doc comment):
    /// `want_config` first, the node database second. Short, fixed
    /// delays between stages — not zero, so a screenshot/video script
    /// driving this through `.task` sees the same CONNECTING/
    /// HANDSHAKING chrome a real connect briefly shows, never an
    /// instant teleport to READY that would make Connect's own states
    /// untestable in demo mode.
    public func connect() async throws {
        linkHub.yield(.connecting)
        try? await Task.sleep(nanoseconds: 150_000_000)
        linkHub.yield(.handshaking)
        try? await Task.sleep(nanoseconds: 150_000_000)
        for node in scriptedNodes {
            nodeHub.yield(node)
        }
        connectedNodeNum = myNodeNum
        linkHub.yield(.ready)
    }

    public func disconnect() async {
        // PR #265 review, should-fix: a disconnect must clear who we
        // were connected to — the same fix as `MeshtasticClient
        // .disconnect()` (`MeshtasticClient.swift`), applied here too
        // so the demo client cannot disagree with the real one about
        // what "disconnected" means to `connectedNodeNum` readers
        // (`PhoneGPSUplink.destinationNodeNum`, Diagnostics).
        connectedNodeNum = nil
        linkHub.yield(.disconnected)
    }

    @discardableResult
    public func sendText(_ text: String, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        let outboxID = nextOutbox()
        deliveryHub.yield(.waiting(outboxID: OutboxID(outboxID)))
        let packetID = nextPacket()
        lock.lock(); sentTexts.append((text, destination, wantAck)); lock.unlock()
        deliveryHub.yield(.sent(outboxID: OutboxID(outboxID), packetID: PacketID(packetID), wantAck: wantAck))
        if wantAck {
            scheduleAck(packetID: packetID)
        }
        return packetID
    }

    @discardableResult
    public func sendPosition(_ fix: ExternalPositionFix, to destination: UInt32) async throws -> UInt32 {
        lock.lock(); sentPositions.append((fix, destination)); lock.unlock()
        return nextPacket()
    }

    @discardableResult
    public func sendPrivate(_ payload: Data, to destination: UInt32, wantAck: Bool) async throws -> UInt32 {
        onSendPrivate?(payload, destination, wantAck)
        lock.lock(); sentPrivate.append((payload, destination, wantAck)); lock.unlock()
        let packetID = nextPacket()
        if wantAck {
            scheduleAck(packetID: packetID)
        }
        return packetID
    }

    // MARK: - Script injection (DemoRunner's own vocabulary — never
    // used by anything downstream of the client, exactly like the
    // rest of this file: only THIS type is allowed to be fictional).

    public func injectNodeUpdate(_ snapshot: MeshNodeSnapshot) { nodeHub.yield(snapshot) }
    public func injectIncomingText(_ incoming: IncomingText) { incomingTextHub.yield(incoming) }
    public func injectIncomingPrivate(_ incoming: IncomingPrivate) { incomingPrivateHub.yield(incoming) }

    // MARK: - Test/inspection surface, same shape as StubMeshtasticClient

    public var sentTextLog: [(String, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }; return sentTexts
    }
    public var sentPositionLog: [(ExternalPositionFix, UInt32)] {
        lock.lock(); defer { lock.unlock() }; return sentPositions
    }
    public var sentPrivateLog: [(Data, UInt32, Bool)] {
        lock.lock(); defer { lock.unlock() }; return sentPrivate
    }

    // MARK: - Private

    private func scheduleAck(packetID: UInt32) {
        let outcome = nextAckOutcome()
        guard outcome != .none else { return }
        let hub = deliveryHub
        let delay = ackDelayMs
        Task {
            try? await Task.sleep(nanoseconds: delay * 1_000_000)
            switch outcome {
            case .delivered: hub.yield(.delivered(packetID: PacketID(packetID)))
            case .noAck: hub.yield(.noAck(packetID: PacketID(packetID)))
            case .none: break
            }
        }
    }

    private func nextAckOutcome() -> DemoAckOutcome {
        lock.lock(); defer { lock.unlock() }
        guard !ackOutcomes.isEmpty else { return .delivered }
        if ackOutcomes.count > 1 {
            return ackOutcomes.removeFirst()
        }
        return ackOutcomes[0] // repeat the last entry rather than draining to a hidden default
    }

    private func nextOutbox() -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        let id = nextOutboxID
        nextOutboxID &+= 1
        return id
    }

    private func nextPacket() -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        let id = nextPacketID
        nextPacketID &+= 1
        return id
    }
}
