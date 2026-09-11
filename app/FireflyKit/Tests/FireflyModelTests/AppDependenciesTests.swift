//
//  AppDependenciesTests.swift — the stub stack never fabricates.
//
import FireflyMesh
import FireflyModel
import XCTest

final class AppDependenciesTests: XCTestCase {

    func testStubReportsLocationUnavailableNeverAFakeFix() async {
        let deps = AppDependencies.stub()
        XCTAssertEqual(deps.location.authorization, .deniedOrRestricted)
        var seen: [LocationFix?] = []
        for await fix in deps.location.fixes() { seen.append(fix) }
        XCTAssertEqual(seen, [nil], "an unavailable provider must yield nil, never a coordinate")
    }

    func testStubReportsNoHeadingEver() async {
        let deps = AppDependencies.stub()
        var seen: [HeadingReading?] = []
        for await heading in deps.heading.headings() { seen.append(heading) }
        XCTAssertEqual(seen, [nil], "NOHDG, never a stuck or fabricated arrow")
    }

    func testSettingsStoreRoundTrips() {
        let deps = AppDependencies.stub()
        XCTAssertNil(deps.store.string(.lastPeripheralID))
        deps.store.setString("abc123", .lastPeripheralID)
        XCTAssertEqual(deps.store.string(.lastPeripheralID), "abc123")

        XCTAssertFalse(deps.store.bool(.locationSharingEnabled))
        deps.store.setBool(true, .locationSharingEnabled)
        XCTAssertTrue(deps.store.bool(.locationSharingEnabled))
    }
}
