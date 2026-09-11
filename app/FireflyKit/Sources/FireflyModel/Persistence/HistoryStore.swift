//
//  HistoryStore.swift — the durable half of message history (docs/specs/
//  A01-companion-app.md, M3 + Persistence). `PersistingInboxProvider`
//  (this directory) is the only writer; `AppGraph.init` and
//  `flushPersistedOutbox()` are the only readers.
//
//  `@MainActor`-confined, the same discipline every `ff_*`-backed type
//  in this app follows (A01, "Threading model") — `ModelContext` is not
//  thread-safe, and there is exactly one of these per process, so
//  confining it to one isolation domain costs nothing and buys the same
//  "nothing else may touch this" guarantee `CoreStore` gives `ff_crew_t`.
//
import FireflyMesh
import Foundation
import SwiftData

@MainActor
public final class HistoryStore {
    /// Every persisted row beyond this many (oldest-first) is pruned on
    /// write. Generous relative to the live C ring's own 32-item cap
    /// (`HistoryRestorer.coreReseedCap`) — disk has no `ff_feed_t` 8 KB
    /// struct budget — but still bounded: an unattended multi-day
    /// festival phone must not grow this file without limit.
    public static let pruneCap = 2000

    private let context: ModelContext

    public init(container: ModelContainer) {
        context = ModelContext(container)
        // Autosave off: every write method below calls `save()` itself,
        // exactly once, at the point its own mutation is actually
        // complete — the default idle-triggered autosave has no such
        // guarantee for a process that can be killed at any moment (a
        // backgrounded festival phone, reaped for memory).
        context.autosaveEnabled = false
    }

    // MARK: - Writes — mirrors `InboxProviding`'s four mutating methods
    // exactly, one per method, because `PersistingInboxProvider` calls
    // these as a direct write-through after forwarding to whatever it
    // wraps (that type's own header comment).

    public func record(_ message: FeedMessage, in conversation: ConversationKind) {
        if let existing = find(messageID: message.id) {
            existing.apply(message, in: conversation)
        } else {
            context.insert(PersistedMessage(message, in: conversation))
        }
        save()
        prune()
    }

    public func markSent(outboxID: UInt64, packetID: UInt32, at: Date) {
        guard let row = find(messageID: outboxID) else { return }
        row.packetID = Int(packetID)
        row.deliveryStateRaw = DeliveryState.sent.rawValue
        row.statusAt = at
        save()
    }

    public func setStatus(outboxID: UInt64, state: DeliveryState, at: Date) {
        guard let row = find(messageID: outboxID) else { return }
        row.deliveryStateRaw = state.rawValue
        row.statusAt = at
        save()
    }

    public func setStatus(packetID: UInt32, state: DeliveryState, at: Date) {
        guard let row = find(packetID: packetID) else { return }
        row.deliveryStateRaw = state.rawValue
        row.statusAt = at
        save()
    }

    /// Settings' "Clear history" action (docs/specs/A01-companion-app.md
    /// M3) — every row, unconditionally. `PersistingInboxProvider
    /// .clearAll()` is what also wipes the live ring; this is only ever
    /// called alongside that, never alone (a cleared disk with a still-
    /// populated live ring would just refill the very next `push`).
    public func clearAll() {
        (try? context.fetch(FetchDescriptor<PersistedMessage>()))?.forEach { context.delete($0) }
        save()
    }

    // MARK: - Reads

    /// Every persisted message, decoded, in no particular order — the
    /// raw material `HistoryRestorer.plan(from:cap:)` sorts and caps.
    public func loadAllForRestore() -> [(ConversationKind, FeedMessage)] {
        let rows = (try? context.fetch(FetchDescriptor<PersistedMessage>())) ?? []
        return rows.compactMap { $0.decoded() }
    }

