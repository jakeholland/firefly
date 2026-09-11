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

/// `CurrentValueEventHub` — `linkState()`'s current-value fix (M1 review
/// follow-up, #267): unlike plain `EventHub`, a subscriber that arrives
/// AFTER a value was published still sees it, immediately, as the first
/// element on its own stream.
final class CurrentValueEventHubTests: XCTestCase {

    /// The exact scenario the bug report named: something (the real
    /// client, or the demo client) already reached `.ready` before
    /// Thread/Diagnostics ever subscribed. The late subscriber's first
    /// value must be `.ready` — not silence, and not the type's own
    /// construction-time default.
    func testLateSubscriberAfterReadySeesReadyFirst() async {
        let hub = CurrentValueEventHub<LinkState>()
        hub.yield(.connecting)
        hub.yield(.handshaking)
        hub.yield(.ready)

        let late = hub.subscribe()
        hub.finish()

        var seen: [LinkState] = []
        for await v in late { seen.append(v) }
        XCTAssertEqual(seen.first, .ready)
        XCTAssertEqual(seen, [.ready])
    }

    /// A subscriber that arrives before anything was ever published sees
    /// nothing until the first real value — exactly a plain `EventHub`'s
    /// behavior. This type must never invent a starting value the hub
    /// was never told to publish (every existing handshake test
    /// subscribes before calling `connect()` and asserts the exact
    /// sequence that follows, with no extra leading element).
    func testSubscriberBeforeAnyYieldSeesNothingUntilFirstValue() async {
        let hub = CurrentValueEventHub<LinkState>()
        let early = hub.subscribe()
        hub.yield(.connecting)
        hub.finish()

        var seen: [LinkState] = []
        for await v in early { seen.append(v) }
        XCTAssertEqual(seen, [.connecting])
    }

    /// Multicast still holds: an EARLIER subscriber is unaffected by a
    /// later one arriving and being replayed its own current value.
    func testEarlierSubscriberUnaffectedByALateReplayedSubscriber() async {
        let hub = CurrentValueEventHub<LinkState>()
        let early = hub.subscribe()
        hub.yield(.ready)
        let late = hub.subscribe()
        hub.finish()

        var earlySeen: [LinkState] = []
        for await v in early { earlySeen.append(v) }
        var lateSeen: [LinkState] = []
        for await v in late { lateSeen.append(v) }

        XCTAssertEqual(earlySeen, [.ready])
        XCTAssertEqual(lateSeen, [.ready])
    }
}
