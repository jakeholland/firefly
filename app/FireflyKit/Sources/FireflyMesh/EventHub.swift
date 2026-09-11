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
