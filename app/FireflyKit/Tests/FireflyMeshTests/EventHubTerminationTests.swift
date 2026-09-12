//
//  EventHubTerminationTests.swift — the hardening QA pass's addition to
//  `EventHubTests`: a hub that has been `finish()`ed must never deliver
//  another value to anybody, INCLUDING a subscriber that registered
//  concurrently with the `finish()` itself.
//
//  Why this is festival-critical rather than housekeeping: every
//  `for await` in this app sits on a hub stream, and a stream nothing
//  ever finishes is a task that never returns. `MeshtasticClient
//  .connect()` subscribes to `transport.events()` and parks its
//  `receiveTask` on it; `LoopbackTransport`/`BLETransport.disconnect()`
//  finish that same hub. A disconnect racing a connect — Bluetooth
//  switched off mid-handshake, or a pocket drop landing on the Connect
//  screen's CONNECT button — IS that race, and before the fix this
//  pins, each occurrence leaked one unkillable task AND left a
//  finished hub still feeding it events.
//
//  DETECTION, deliberately non-blocking: after racing subscribe against
//  finish, the hub is told to `yield` a sentinel. A correctly finished
//  stream ends without producing it; an orphaned one produces it
//  immediately. So each iteration resolves instantly either way — this
//  test can never hang and never needs a watchdog.
//
//  FLAKE DIRECTION, stated on purpose: on a host that never actually
//  interleaves the two closures (a single-core CI runner), the race
//  simply does not occur and this passes vacuously. It can therefore be
//  flaky-GREEN, never flaky-RED — the only acceptable direction for a
//  concurrency probe in this repo's CI. The bug it pins was measured,
//  not reasoned: 8 orphans in 5000 iterations here before the fix, 0
//  after.
//
import Dispatch
import FireflyMesh
import XCTest

final class EventHubTerminationTests: XCTestCase {
    private static let raceIterations = 5000

    func testFinishedHubNeverFeedsASubscriberThatRacedTheFinish() async {
        var delivered = 0
        for _ in 0..<Self.raceIterations {
            let hub = EventHub<Int>()
            nonisolated(unsafe) var stream: AsyncStream<Int>?
            DispatchQueue.concurrentPerform(iterations: 2) { i in
                if i == 0 { stream = hub.subscribe() } else { hub.finish() }
            }
            hub.yield(sentinel)
            for await value in stream! where value == sentinel { delivered += 1; break }
        }
        XCTAssertEqual(delivered, 0,
                       "\(delivered) subscriber(s) were still fed by a finished EventHub — each is a stream nothing will ever finish")
    }

    func testFinishedCurrentValueHubNeverFeedsASubscriberThatRacedTheFinish() async {
        var delivered = 0
        for _ in 0..<Self.raceIterations {
            let hub = CurrentValueEventHub<Int>()
            // A replayed current value is legitimate even after finish
            // (that type's own doc comment), so it is deliberately
            // distinct from the sentinel this test counts.
            hub.yield(replayed)
            nonisolated(unsafe) var stream: AsyncStream<Int>?
            DispatchQueue.concurrentPerform(iterations: 2) { i in
                if i == 0 { stream = hub.subscribe() } else { hub.finish() }
            }
            hub.yield(sentinel)
            for await value in stream! where value == sentinel { delivered += 1; break }
        }
        XCTAssertEqual(delivered, 0,
                       "\(delivered) subscriber(s) were still fed by a finished CurrentValueEventHub")
    }

    /// The uncontended halves of the same invariant — deterministic on
    /// every host, so a single-core runner that never trips the race
    /// above still asserts something real here.
    func testSubscribingAfterFinishYieldsNothingFurther() async {
        let hub = EventHub<Int>()
        hub.finish()
        let stream = hub.subscribe()
        hub.yield(sentinel)
        var received: [Int] = []
        for await value in stream { received.append(value) }
        XCTAssertEqual(received, [], "a hub finished before subscribe() must hand out an already-finished stream")
    }

    func testCurrentValueSubscribingAfterFinishReplaysOnlyTheCurrentValue() async {
        let hub = CurrentValueEventHub<Int>()
        hub.yield(replayed)
        hub.finish()
        let stream = hub.subscribe()
        hub.yield(sentinel)
        var received: [Int] = []
        for await value in stream { received.append(value) }
        XCTAssertEqual(received, [replayed],
                       "a finished CurrentValueEventHub replays its last value, then nothing more")
    }
}

private let sentinel = 0x5EE_D
private let replayed = 0x11
