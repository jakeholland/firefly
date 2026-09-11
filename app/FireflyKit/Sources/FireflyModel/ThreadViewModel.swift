//
//  ThreadViewModel.swift — one conversation's thread (docs/specs/
//  A01-companion-app.md, slice E; docs/specs/S24-signals-inbox.md).
//
//  Bubbles oldest -> newest, a real-keyboard compose bar (no T9 — A01's
//  "Scope cuts": "T9 (S08). Out, permanently. A phone has a keyboard."),
//  quick-reply chips, FLARE send/receive, and the outbox: WAITING ->
//  SENT -> DELIVERED / NO ACK, queued and flushed on reconnect.
//
//  The outbox queue itself is this app's own: `ff_shell.c` (the puck's
//  `shell_send_or_queue_text` / bounded FIFO / `FF_SHELL_OUTBOX_CAP`,
//  `S24-signals-inbox.md`'s 2026-09-07 amendment) is not part of
//  `firmware/core` and so is not linked into this app (A01's "Building
//  blocks reused" table lists only `core/`). Reimplementing the SAME
//  bounded-FIFO-drop-oldest policy here is not a second implementation of
//  shared core logic — it is this app's own equivalent of a module that
//  was never shared to begin with.
//
import FireflyMesh
import Foundation
import Observation

/// A one-tap thread affordance. `Meet at…` seeds the compose bar instead
/// of sending immediately: a real place picker (Rally/festpack) is out of
/// scope for M1 (A01, "Scope cuts": "Map face (S09)... festpack (S05).
/// Out of M1-M3"), so the honest one-tap affordance is "start the
/// sentence for me," never a fabricated place.
public struct QuickReply: Sendable, Equatable, Identifiable {
    public var id: String { label }
    public let label: String
    public let seedsComposeText: Bool

    public init(label: String, seedsComposeText: Bool) {
        self.label = label
        self.seedsComposeText = seedsComposeText
    }
}

private final class OutboxIDGenerator: @unchecked Sendable {
    static let shared = OutboxIDGenerator()
    private let lock = NSLock()
    private var counter: UInt64 = 1
    func next() -> UInt64 {
        lock.lock(); defer { lock.unlock() }
        let value = counter
        counter &+= 1
        return value
    }
}

@MainActor
@Observable
public final class ThreadViewModel {
    public let conversation: ConversationKind
    public private(set) var messages: [FeedMessage] = []
    public var composeText: String = ""
    public private(set) var isLinkReady = false
    /// How many of this thread's sends are sitting in the local outbox,
    /// waiting for the link to come back — the thread's own "N queued"
    /// affordance.
    public private(set) var queuedCount = 0

    public static let quickReplies: [QuickReply] = [
        QuickReply(label: "Omw", seedsComposeText: false),
        QuickReply(label: "Here", seedsComposeText: false),
        QuickReply(label: "Wait", seedsComposeText: false),
        QuickReply(label: "Meet at…", seedsComposeText: true),
    ]

    /// A01, "Routing ACK -> delivery state": "5 minutes elapsed with no
    /// routing packet" -> NO ACK, "derived at render time... not driven
    /// by a timer" — a timer that fires while the app is suspended would
    /// lie. `renderedDeliveryState(for:now:)` is this rule, applied at
    /// render time on top of whatever is actually stored, never mutating
    /// the stored value.
    public static let noAckRenderWindow: TimeInterval = 5 * 60

    /// This app's own bounded outbox cap — see this file's header
    /// comment. 8 mirrors `FF_SHELL_OUTBOX_CAP`
    /// (`S24-signals-inbox.md`'s amendment), the same judgment call
    /// ("generous enough that a brief reconnect never loses a message...
    /// without holding unbounded unsent text in RAM").
    public static let outboxCap = 8

    private struct PendingSend {
        let outboxID: UInt64
        let text: String
        let kind: MessageKind
        let flareDurationSeconds: UInt16?
    }

    private let provider: any InboxProviding
    private let client: any MeshtasticClientProtocol
    private var linkObservation: Task<Void, Never>?
    private var deliveryObservation: Task<Void, Never>?
    private var outbox: [PendingSend] = []

    public init(conversation: ConversationKind, provider: any InboxProviding, client: any MeshtasticClientProtocol) {
        self.conversation = conversation
        self.provider = provider
        self.client = client
    }

    /// Idempotent, like every other view model's `observe()`. Subscribes
    /// to BOTH `linkState()` (to flush the outbox on reconnect) and
    /// `deliveryUpdates()` (A01's Slice E entry: "this view model's own,
    /// independent of CoreStore's").
    public func observe() {
        guard linkObservation == nil else { return }
        let links = client.linkState()
        let deliveries = client.deliveryUpdates()

        linkObservation = Task { [weak self] in
            for await state in links {
                guard let self else { return }
                let ready = (state == .ready)
                let wasReady = self.isLinkReady
                self.isLinkReady = ready
                if ready, !wasReady { await self.flushOutbox() }
            }
        }
        deliveryObservation = Task { [weak self] in
            for await (packetID, state) in deliveries {
                guard let self else { return }
                self.provider.setStatus(packetID: packetID, state: state, at: Date())
                self.refresh()
            }
        }
        refresh()
    }

    public func stopObserving() {
        linkObservation?.cancel(); linkObservation = nil
        deliveryObservation?.cancel(); deliveryObservation = nil
    }

    public func refresh(now: Date = Date()) {
        messages = provider.thread(for: conversation, now: now)
        provider.markRead(conversation)
    }

    // MARK: - Composing and sending

    public func tap(_ reply: QuickReply) async {
        if reply.seedsComposeText {
            composeText = "Meet at "
            return
        }
        await send(text: reply.label, kind: .text)
    }

