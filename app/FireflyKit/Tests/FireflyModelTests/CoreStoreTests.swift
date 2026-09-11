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
}