    /// Outbound items still `WAITING` — never sent at all before the
    /// process that queued them ended — oldest first, capped at `cap`
    /// (`AppGraph.flushPersistedOutbox()` passes `ThreadViewModel
    /// .outboxCap`, the same bound a live thread's own session outbox
    /// enforces).
    public func pendingOutbox(cap: Int, now: Date = Date()) -> [(ConversationKind, FeedMessage)] {
        let waiting = loadAllForRestore().filter { $0.1.direction == .out && $0.1.deliveryState == .waiting }
        return Array(waiting.sorted { $0.1.timestamp < $1.1.timestamp }.prefix(cap))
    }

    // MARK: - Lookup

    private func find(messageID: UInt64) -> PersistedMessage? {
        let key = Int64(bitPattern: messageID)
        return try? context.fetch(FetchDescriptor<PersistedMessage>(
            predicate: #Predicate { $0.messageID == key })).first
    }

    private func find(packetID: UInt32) -> PersistedMessage? {
        let key = Int(packetID)
        return try? context.fetch(FetchDescriptor<PersistedMessage>(
            predicate: #Predicate { $0.packetID == key })).first
    }

    private func save() { try? context.save() }

    /// Oldest-first eviction once the store passes `pruneCap` — the same
    /// "bounded, drop-oldest" policy `ff_feed_t`'s own ring buffer
    /// documents (`ff_feed.h`), applied to disk instead of the 32-item
    /// RAM ring.
    private func prune() {
        guard let count = try? context.fetchCount(FetchDescriptor<PersistedMessage>()), count > Self.pruneCap else {
            return
        }
        var descriptor = FetchDescriptor<PersistedMessage>(sortBy: [SortDescriptor(\.timestamp, order: .forward)])
        descriptor.fetchLimit = count - Self.pruneCap
        (try? context.fetch(descriptor))?.forEach { context.delete($0) }
        save()
    }

    // MARK: - Container construction

    /// The live stack's own store — on disk, under Application Support.
    /// `AppGraph.init`'s own rule picks this automatically for the real
    /// dependency stack (never for `.stub()`/`.demo()`, which get
    /// `.inMemory()` instead — M3's demo-isolation rule).
    public static func live() -> HistoryStore {
        HistoryStore(container: makeContainer(inMemory: false))
    }

    /// Never touches disk. `.stub()`/`.demo()`/every unit test get this
    /// — "Demo doesn't persist across launches: in-memory store only"
    /// (docs/specs/A01-companion-app.md M3).
    public static func inMemory() -> HistoryStore {
        HistoryStore(container: makeContainer(inMemory: true))
    }

    /// Migration policy (documented, per the M3 task): `HistorySchemaV1`
    /// plus drop-and-recreate on any mismatch `HistoryMigrationPlan`
    /// does not cover — see that type's own doc comment for the full
    /// justification. This is the one place in this app that silently
    /// discards user data on purpose.
    private static func makeContainer(inMemory: Bool) -> ModelContainer {
        let schema = Schema(versionedSchema: HistorySchemaV1.self)
        if inMemory {
            let configuration = ModelConfiguration(schema: schema, isStoredInMemoryOnly: true)
            // No stale file can exist for an in-memory store — a
            // migration-plan failure here would be a genuine schema bug,
            // not stale data, so it is allowed to fail loudly rather
            // than loop into a second attempt that could never help.
            return try! ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self,
                                        configurations: [configuration])
        }
        let url = storeURL
        let configuration = ModelConfiguration(schema: schema, url: url, cloudKitDatabase: .none)
        if let container = try? ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self,
                                                 configurations: [configuration]) {
            return container
        }
        // Drop and recreate: delete whatever is on disk at `url` (and
        // its WAL/SHM siblings) and try exactly once more against a
        // clean slate.
        deleteStoreFiles(at: url)
        return try! ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self, configurations: [configuration])
    }

    private static var storeURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let dir = base.appending(path: "Firefly", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appending(path: "History.sqlite")
    }

    private static func deleteStoreFiles(at url: URL) {
        for suffix in ["", "-wal", "-shm"] {
            try? FileManager.default.removeItem(atPath: url.path + suffix)
        }
    }
}
