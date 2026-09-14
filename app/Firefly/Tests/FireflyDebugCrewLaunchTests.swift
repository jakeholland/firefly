//
//  FireflyDebugCrewLaunchTests.swift — `-FireflyDebugJoinCrew <code>` /
//  `-FireflyDebugStartCrew [name]` argument parsing, the bench seam the
//  Mac orchestrator drives A02's Join/Start with.
//
//  Same shape as `FireflyDebugStartDestinationLaunchTests`: pure
//  parsing over an injected arguments array, no process launch. Both
//  functions are `#if DEBUG`-gated and this suite only ever runs in a
//  DEBUG configuration, so the assertions below are about the DEBUG
//  behaviour — the Release behaviour ("always nil") is a compile-time
//  fact, not something a test in this target can observe.
//
import XCTest

final class FireflyDebugCrewLaunchTests: XCTestCase {
    func testJoinCodeIsTheArgumentAfterTheFlag() {
        XCTAssertEqual(
            FireflyDebugCrewLaunch.requestedJoinCode(arguments: ["Firefly", "-FireflyDebugJoinCrew", "FIRE-4K9M7X"]),
            "FIRE-4K9M7X")
    }

    /// Handed through verbatim — normalisation is `CrewScanPayload
    /// .classify`/`CrewCode.parse`'s job (§1.2), and doing it twice, in
    /// two places, is how two spellings start disagreeing.
    func testJoinCodeIsNotNormalisedHere() {
        XCTAssertEqual(
            FireflyDebugCrewLaunch.requestedJoinCode(arguments: ["-FireflyDebugJoinCrew", "fire 4kim7x"]),
            "fire 4kim7x")
        XCTAssertEqual(
            FireflyDebugCrewLaunch.requestedJoinCode(
                arguments: ["-FireflyDebugJoinCrew", "firefly://crew?v=1&code=FIRE-4K9M7X"]),
            "firefly://crew?v=1&code=FIRE-4K9M7X")
    }

    func testJoinCodeIsNilWithoutTheFlagOrWithoutAValue() {
        XCTAssertNil(FireflyDebugCrewLaunch.requestedJoinCode(arguments: ["Firefly"]))
        XCTAssertNil(FireflyDebugCrewLaunch.requestedJoinCode(arguments: ["Firefly", "-FireflyDebugJoinCrew"]))
    }

    /// The three-way distinction that matters: not asked for (`nil`),
    /// asked for with no name (`""` -> `CrewController`'s own "My crew"
    /// default), asked for with one.
    func testStartCrewDistinguishesAbsentFromBareFromNamed() {
        XCTAssertNil(FireflyDebugCrewLaunch.requestedStartName(arguments: ["Firefly"]))
        XCTAssertEqual(FireflyDebugCrewLaunch.requestedStartName(arguments: ["-FireflyDebugStartCrew"]), "")
        XCTAssertEqual(
            FireflyDebugCrewLaunch.requestedStartName(arguments: ["-FireflyDebugStartCrew", "Camp Firefly"]),
            "Camp Firefly")
    }

    /// A following FLAG is not a name — `-FireflyDebugStartCrew
    /// -FireflyStartTab find` starts an unnamed crew and lands on Find,
    /// rather than minting a crew called "-FireflyStartTab".
    func testStartCrewDoesNotSwallowTheNextFlagAsAName() {
        XCTAssertEqual(
            FireflyDebugCrewLaunch.requestedStartName(
                arguments: ["-FireflyDebugStartCrew", "-FireflyStartTab", "find"]),
            "")
    }
}
