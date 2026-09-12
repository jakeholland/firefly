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

    /// `false` when this store's rows will NOT survive a relaunch —
    /// i.e. `live()` could not open (or recreate) its on-disk store and
    /// fell back to an in-memory container so the app could still
    /// launch. Honest data rule: the app must not imply a durable
    /// history it does not actually have. Always `false` for
    /// `inMemory()` too, which is in-memory on purpose.
    ///
    /// Nothing in the app FAILS on this — history simply does not
    /// persist for that session — but it is surfaced rather than
    /// swallowed, and `live()` logs the reason to stderr when it
    /// happens.
    public let isPersistent: Bool

    public init(container: ModelContainer, isPersistent: Bool = false) {
        self.isPersistent = isPersistent
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
    /// M3) — every row, unconditionally, deleted one at a time through
    /// the live `ModelContext`. `PersistingInboxProvider.clearAll()` is
    /// what also wipes the live ring; this is only ever called alongside
    /// that, never alone (a cleared disk with a still-populated live
    /// ring would just refill the very next `push`).
    ///
    /// PR #281 review, SHOULD-FIX 3: this is a DIFFERENT mechanism from
    /// `makeContainer`'s drop-and-recreate migration fallback
    /// (`deleteStoreFiles`, below), never the same code path, even
    /// though both end at an empty store — there is no live
    /// `ModelContext` for a container that failed to even OPEN to hand
    /// this method, so the two cannot share a routine without inventing
    /// a third abstraction neither caller needs. See
    /// `HistorySchema.swift`'s own header comment for the full
    /// three-places-disclosed list this correction applies to.
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

    // MARK: - ID generator watermarks (PR #281 review, BLOCKING 1)

    /// `IDGeneratorWatermark.generatorKey` for `OutboxIDGenerator`.
    public static let outboxWatermarkKey = "outbox"
    /// `IDGeneratorWatermark.generatorKey` for `InboundFeedIDGenerator`.
    public static let inboundWatermarkKey = "inbound"

    /// The durable floor persisted for `key`, or `0` if this store has
    /// never recorded one (a fresh store, or an old on-disk V1 store
    /// from before this fix — `0` is a safe floor either way, since
    /// `AppGraph.seedIDGenerators(from:store:)` always combines this
    /// with the max id actually present in `PersistedMessage` too).
    public func watermark(for key: String) -> UInt64 {
        guard let row = findWatermark(key) else { return 0 }
        return UInt64(bitPattern: row.nextValue)
    }

    /// Raises `key`'s durable floor to `next` — never lowers it, the
    /// same monotonic contract `OutboxIDGenerator.seed(atLeast:)` itself
    /// carries (this is what feeds that call, every launch, in
    /// `AppGraph.init`).
    public func raiseWatermark(for key: String, to next: UInt64) {
        let value = Int64(bitPattern: next)
        if let row = findWatermark(key) {
            if value > row.nextValue { row.nextValue = value }
        } else {
            context.insert(IDGeneratorWatermark(generatorKey: key, nextValue: value))
        }
        save()
    }

    private func findWatermark(_ key: String) -> IDGeneratorWatermark? {
        try? context.fetch(FetchDescriptor<IDGeneratorWatermark>(
            predicate: #Predicate { $0.generatorKey == key })).first
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
        store(at: storeURL)
    }

    /// `live()`, with the on-disk location named explicitly. Public for
    /// one reason, stated so it is not mistaken for a general-purpose
    /// multi-store API: it is the only seam a test can use to prove the
    /// launch-path fallback below actually works, by pointing it at a
    /// path that genuinely cannot be opened. Production has exactly one
    /// history store and calls `live()`.
    public static func store(at url: URL) -> HistoryStore {
        let opened = makeContainer(onDiskAt: url)
        return HistoryStore(container: opened.container, isPersistent: opened.isPersistent)
    }

    /// Never touches disk. `.stub()`/`.demo()`/every unit test get this
    /// — "Demo doesn't persist across launches: in-memory store only"
    /// (docs/specs/A01-companion-app.md M3).
    public static func inMemory() -> HistoryStore {
        HistoryStore(container: makeContainer(onDiskAt: nil).container, isPersistent: false)
    }

    /// Migration policy (documented, per the M3 task): `HistorySchemaV1`
    /// plus drop-and-recreate on any mismatch `HistoryMigrationPlan`
    /// does not cover — see that type's own doc comment for the full
    /// justification. This is the one place in this app that silently
    /// discards user data on purpose.
    /// Hardening QA pass — THIS FUNCTION MUST NOT TRAP. It runs inside
    /// `AppGraph.init`, i.e. on the launch path, so anything it throws
    /// is a crash on every single launch until the user deletes the app
    /// — the worst possible failure mode for a phone at a festival with
    /// no cell service, where reinstalling is not an option.
    ///
    /// It used to end in `try!` twice. `HistorySchema.swift`'s own
    /// header already promised this path fails soft ("never a crash"),
    /// and for the FIRST failure it did — the drop-and-recreate below.
    /// But the retry after that deletion was itself `try!`, so any
    /// reason the store could not be opened that deleting the old files
    /// does not fix — a full disk (three days of festival video), an
    /// unwritable/absent Application Support directory, a data-
    /// protection-locked container — turned into an unrecoverable
    /// launch crash rather than the disclosed "history was cleared".
    ///
    /// Four steps, each one strictly more conservative than the last:
    /// open on disk; drop the files and reopen; fall back to in-memory
    /// (the app launches, history simply does not persist this session
    /// — reported through `isPersistent`, never implied to be durable);
    /// and, if even THAT fails, an unconfigured in-memory container as
    /// the last resort. The final `try` is still a `try!` in shape but
    /// cannot be reached by any on-disk condition, only by a genuine
    /// schema bug — which `HistoryStoreTests` would fail on long before
    /// a build ships.
    private static func makeContainer(onDiskAt url: URL?) -> (container: ModelContainer, isPersistent: Bool) {
        let schema = Schema(versionedSchema: HistorySchemaV1.self)

        if let url {
            let configuration = ModelConfiguration(schema: schema, url: url, cloudKitDatabase: .none)
            if let container = try? ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self,
                                                     configurations: [configuration]) {
                return (container, true)
            }
            // Drop and recreate: delete whatever is on disk at `url`
            // (and its WAL/SHM siblings) and try exactly once more
            // against a clean slate. A DIFFERENT mechanism from
            // `clearAll()` above, not a call to it — this runs before
            // any `ModelContainer` (and therefore any `ModelContext`/
            // `HistoryStore` instance) exists at all, so there is no
            // live context's rows to delete through; this deletes the
            // files behind a store that failed to open instead (PR #281
            // review, SHOULD-FIX 3).
            log("on-disk history store failed to open — dropping it and retrying once")
            deleteStoreFiles(at: url)
            if let container = try? ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self,
                                                     configurations: [configuration]) {
                return (container, true)
            }
            log("on-disk history store still would not open after drop-and-recreate — "
                + "falling back to an in-memory store; message history will NOT survive this launch")
        }

        let memoryConfiguration = ModelConfiguration(schema: schema, isStoredInMemoryOnly: true)
        if let container = try? ModelContainer(for: schema, migrationPlan: HistoryMigrationPlan.self,
                                                 configurations: [memoryConfiguration]) {
            return (container, false)
        }
        // Unreachable except for a genuine schema/migration-plan bug —
        // no file, no disk and no permission is involved any more. Kept
        // as a hard failure rather than papered over: a schema that
        // cannot even be instantiated in memory is a build-time defect
        // every `HistoryStoreTests` run would catch, not a field
        // condition to degrade around.
        log("in-memory history store failed to open — this is a schema bug, not a disk condition")
        return (try! ModelContainer(for: schema, configurations: [memoryConfiguration]), false)
    }

    /// Same discipline as `MeshtasticClient.log(_:)` — a raw stderr
    /// write, not `print()`, so a line is never lost to stdout's block
    /// buffering under `xcodebuild test` or `open --stderr`.
    private static func log(_ message: String) {
        FileHandle.standardError.write(Data("[HistoryStore] \(message)\n".utf8))
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
