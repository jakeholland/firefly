//
//  BridgeMemorySafetyTests.swift — memory-safety heuristics for every
//  Bridge/* type's heap-allocated C context, plus the fixed-char-array
//  over-read guard (docs/specs/A01-companion-app.md, slice B's "Must
//  add": "a lifetime test (allocate/free a CrewStore in a loop under the
//  address sanitiser without a leak or a use-after-free); ... a test
//  that a char[16] name containing no terminator does not over-read").
//
//  ## Running under the address sanitiser
//  `swift test` does not enable ASan by default. Run this file under it
//  explicitly when changing anything in Bridge/*:
//
//    swift test --sanitize=address --filter BridgeMemorySafetyTests
//
//  CI (A01_AC1) runs plain `swift test`, which still exercises every
//  loop below — ASan additionally turns a latent use-after-free/
//  double-free into a hard failure instead of one that only shows up as
//  flakiness later. The loop counts here (500+) are chosen to be a
//  meaningful heuristic without materially slowing down a non-ASan run.
//
//  ## Thread safety (docs/specs/A01-companion-app.md, "Threading model")
//  `firmware/core` has NO locks, by design — it was written for a
//  single embedded main loop, and every `ff_*` context this bridge
//  wraps inherits that: `CrewStore`, `InboxBridge`, `FindBridge` and
//  `RadarBridge` are plain (non-`Sendable`) classes on purpose, NOT
//  because nobody got around to synchronizing them. There is
//  deliberately no test here that calls one from two threads
//  concurrently — that would be undefined behavior in the C core, not a
//  bug this bridge could catch or fix; the correct fix for a data race
//  is confinement, not a lock. The app confines exactly one instance of
//  each to `CoreStore`'s single `@MainActor` isolation domain
//  (CoreStore.swift); that confinement is real and correctly designed,
//  but it is confinement BY CONVENTION AND CODE REVIEW today, not a
//  compiler guarantee (PR #261 review, finding 5): `Package.swift`
//  builds this target under Swift's default (`minimal`) strict-
//  concurrency checking, which does not reliably flag a non-`Sendable`
//  type like these being captured off the actor that "owns" it — that
//  diagnostic only becomes dependable under `SWIFT_STRICT_CONCURRENCY:
//  complete`, explicitly deferred to M3. Nothing in this file enforces
//  the confinement either; it is enforced by every caller routing
//  through `CoreStore`'s `@MainActor` surface (or a view model's own
//  `@MainActor` context), which is discipline, not a type-system proof.
//
@testable import FireflyModel
import FireflyCore
import XCTest

final class BridgeMemorySafetyTests: XCTestCase {

    // MARK: - Allocate/free loops (heuristic for leaks and UAF).

    func testCrewStoreAllocateDestroyLoop() {
        for i in 0..<500 {
            let store = CrewStore(now: { 0 })
            store.onPosition(nodeID: UInt32(i), latitude: 47.707135, longitude: -122.2820993, rxTimeMs: 0)
            store.onRSSI(nodeID: UInt32(i), rssiDbm: -70)
            store.onHeard(nodeID: UInt32(i), rxTimeMs: 0, direct: true)
            _ = store.member(nodeID: UInt32(i), now: 1_000)
            // `store` and its CoreClock both go out of scope here —
            // deinit must free both without crashing or leaking.
        }
    }

    func testInboxBridgeAllocateDestroyLoop() {
        for i in 0..<500 {
            let inbox = InboxBridge()
            inbox.push(FeedItem(kind: .text, fromNode: UInt32(i), atMs: 0, text: "hi", direction: .direct))
            _ = inbox.unreadCount
        }
    }

    func testFindBridgeAllocateDestroyLoop() {
        for i in 0..<500 {
            let find = FindBridge()
            find.start(targetNodeID: UInt32(i), now: 0)
            _ = find.tick(now: 0)
        }
    }

    func testRadarBridgeAllocateDestroyLoop() {
        let crew = CrewStore(now: { 0 })
        crew.setPaired(nodeID: 1, paired: true)
        crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
        for _ in 0..<500 {
            let radar = RadarBridge()
            _ = radar.compute(crew: crew, headingDeg: 0, myPosition: (47.707135, -122.2820993),
                               imperial: false, now: 0)
        }
    }

    /// The whole bridge stack, allocated and torn down together, the
    /// same shape `CoreStore` holds them in — proof the FOUR heap
    /// contexts (`ff_crew_t`+clock, `ff_feed_t`, `ff_find_t`,
    /// `ff_radar_smooth_t`) don't interact badly when freed in whatever
    /// order ARC happens to choose.
    func testWholeBridgeStackAllocateDestroyLoop() {
        for i in 0..<200 {
            let crew = CrewStore(now: { 0 })
            let inbox = InboxBridge()
            let find = FindBridge()
            let radar = RadarBridge()

            crew.setPaired(nodeID: 1, paired: true)
            crew.onPosition(nodeID: 1, latitude: 47.705785, longitude: -122.2820993, rxTimeMs: 0)
            inbox.push(FeedItem(kind: .text, fromNode: 1, atMs: 0, text: "\(i)", direction: .direct))
            find.start(targetNodeID: 1, now: 0)
            _ = radar.compute(crew: crew, headingDeg: 0, myPosition: (47.707135, -122.2820993),
                               imperial: false, now: 0)
        }
    }

    // MARK: - Fixed C char array decode never over-reads.

    /// `char name[16]` with NO NUL byte anywhere in it — a hostile or
    /// simply malformed fixture. `FixedCString.decode` must read
    /// exactly the tuple's own 16 bytes and stop, never walk into
    /// whatever memory happens to sit after it.
    func testCharArrayWithNoTerminatorDoesNotOverRead() {
        var raw = ff_crew_member_t()
        withUnsafeMutableBytes(of: &raw.name) { buf in
            for i in buf.indices { buf[i] = UInt8(ascii: "A") + UInt8(i % 26) }
        }
        let decoded = FixedCString.decode(raw.name)
        XCTAssertEqual(decoded.utf8.count, MemoryLayout.size(ofValue: raw.name),
                       "must decode exactly the tuple's own byte count, no more")
    }

    func testCharArrayWithTerminatorStopsAtIt() {
        var raw = ff_crew_member_t()
        withUnsafeMutableBytes(of: &raw.name) { buf in
            for i in buf.indices { buf[i] = 0x41 } // fill with 'A'
            buf[3] = 0 // terminate after 3 bytes
        }
        XCTAssertEqual(FixedCString.decode(raw.name), "AAA")
    }

    func testEncodeAlwaysLeavesRoomForATerminator() {
        var raw = ff_crew_member_t()
        let exact = String(repeating: "x", count: MemoryLayout.size(ofValue: raw.name))
        FixedCString.encode(exact, into: &raw.name)
        // Truncated by one byte so the buffer still ends in a real NUL.
        XCTAssertEqual(FixedCString.decode(raw.name).utf8.count, MemoryLayout.size(ofValue: raw.name) - 1)
    }
}