    public func sendCompose() async {
        let text = composeText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        composeText = ""
        await send(text: text, kind: .text)
    }

    /// FLARE send (`S04-firefly-protocol.md` type `0x02`, default
    /// 300 s). Routed through the same text/outbox/delivery pipeline as
    /// everything else: `MeshtasticClientProtocol` — the one seam this
    /// slice may depend on — has no portnum-269 send entry point yet
    /// (`ff_proto`'s wire encoding is S04/slice A territory, and
    /// `MeshtasticClientProtocol.swift` is not one of slice E's owned or
    /// shared-hunk files). Disclosed gap, not a silent shortcut: once the
    /// client grows a private-port send method, only this function's
    /// body changes.
    public func sendFlare(durationSeconds: UInt16 = 300) async {
        await send(text: "FLARE", kind: .flare, flareDurationSeconds: durationSeconds)
    }

    /// The delivery-state table's own "resend action" for a NO ACK
    /// (or DROPPED) message: re-attempts the exact same content as a
    /// fresh send.
    public func resend(_ message: FeedMessage) async {
        guard message.direction == .out else { return }
        await send(text: message.text, kind: message.kind, flareDurationSeconds: message.flareDurationSeconds)
    }

    /// The state a bubble should RENDER for one of my OUT messages —
    /// `nil` for anything else (an inbound item, or an OUT item that
    /// predates tracking: `FF_SEND_NONE`'s honest "no label at all",
    /// A01/S24). Applies the render-time no-ack window on top of
    /// whatever is actually stored, per this type's `noAckRenderWindow`
    /// doc comment.
    public func renderedDeliveryState(for message: FeedMessage, now: Date = Date()) -> DeliveryState? {
        guard message.direction == .out, let stored = message.deliveryState else { return nil }
        let isBroadcast = (message.destination == nil) || (message.destination == meshBroadcastAddress)
        guard stored == .sent, !isBroadcast else { return stored }
        return now.timeIntervalSince(message.statusAt) >= Self.noAckRenderWindow ? .noAck : stored
    }

    private var destination: UInt32 {
        switch conversation {
        case .crew: return meshBroadcastAddress
        case .member(let id): return id
        }
    }

    private func send(text: String, kind: MessageKind, flareDurationSeconds: UInt16? = nil) async {
        let dest = destination
        let wantAck = (conversation != .crew) // nothing acks a broadcast (A01)
        let outboxID = OutboxIDGenerator.shared.next()
        let now = Date()

        // Pushed WAITING before any send is attempted — visible in the
        // thread the instant the tap lands, whatever the link is doing
        // (S24's 2026-09-07 amendment: "it is visible in its thread the
        // instant SEND is pressed").
        let pending = FeedMessage(id: outboxID, kind: kind, direction: .out, text: text, timestamp: now,
                                   flareDurationSeconds: flareDurationSeconds, destination: dest,
                                   deliveryState: .waiting, statusAt: now)
        provider.push(pending, into: conversation)
        refresh()

        guard isLinkReady else {
            enqueue(PendingSend(outboxID: outboxID, text: text, kind: kind,
                                 flareDurationSeconds: flareDurationSeconds))
            return
        }
        await attemptSend(outboxID: outboxID, text: text, dest: dest, wantAck: wantAck,
                           fallback: PendingSend(outboxID: outboxID, text: text, kind: kind,
                                                  flareDurationSeconds: flareDurationSeconds))
    }

    private func attemptSend(outboxID: UInt64, text: String, dest: UInt32, wantAck: Bool,
                              fallback: PendingSend) async {
        do {
            let packetID = try await client.sendText(text, to: dest, wantAck: wantAck)
            provider.markSent(outboxID: outboxID, packetID: packetID, at: Date())
            refresh()
        } catch {
            // A genuine transport error, not just "link not ready" (that
            // path never reaches here — see `send(text:kind:)`): queue it
            // the same way, rather than silently dropping it.
            enqueue(fallback)
        }
    }

    private func enqueue(_ item: PendingSend) {
        outbox.append(item)
        if outbox.count > Self.outboxCap {
            // Bounded FIFO, drop-oldest — and the eviction is made
            // VISIBLE, never silent (S24's amendment): the dropped
            // entry's own feed item flips to DROPPED.
            let dropped = outbox.removeFirst()
            provider.setStatus(outboxID: dropped.outboxID, state: .dropped, at: Date())
            refresh()
        }
        queuedCount = outbox.count
    }

    /// Flushed automatically on the link's next not-ready -> ready edge
    /// (S24's amendment: "flushed automatically the next time the link
    /// reaches MC_STATE_READY").
    private func flushOutbox() async {
        guard isLinkReady else { return }
        let pending = outbox
        outbox.removeAll()
        queuedCount = 0
        let wantAck = (conversation != .crew)
        let dest = destination
        for item in pending {
            guard isLinkReady else { enqueue(item); continue }
            await attemptSend(outboxID: item.outboxID, text: item.text, dest: dest, wantAck: wantAck, fallback: item)
        }
    }
}

// Reference identity is the right notion of equality/hashability here —
// two `ThreadViewModel`s are "the same thread" only if they are the same
// observed instance (SwiftUI's `navigationDestination(item:)` needs
// `Hashable`, and `Identifiable` makes the view-side binding natural).
extension ThreadViewModel: Identifiable, Hashable {
    public nonisolated var id: ObjectIdentifier { ObjectIdentifier(self) }
    public static nonisolated func == (lhs: ThreadViewModel, rhs: ThreadViewModel) -> Bool { lhs === rhs }
    public nonisolated func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }
}
