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

/// This seam's own node-address type — kept distinct from a bare
/// `UInt32` only so a future real conformance's call site reads as "a
/// node," matching `MeshNodeSnapshot.num`'s underlying type
/// (`MeshtasticClientProtocol.swift`) without this slice needing to
/// depend on that file to say so.
public typealias NodeID = UInt32

/// The narrow seam FLARE needs and `MeshtasticClientProtocol` does not
/// provide today: a private-portnum send (`S04-firefly-protocol.md`:
/// portnum 269, type `0x02`, body `[dur_s:2]`, `want_ack = true`) —
/// never `TEXT_MESSAGE_APP`/`sendText` (BLOCKING review item 2: a real
/// FLARE transmitted as plain text is worse than failing honestly,
/// since a receiver never takes over its screen or locks its arrow the
/// way S04 promises). The actual wire encoding is slice B's
/// `FireflyPacket`/`ff_proto` territory, and wiring a real conformance
/// onto a portnum-269 send is slice A's client — neither has landed in
/// this worktree (A01's six slices build in parallel). This slice
/// defines ONLY the seam (here) and, in its own tests, a mock
/// conformance — never a body that falls back to `sendText`.
/// `to: nil` means broadcast, with crew filtering happening
/// receiver-side, exactly as S04's own "Addressing" rule specifies —
/// this seam does not filter.
public protocol FireflyPacketSending: AnyObject, Sendable {
    func sendFlare(to: NodeID?, durationSeconds: UInt16) async throws
}

/// A transient, non-queued failure surfaced by a quick-reply or FLARE
/// tap. Unlike free-typed compose text, neither is ever queued into
/// the bounded outbox (BLOCKING review item 3 — S24's 2026-09-07
/// amendment: canned replies "still call send_text with no
/// out_packet_id and no outbox tracking; a link-down tap on one still
/// fails outright exactly as it did before this amendment... Flare/
/// Rally sends are likewise unchanged... only FEED_TEXT sends... go
/// through the outbox"). The view reads this, shows it, and it is
/// cleared on the next attempt — never silently retried later.
public enum ImmediateSendFailure: Sendable, Equatable {
    /// The link was down at the moment of the tap.
    case linkDown
    /// The link was up but the send itself failed (a genuine transport
    /// error, or — for FLARE only — the routing ack simply never came
    /// back before some future tracking window; not modeled yet).
    case transportError
    /// FLARE specifically: no `FireflyPacketSending` conformance was
    /// injected, so there is no wire path to attempt at all.
    case flareUnavailable
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
    /// affordance. Free-text compose only: quick replies and FLARE
    /// never contribute to this count (BLOCKING review item 3).
    public private(set) var queuedCount = 0
    /// The view's transient, non-queued failure for a quick-reply or
    /// FLARE tap — see `ImmediateSendFailure`'s own doc comment. Reset
    /// to `nil` at the start of every quick-reply/FLARE attempt.
    public private(set) var immediateSendFailure: ImmediateSendFailure?

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
    /// The FLARE seam — see `FireflyPacketSending`'s doc comment. `nil`
    /// is today's default live wiring: no slice has landed a real
    /// portnum-269 conformance yet, so FLARE renders disabled
    /// (`flareAvailable`) rather than falling back to a placeholder
    /// transmission.
    private let flareSender: (any FireflyPacketSending)?
    private var linkObservation: Task<Void, Never>?
    private var deliveryObservation: Task<Void, Never>?
    private var outbox: [PendingSend] = []
    /// The single send queue this thread's transport-touching work runs
    /// on — every unit is chained after whatever was chained before it,
    /// so `flushOutbox()` and a freshly tapped compose send can never
    /// interleave on the wire (SHOULD-FIX 5): a send tapped mid-flush is
    /// chained after the flush and only reaches the transport once the
    /// flush's own chained unit has fully finished.
    private var sendChainTail: Task<Void, Never>?

    public init(conversation: ConversationKind, provider: any InboxProviding, client: any MeshtasticClientProtocol,
                flareSender: (any FireflyPacketSending)? = nil) {
        self.conversation = conversation
        self.provider = provider
        self.client = client
        self.flareSender = flareSender
    }

    /// Whether the FLARE control should be usable at all — `false`
    /// whenever no `FireflyPacketSending` conformance was injected.
    /// The view is expected to render FLARE disabled with
    /// `flareUnavailableLabel` in that case, never a tappable control
    /// that silently no-ops or, worse, falls back to plain text.
    public var flareAvailable: Bool { flareSender != nil }
    /// The honest label the view shows next to a disabled FLARE
    /// control (BLOCKING review item 2).
    public static let flareUnavailableLabel = "Flare needs the mesh client"

