//
//  HistoryRestorer.swift — cold-launch restore (docs/specs/
//  A01-companion-app.md, M3).
//
//  Replays persisted history back into a live `InboxProviding`, oldest
//  -> newest, with each message's ORIGINAL timestamp threaded through
//  `push`/`setStatus` — never `Date()` — so whatever renders it (a
//  C-core-backed provider's own age/presence math, or a test double)
//  ages it exactly as honestly as it would have aged had the process
//  never restarted at all ("feed restored positions into the core with
//  their ORIGINAL timestamps so the core ages them honestly, never with
//  now").
//
//  Pure over `InboxProviding` — no SwiftData import, no `HistoryStore`
//  reference — so `HistoryRestorerTests` can pin the ordering/honesty
//  rules against `InMemoryInboxStore` with no C core and no disk
//  involved at all, the same "logic separate from storage" split every
//  other `*Store`/`*Restorer` pair in this app follows
//  (`CrewPairingRestorer`/`CrewPairingStore`).
//
//  Two honesty transforms happen here, nowhere else:
//
//   1. SENT -> NO ACK. A routing ack cannot arrive for a packet this
//      process no longer has a live send in flight for: either the ack
//      already came back before the process ended (DELIVERED, left
//      untouched) or it did not, and after a relaunch there is no
//      future in which it still could. Say so here, in code, because
//      nothing about `DeliveryState.sent`'s own definition makes this
//      obvious to a future reader: "the radio accepted it and gave us a
//      packet id" was true and stays true; what is no longer true is
//      that ANYTHING in this process is still listening for the ack
//      that fact was waiting on.
//   2. WAITING items are ALWAYS included, regardless of the reseed cap
//      — see `plan(from:cap:)`'s own doc comment.
//
import Foundation

/// M3 / Swift 6: `@MainActor` — `InboxProviding`'s every mutating/
/// reading method is itself `@MainActor`-isolated now
/// (`InboxViewModel.swift`'s own doc comment on that protocol), and
/// `restore(_:into:cap:)`'s only caller, `AppGraph.init`, is already on
/// the main actor — so this is a synchronous call, not an `await`.
@MainActor
public enum HistoryRestorer {
    /// Mirrors `FF_FEED_CAP` (`firmware/core/include/ff_feed.h`) — the
    /// live C ring's own hard cap. Reseeding more than this would just
    /// evict the oldest of THOSE on the very next live push, so there is
    /// no honest way to show more than this many restored messages at
    /// once in the ring a screen actually reads from
    /// (`CoreInboxProvider`/`InboxBridge`). This is not a new limitation
    /// M3 introduces — a session that never restarted at all already
    /// loses anything past the 32 most recent items to the same ring —
    /// only the existing one, now applied across a relaunch too. The
    /// rest of a longer history stays on disk in `HistoryStore`,
    /// unreachable to any live screen until a future feature reads it
    /// directly.
    public static let coreReseedCap = 32

    /// Which persisted messages get reseeded, and in what push order
    /// (oldest first, matching how a live session would have pushed
    /// them): every currently-WAITING outbound item, UNCONDITIONALLY —
    /// they are this process's own unfinished business, and a relaunch
    /// must not silently forget something the user is still owed a
    /// delivery outcome for — plus the most recent OTHER messages, up to
    /// `cap` total.
    static func plan(from all: [(ConversationKind, FeedMessage)],
                      cap: Int = coreReseedCap) -> [(ConversationKind, FeedMessage)] {
        let waiting = all.filter { $0.1.direction == .out && $0.1.deliveryState == .waiting }
        let others = all.filter { !($0.1.direction == .out && $0.1.deliveryState == .waiting) }
            .sorted { $0.1.timestamp < $1.1.timestamp }
        let remaining = max(0, cap - waiting.count)
        let combined = waiting + Array(others.suffix(remaining))
        return combined.sorted { $0.1.timestamp < $1.1.timestamp }
    }

    /// Restores `all` into `provider` and returns the set of message ids
    /// now sitting in `provider` as a DIRECT RESULT of this call — read
    /// back from `provider` itself afterward, never predicted, so it is
    /// correct regardless of whether `provider`'s own id space is
    /// exactly what was pushed (`InMemoryInboxStore`) or independently
    /// re-derived on read (`CoreInboxProvider.feedMessageID(for:)`, for
    /// an inbound item).
    ///
    /// Call this BEFORE anything else has ever pushed into `provider` —
    /// `AppGraph.init`'s own ordering, before `core.observe(client:)`
    /// ever runs (the identical rule `CrewPairingRestorer.restore`
    /// follows for `ff_crew`, and for the identical reason: a
    /// want_config replay's first live event must never race a
    /// still-in-progress restore) — so every id `provider` reports back
    /// afterward really did come from this restore, and nothing this
    /// call touches is itself mistaken for live traffic.
    @discardableResult
    public static func restore(_ all: [(ConversationKind, FeedMessage)], into provider: any InboxProviding,
                                cap: Int = coreReseedCap) -> Set<UInt64> {
        var touched: Set<ConversationKind> = []
        for (conversation, original) in plan(from: all, cap: cap) {
            var message = original
            if message.direction == .out, message.deliveryState == .sent {
                message.deliveryState = .noAck
            }
            provider.push(message, into: conversation)
            // `push` alone only guarantees WAITING/DROPPED land correctly
            // (`CoreInboxProvider.push`'s own doc comment: `ff_feed_push`
            // always resets `send_status` to NONE, and only WAITING/
            // DROPPED are re-applied automatically) — DELIVERED and the
            // post-transform NO ACK need an explicit write, keyed by the
            // very outbox id this restore just pushed.
            if message.direction == .out, let state = message.deliveryState, state != .waiting, state != .dropped {
                provider.setStatus(outboxID: message.id, state: state, at: message.statusAt)
            }
            touched.insert(conversation)
        }
        var restoredIDs: Set<UInt64> = []
        let now = Date()
        for conversation in touched {
            for message in provider.thread(for: conversation, now: now) { restoredIDs.insert(message.id) }
        }
        return restoredIDs
    }
}
