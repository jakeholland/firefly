//
//  BLEDelegateDeliveryTests.swift — A03 §3.1's ordering fix, pinned.
//
//  The bug this closes cannot be reproduced through `BLEDelegateBridge`
//  itself in a unit test: its methods take a `CBCentralManager`, and
//  constructing one outside a signed `.app` aborts the process (B1). So
//  the ordering MECHANISM is a type of its own (`BLEDelegateDelivery`),
//  and this file drives it exactly the way the bridge does — a scripted
//  stand-in for CoreBluetooth's delegate queue, calling the same two
//  methods in the order Apple documents.
//
import FireflyMesh
import XCTest

final class BLEDelegateDeliveryTests: XCTestCase {

    /// A recorder shaped like the actor on the far side of the hop:
    /// every handler appends its own name, so "what order did the actor
    /// see these in" is a readable array rather than an inference.
    private actor Recorder {
        private(set) var events: [String] = []
        func record(_ name: String) { events.append(name) }
        /// Simulates a handler that yields at least once — the shape of
        /// every real `BLETransport` handler, which reaches the actor
        /// through a hop rather than running inline.
        func recordAfterAYield(_ name: String) async {
            await Task.yield()
            events.append(name)
        }
    }

    /// **The ordering criterion (§3.1).** `willRestoreState` is raised
    /// before `didUpdateState`, so the actor must SEE them in that
    /// order.
    ///
    /// This is the test that fails on pre-S1b `main`: every callback was
    /// its own unstructured `Task`, and unstructured tasks carry no
    /// ordering guarantee relative to each other — so
    /// `handleCentralStateUpdate(.poweredOn)` could reach the actor
    /// first and (once §3.5 gave `.poweredOn` real work) tear down the
    /// very session the restore was adopting. The first hop deliberately
    /// yields before recording, which is what an unchained pair of
    /// `Task`s reorders.
    func testWillRestoreStateIsDeliveredBeforeDidUpdateState() async {
        let delivery = BLEDelegateDelivery()
        let recorder = Recorder()

        // Exactly what the bridge does, in the order CoreBluetooth
        // delivers it on its (serial) delegate queue.
        delivery.markRestorePending()
        delivery.enqueue { await recorder.recordAfterAYield("willRestoreState") }
        delivery.enqueue { await recorder.record("didUpdateState") }

        await delivery.drain()
        let events = await recorder.events
        XCTAssertEqual(events, ["willRestoreState", "didUpdateState"])
    }

    /// The chain is FIFO for any number of callbacks, not just two — a
    /// real wake delivers a whole burst (`didConnect`,
    /// `didDiscoverServices`, `didDiscoverCharacteristicsFor`, …) and
    /// CoreBluetooth's own contract is that they are serial. Carrying
    /// that across the hop is the guarantee; dropping it at the hop was
    /// the bug.
    func testDeliveryIsFIFOAcrossABurstOfCallbacks() async {
        let delivery = BLEDelegateDelivery()
        let recorder = Recorder()
        let names = (0..<20).map { "callback-\($0)" }

        for (index, name) in names.enumerated() {
            // Alternate yielding and non-yielding handlers: a chain that
            // only happens to be ordered because every hop is uniform
            // would pass a weaker version of this test.
            if index.isMultiple(of: 2) {
                delivery.enqueue { await recorder.recordAfterAYield(name) }
            } else {
                delivery.enqueue { await recorder.record(name) }
            }
        }

        await delivery.drain()
        let events = await recorder.events
        XCTAssertEqual(events, names)
    }

    /// The marker is raised SYNCHRONOUSLY — visible the instant
    /// `willRestoreState` returns, with no hop having run yet. That is
    /// the half of the fix that does not depend on task scheduling at
    /// all, and the reason the bridge sets it before building its hop.
    func testTheRestoreMarkerIsVisibleSynchronously() {
        let delivery = BLEDelegateDelivery()
        XCTAssertFalse(delivery.isRestorePending)
        delivery.markRestorePending()
        XCTAssertTrue(delivery.isRestorePending, "the marker must not wait on an actor hop to become true")
    }

    /// It is CONSUMED by the state update it guards — one `.poweredOn`,
    /// not every future one. A marker that outlived its callback would
    /// suppress the Bluetooth-off-and-back-on recovery §3.5 exists for,
    /// which is a silent "never reconnects again" rather than a crash.
    func testTheRestoreMarkerIsConsumedByExactlyOneStateUpdate() {
        let delivery = BLEDelegateDelivery()
        delivery.markRestorePending()
        XCTAssertTrue(delivery.consumeRestorePending())
        XCTAssertFalse(delivery.consumeRestorePending(), "the second state update must see a clear marker")
        XCTAssertFalse(delivery.isRestorePending)
    }

    /// An abandoned restore (`willRestoreState` with nothing to adopt)
    /// clears the marker outright, so the `.poweredOn` that follows is
    /// free to do its ordinary §3.5 work.
    func testAnAbandonedRestoreClearsTheMarker() {
        let delivery = BLEDelegateDelivery()
        delivery.markRestorePending()
        delivery.clearRestorePending()
        XCTAssertFalse(delivery.consumeRestorePending())
    }
}
