//
//  Transport.swift — the byte/message transport seam.
//
//  The C library's seam is a vtable (`mc_transport_t`, three function
//  pointers). The Swift seam is a protocol with an `AsyncStream` of
//  inbound messages, for the same reason: nothing above this line knows
//  whether the bytes came from CoreBluetooth, a USB-serial device, or a
//  TCP socket to meshtasticd.
//
//  Framing lives on ONE side of this seam: a `.stream` transport hands
//  up raw bytes and the client runs `StreamFramer` over them; a
//  `.message` transport (BLE) hands up whole `FromRadio` protobufs.
//  `kind` is how the client knows which, rather than guessing.
//
import Foundation

public enum TransportKind: Sendable {
    /// Byte stream — serial, TCP. Needs `StreamFramer`.
    case stream
    /// Already message-framed — BLE GATT.
    case message
}

public enum TransportEvent: Sendable {
    case connecting
    /// The link is up AND, for BLE, the FROMNUM subscription is ACKed —
    /// see `FromRadioDrainPolicy`. Only now may a client send
    /// `want_config`.
    case ready
    case received(Data)
    case disconnected(reason: String?)
}

public protocol MeshTransport: AnyObject, Sendable {
    var kind: TransportKind { get }
    /// A fresh, independent stream of inbound events for the caller.
    /// Multicast via `EventHub` (docs/specs/A01-companion-app.md, S1):
    /// more than one subscriber is expected, and each gets its own
    /// `.bufferingNewest(4096)` stream rather than competing with the
    /// others for one shared `AsyncStream`. The transport finishes every
    /// outstanding stream on permanent failure.
    func events() -> AsyncStream<TransportEvent>
    func connect() async throws
    func disconnect() async
    /// Write one `ToRadio` message. For `.stream` transports the client
    /// has already applied `StreamFramer.frame(_:)`.
    func send(_ data: Data) async throws
}

public enum TransportError: Error, Equatable, Sendable {
    case notConnected
    case writeFailed(String)
    case unsupportedOnThisPlatform(String)
}

/// A transport that is wired to nothing — the milestone-1 stand-in, and
/// permanently useful as the thing unit tests inject.
///
/// It is deliberately NOT a fake radio: it invents no nodes, no
/// positions and no messages. Anything it reported would be fabricated
/// data on a screen whose entire design promise is that nothing on it is
/// fabricated (docs/ARCHITECTURE.md, "Honest state"). Tests that need
/// traffic push exact bytes in with `inject(_:)`.
// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access to `sent` goes through `lock`, never unguarded; `hub`
// itself is already thread-safe on its own.
public final class LoopbackTransport: MeshTransport, @unchecked Sendable {
    public let kind: TransportKind
    private let hub = EventHub<TransportEvent>()
    private let lock = NSLock()
    private var sent: [Data] = []

    public init(kind: TransportKind = .message) {
        self.kind = kind
    }

    public func events() -> AsyncStream<TransportEvent> {
        hub.subscribe()
    }

    public func connect() async throws {
        hub.yield(.connecting)
        hub.yield(.ready)
    }

    public func disconnect() async {
        hub.yield(.disconnected(reason: nil))
        hub.finish()
    }

    public func send(_ data: Data) async throws {
        if let error = takeFailureForNextAttempt() {
            throw error
        }
        record(data)
    }

    // Deliberately non-async: NSLock may not be held across a suspension
    // point, so the critical section is its own synchronous function.
    // Resolves any `waitForSentCount(_:)` waiter whose threshold is now
    // met — see that method's own doc comment for why this replaced a
    // polling wait.
    private func record(_ data: Data) {
        lock.lock()
        sent.append(data)
        let count = sent.count
        var readyWaiters: [CheckedContinuation<Void, Error>] = []
        sentCountWaiters.removeAll { waiter in
            guard waiter.threshold <= count else { return false }
            readyWaiters.append(waiter.continuation)
            return true
        }
        lock.unlock()
        for continuation in readyWaiters { continuation.resume() }
    }

    /// Everything written to this transport so far, in order.
    public var sentMessages: [Data] {
        lock.lock(); defer { lock.unlock() }
        return sent
    }

    /// Root-caused 2026-09-11 against
    /// `ClientReconnectTests.testHandshakeFailsHonestlyOnceEveryBounded
    /// RetryIsSpent` flaking on GitHub's macOS runner ("timed out
    /// waiting for 3 sent message(s); saw 2") even after that file's
    /// `handshakeRetryClock` injection made the RETRY backoff
    /// deterministic: every caller used to notice a new `sentMessages`
    /// count by re-checking it on a fixed timer (a bare
    /// `Task.sleep(for: .milliseconds(5))` loop) — real, if small, wall-
    /// clock time between an actual send and the test noticing it, that
    /// a loaded CI runner's cooperative-thread-pool contention can
    /// stretch out well past a single poll's own nominal interval.
    /// `MeshtasticClient.requestConfig(nonce:timeout:)`'s per-phase
    /// completion race still waits out a REAL (if deliberately tiny, for
    /// that one test) `configPhaseTimeout` via a bare `Task.sleep` of
    /// its own — by design; that phase timeout stands in for a real
    /// node's boot time — so any test-side delay noticing a send and
    /// injecting the matching reply eats directly into that real budget.
    /// This closes that gap: resolved directly from `record(_:)` the
    /// instant the threshold is met, no interval to stretch.
    ///
    /// PR #286 review, BLOCKING 3: each waiter is keyed by its own `id`
    /// (not just `threshold`, which several concurrent waiters can
    /// share) so `cancelSentCountWaiter(id:)` can find and resume
    /// exactly the one continuation a cancelled `waitForSentCount(_:)`
    /// call is suspended on, without disturbing any other waiter parked
    /// on the same threshold.
    private struct Waiter {
        let id: UUID
        let threshold: Int
        let continuation: CheckedContinuation<Void, Error>
    }
    private var sentCountWaiters: [Waiter] = []

