//
//  CoreStoreTests.swift — CoreStore and a view model can observe the
//  SAME client independently, without stealing each other's events
//  (docs/specs/A01-companion-app.md, S1). This is the regression test
//  for the bug the single-consumer AsyncStream design would have had.
//
import FireflyMesh
import FireflyModel
import XCTest

@MainActor
final class CoreStoreTests: XCTestCase {

    func testCoreStoreAndAViewModelBothReachReadyFromTheSameClient() async {
        let client = StubMeshtasticClient()
        let vm = ConnectViewModel(client: client)
        let store = CoreStore()

        vm.observe()
        store.observe(client: client)
        await vm.connect()

        for _ in 0..<200 where vm.link != .ready || store.linkState != .ready {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        XCTAssertEqual(vm.link, .ready, "the view model's own subscription must still see .ready")
        XCTAssertEqual(store.linkState, .ready,
                        "CoreStore's independent subscription must ALSO see .ready — "
                        + "a single-consumer AsyncStream would have let one of these steal the other's events")
    }

    func testObserveIsIdempotent() {
        let store = CoreStore()
        let client = StubMeshtasticClient()
        store.observe(client: client)
        store.observe(client: client) // must not crash or double-subscribe
        store.stopObserving()
    }

    // MARK: - apply(nodeUpdate:) lands in ff_crew_t (slice B).
    //
    // docs/specs/A01-companion-app.md, slice B's acceptance: "CoreStore's
    // apply() hooks are real and CoreStoreTests ... grows tests that
    // they land in ff_crew_t/ff_feed_t." These call `apply` directly
    // (the same entry point `nodeObservation`'s `for await` loop calls)
    // rather than routing bytes through a whole client, so the
    // assertion is squarely about the bridge wiring, not the transport.

    func testApplyNodeUpdateWithAPositionLandsInCrew() {
        let store = CoreStore()
        let snapshot = MeshNodeSnapshot(
            num: 1, shortName: "TAYL", longName: "Taylor",
            position: NodePosition(latitude: 47.705785, longitude: -122.2820993, time: nil,
                                    source: .externalGPS, precisionBits: 32),
            lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)

        store.apply(nodeUpdate: snapshot)

        let member = store.crew.member(nodeID: 1, now: FireflyClock.nowMillis())
        XCTAssertNotNil(member?.position)
        XCTAssertEqual(member?.position?.latitude ?? 0, 47.705785, accuracy: 0.0001)
        XCTAssertFalse(member?.position?.asserted ?? true, "externalGPS is a measurement, not an assertion")
    }

    /// LOC_MANUAL (`.manual`) must land as `asserted`, never as an aging
    /// measurement (issue #33) — the whole point of routing `meta`
    /// through, not just the coordinate.
    func testApplyNodeUpdateWithManualSourceLandsAsAsserted() {
        let store = CoreStore()
        let snapshot = MeshNodeSnapshot(
            num: 2, shortName: nil, longName: nil,
            position: NodePosition(latitude: 47.708135, longitude: -122.2820993, time: nil,
                                    source: .manual, precisionBits: nil),
            lastHeard: nil, rssiDbm: nil, snrDb: nil, hopsAway: nil)

        store.apply(nodeUpdate: snapshot)

        let member = store.crew.member(nodeID: 2, now: FireflyClock.nowMillis())
        XCTAssertEqual(member?.freshness, .asserted)
    }

    /// RSSI is only attributable when the packet arrived directly —
    /// `hopsAway == 0`. A relayed reading must never land as a direct
    /// signal.
    func testApplyNodeUpdateOnlyRecordsRSSIWhenDirect() {
        let store = CoreStore()
        let relayed = MeshNodeSnapshot(num: 3, shortName: nil, longName: nil, position: nil,
                                        lastHeard: nil, rssiDbm: -70, snrDb: nil, hopsAway: 2)
        store.apply(nodeUpdate: relayed)
        XCTAssertNil(store.crew.member(nodeID: 3, now: FireflyClock.nowMillis())?.directSignal)

        let direct = MeshNodeSnapshot(num: 4, shortName: nil, longName: nil, position: nil,
                                       lastHeard: nil, rssiDbm: -70, snrDb: nil, hopsAway: 0)
        store.apply(nodeUpdate: direct)
        XCTAssertEqual(store.crew.member(nodeID: 4, now: FireflyClock.nowMillis())?.directSignal?.rssiDbm, -70)
    }

    // MARK: - apply(delivery:) lands in ff_feed_t (slice B).

    func testApplyDeliveryRoutesIntoInboxSendStatus() {
        let store = CoreStore()
        let crew = store.crew
        let inbox = store.inbox
        crew.setPaired(nodeID: 1, paired: true)
        inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "omw", direction: .out, toNode: 1,
                             outboxID: 7), unread: false)

        // `apply(delivery:)` — the exact entry point `deliveryObservation`
        // routes into — must land the state in the real `ff_feed_t`.
        store.apply(delivery: (packetID: 7, state: .delivered))

        let msg = inbox.thread(.member(1), crew: crew, now: 0).first
        XCTAssertEqual(msg?.sendStatus, .delivered)
    }

    func testApplyDeliveryOfEachStateLandsHonestly() {
        let cases: [(DeliveryState, FeedSendStatus)] = [
            (.waiting, .waiting), (.sent, .sent), (.delivered, .delivered),
            (.noAck, .noAck), (.dropped, .dropped),
        ]
        for (index, pair) in cases.enumerated() {
            let store = CoreStore()
            store.crew.setPaired(nodeID: 1, paired: true)
            let outboxID = UInt32(index + 1)
            store.inbox.push(FeedItem(kind: .text, fromNode: 0, atMs: 0, text: "x", direction: .out,
                                       toNode: 1, outboxID: outboxID), unread: false)
            store.apply(delivery: (packetID: outboxID, state: pair.0))
            let msg = store.inbox.thread(.member(1), crew: store.crew, now: 0).first
            XCTAssertEqual(msg?.sendStatus, pair.1, "DeliveryState.\(pair.0) must land as FeedSendStatus.\(pair.1)")
        }
    }
}
