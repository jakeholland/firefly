//
//  EventHubTests.swift — the multicast fan-out and its back-pressure
//  rule, pinned (docs/specs/A01-companion-app.md, S1 and S2).
//
import FireflyMesh
import XCTest

final class EventHubTests: XCTestCase {

    /// Two subscribers, one hub: both see every value published while
    /// they were subscribed. This is the property `AsyncStream` alone
    /// does not have — the reason `EventHub` exists.
    func testEachSubscriberGetsItsOwnStream() async {
        let hub = EventHub<Int>()
        let a = hub.subscribe()
        let b = hub.subscribe()
        hub.yield(1)
        hub.yield(2)
        hub.finish()

        var aSeen: [Int] = []
        for await v in a { aSeen.append(v) }
        var bSeen: [Int] = []
        for await v in b { bSeen.append(v) }

        XCTAssertEqual(aSeen, [1, 2])
        XCTAssertEqual(bSeen, [1, 2])
    }

    /// Multicast, not replay: a subscriber only sees what is published
    /// AFTER it subscribes. Callers that need every value (like
    /// `ConnectViewModel.observe()`) must call the `...() ->
    /// AsyncStream<...>` method before triggering anything that
    /// publishes — see that method's own comment.
    func testALateSubscriberMissesEarlierValues() async {
        let hub = EventHub<Int>()
        hub.yield(1)
        let late = hub.subscribe()
        hub.yield(2)
        hub.finish()

        var seen: [Int] = []
        for await v in late { seen.append(v) }
        XCTAssertEqual(seen, [2])
    }

    /// `.bufferingNewest(4096)`, as the spec prescribes for every
    /// stream: a subscriber that falls behind loses the OLDEST values,
    /// never the newest — the newest packet is the one live presence
    /// cares about.
    func testBuffersNewestNotOldest() async {
        let hub = EventHub<Int>()
        let stream = hub.subscribe()
        for i in 1...4100 { hub.yield(i) }
        hub.finish()

        var seen: [Int] = []
        for await v in stream { seen.append(v) }
        XCTAssertEqual(seen.count, 4096)
        XCTAssertEqual(seen.first, 5)
        XCTAssertEqual(seen.last, 4100)
    }
}
