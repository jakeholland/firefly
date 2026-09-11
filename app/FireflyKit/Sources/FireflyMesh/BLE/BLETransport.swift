//
//  BLETransport.swift — CoreBluetooth `MeshTransport`, iOS and macOS,
//  the same code (docs/specs/A01-companion-app.md, "BLE — iOS and
//  macOS, the same code"). Every rule below is borrowed and pinned by a
//  test (`BLEContractTests`, `FireflyHardwareTests`), not invented:
//  cross-checked against Meshtastic-Apple
//  (`Meshtastic/Accessory/Transports/Bluetooth Low Energy/
//  BLEConnection.swift` and `Accessory Manager/AccessoryManager+Connect.swift`,
//  GPL-3.0, license-compatible per docs/LICENSING.md) and this repo's
//  archived app (`git show 8b0967f:
//  Firefly/Services/CoreBluetoothService.swift`). Behaviour is
//  re-implemented, not copied source; every borrowed rule is cited at
//  its use site below.
//
//  macOS note: constructing a `CBCentralManager` outside a signed,
//  LaunchServices-launched `.app` bundle aborts the process
//  (`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`) — this type itself is
//  safe to compile and reference anywhere, but must only be
//  INSTANTIATED from `Firefly.app` (unit tests: `FireflyHardwareTests`,
//  never plain `swift test`). See docs/specs/A01-companion-app.md, "B1".
//
import Foundation
@preconcurrency import CoreBluetooth

/// A Meshtastic peripheral seen while scanning — for a future node
/// picker (slice C's Connect screen) and for `FireflyHardwareTests`,
/// which must pick `Meshtastic_e7d4` specifically out of two boards on
/// the bench rather than connecting to whichever answers first.
public struct BLEDiscoveredPeripheral: Sendable, Identifiable, Equatable {
    public let id: UUID
    public let name: String?
    public let rssi: Int

    /// Public so a test can construct a sighting without a radio — the
    /// node picker's own tests (`PeripheralDiscoveryTests`) drive
    /// `NodeScanning` with hand-built peripherals, which is the point of
    /// that seam existing at all.
    public init(id: UUID, name: String?, rssi: Int) {
        self.id = id
        self.name = name
        self.rssi = rssi
    }
}

/// Pairing-failure classification (docs/specs/A01-companion-app.md,
/// "BLE" > "Pairing"; borrowed from Meshtastic-Apple's
/// `AccessoryError.forNotifyFailure`/`BLEConnection.isPairingFailure`).
/// `.bondLost` is terminal: retrying it forever is a known dead end, so
/// callers must stop and tell the user to forget the device in Settings
/// rather than loop.
public enum BLEPairingFailure: Error, Equatable, Sendable {
    case insufficientAuthentication
    case insufficientEncryption
    case encryptionTimedOut
    /// The peripheral's own bond record is gone; ours is not. No retry
    /// can fix this — the user must forget the device in iOS/macOS
    /// Settings > Bluetooth.
    case bondLost

    /// `nil` when `error` is not one of the four CoreBluetooth errors
    /// this case classifies as a pairing failure at all.
    public init?(classifying error: Error) {
        if let attError = error as? CBATTError {
            switch attError.code {
            case .insufficientAuthentication: self = .insufficientAuthentication
            case .insufficientEncryption: self = .insufficientEncryption
            default: return nil
            }
            return
        }
        if let cbError = error as? CBError {
            switch cbError.code {
            case .encryptionTimedOut: self = .encryptionTimedOut
            case .peerRemovedPairingInformation: self = .bondLost
            default: return nil
            }
            return
        }
        return nil
    }
}