    /// Appends `work` to this thread's single send chain and returns
    /// the `Task` representing "my turn, after everyone chained before
    /// me." Every unit that touches `client.sendText` — a fresh
    /// compose send and a reconnect flush alike — goes through this,
    /// so the two can never race for the wire.
    @discardableResult
    private func chained(_ work: @escaping () async -> Void) -> Task<Void, Never> {
        let previous = sendChainTail
        let task = Task { [work] in
            await previous?.value
            await work()
        }
        sendChainTail = task
        return task
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
            for await event in deliveries {
                guard let self else { return }
                // `.waiting`/`.sent`/`.dropped` carry the CLIENT's own
                // `OutboxID` (`MeshtasticClientProtocol.deliveryUpdates()`
                // fires one of these for every `sendText`, not just this
                // view model's), a different id space from this view
                // model's locally-generated `outboxID: UInt64`
                // (`OutboxIDGenerator`) — there is no mapping from one to
                // the other here, and none is needed: this view model
                // already marks SENT synchronously off `sendText`'s own
                // return value (`attemptSend`) and DROPPED synchronously
                // in its own catch block. Only `.delivered`/`.noAck` key
                // off `packetID`, which this view model DOES track
                // (`FeedMessage.packetID`, stamped by that same
                // `markSent`), so those two are the only cases that ever
                // reach the provider from this subscription.
                switch event {
                case .delivered(let packetID):
                    self.provider.setStatus(packetID: packetID.rawValue, state: .delivered, at: Date())
                    self.refresh()
                case .noAck(let packetID):
                    self.provider.setStatus(packetID: packetID.rawValue, state: .noAck, at: Date())
                    self.refresh()
                case .waiting, .sent, .dropped:
                    break
                }
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

    /// Omw/Here/Wait: fire-and-forget, exactly like FLARE — NEVER
    /// enters the bounded outbox (BLOCKING review item 3). Sends
    /// immediately when the link is up; when it is down the tap fails
    /// visibly (`immediateSendFailure`), not queued for a later flush.
    public func tap(_ reply: QuickReply) async {
        if reply.seedsComposeText {
            composeText = "Meet at "
            return
        }
        await sendImmediate(text: reply.label)
    }

    /// Free-text compose is the ONLY sender that uses the bounded
    /// outbox — quick replies and FLARE deliberately do not (item 3).
    public func sendCompose() async {
        let text = composeText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        composeText = ""
        await send(text: text, kind: .text)
    }

    /// FLARE send (`S04-firefly-protocol.md` type `0x02`, default
    /// 300 s) — goes ONLY through `FireflyPacketSending.sendFlare`,
    /// NEVER through `send(text:kind:)`/`client.sendText`
    /// (BLOCKING review items 2 and 3). Fire-and-forget like the quick
    /// replies: never enters the outbox, fails visibly when the link
    /// is down or the seam is missing.
    public func sendFlare(durationSeconds: UInt16 = 300) async {
        immediateSendFailure = nil
        guard let flareSender else {
            immediateSendFailure = .flareUnavailable
            return
        }
        guard isLinkReady else {
            immediateSendFailure = .linkDown
            return
        }
        let dest: NodeID? = (conversation == .crew) ? nil : destination
        do {
            try await flareSender.sendFlare(to: dest, durationSeconds: durationSeconds)
            let now = Date()
            let sent = FeedMessage(id: OutboxIDGenerator.shared.next(), kind: .flare, direction: .out,
                                    text: "FLARE", timestamp: now, flareDurationSeconds: durationSeconds,
                                    destination: dest ?? meshBroadcastAddress, deliveryState: .sent, statusAt: now)
            provider.push(sent, into: conversation)
            refresh()
        } catch {
            immediateSendFailure = .transportError
        }
    }

    /// The single non-outbox send path shared by quick replies (FLARE
    /// has its own, above, since it uses a different seam entirely).
    /// Pushes a local record only once the send has actually been
    /// attempted — no phantom WAITING row sitting forever behind a
    /// link that may never come back, since (unlike compose) this is
    /// never going to be flushed later.
    private func sendImmediate(text: String) async {
        immediateSendFailure = nil
        guard isLinkReady else {
            immediateSendFailure = .linkDown
            return
        }
        let dest = destination
        let wantAck = (conversation != .crew)
        let outboxID = OutboxIDGenerator.shared.next()
        let now = Date()
        let pending = FeedMessage(id: outboxID, kind: .text, direction: .out, text: text, timestamp: now,
                                   destination: dest, deliveryState: .waiting, statusAt: now)
        provider.push(pending, into: conversation)
        refresh()
        do {
            let packetID = try await client.sendText(text, to: dest, wantAck: wantAck)
            provider.markSent(outboxID: outboxID, packetID: packetID, at: Date())
            refresh()
        } catch {
            provider.setStatus(outboxID: outboxID, state: .dropped, at: Date())
            refresh()
            immediateSendFailure = .transportError
        }
    }

    /// The delivery-state table's own "resend action" for a NO ACK
    /// (or DROPPED) message: re-attempts the exact same content as a
    /// fresh send. A FLARE resend goes back through `sendFlare`, never
    /// through the text/outbox pipeline — the same BLOCKING item 2/3
    /// rule applies to a retry as it does to the original tap.
    public func resend(_ message: FeedMessage) async {
        guard message.direction == .out else { return }
        if message.kind == .flare {
            await sendFlare(durationSeconds: message.flareDurationSeconds ?? 300)
            return
        }
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

        // Only the transport-touching half is chained (SHOULD-FIX 5):
        // the WAITING row above is visible immediately regardless of
        // whatever a concurrent flush is doing, but the actual
        // enqueue-or-attempt decision waits its turn on the single
        // send chain shared with `flushOutbox()`.
        await chained { [weak self] in
            guard let self else { return }
            guard self.isLinkReady else {
                self.enqueue(PendingSend(outboxID: outboxID, text: text, kind: kind,
                                          flareDurationSeconds: flareDurationSeconds))
                return
            }
            await self.attemptSend(outboxID: outboxID, text: text, dest: dest, wantAck: wantAck,
                                    fallback: PendingSend(outboxID: outboxID, text: text, kind: kind,
                                                           flareDurationSeconds: flareDurationSeconds))
        }.value
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
    /// reaches MC_STATE_READY"). Runs as ONE unit on the single send
    /// chain (SHOULD-FIX 5), so every queued item is fully drained
    /// before any send chained after it — including a compose tap that
    /// landed while this flush was still in flight — gets its turn.
    private func flushOutbox() async {
        await chained { [weak self] in
            await self?.drainOutbox()
        }.value
    }

    private func drainOutbox() async {
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
