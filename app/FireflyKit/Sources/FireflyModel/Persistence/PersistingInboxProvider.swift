//
//  PersistingInboxProvider.swift — the `InboxProviding` decorator that
//  makes message history durable (docs/specs/A01-companion-app.md, M3).
//
//  Wraps any `InboxProviding` and mirrors every WRITE into
//  `HistoryStore`, forwarding every READ unchanged — the same
//  transparent-decorator shape this module could have used for
//  `CoreInboxProvider` itself, kept separate instead so
//  `CoreInboxProviderTests` stays a pure C-bridge test with no SwiftData
//  in it at all, and so `InboxViewModel`/`ThreadViewModel` never have to
//  know persistence exists — they depend on `InboxProviding`, exactly as
//  before (MVVM convention #2).
//
//  There is no "this write is live, that one is a restore" branch here
//  ON PURPOSE: `AppGraph.init` restores BEFORE this wrapper is even
//  constructed, pushing straight into the WRAPPED provider (never
//  through this type — see that file's own comment), so every write
//  THIS type ever sees really is new, and every one of them is
//  persisted the same way. One code path, not two that could drift.
//
//  `restoredMessageIDs` is this process's own memory of which ids came
//  from storage rather than a live event THIS launch actually observed
//  — see `FeedMessage.isRestored`'s doc comment for the rendering rule
//  it exists to serve. It is populated exactly once, at construction,
//  from `HistoryRestorer.restore`'s return value, and never added to
//  again: a live push always mints a FRESH id
//  (`OutboxIDGenerator`/`InboundFeedIDGenerator`/`CoreInboxProvider
//  .feedMessageID(for:)`, all of which are disjoint from a restored id
//  by construction — see those types' own doc comments), so nothing
//  after restore can accidentally mark a live message as restored, and
//  a restored message's own "from storage" tag never flips back to
//  live — it earns that label once, permanently, for the rest of this
//  process's life. Only clearing history (`clearAll()`) empties it.
//
//  M3 / Swift 6: a plain `@MainActor` class conforming to the now-
//  `@MainActor`-isolated `InboxProviding` (`InboxViewModel.swift`'s own
//  doc comment on that protocol) — no `nonisolated`/
//  `MainActor.assumeIsolated`/`@unchecked Sendable` needed, the same
//  simplification PR #275 made to `CoreInboxProvider`. `wrapped` is
//  typed `any InboxProviding`, itself `@MainActor`-isolated, so calling
//  it from these methods is already main-actor-to-main-actor — no hop,
//  no assertion required.
//
import FireflyMesh
import Foundation

@MainActor
public final class PersistingInboxProvider: InboxProviding {
    private let wrapped: any InboxProviding
    private let history: HistoryStore
    private var restoredMessageIDs: Set<UInt64>

    public init(wrapping: any InboxProviding, history: HistoryStore, restoredMessageIDs: Set<UInt64> = []) {
        self.wrapped = wrapping
        self.history = history
        self.restoredMessageIDs = restoredMessageIDs
    }

    // MARK: - Reads (forwarded, with the restored tag applied)

    public func conversations(now: Date) -> [InboxConversationRow] {
        wrapped.conversations(now: now).map { row in
            guard row.hasPreview else { return row }
            var updated = row
            let newest = wrapped.thread(for: row.kind, now: now).last
            updated.previewIsRestored = newest.map { restoredMessageIDs.contains($0.id) } ?? false
            return updated
        }
    }

    public func thread(for conversation: ConversationKind, now: Date) -> [FeedMessage] {
        wrapped.thread(for: conversation, now: now).map { message in
            var tagged = message
            tagged.isRestored = restoredMessageIDs.contains(tagged.id)
            return tagged
        }
    }

    public func markRead(_ conversation: ConversationKind) -> Int { wrapped.markRead(conversation) }

    // MARK: - Writes (forwarded, then mirrored into `history`)

    public func push(_ message: FeedMessage, into conversation: ConversationKind) {
        wrapped.push(message, into: conversation)
        history.record(message, in: conversation)
    }

    public func markSent(outboxID: UInt64, packetID: UInt32, at: Date) {
        wrapped.markSent(outboxID: outboxID, packetID: packetID, at: at)
        history.markSent(outboxID: outboxID, packetID: packetID, at: at)
    }

    public func setStatus(outboxID: UInt64, state: DeliveryState, at: Date) {
        wrapped.setStatus(outboxID: outboxID, state: state, at: at)
        history.setStatus(outboxID: outboxID, state: state, at: at)
    }

    public func setStatus(packetID: UInt32, state: DeliveryState, at: Date) {
        wrapped.setStatus(packetID: packetID, state: state, at: at)
        history.setStatus(packetID: packetID, state: state, at: at)
    }

    /// Settings' "Clear history" action: wipes the live ring/store AND
    /// the disk-backed one together — a live-only clear would look
    /// cleared until the very next relaunch silently restored everything
    /// again, and a disk-only clear would leave the current session's
    /// screen unchanged, neither of which is what "clear history" means
    /// to whoever tapped it. Also empties `restoredMessageIDs`: nothing
    /// is "from storage" once storage has nothing in it.
    public func clearAll() {
        wrapped.clearAll()
        history.clearAll()
        restoredMessageIDs.removeAll()
    }
}