    /// `waitForSentCount`/`cancelSentCountWaiter` can race each other:
    /// `withTaskCancellationHandler`'s `onCancel` closure may run
    /// concurrently with — including strictly BEFORE — the operation
    /// closure that registers the waiter (this is explicitly permitted
    /// by the API's own contract, not a bug in the caller). Recording an
    /// early cancellation here, keyed by `id`, lets
    /// `registerSentCountWaiter(id:threshold:continuation:)` notice it
    /// and resume with `CancellationError` immediately instead of
    /// registering a waiter that has already been cancelled and will
    /// then never be resumed (the exact hang this whole mechanism exists
    /// to prevent).
    private var earlyCancellations: Set<UUID> = []

    /// Suspends until at least `n` messages have been sent so far, or
    /// returns immediately if that is already true — see
    /// `sentCountWaiters`'s own doc comment for why this replaced a
    /// polling wait.
    ///
    /// PR #286 review, BLOCKING 3: cancellation-safe. Cancelling the
    /// awaiting task (e.g. a test's own watchdog racing this against a
    /// timeout) removes the waiter under `lock` and resumes it by
    /// throwing `CancellationError`, rather than leaving it parked on
    /// the continuation forever — the prior version had no
    /// `withTaskCancellationHandler` at all, so `Task.cancel()` only
    /// flipped `Task.isCancelled` and never actually unblocked a waiter
    /// whose threshold never arrived.
    public func waitForSentCount(_ n: Int) async throws {
        let id = UUID()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                registerSentCountWaiter(id: id, threshold: n, continuation: continuation)
            }
        } onCancel: {
            cancelSentCountWaiter(id: id)
        }
    }

    // Deliberately non-async, same reason as `record(_:)` above: NSLock
    // may not be held across a suspension point. Checking-and-registering
    // in one locked critical section (rather than `waitForSentCount`
    // checking `sentMessages` first and registering separately) closes
    // the TOCTOU race a send arriving between those two steps would
    // otherwise open. Also consults `earlyCancellations` under the same
    // lock, closing the `onCancel`-before-registration race described on
    // that set's own doc comment.
    private func registerSentCountWaiter(id: UUID, threshold: Int, continuation: CheckedContinuation<Void, Error>) {
        lock.lock()
        if earlyCancellations.remove(id) != nil {
            lock.unlock()
            continuation.resume(throwing: CancellationError())
            return
        }
        if sent.count >= threshold {
            lock.unlock()
            continuation.resume()
            return
        }
        sentCountWaiters.append(Waiter(id: id, threshold: threshold, continuation: continuation))
        lock.unlock()
    }

    // Same locking rationale as `registerSentCountWaiter` above.
    private func cancelSentCountWaiter(id: UUID) {
        lock.lock()
        guard let index = sentCountWaiters.firstIndex(where: { $0.id == id }) else {
            // Not registered yet — `onCancel` beat the operation closure
            // to the lock. Remember it so registration resumes with
            // `CancellationError` instead of parking forever.
            earlyCancellations.insert(id)
            lock.unlock()
            return
        }
        let waiter = sentCountWaiters.remove(at: index)
        lock.unlock()
        waiter.continuation.resume(throwing: CancellationError())
    }

    /// Deliver bytes as if the radio had sent them.
    public func inject(_ data: Data) {
        hub.yield(.received(data))
    }

    // MARK: - M2 background-reconnect test support

    /// Simulate the underlying link dropping WITHOUT tearing this
    /// transport down (`disconnect()` calls `hub.finish()`, which is an
    /// irreversible teardown — a real BLE loss in a pocket is not: the
    /// link comes back on its own, per `BLETransport`'s own
    /// reconnect-on-loss). Lets a mocked-transport test drive
    /// `MeshtasticClient`'s reconnect/handshake-retry path
    /// (docs/specs/A01-companion-app.md, M2) the same shape a real
    /// `BLETransport` would: `.disconnected` now, `.ready` again later
    /// via `simulateReconnect()`.
    public func simulateDisconnect(reason: String? = nil) {
        hub.yield(.disconnected(reason: reason))
    }

    // MARK: - M3 admin-write test support (PR #274 review, SHOULD-FIX 7)

    /// Schedules `send(_:)`'s Nth call EVER (1-based, counting every
    /// attempt including ones scheduled to fail) to throw `error` instead
    /// of recording. Keyed by an absolute attempt count rather than "the
    /// next call" so arming it has no race against the client's own
    /// concurrent sends — call this any time before the write starts, not
    /// only right before the targeted send. Used to simulate a lost/
    /// failed packet partway through a multi-item admin write — proving
    /// `applyChannelSet` names which step failed and warns the node may
    /// be partially configured, without a real dropped BLE/serial write.
    private var scheduledSendFailures: [Int: Error] = [:]
    private var sendAttemptCount = 0

    public func failSend(atAttempt attempt: Int, with error: Error) {
        lock.lock(); scheduledSendFailures[attempt] = error; lock.unlock()
    }

    private func takeFailureForNextAttempt() -> Error? {
        lock.lock(); defer { lock.unlock() }
        sendAttemptCount += 1
        return scheduledSendFailures[sendAttemptCount]
    }

    /// The transport-level half of "reconnects on its own" — see
    /// `simulateDisconnect(reason:)`.
    public func simulateReconnect() {
        hub.yield(.ready)
    }
}
