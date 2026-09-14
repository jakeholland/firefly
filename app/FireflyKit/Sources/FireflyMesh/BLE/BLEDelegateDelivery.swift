//
//  BLEDelegateDelivery.swift — A03 §3.1, the ordering fix.
//
//  Apple guarantees `centralManager(_:willRestoreState:)` is delivered
//  BEFORE `centralManagerDidUpdateState(_:)` ("the FIRST method ... is
//  invoked when your app is relaunched into the background", §1.2).
//  `BLEDelegateBridge` used to throw that guarantee away: it hopped
//  every callback onto the `BLETransport` actor as its own unstructured
//  `Task`, and unstructured tasks carry NO ordering guarantee relative
//  to each other — so `handleCentralStateUpdate(.poweredOn)` could
//  reach the actor before `handleWillRestoreState` did.
//
//  That was harmless while `.poweredOn` did nothing (audit 2.2.3). It
//  stops being harmless the moment §3.5 gives it real work:
//  `retrievePeripherals(withIdentifiers:)` returns a DIFFERENT
//  `CBPeripheral` instance than the one in the restore dictionary,
//  `issueConnect` would overwrite `BLETransport.peripheral` with it,
//  and releasing the restored object implicitly calls
//  `cancelPeripheralConnection(_:)` (§1.2) — tearing down the exact
//  connection the restore was adopting.
//
//  Two mechanisms, both here, both testable with no CoreBluetooth at
//  all (`BLEDelegateDeliveryTests`):
//
//  1. **A restore-pending marker**, raised SYNCHRONOUSLY on the delegate
//     queue inside `willRestoreState`, before any `Task` is built. The
//     delegate queue is serial (the main queue — `BLETransport
//     .ensureCentralManagerExists`), so a marker set there is visible to
//     every later callback on that queue no matter what the actor hops
//     do. `BLETransport.powerStateAction(...)` takes it as an input.
//  2. **A serial delivery chain**: each callback's actor hop awaits the
//     previous one, so the actor sees callbacks in the order
//     CoreBluetooth delivered them. That is the guarantee CoreBluetooth
//     itself makes on its own queue, carried across the hop rather than
//     dropped at it.
//
import Foundation

/// The bridge's ordering state — shared by `BLEDelegateBridge` (which
/// writes it from the delegate queue) and `BLETransport` (which reads
/// and consumes it from its own executor), which is why it is a small
/// lock-guarded class rather than state on either of them.
///
/// `@unchecked Sendable` for the ordinary reason: every stored property
/// is private and only ever touched under `lock`.
public final class BLEDelegateDelivery: @unchecked Sendable {
    private let lock = NSLock()
    /// The tail of the delivery chain — the most recently enqueued hop.
    /// Each new hop awaits it before running, which is what makes
    /// delivery FIFO.
    private var tail: Task<Void, Never>?
    private var restorePending = false

    public init() {}

    /// Raised synchronously, on the delegate queue, by
    /// `centralManager(_:willRestoreState:)` — BEFORE that method builds
    /// its actor hop and BEFORE it returns to CoreBluetooth.
    public func markRestorePending() {
        lock.lock(); defer { lock.unlock() }
        restorePending = true
    }

    /// Reads the marker AND clears it — it is consumed by the one
    /// callback it exists to guard.
    ///
    /// `handleCentralStateUpdate` is that callback: the marker's whole
    /// job is to stop the `.poweredOn` arriving in the SAME wake as a
    /// restore from racing the adoption (this file's own header). A
    /// marker that outlived that update would suppress the NEXT,
    /// genuinely unrelated `.poweredOn` — the Bluetooth-off-and-back-on
    /// recovery §3.5 exists for — so "read" and "clear" are one
    /// operation rather than two a caller could forget to pair.
    public func consumeRestorePending() -> Bool {
        lock.lock(); defer { lock.unlock() }
        let value = restorePending
        restorePending = false
        return value
    }

    /// Clears the marker without consuming a state update — used when
    /// the restore is ABANDONED (`willRestoreState` with nothing in the
    /// dictionary to adopt), so a `.poweredOn` that follows is free to
    /// do its ordinary §3.5 work.
    public func clearRestorePending() {
        lock.lock(); defer { lock.unlock() }
        restorePending = false
    }

    /// Read-only peek, for logging and tests. Never use this to make the
    /// `.poweredOn` decision — that is `consumeRestorePending()`'s job.
    public var isRestorePending: Bool {
        lock.lock(); defer { lock.unlock() }
        return restorePending
    }

    /// Hop one delegate callback onto the actor, AFTER every callback
    /// enqueued before it has finished.
    ///
    /// The chain is a `Task` per callback, same as before, but each one
    /// awaits its predecessor's `value` first. `BLETransport`'s handlers
    /// are all synchronous actor methods, so "awaits its predecessor"
    /// costs one actor hop and can never park on I/O — a callback can
    /// queue behind another, never behind a network or radio wait.
    public func enqueue(_ work: @escaping @Sendable () async -> Void) {
        lock.lock()
        let previous = tail
        let task = Task {
            _ = await previous?.value
            await work()
        }
        tail = task
        lock.unlock()
    }

    /// Await everything enqueued so far. Exists for tests (and for a
    /// future teardown path); production code never needs it, because
    /// the chain is the ordering guarantee, not a thing to wait on.
    public func drain() async {
        _ = await currentTail()?.value
    }

    /// Deliberately its own SYNCHRONOUS function: `NSLock.lock()` is
    /// unavailable from an asynchronous context (it must never be held
    /// across a suspension point), so the read and the await are
    /// separated rather than written as one `defer`-guarded block —
    /// the same shape `LoopbackTransport.record(_:)` uses for the same
    /// reason.
    private func currentTail() -> Task<Void, Never>? {
        lock.lock(); defer { lock.unlock() }
        return tail
    }
}
