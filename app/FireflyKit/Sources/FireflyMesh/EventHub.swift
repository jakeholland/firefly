//
//  EventHub.swift — a multicast AsyncStream, so more than one subscriber
//  can observe the same event source.
//
//  `AsyncStream` itself is single-consumer: a second `for await` over the
//  same instance competes with the first for elements instead of getting
//  its own copy. `MeshtasticClientProtocol`'s link/node/delivery streams
//  each need multiple independent readers in this app — a view model
//  AND `CoreStore`, and for `linkState` also Diagnostics — so a stored
//  `AsyncStream` property is the wrong shape (docs/specs/
//  A01-companion-app.md, S1). Likewise `MeshTransport.events`.
//
//  `EventHub` hands every subscriber its OWN `AsyncStream`, each
//  buffered `.bufferingNewest(4096)` per the spec's back-pressure rule:
//  a stalled consumer must not grow memory without limit, and for live
//  presence the newest element is the one that matters.
//
import Foundation

/// Broadcasts values published with `yield(_:)` to every current and
/// future subscriber. Thread-safe; publishers may call from any thread —
/// CoreBluetooth delegate callbacks, a serial read source and a TCP
/// receive loop all land off the main thread (see the spec's threading
/// model) and all publish through a hub like this one.
public final class EventHub<Element>: @unchecked Sendable {
    private let lock = NSLock()
    private var continuations: [Int: AsyncStream<Element>.Continuation] = [:]
    private var nextID = 0
    private var finished = false

    public init() {}

    /// A fresh, independent stream for one subscriber. Buffers the
    /// newest 4096 elements — a subscriber that falls behind loses the
    /// oldest element, never the newest.
    public func subscribe() -> AsyncStream<Element> {
        lock.lock()
        let id = nextID
        nextID += 1
        let alreadyFinished = finished
        lock.unlock()

        return AsyncStream(bufferingPolicy: .bufferingNewest(4096)) { continuation in
            if alreadyFinished {
                continuation.finish()
                return
            }
            lock.lock()
            continuations[id] = continuation
            lock.unlock()
            continuation.onTermination = { [weak self] _ in
                self?.remove(id)
            }
        }
    }

    /// Publish one element to every subscriber that exists right now.
    /// A subscriber that arrives after this call does not see it — the
    /// same "you get what happens while you're listening" semantics as
    /// the single-consumer `AsyncStream` this replaces.
    public func yield(_ element: Element) {
        lock.lock()
        let subs = Array(continuations.values)
        lock.unlock()
        for continuation in subs { continuation.yield(element) }
    }

    /// Close every current and future subscriber's stream. Idempotent.
    public func finish() {
        lock.lock()
        guard !finished else { lock.unlock(); return }
        finished = true
        let subs = Array(continuations.values)
        continuations.removeAll()
        lock.unlock()
        for continuation in subs { continuation.finish() }
    }

    private func remove(_ id: Int) {
        lock.lock()
        continuations.removeValue(forKey: id)
        lock.unlock()
    }
}

/// Like `EventHub`, but remembers its most recently published value and
/// replays it to every NEW subscriber, BEFORE any future values — the
/// "current value" semantic `MeshtasticClientProtocol.linkState()` needs
/// (M1 review follow-up, #267): a Thread/Diagnostics screen opened AFTER
/// the client already reached `.ready` subscribed to a plain `EventHub`
/// and saw nothing until the NEXT transition, so it sat on its own
/// `.disconnected` default and rendered a stale "NODE NOT CONNECTED" /
/// "NOT CONNECTED" banner for a connection that was, in fact, up.
///
/// This is a deliberate, separate type — not a change to `EventHub`
/// itself. `EventHub`'s multicast-only, no-replay contract is pinned by
/// `EventHubTests.testALateSubscriberMissesEarlierValues` and relied on
/// by every OTHER hub (`nodeUpdates`, `deliveryUpdates`,
/// `incomingTexts`...), where replaying an old node/packet to a late
/// subscriber would be exactly the kind of fabricated freshness this
/// codebase refuses elsewhere. Link state is different: it has one true
/// CURRENT value once the client starts connecting, so a late subscriber
/// asking "is the link up right now" deserves an honest answer
/// immediately, not silence until the next edge.
///
/// There is deliberately no "initial value" constructor parameter: only
/// a value that was actually `yield`ed is ever replayed. A subscriber
/// that arrives before the first `yield` (every existing handshake test
/// subscribes before calling `connect()`) sees exactly what a plain
/// `EventHub` would show it — nothing until the first real transition —
/// so this type never invents a starting state the hub was never told
/// to publish.
public final class CurrentValueEventHub<Element>: @unchecked Sendable {
    private let lock = NSLock()
    private var continuations: [Int: AsyncStream<Element>.Continuation] = [:]
    private var nextID = 0
    private var finished = false
    private var current: Element?

    public init() {}

    /// A fresh, independent stream for one subscriber — immediately
    /// replayed the last `yield`ed value, if any, then every value
    /// published from this point on. Same `.bufferingNewest(4096)`
    /// back-pressure rule as `EventHub.subscribe()`.
    public func subscribe() -> AsyncStream<Element> {
        lock.lock()
        let id = nextID
        nextID += 1
        let alreadyFinished = finished
        let replay = current
        lock.unlock()

        return AsyncStream(bufferingPolicy: .bufferingNewest(4096)) { continuation in
            if let replay {
                continuation.yield(replay)
            }
            if alreadyFinished {
                continuation.finish()
                return
            }
            lock.lock()
            continuations[id] = continuation
            lock.unlock()
            continuation.onTermination = { [weak self] _ in
                self?.remove(id)
            }
        }
    }

    /// Publish one element to every subscriber that exists right now,
    /// AND remember it as the current value future subscribers replay.
    public func yield(_ element: Element) {
        lock.lock()
        current = element
        let subs = Array(continuations.values)
        lock.unlock()
        for continuation in subs { continuation.yield(element) }
    }

    /// Close every current and future subscriber's stream. Idempotent.
    /// A subscriber arriving after `finish()` still gets replayed the
    /// last current value (if any) before its stream closes — "what was
    /// the link state" stays answerable even once the hub itself is done.
    public func finish() {
        lock.lock()
        guard !finished else { lock.unlock(); return }
        finished = true
        let subs = Array(continuations.values)
        continuations.removeAll()
        lock.unlock()
        for continuation in subs { continuation.finish() }
    }

    private func remove(_ id: Int) {
        lock.lock()
        continuations.removeValue(forKey: id)
        lock.unlock()
    }
}

/// A one-value, lock-protected box — the smallest thing that lets an
/// `actor` publish a single piece of its own state to a `nonisolated`
/// synchronous reader (`MeshtasticClient.connectedNodeNum`). Not a
/// general-purpose escape hatch from actor isolation: the actor stays
/// the only writer, so the box can never disagree with it except by
/// being momentarily behind, which is exactly what any snapshot read of
/// another isolation domain's state is.
final class LockedValue<Value>: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: Value

    init(_ initial: Value) { storage = initial }

    var value: Value {
        get { lock.lock(); defer { lock.unlock() }; return storage }
        set { lock.lock(); defer { lock.unlock() }; storage = newValue }
    }
}
