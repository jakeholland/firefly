//
//  DemoHeadingProvider.swift — the phone's own scripted compass
//  heading for demo mode. Same "replay latest" shape as
//  `DemoLocationProvider` and for the same reason — see that file's
//  header comment.
//
import Foundation

// PR #275 review, SHOULD-FIX 3: `@unchecked Sendable` justified the
// same way as `EventHub` (`EventHub.swift`'s own comment) — every
// mutable access below goes through `lock`, never unguarded.
public final class DemoHeadingProvider: HeadingProviding, @unchecked Sendable {
    private let lock = NSLock()
    private var current: HeadingReading?
    private var nextID = 0
    private var continuations: [Int: AsyncStream<HeadingReading?>.Continuation] = [:]

    public init(initialHeading: HeadingReading?) {
        self.current = initialHeading
    }

    public func headings() -> AsyncStream<HeadingReading?> {
        lock.lock()
        let id = nextID
        nextID += 1
        let value = current
        lock.unlock()
        return AsyncStream(bufferingPolicy: .bufferingNewest(4)) { continuation in
            continuation.yield(value)
            lock.lock()
            continuations[id] = continuation
            lock.unlock()
            continuation.onTermination = { [weak self] _ in self?.remove(id) }
        }
    }

    public func setHeading(_ heading: HeadingReading?) {
        lock.lock()
        current = heading
        let subs = Array(continuations.values)
        lock.unlock()
        for continuation in subs { continuation.yield(heading) }
    }

    private func remove(_ id: Int) {
        lock.lock(); continuations.removeValue(forKey: id); lock.unlock()
    }
}