public actor BLETransport: MeshTransport, NodeScanning {
    public let kind: TransportKind = .message

    private let hub = EventHub<TransportEvent>()
    private let discoveryHub = EventHub<BLEDiscoveredPeripheral>()

    private var central: CBCentralManager?
    private lazy var bridge = BLEDelegateBridge(transport: self)

    private var peripheral: CBPeripheral?
    private var toRadioChar: CBCharacteristic?
    private var fromRadioChar: CBCharacteristic?
    private var fromNumChar: CBCharacteristic?
    private var logRadioChar: CBCharacteristic?

    /// BUGFIX (app: fix live connect path never reaching CONNECTED on
    /// macOS) — keyed by `nextToken()`, same shape as
    /// `writeContinuations`/`readContinuations` below, and for the same
    /// reason: `waitForPoweredOn()` has TWO independent callers that can
    /// genuinely run concurrently — `connect()` (via `attemptConnect()`)
    /// and `scan()` (the node picker's own RESCAN) — most reliably right
    /// after a cold launch, while `CBCentralManager`'s `state` is still
    /// `.unknown` and BOTH `AppGraph`'s own launch auto-connect AND the
    /// Connect screen's first scan race to be the first to ask "is
    /// Bluetooth on yet". A single `CheckedContinuation?` here (what this
    /// used to be) can hold only ONE waiter: the second concurrent call
    /// overwrites it, orphaning the first — a continuation the Swift
    /// runtime reports "leaked... without resuming it", and the caller
    /// it belonged to hangs forever, `isConnectAttemptInFlight` stuck
    /// `true`, so every subsequent CONNECT tap loses the reentrancy
    /// guard to `.alreadyConnecting` and is silently swallowed
    /// (`ConnectViewModel.connect()`'s own doc comment) — "NOT
    /// CONNECTED", indefinitely, with `BLETransport`'s own log showing a
    /// clean connect sequence for whichever call WON the race, because
    /// the one that mattered never got to run at all. Reproduced live
    /// (2026-09-11, signed macOS build, `-FireflyAutoConnect
    /// Meshtastic_06b0` racing `AppGraph`'s launch auto-connect against a
    /// remembered peripheral): "SWIFT TASK CONTINUATION MISUSE:
    /// waitForPoweredOn() leaked its continuation without resuming it."
    /// followed by the OTHER caller's `transport.connect()` throwing
    /// `CancellationError()` — the orphaned continuation's own
    /// cancellation handler resuming whichever continuation the shared
    /// slot happened to hold by the time it ran, not necessarily its
    /// own. A dictionary gives every caller its own slot, so `handleCentralStateUpdate`
    /// below resumes ALL of them together (state is one value, true for
    /// every waiter at once) and a cancelled caller's own handler
    /// (`cancelPoweredOnWait(token:)`) can only ever remove and resume
    /// its OWN entry, never a stranger's.
    private var poweredOnContinuations: [UInt64: CheckedContinuation<Void, Error>] = [:]
    /// Resumed once, at the END of the connect chain — the FROMNUM
    /// subscription ACK (`didUpdateNotificationStateFor`). Every
    /// intermediate delegate step (discoverServices →
    /// discoverCharacteristics → setNotifyValue) chains directly to the
    /// next step rather than its own continuation; any error along the
    /// way resumes this one, throwing.
    private var connectContinuation: CheckedContinuation<Void, Error>?
    private var discoveredPeripheralContinuation: CheckedContinuation<CBPeripheral, Error>?

    /// FIFO, not a keyed store: TORADIO is the only characteristic this
    /// transport ever writes, and FROMRADIO the only one it ever reads
    /// (docs/specs/A01-companion-app.md's reuse assessment notes this is
    /// exactly the condition under which the archived app's FIFO
    /// `removeFirst()` continuation queue was "safe only because exactly
    /// one characteristic is ever written" — true here for both
    /// directions, so the simpler shape is adopted deliberately rather
    /// than Meshtastic-Apple's keyed store, which exists for a client
    /// that writes more than one characteristic). STILL a single FIFO
    /// array per characteristic, not a dictionary — `.cancelled` is a
    /// TOMBSTONE, not a removal (PR #264 review, SHOULD-FIX 3): CoreBluetooth
    /// has no "cancel a specific write/read" API, so a native op whose
    /// Swift-level awaiter was cancelled is still in flight and WILL
    /// still produce a delegate callback, in the same native issuance
    /// order as every other queued op. Removing the slot outright would
    /// let that still-coming callback resume the WRONG (next,
    /// genuinely-still-pending) continuation once it arrives;
    /// tombstoning keeps the slot's position so `handleWriteConfirmation`/
    /// `handleValueUpdate` can consume that one callback silently and
    /// leave every later slot's positional correspondence intact.
    private enum PendingContinuation<Success> {
        case pending(CheckedContinuation<Success, Error>)
        case cancelled
    }
    /// Each queued entry carries an immutable `token` alongside its
    /// state so `cancelPendingWrite(token:)`/`cancelPendingRead(token:)`
    /// can find and tombstone THIS request's slot specifically. The
    /// token — not a captured array index — is what an `onCancel`
    /// handler closes over: `onCancel` runs on the cancelling task's own
    /// context, not this actor's, so it may only capture a `let` value,
    /// never read back a `var` mutated inside the (actor-isolated)
    /// operation closure (a real data race under Swift 6 strict
    /// concurrency, not just a diagnostic to silence).
    private struct QueuedContinuation<Success> {
        let token: UInt64
        var state: PendingContinuation<Success>
    }
    private var writeContinuations: [QueuedContinuation<Void>] = []
    private var readContinuations: [QueuedContinuation<Data>] = []
    private var nextContinuationToken: UInt64 = 0
    private func nextToken() -> UInt64 {
        nextContinuationToken &+= 1
        return nextContinuationToken
    }

    private var isDraining = false
    private var needsDrain = false

    /// Observed at TORADIO discovery via
    /// `peripheral.maximumWriteValueLength(for:)` — NOT negotiated by
    /// this transport. A ~20 byte value means MTU negotiation did not
    /// take; logged, never chunked (one `ToRadio` per write).
    public private(set) var observedWriteValueLimit: Int = 20
    public private(set) var writeType: CBCharacteristicWriteType = .withResponse

    public private(set) var preferredPeripheralID: UUID?
    public private(set) var bondedPeripheralIDs: Set<UUID>
    public private(set) var lastPairingFailure: BLEPairingFailure?

    /// M2 — "stays connected in a pocket, reconnects on its own"
    /// (docs/specs/A01-companion-app.md). Whether an UNEXPECTED
    /// disconnect (`handleDisconnected`) should re-arm a pending
    /// `central.connect()` on the same `CBPeripheral`. Set true the
    /// moment a connect chain actually succeeds (`completeConnect
    /// (throwing: nil)`) or a restored session is picked back up
    /// (`handleWillRestoreState`); cleared by an explicit `disconnect()`
    /// (user-initiated — never reconnect after that) and by a terminal
    /// `BLEPairingFailure.bondLost` (retrying a lost bond can never fix
    /// it — that case's own doc comment).
    private var shouldAutoReconnect = false

    /// SHOULD-FIX 4 (PR #272 review): the identifier `central.connect()`
    /// was last issued for and has not yet resolved (`completeConnect
    /// (throwing:)` — "the ONE place a connect chain finishes" — clears
    /// it either way). Three call sites can each independently decide to
    /// (re-)arm a connect on the SAME peripheral: an explicit `connect()`
    /// call's own `performConnectSequence()`, `handleDisconnected`'s
    /// reconnect-on-loss re-arm, and `handleWillRestoreState`'s
    /// re-arm for a `.disconnected`/`.disconnecting` restored session —
    /// e.g. a cold relaunch where restoration races `AppGraph.start()`'s
    /// own launch auto-connect, or a manual CONNECT tap landing in the
    /// brief `.disconnected` window between a BLE-level loss and
    /// `handleDisconnected` regaining `.ready`. All three now go through
    /// `issueConnect(_:)`, which checks this before calling
    /// `central.connect()` again. CoreBluetooth tolerates a redundant
    /// `connect()` on an already-connecting peripheral in practice
    /// (coalescing onto the one real operation), but that is observed
    /// behaviour, not a documented contract — this makes "at most one
    /// native connect outstanding per peripheral" an explicit guarantee
    /// instead, and one `BLETransportConnectGatingTests` can verify with
    /// no `CBCentralManager` at all (`shouldIssueConnect(for:pending:)`,
    /// below, is the pure predicate it drives).
    private var pendingConnectPeripheralID: UUID?

    /// Pure and testable with no `CBCentralManager` — constructing one
    /// outside a signed, LaunchServices-launched `.app` bundle aborts
    /// the process (this file's own header comment), which is exactly
    /// why the identifier-equality decision itself is split out rather
    /// than only living inline in `issueConnect(_:)`.
    public static func shouldIssueConnect(for target: UUID, pendingConnectPeripheralID: UUID?) -> Bool {
        pendingConnectPeripheralID != target
    }

    /// The single place a NATIVE `central.connect()` is issued
    /// (SHOULD-FIX 4). No-ops — does not touch CoreBluetooth at all —
    /// when a connect for this SAME peripheral is already outstanding;
    /// whichever delegate callback the already-pending native connect
    /// eventually produces resolves everyone waiting on it, via the one
    /// shared `completeConnect(throwing:)` path.
    private func issueConnect(_ target: CBPeripheral) {
        guard Self.shouldIssueConnect(for: target.identifier, pendingConnectPeripheralID: pendingConnectPeripheralID) else {
            BLETransport.log("issueConnect: connect already pending for \(target.identifier) — not re-issuing")
            return
        }
        pendingConnectPeripheralID = target.identifier
        // ROOT CAUSE (2026-09-11 bench power-cycle failure) — this line
        // itself was never the bug, but it was NEVER LOGGED: every other
        // native CoreBluetooth call this file makes has a `BLETransport
        // .log(...)` line right next to it (this file's own "lightweight,
        // unconditional diagnostics" comment), except this one — the ONE
        // place `central.connect()` is issued for a reconnect-on-loss or
        // restored-session re-arm (`performConnectSequence()`'s own
        // explicit-connect path logs separately, "calling
        // central.connect(...)"). That silence is exactly why a real
        // bench failure ("no central.connect ... for 180s") could not be
        // told apart from "this genuinely never ran" from the log alone —
        // fixed here, unconditionally, not only on the guard's failure
        // branch above.
        BLETransport.log("issueConnect: calling central.connect(\(target.identifier))")
        central?.connect(target, options: nil)
    }

    /// M2 follow-up (2026-09-11 bench power-cycle failure) — a bounded
    /// backstop for `handleDisconnected`'s reconnect-on-loss re-arm
    /// above. `central.connect()` on the SAME `CBPeripheral` is the
    /// battery-conscious, no-polling mechanism the M2 task calls for, and
    /// Apple's own contract for it is exactly "completes when the
    /// peripheral is next seen" — but that contract is observed
    /// behaviour for a peripheral that merely went briefly out of range,
    /// not a hard guarantee for one that was fully powered off: a Heltec
    /// re-advertising after a cold boot can present a BLE identity
    /// CoreBluetooth does not reliably reassociate with the OLD
    /// `CBPeripheral` object's still-pending connect (observed on the
    /// bench: `didDisconnectPeripheral` with `CBError.connectionTimeout`,
    /// then total silence — no `central.connect` completion, no scan, no
    /// `.reconnecting` — for the full 180s the acceptance test bounds
    /// itself to). A plain `scanForPeripherals` — the SAME mechanism
    /// `performConnectSequence()`'s own `discoverTarget(central:)` already
    /// uses for a cold, never-yet-known peripheral — reliably rediscovers
    /// it either way, so this arms exactly ONE such scan,
    /// `reconnectFallbackDelay` after the pending connect was issued,
    /// purely as a backstop: if the pending connect above already
    /// completed by then (`completeConnect(throwing:)` cancels this task
    /// unconditionally, success or failure), this never fires at all.
    private let reconnectFallbackDelay: Duration
    private var reconnectFallbackTask: Task<Void, Never>?
    /// True only while a fallback scan armed by `armReconnectFallback
    /// (for:)` is actually running — lets `handleDiscovered` tell "this
    /// sighting is the fallback scan's own target reappearing" apart from
    /// the unrelated ordinary node-picker `scan()` (`NodeScanning`) also
    /// running concurrently, which must never have its sightings treated
    /// as a reconnect.
    private var isFallbackScanning = false

    private func armReconnectFallback(for target: UUID) {
        reconnectFallbackTask?.cancel()
        reconnectFallbackTask = Task { [weak self, reconnectFallbackDelay] in
            try? await Task.sleep(for: reconnectFallbackDelay)
            guard !Task.isCancelled else { return }
            await self?.runReconnectFallbackScan(for: target)
        }
    }

    /// Pure and testable with no `CBCentralManager`, same reasoning as
    /// `shouldIssueConnect(for:pendingConnectPeripheralID:)` above: split
    /// out so the DECISION (is a fallback scan for this target still
    /// actually warranted, this long after it was armed?) is pinned by a
    /// test with no radio at all. `false` — the fallback must NOT scan —
    /// covers both "we already reconnected" (`completeConnect(throwing:)`
    /// clears `pendingConnectPeripheralID` unconditionally, so a stale
    /// timer firing late after a fast reconnect is a no-op here) and "the
    /// disconnect that armed this was superseded by a newer one for a
    /// DIFFERENT peripheral" (`pendingConnectPeripheralID` would name the
    /// new target, not this timer's own).
    public static func shouldRunReconnectFallbackScan(
        for target: UUID, pendingConnectPeripheralID: UUID?, shouldAutoReconnect: Bool
    ) -> Bool {
        shouldAutoReconnect && pendingConnectPeripheralID == target
    }

    private func runReconnectFallbackScan(for target: UUID) async {
        guard Self.shouldRunReconnectFallbackScan(
            for: target, pendingConnectPeripheralID: pendingConnectPeripheralID, shouldAutoReconnect: shouldAutoReconnect
        ) else {
            BLETransport.log("reconnect fallback: \(target) already resolved or superseded — not scanning")
            return
        }
        guard let central else { return }
        BLETransport.log("reconnect fallback: central.connect(\(target)) has not completed after " +
                          "\(reconnectFallbackDelay) — scanning for it by identity")
        isFallbackScanning = true
        central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
    }

    /// Fired from `setPreferredPeripheral(_:)` — the composition root's
    /// hook for persisting "which peripheral to auto-connect to at
    /// launch" (`SettingsKey.lastPeripheralID`,
    /// `AppDependencies.live()`). `FireflyMesh` cannot depend on
    /// `FireflyModel` (`SettingsStoring` lives there, and the dependency
    /// graph runs the other way — `Package.swift`'s own header), so this
    /// is a plain closure injected at construction rather than a stored
    /// settings reference.
    private let onPreferredPeripheralChanged: (@Sendable (UUID) -> Void)?
    /// Fired from `markBonded(_:)` — same reasoning, for
    /// `SettingsKey.bondedPeripheralIDs`.
    private let onBonded: (@Sendable (UUID) -> Void)?

    private static let serviceUUID = CBUUID(string: MeshtasticBLE.serviceUUIDString)
    private static let toRadioUUID = CBUUID(string: MeshtasticBLE.toRadioUUIDString)
    private static let fromRadioUUID = CBUUID(string: MeshtasticBLE.fromRadioUUIDString)
    private static let fromNumUUID = CBUUID(string: MeshtasticBLE.fromNumUUIDString)
    private static let logRadioUUID = CBUUID(string: MeshtasticBLE.logRadioUUIDString)

    /// Cap on connect-step retries — 2 attempts, 2s apart (Meshtastic-Apple's
    /// `AccessoryManager+Connect.swift`: `maxRetries = 2`, `retryDelay =
    /// .seconds(2)`) — so a node that is off does not hot loop.
    private let connectRetryLimit: Int
    private static let connectRetryDelay: Duration = .seconds(2)
    /// First-ever bond: 90s (the user is typing a PIN or tapping Pair).
    /// A remembered bond: 5s, so a dead/out-of-range radio still fails
    /// fast on reconnect. Exact values Meshtastic-Apple's
    /// `AccessoryManager+Connect.swift` uses
    /// (`connectStepTimeout: Duration = isFirstTimeBLEBond ? .seconds(90)
    /// : .seconds(5)`). Instance properties, not `static let`, so a
    /// caller (a hardware test wanting fast failure, say) can override
    /// them — the spec's own values are the defaults.
    private let firstBondConnectTimeout: Duration
    private let knownBondConnectTimeout: Duration
    /// `insufficientResources` write retry: the initial write plus 3
    /// retries, 120/240/360ms backoff (Meshtastic-Apple's
    /// `BLEConnection.swift`, `writeAttemptLimit = 4`,
    /// `Duration.milliseconds(120 * (attempt + 1))`) — that error is the
    /// radio momentarily out of buffers, not a broken link.
    private static let writeAttemptLimit = 4

    public init(
        preferredPeripheralID: UUID? = nil,
        bondedPeripheralIDs: Set<UUID> = [],
        connectRetryLimit: Int = 2,
        firstBondConnectTimeout: Duration = .seconds(90),
        knownBondConnectTimeout: Duration = .seconds(5),
        // 20s: the M2 task's own "15-30s to re-advertise after a power
        // cycle" — a Heltec's own boot time, not a tuned magic number
        // (`armReconnectFallback(for:)`'s own doc comment). An instance
        // property, not `static let`, for the same reason
        // `firstBondConnectTimeout`/`knownBondConnectTimeout` already are:
        // a caller (a hardware test wanting a fast fallback) can override
        // it.
        reconnectFallbackDelay: Duration = .seconds(20),
        onPreferredPeripheralChanged: (@Sendable (UUID) -> Void)? = nil,
        onBonded: (@Sendable (UUID) -> Void)? = nil
    ) {
        self.preferredPeripheralID = preferredPeripheralID
        self.bondedPeripheralIDs = bondedPeripheralIDs
        self.connectRetryLimit = connectRetryLimit
        self.firstBondConnectTimeout = firstBondConnectTimeout
        self.knownBondConnectTimeout = knownBondConnectTimeout
        self.reconnectFallbackDelay = reconnectFallbackDelay
        self.onPreferredPeripheralChanged = onPreferredPeripheralChanged
        self.onBonded = onBonded
    }

    public func setPreferredPeripheral(_ id: UUID?) {
        preferredPeripheralID = id
        if let id { onPreferredPeripheralChanged?(id) }
    }

    public func markBonded(_ id: UUID) {
        bondedPeripheralIDs.insert(id)
        onBonded?(id)
    }

    public var currentPeripheralID: UUID? { peripheral?.identifier }

    // MARK: - MeshTransport

    // `nonisolated`: `MeshTransport.events()` is a synchronous
    // requirement, same reasoning as `MeshtasticClient`'s three stream
    // accessors — `hub` is an immutable `let` of the `@unchecked
    // Sendable` `EventHub` class.
    public nonisolated func events() -> AsyncStream<TransportEvent> { hub.subscribe() }

    public func connect() async throws {
        ensureCentralManagerExists()
        // Blocked by a Bluetooth permission prompt (or Bluetooth simply
        // off/unsupported)? Say so plainly rather than hanging silently
        // — see `waitForPoweredOn()`'s own doc comment.
        try await waitForPoweredOn()

        var lastError: Error = TransportError.writeFailed("no attempts made")
        for attempt in 0..<connectRetryLimit {
            do {
                try await attemptConnect()
                return
            } catch {
                lastError = error
                // A lost bond cannot be fixed by retrying — stop.
                if case BLEPairingFailure.bondLost = error {
                    throw error
                }
                if attempt + 1 < connectRetryLimit {
                    try? await Task.sleep(for: Self.connectRetryDelay)
                }
            }
        }
        throw lastError
    }

    private func attemptConnect() async throws {
        hub.yield(.connecting)
        let isFirstTimeBond = preferredPeripheralID.map { !bondedPeripheralIDs.contains($0) } ?? true
        let timeout: Duration = isFirstTimeBond ? firstBondConnectTimeout : knownBondConnectTimeout

        try await withThrowingTaskGroup(of: Void.self) { group in
            group.addTask { [weak self] in
                // `withTaskCancellationHandler`: when the timeout task
                // below wins the race, the task group's implicit
                // teardown cancels this child — but plain
                // `CheckedContinuation`s (`discoveredPeripheralContinuation`
                // / `connectContinuation`, set deeper inside
                // `performConnectSequence`) do NOT auto-resume on
                // cancellation. Left unresumed, this child task would
                // never actually finish, and the task group cannot
                // fully exit until every child does — so `connect()`
                // itself would hang, exactly the failure this handler
                // exists to prevent.
                guard let self else { return }
                try await withTaskCancellationHandler {
                    try await self.performConnectSequence()
                } onCancel: {
                    Task { await self.cancelPendingConnectContinuations() }
                }
            }
            group.addTask {
                try await Task.sleep(for: timeout)
                throw TransportError.writeFailed("BLE connect timed out")
            }
            try await group.next()
            group.cancelAll()
        }
        // `.ready` is yielded by `completeConnect(throwing:)` — the ONE
        // place a connect chain actually finishes successfully, whether
        // driven by THIS method's own continuation or by a background
        // reconnect-on-loss / restored-session completion that has no
        // continuation waiting at all (see that method's own doc
        // comment). Yielding it here too would double-publish `.ready`
        // for the ordinary path this method drives.
    }

    /// Resumes whichever `performConnectSequence()` continuation is
    /// currently outstanding with `CancellationError`, so a cancelled
    /// connect attempt's child task can actually finish rather than
    /// leak forever suspended. See the cancellation handler above.
    private func cancelPendingConnectContinuations() {
        if let cont = discoveredPeripheralContinuation {
            discoveredPeripheralContinuation = nil
            cont.resume(throwing: CancellationError())
        }
        if let cont = connectContinuation {
            connectContinuation = nil
            cont.resume(throwing: CancellationError())
        }
    }

    private func performConnectSequence() async throws {
        guard let central else { throw TransportError.notConnected }

        // Pause discovery-only scanning for the duration of a connect —
        // duplicate advertisements during the pairing window break the
        // handshake (Meshtastic-Apple's own note, cited in the spec).
        central.stopScan()

        let target: CBPeripheral
        if let preferredPeripheralID, let known = central.retrievePeripherals(withIdentifiers: [preferredPeripheralID]).first {
            BLETransport.log("performConnectSequence: using already-known peripheral \(known.identifier)")
            target = known
        } else {
            BLETransport.log("performConnectSequence: no known peripheral for \(String(describing: preferredPeripheralID)) — scanning")
            target = try await discoverTarget(central: central)
        }
        peripheral = target
        target.delegate = bridge

        BLETransport.log("performConnectSequence: calling central.connect(\(target.identifier))")
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            self.connectContinuation = cont
            // SHOULD-FIX 4 (PR #272 review): `issueConnect(_:)`, not a
            // bare `central.connect(target, options: nil)` — see
            // `pendingConnectPeripheralID`'s own doc comment for why a
            // redundant native connect on this identifier must not be
            // re-issued when a restore or a reconnect-on-loss already
            // armed one.
            self.issueConnect(target)
        }
        BLETransport.log("performConnectSequence: central.connect completed")
    }

    /// Lightweight, unconditional diagnostics for the connect sequence —
    /// bench debugging showed this is the ONE place CoreBluetooth's
    /// silence (no delegate callback at all, ever) is otherwise
    /// indistinguishable from this code being stuck. Cheap enough to
    /// leave in permanently rather than strip after the fact.
    private static func log(_ message: String) {
        // `FileHandle.standardError.write`, not `print()`: stdio's
        // stdout becomes fully block-buffered once it is a pipe rather
        // than a TTY — which `xcodebuild test`'s captured output always
        // is — so short diagnostic lines can sit invisible for the
        // whole run. A raw file-descriptor write bypasses that
        // buffering and appears immediately, which is what actually let
        // this transport's connect-sequence bugs get bisected against a
        // real board on the bench.
        let line = "[BLETransport] \(message)\n"
        FileHandle.standardError.write(Data(line.utf8))
    }

    /// Scan on the service UUID, never on a name prefix — a renamed node
    /// still advertises the service, and a name is spoofable
    /// (MeshtasticBLE.swift).
    private func discoverTarget(central: CBCentralManager) async throws -> CBPeripheral {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<CBPeripheral, Error>) in
            discoveredPeripheralContinuation = cont
            central.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
        }
    }

    public func disconnect() async {
        // User-initiated — clear the reconnect-on-loss flag BEFORE
        // `cancelPeripheralConnection`, which itself delivers an
        // asynchronous `didDisconnectPeripheral` callback
        // (`handleDisconnected`) later: without this, that callback
        // would see a peripheral that just disconnected and re-arm a
        // pending connect on it, undoing the very disconnect the caller
        // asked for.
        shouldAutoReconnect = false
        // Same reasoning, for the reconnect fallback
        // (`armReconnectFallback(for:)`'s own doc comment): a user who
        // asked to disconnect must never have a scan silently start back
        // up N seconds later looking for the peripheral they just walked
        // away from.
        reconnectFallbackTask?.cancel()
        reconnectFallbackTask = nil
        isFallbackScanning = false
        central?.stopScan()
        if let peripheral {
            central?.cancelPeripheralConnection(peripheral)
        }
        failAllPending(TransportError.notConnected)
        peripheral = nil
        pendingConnectPeripheralID = nil
        toRadioChar = nil; fromRadioChar = nil; fromNumChar = nil; logRadioChar = nil
        hub.yield(.disconnected(reason: nil))
    }

    public func send(_ data: Data) async throws {
        guard let peripheral, let characteristic = toRadioChar else {
            throw TransportError.notConnected
        }

        for attempt in 0..<Self.writeAttemptLimit {
            try Task.checkCancellation()
            do {
                try await performWrite(data, peripheral: peripheral, characteristic: characteristic)
                // Re-kick the FROMRADIO drain after every successful
                // write — the radio can queue a reply before the FROMNUM
                // notification lands (FromRadioDrainPolicy.Trigger
                // .toRadioWriteCompleted; dropping this trigger is the
                // subtle bug).
                kickDrain()
                return
            } catch let attError as CBATTError where attError.code == .insufficientResources {
                guard attempt + 1 < Self.writeAttemptLimit else { throw attError }
                let backoffMillis = 120 * (attempt + 1)
                try await Task.sleep(for: .milliseconds(backoffMillis))
            }
        }
    }

    private func performWrite(_ data: Data, peripheral: CBPeripheral, characteristic: CBCharacteristic) async throws {
        logOversizedWriteIfNeeded(data.count, writeType: writeType)
        if writeType == .withoutResponse {
            peripheral.writeValue(data, for: characteristic, type: .withoutResponse)
            return
        }
        // Cancellation-safe like `performConnectSequence`'s wrapper
        // (SHOULD-FIX 3, PR #264 review): a cancelled `send()` used to
        // leave this continuation dangling until a real CoreBluetooth
        // callback or `disconnect()`'s `failAllPending()` eventually
        // resolved it. `token` is a `let`, minted before any suspension
        // point — safe to capture in `onCancel`, which runs on the
        // cancelling task's own context, not this actor's (a captured
        // `var` mutated inside the operation closure below would be a
        // real data race there, not just a diagnostic to silence). See
        // `writeContinuations`'s own doc comment for why a cancellation
        // tombstones the slot rather than removing it.
        let token = nextToken()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
                writeContinuations.append(QueuedContinuation(token: token, state: .pending(cont)))
                peripheral.writeValue(data, for: characteristic, type: .withResponse)
            }
        } onCancel: {
            Task { await self.cancelPendingWrite(token: token) }
        }
    }

    private func cancelPendingWrite(token: UInt64) {
        guard let idx = writeContinuations.firstIndex(where: { $0.token == token }),
              case .pending(let cont) = writeContinuations[idx].state else { return }
        writeContinuations[idx].state = .cancelled
        cont.resume(throwing: CancellationError())
    }

    private func cancelPendingRead(token: UInt64) {
        guard let idx = readContinuations.firstIndex(where: { $0.token == token }),
              case .pending(let cont) = readContinuations[idx].state else { return }
        readContinuations[idx].state = .cancelled
        cont.resume(throwing: CancellationError())
    }

    /// SHOULD-FIX 6 (PR #264 review): Meshtastic-Apple's `BLEConnection.
    /// swift:592-601` (the file this transport is explicitly modeled on)
    /// logs before writing an oversized value — worth carrying over,
    /// especially since a `.withoutResponse` write fails SILENTLY when
    /// oversized (no delegate callback at all), unlike `.withResponse`'s
    /// automatic ATT long-write queuing. Diagnostic only — never chunks,
    /// same "one `ToRadio` per write, no explicit chunking" decision
    /// `observedWriteValueLimit`'s own doc comment already documents.
    private func logOversizedWriteIfNeeded(_ byteCount: Int, writeType: CBCharacteristicWriteType) {
        guard byteCount > observedWriteValueLimit else { return }
        switch writeType {
        case .withoutResponse:
            BLETransport.log("performWrite: \(byteCount)B EXCEEDS negotiated limit \(observedWriteValueLimit)B for " +
                              ".withoutResponse — expect a SILENT ATT failure (no delegate callback at all)")
        default:
            BLETransport.log("performWrite: \(byteCount)B EXCEEDS negotiated limit \(observedWriteValueLimit)B for " +
                              ".withResponse — expect an ATT failure")
        }
    }

    // MARK: - FROMRADIO drain (empty-read-terminates-drain loop)

    /// Drain FROMRADIO by reading until an empty read
    /// (`FromRadioDrainPolicy.isQueueDrained`). Re-kicked on subscription
    /// ACK, every FROMNUM notification, and after every successful
    /// TORADIO write — `kickDrain()`'s three call sites are exactly those
    /// three triggers.
    private func kickDrain() {
        needsDrain = true
        guard !isDraining else { return }
        isDraining = true
        Task { [weak self] in await self?.runDrainLoop() }
    }

    private func runDrainLoop() async {
        while needsDrain {
            needsDrain = false
            do {
                try await drainOnce()
            } catch {
                // A read failure ends the drain pass; the next trigger
                // (or the disconnect it likely preceded) will retry.
                break
            }
        }
        isDraining = false
    }

    private func drainOnce() async throws {
        guard let peripheral, let characteristic = fromRadioChar else { return }
        while true {
            let data = try await readOnce(peripheral: peripheral, characteristic: characteristic)
            if FromRadioDrainPolicy.isQueueDrained(read: data) { return }
            hub.yield(.received(data))
        }
    }

    private func readOnce(peripheral: CBPeripheral, characteristic: CBCharacteristic) async throws -> Data {
        // Cancellation-safe like `performWrite` above — see that
        // method's comment and `writeContinuations`'s own doc comment
        // for why a cancellation tombstones the slot instead of
        // removing it.
        let token = nextToken()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Data, Error>) in
                readContinuations.append(QueuedContinuation(token: token, state: .pending(cont)))
                peripheral.readValue(for: characteristic)
            }
        } onCancel: {
            Task { await self.cancelPendingRead(token: token) }
        }
    }

    // MARK: - Scanning (node picker; independent of connect()'s own internal scan)

    /// Subscribes FIRST (so no discovery is missed), then waits for
    /// `CBManagerState.poweredOn` before actually starting the scan.
    /// Calling `scanForPeripherals` before that state is reached is a
    /// CoreBluetooth API-misuse warning ("can only accept this command
    /// while in the powered on state") on a just-constructed
    /// `CBCentralManager`, whose initial state is `.unknown` until the
    /// delegate's first `centralManagerDidUpdateState` — hardware-tested
    /// the hard way (B1) rather than assumed.
    public func scan() async -> AsyncStream<BLEDiscoveredPeripheral> {
        BLETransport.log("scan() called")
        ensureCentralManagerExists()
        BLETransport.log("scan(): central state = \(String(describing: central?.state.rawValue))")
        let stream = discoveryHub.subscribe()
        do {
            try await waitForPoweredOn()
            BLETransport.log("scan(): powered on, starting scanForPeripherals")
            central?.scanForPeripherals(withServices: [Self.serviceUUID], options: nil)
        } catch {
            BLETransport.log("scan(): waitForPoweredOn threw \(error)")
            // Bluetooth not ready (permission denied, off, unsupported):
            // the stream simply never yields anything. A caller with its
            // own timeout (e.g. FireflyHardwareTests' discoverTarget)
            // reports that as "not discovered" — `connect()` is where a
            // caller learns the underlying reason plainly (see
            // `waitForPoweredOn`'s own doc comment).
        }
        return stream
    }

    public func stopScanning() {
        central?.stopScan()
    }

    // MARK: - Setup

    private func ensureCentralManagerExists() {
        guard central == nil else { return }
        central = CBCentralManager(delegate: bridge, queue: nil, options: Self.centralManagerOptions)
    }

    /// M2 — CoreBluetooth state restoration
    /// (`CBCentralManagerOptionRestoreIdentifierKey`). A FIXED identifier
    /// is what lets CoreBluetooth reassociate a process iOS relaunches in
    /// the background (after suspending or killing it while still
    /// connected) with the peripheral it was talking to —
    /// `BLEDelegateBridge.centralManager(_:willRestoreState:)` /
    /// `handleWillRestoreState(peripherals:)` below only fire when the
    /// manager was created with this key. Behaviour borrowed from
    /// Meshtastic-Apple's `BLETransport.swift`
    /// (`kCentralRestoreID`/`centralManagerOptions(restoreIdentifier:)`,
    /// GPL-3.0, license-compatible per docs/LICENSING.md) —
    /// re-implemented, not copied source.
    ///
    /// iOS only: macOS apps are not relaunched in the background by
    /// CoreBluetooth the way iOS apps are (there is no background-app
    /// lifecycle to restore INTO), so `willRestoreState` has nothing to
    /// do there — the M2 task's own scope note, "on macOS: reconnect on
    /// loss the same way minus restoration". An empty options dict on
    /// macOS is the honest "nothing extra requested" default.
    private static var centralManagerOptions: [String: Any] {
        #if os(iOS)
        [CBCentralManagerOptionRestoreIdentifierKey: "com.jakeholland.firefly.ble-central"]
        #else
        [:]
        #endif
    }

    /// Waits for `CBManagerState.poweredOn`. Throws plainly for the
    /// terminal states that a silent wait would otherwise hang on
    /// forever: `.unauthorized` (a Bluetooth permission prompt the user
    /// has not yet answered, or answered no) and `.unsupported`. A
    /// working-as-designed hang here is exactly the failure mode B1
    /// warns against — "if the run is blocked by a Bluetooth permission
    /// prompt, say so plainly rather than working around it."
    /// `.resetting`/`.unknown` are transient and keep waiting.
    private func waitForPoweredOn() async throws {
        if let state = central?.state {
            if state == .poweredOn { return }
            try throwIfTerminal(state)
        }
        // Cancellation-safe like `performConnectSequence`'s wrapper
        // above: an externally-cancelled `connect()` must not leave this
        // continuation dangling forever. `token` (own doc comment on
        // `poweredOnContinuations`) is what keeps THIS call's own
        // cancellation from ever resuming a DIFFERENT concurrent
        // `waitForPoweredOn()` caller's continuation instead of its own.
        let token = nextToken()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
                poweredOnContinuations[token] = cont
            }
        } onCancel: {
            Task { await self.cancelPoweredOnWait(token: token) }
        }
    }

    private func cancelPoweredOnWait(token: UInt64) {
        guard let cont = poweredOnContinuations.removeValue(forKey: token) else { return }
        cont.resume(throwing: CancellationError())
    }

    private func throwIfTerminal(_ state: CBManagerState) throws {
        switch state {
        case .unauthorized:
            throw TransportError.unsupportedOnThisPlatform(
                "Bluetooth permission not granted (CBManagerState.unauthorized) — grant it in " +
                "System Settings > Privacy & Security > Bluetooth (macOS) or Settings > Firefly (iOS), then retry.")
        case .unsupported:
            throw TransportError.unsupportedOnThisPlatform("Bluetooth is not supported on this device.")
        case .poweredOff:
            throw TransportError.notConnected
        default:
            break
        }
    }

    private func failAllPending(_ error: Error) {
        if let cont = connectContinuation { connectContinuation = nil; cont.resume(throwing: error) }
        if let cont = discoveredPeripheralContinuation { discoveredPeripheralContinuation = nil; cont.resume(throwing: error) }
        let writes = writeContinuations; writeContinuations.removeAll()
        for case .pending(let c) in writes.map(\.state) { c.resume(throwing: error) }
        let reads = readContinuations; readContinuations.removeAll()
        for case .pending(let c) in reads.map(\.state) { c.resume(throwing: error) }
    }

    // MARK: - Delegate callbacks (forwarded from BLEDelegateBridge)

    func handleCentralStateUpdate(_ state: CBManagerState) {
        BLETransport.log("centralManagerDidUpdateState \(state.rawValue) (waiters=\(poweredOnContinuations.count))")
        guard !poweredOnContinuations.isEmpty else { return }
        // State is one value, true for every current waiter at once —
        // unlike a write/read reply, which answers exactly one queued
        // request, ALL of them resolve together here. See
        // `poweredOnContinuations`'s own doc comment for why this used
        // to be a single `CheckedContinuation?` (and the live-graph bug
        // that shape caused).
        if state == .poweredOn {
            let waiters = poweredOnContinuations
            poweredOnContinuations.removeAll()
            for (_, cont) in waiters { cont.resume() }
        } else if let error = terminalError(for: state) {
            let waiters = poweredOnContinuations
            poweredOnContinuations.removeAll()
            for (_, cont) in waiters { cont.resume(throwing: error) }
        }
        // .resetting / .unknown: transient, keep waiting.
    }

    private func terminalError(for state: CBManagerState) -> Error? {
        do {
            try throwIfTerminal(state)
            return nil
        } catch {
            return error
        }
    }

    func handleDiscovered(peripheral: CBPeripheral, name: String?, rssi: Int) {
        BLETransport.log("didDiscover \(peripheral.identifier) name=\(name ?? "nil") rssi=\(rssi)")
        discoveryHub.yield(BLEDiscoveredPeripheral(id: peripheral.identifier, name: name, rssi: rssi))

        // M2 follow-up (`armReconnectFallback(for:)`'s own doc comment):
        // the fallback scan's own sighting of the SAME peripheral it was
        // armed for — checked and consumed BEFORE the ordinary
        // `discoveredPeripheralContinuation` path below, which exists for
        // a completely different caller (`discoverTarget(central:)`,
        // inside an explicit `connect()`'s own `performConnectSequence()`)
        // and must not be cross-wired with this one.
        if isFallbackScanning, peripheral.identifier == pendingConnectPeripheralID {
            isFallbackScanning = false
            central?.stopScan()
            BLETransport.log("reconnect fallback: rediscovered \(peripheral.identifier) — reissuing central.connect")
            self.peripheral = peripheral
            peripheral.delegate = bridge
            // Clear the pending id first: `issueConnect(_:)`'s own guard
            // (SHOULD-FIX 4) would otherwise see THIS identifier already
            // "pending" (from the original `handleDisconnected` re-arm
            // that never completed) and silently no-op the very
            // reconnect this fallback exists to force.
            pendingConnectPeripheralID = nil
            issueConnect(peripheral)
            return
        }

        guard let discoveredPeripheralContinuation else { return }
        let matchesPreferred = preferredPeripheralID.map { $0 == peripheral.identifier } ?? true
        guard matchesPreferred else { return }
        self.discoveredPeripheralContinuation = nil
        central?.stopScan()
        discoveredPeripheralContinuation.resume(returning: peripheral)
    }

    func handleConnected(peripheral: CBPeripheral) {
        BLETransport.log("didConnect \(peripheral.identifier)")
        peripheral.discoverServices([Self.serviceUUID])
    }

    func handleFailedToConnect(peripheral: CBPeripheral, error: Error?) {
        BLETransport.log("didFailToConnect \(peripheral.identifier) error=\(String(describing: error))")
        completeConnect(throwing: error ?? TransportError.writeFailed("failed to connect"))
    }

    func handleDisconnected(peripheral: CBPeripheral, error: Error?) {
        BLETransport.log("didDisconnectPeripheral \(peripheral.identifier) error=\(String(describing: error))")
        // Whatever connect WAS pending (if any) is over now, from
        // CoreBluetooth's own perspective — cleared unconditionally
        // before the reconnect-on-loss branch below re-arms a fresh one
        // through `issueConnect(_:)`, so a stale identifier here can
        // never suppress a legitimate re-arm.
        pendingConnectPeripheralID = nil
        var isBondLost = false
        if let error, let failure = BLEPairingFailure(classifying: error) {
            lastPairingFailure = failure
            isBondLost = (failure == .bondLost)
        }
        toRadioChar = nil; fromRadioChar = nil; fromNumChar = nil; logRadioChar = nil
        failAllPending(error ?? TransportError.notConnected)
        hub.yield(.disconnected(reason: error.map { String(describing: $0) }))

        // M2 — reconnect-on-loss ("stays connected in a pocket,
        // reconnects on its own", docs/specs/A01-companion-app.md). A
        // lost bond is terminal (`BLEPairingFailure`'s own doc comment);
        // any other unexpected loss re-issues `central.connect()` on the
        // SAME `CBPeripheral` object rather than clearing it.
        if shouldAutoReconnect, !isBondLost {
            // This is a PENDING connect, not a poll: CoreBluetooth holds
            // it open — even backgrounded, given `bluetooth-central` in
            // `UIBackgroundModes` — until the peripheral is back in range
            // or powered back on, and resumes exactly where
            // `handleConnected` picks up. No scanning, no timer for the
            // ORDINARY case — the battery-conscious mechanism the M2 task
            // calls out ("use that rather than polling scans").
            // `issueConnect(_:)`, not a bare `central?.connect(...)` —
            // SHOULD-FIX 4.
            issueConnect(peripheral)
            // Bounded backstop for the case that pending connect never
            // completes on its own (`armReconnectFallback(for:)`'s own
            // doc comment — a real Heltec power-cycle, bench-reproduced
            // 2026-09-11) — a SINGLE scan, `reconnectFallbackDelay` from
            // now, not a poll: `completeConnect(throwing:)` cancels this
            // unconditionally the moment the pending connect above (or
            // this fallback's own rediscovery) actually lands.
            armReconnectFallback(for: peripheral.identifier)
        } else {
            self.peripheral = nil
            shouldAutoReconnect = false
        }
    }

    func handleDiscoveredServices(peripheral: CBPeripheral, error: Error?) {
        BLETransport.log("didDiscoverServices error=\(String(describing: error)) services=\(String(describing: peripheral.services?.map(\.uuid)))")
        if let error { completeConnect(throwing: error); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            completeConnect(throwing: TransportError.writeFailed("Meshtastic service not found"))
            return
        }
        peripheral.discoverCharacteristics(
            [Self.toRadioUUID, Self.fromRadioUUID, Self.fromNumUUID, Self.logRadioUUID], for: service)
    }

    func handleDiscoveredCharacteristics(service: CBService, error: Error?) {
        BLETransport.log("didDiscoverCharacteristicsFor error=\(String(describing: error)) chars=\(String(describing: service.characteristics?.map(\.uuid)))")
        if let error { completeConnect(throwing: error); return }
        guard let peripheral, let characteristics = service.characteristics else {
            completeConnect(throwing: TransportError.writeFailed("no characteristics"))
            return
        }

        for characteristic in characteristics {
            switch characteristic.uuid {
            case Self.toRadioUUID:
                toRadioChar = characteristic
                // MTU is observed, not negotiated — logged via the
                // stored value below; callers inspect
                // `observedWriteValueLimit` (~20B means negotiation did
                // not take).
                writeType = characteristic.properties.contains(.writeWithoutResponse) ? .withoutResponse : .withResponse
                observedWriteValueLimit = peripheral.maximumWriteValueLength(for: writeType)
            case Self.fromRadioUUID:
                fromRadioChar = characteristic
            case Self.fromNumUUID:
                fromNumChar = characteristic
            case Self.logRadioUUID:
                logRadioChar = characteristic
            default:
                break
            }
        }

        guard toRadioChar != nil, fromRadioChar != nil, let fromNum = fromNumChar else {
            completeConnect(throwing: TransportError.writeFailed("missing required characteristics"))
            return
        }

        // Optional, best-effort — a missing LOGRADIO is not a connection
        // failure.
        if let logRadio = logRadioChar {
            peripheral.setNotifyValue(true, for: logRadio)
        }

        // Gate on THIS subscription's ACK — do not send want_config
        // until it lands (see the type-level doc comment and
        // MeshtasticBLE.FromRadioDrainPolicy).
        peripheral.setNotifyValue(true, for: fromNum)
    }

    func handleNotificationStateUpdate(characteristic: CBCharacteristic, error: Error?) {
        BLETransport.log("didUpdateNotificationStateFor \(characteristic.uuid) error=\(String(describing: error))")
        guard characteristic.uuid == Self.fromNumUUID else { return }
        if let error {
            completeConnect(throwing: error)
            return
        }
        if let id = peripheral?.identifier {
            markBonded(id)
        }
        completeConnect(throwing: nil)
        kickDrain() // trigger: subscriptionAcknowledged
    }

    func handleValueUpdate(characteristic: CBCharacteristic, value: Data?, error: Error?) {
        if characteristic.uuid == Self.fromRadioUUID {
            if !readContinuations.isEmpty {
                let head = readContinuations.removeFirst()
                // `.cancelled`: this callback answers a read whose
                // Swift-level awaiter already resumed with
                // `CancellationError` (SHOULD-FIX 3) — the tombstone is
                // consumed so the NEXT callback lines up with the NEXT
                // real slot, but nothing is resumed a second time.
                if case .pending(let cont) = head.state {
                    if let error {
                        cont.resume(throwing: error)
                    } else {
                        cont.resume(returning: value ?? Data())
                    }
                }
            }
            return
        }
        if characteristic.uuid == Self.fromNumUUID {
            kickDrain() // trigger: fromNumNotification
        }
        // LOGRADIO: not surfaced through TransportEvent in M1 — debug
        // console output only, out of scope for the client's own event
        // stream.
    }

    func handleWriteConfirmation(characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.toRadioUUID, !writeContinuations.isEmpty else { return }
        let head = writeContinuations.removeFirst()
        // See `handleValueUpdate`'s comment on the `.cancelled` case —
        // same tombstone-consumption rule.
        guard case .pending(let cont) = head.state else { return }
        if let error {
            cont.resume(throwing: error)
        } else {
            cont.resume()
        }
    }

    /// The ONE place a connect chain finishes — successfully or not,
    /// whether driven by an active `connect()` call's own continuation
    /// or (M2) by a background reconnect-on-loss / restored-session
    /// completion that has none (`handleDisconnected`'s re-armed
    /// `central.connect()`, `handleWillRestoreState`'s `.connecting`/
    /// `.connected` cases — none of those go through
    /// `performConnectSequence()`, so `connectContinuation` is nil for
    /// them). `hub.yield(.ready)` and `shouldAutoReconnect = true` on
    /// success live HERE, unconditionally, rather than in
    /// `attemptConnect()` (which only drives the continuation-backed
    /// path), so both shapes end up honestly `.ready` and re-armed for
    /// the NEXT loss.
    private func completeConnect(throwing error: Error?) {
        // SHOULD-FIX 4 (PR #272 review): this IS the connect chain's own
        // completion, whichever of the three `issueConnect(_:)` call
        // sites armed it — clear the pending-identifier guard
        // unconditionally, success or failure, so the NEXT legitimate
        // connect attempt for this (or any) peripheral is never silently
        // suppressed by a stale value.
        pendingConnectPeripheralID = nil
        // M2 follow-up: same "success or failure, unconditionally"
        // reasoning extends to the reconnect-on-loss fallback
        // (`armReconnectFallback(for:)`'s own doc comment) — this IS the
        // connect chain finishing, by whichever of the two paths that
        // fallback itself can now complete through (the original pending
        // `central.connect()`, or its own rediscovery scan reissuing
        // one), so any timer still outstanding for it is stale the
        // instant this runs.
        reconnectFallbackTask?.cancel()
        reconnectFallbackTask = nil
        isFallbackScanning = false
        if let error {
            guard let cont = connectContinuation else { return }
            connectContinuation = nil
            cont.resume(throwing: error)
            return
        }
        if let cont = connectContinuation {
            connectContinuation = nil
            shouldAutoReconnect = true
            hub.yield(.ready)
            cont.resume()
            return
        }
        // No continuation waiting: only a background reconnect-on-loss /
        // restored-session completion should reach here (see this
        // method's own doc comment) — and only while a peripheral is
        // still actually tracked. A stray delegate callback arriving
        // after an explicit `disconnect()` (which clears `peripheral`)
        // must not resurrect a `.ready` for a connection nobody asked to
        // keep.
        guard peripheral != nil else { return }
        shouldAutoReconnect = true
        hub.yield(.ready)
    }

    /// M2 — CoreBluetooth state restoration
    /// (`CBCentralManagerOptionRestoreIdentifierKey`, `ensureCentralManagerExists`'s
    /// own doc comment). Called from `BLEDelegateBridge.centralManager
    /// (_:willRestoreState:)` when the OS relaunches this process while
    /// it was still connected (or mid-connect) to a peripheral in the
    /// background. Never constructs a second `CBCentralManager` — this
    /// fires on the SAME manager `ensureCentralManagerExists()` just
    /// created with the restore identifier — and never duplicates
    /// `hub`/`discoveryHub` subscriptions, since it does not touch them
    /// at all beyond the normal connect-chain path every other
    /// connection completes through (`completeConnect(throwing:)`).
    ///
    /// Behaviour borrowed from Meshtastic-Apple's `BLETransport.swift`
    /// (`handleWillRestoreState(dict:central:)`, GPL-3.0,
    /// license-compatible per docs/LICENSING.md) — re-implemented, not
    /// copied source: branch on the restored peripheral's own
    /// `CBPeripheralState` rather than assume one.
    func handleWillRestoreState(peripherals: [CBPeripheral]) {
        guard let restored = peripherals.first(where: { $0.identifier == preferredPeripheralID }) ?? peripherals.first else {
            BLETransport.log("willRestoreState: no peripherals in the restore dictionary")
            return
        }
        BLETransport.log("willRestoreState: restoring \(restored.identifier), CBPeripheralState=\(restored.state.rawValue)")
        peripheral = restored
        restored.delegate = bridge
        // `setPreferredPeripheral(_:)`, not a raw assignment: on the
        // (rare) path where no already-remembered id matched anything
        // in the restore dictionary and this fell back to `.first`,
        // `restored.identifier` may be a DIFFERENT id than whatever was
        // last persisted — routing it through the same setter keeps
        // `SettingsKey.lastPeripheralID` honestly current either way.
        setPreferredPeripheral(restored.identifier)
        shouldAutoReconnect = true

        switch restored.state {
        case .connected:
            // Already connected at the GATT level. THIS process's own
            // characteristic references are gone (a fresh launch) —
            // rediscover them; no `central.connect()` needed, it already
            // is connected. Flows into the same `handleDiscoveredServices`
            // → ... → `completeConnect(throwing: nil)` chain as any other
            // connect, so `.ready` is published the same honest way.
            restored.discoverServices([Self.serviceUUID])
        case .connecting:
            // A pending connect from before the relaunch — CoreBluetooth
            // resumes it on its own; `didConnect` fires when it lands.
            // Recorded as pending (SHOULD-FIX 4, PR #272 review) so an
            // explicit `connect()` call racing this relaunch
            // (`performConnectSequence()`, via `issueConnect(_:)`) does
            // not redundantly issue a SECOND native `central.connect()`
            // for the very same peripheral.
            pendingConnectPeripheralID = restored.identifier
        default:
            // .disconnected/.disconnecting: re-arm a pending connect the
            // same way `handleDisconnected` does for a mid-session drop.
            // `issueConnect(_:)` (SHOULD-FIX 4, PR #272 review) is what
            // keeps this from double-issuing `central.connect()` against
            // an explicit `connect()` call already mid-
            // `performConnectSequence()` for the very same peripheral
            // (both would otherwise race the instant this process
            // launches with a pending auto-connect, `AppGraph.start()`'s
            // own doc comment) — a strict generalization of the old
            // `connectContinuation == nil` guard this replaces: that
            // only caught HALF of this race (restoration losing to an
            // already-in-flight `performConnectSequence()`), never the
            // reciprocal direction.
            issueConnect(restored)
        }
    }

    // MARK: - Test-only hook (`FireflyHardwareTests`, via `@testable
    // import` — `BLEReconnectHardwareTests`)

    /// Simulates an UNEXPECTED BLE-level loss for a hardware test that
    /// cannot power-cycle a real board on its own (that needs a human at
    /// the bench — see app/README.md, "Manual test procedure"): a local
    /// `cancelPeripheralConnection`, which fires `didDisconnectPeripheral`
    /// the SAME WAY a real out-of-range or powered-off node would,
    /// WITHOUT going through the public `disconnect()` API (which
    /// intentionally clears `shouldAutoReconnect` — never true of a
    /// genuine, unexpected loss; see that method's own doc comment).
    /// `internal`, not `public`: this is a test seam, not an app-facing
    /// operation — the 2026-09-11 bench power-cycle investigation's own
    /// hardware-verification test reaches it via `@testable import`.
    func simulateUnexpectedDisconnectForTesting() {
        guard let central, let peripheral else { return }
        BLETransport.log("simulateUnexpectedDisconnectForTesting: cancelPeripheralConnection(\(peripheral.identifier))")
        central.cancelPeripheralConnection(peripheral)
    }
}
