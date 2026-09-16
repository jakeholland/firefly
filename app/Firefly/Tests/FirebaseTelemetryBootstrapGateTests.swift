//
//  FirebaseTelemetryBootstrapGateTests.swift — A04 review gap-close:
//  `FirebaseTelemetryBootstrap.shouldAttachSink(...)`, the pure decision
//  behind "never touch the network from an XCTest process", tested the
//  same way `FireflyDebugNotifyLaunchTests` tests its sibling launch-arg
//  parsers — as a pure function over injected arguments/flags, no
//  process launch, no Firebase SDK involved at all.
//
import XCTest

final class FirebaseTelemetryBootstrapGateTests: XCTestCase {
    func testAttachesOnAnOrdinaryLaunch() {
        XCTAssertTrue(FirebaseTelemetryBootstrap.shouldAttachSink(
            arguments: ["Firefly"], isXCTestRuntimeLoaded: false, isRunningUnderXCTest: false))
    }

    /// The macOS gap this gate exists to close: `-FireflyDemo` alone,
    /// with neither XCTest signal set, still attaches — the live-
    /// Firebase verification launch this repo's own review process
    /// uses.
    func testAttachesUnderPlainFireflyDemoWithNoXCTestSignal() {
        XCTAssertTrue(FirebaseTelemetryBootstrap.shouldAttachSink(
            arguments: ["Firefly", "-FireflyDemo"], isXCTestRuntimeLoaded: false, isRunningUnderXCTest: false))
    }

    func testNeverAttachesWhenTheXCTestRuntimeIsLoaded() {
        XCTAssertFalse(FirebaseTelemetryBootstrap.shouldAttachSink(
            arguments: ["Firefly"], isXCTestRuntimeLoaded: true, isRunningUnderXCTest: false))
    }

    func testNeverAttachesWhenXCTestConfigurationFilePathIsSet() {
        XCTAssertFalse(FirebaseTelemetryBootstrap.shouldAttachSink(
            arguments: ["Firefly"], isXCTestRuntimeLoaded: false, isRunningUnderXCTest: true))
    }

    /// An app-hosted macOS UI test launched with `-FireflyDemo` — the
    /// exact composition `AppDependencies.current()`'s `#if
    /// targetEnvironment(simulator)` guard does NOT catch on macOS
    /// (`.live()` is used regardless of the argument) — must still be
    /// refused, because the XCTest signal alone is decisive.
    func testNeverAttachesUnderXCTestEvenWithFireflyDemoPresent() {
        XCTAssertFalse(FirebaseTelemetryBootstrap.shouldAttachSink(
            arguments: ["Firefly", "-FireflyDemo"], isXCTestRuntimeLoaded: false, isRunningUnderXCTest: true))
    }

    func testNeverAttachesWithAnyFireflyDebugPrefixedArgument() {
        for flag in ["-FireflyDebugNotify", "-FireflyDebugStartDestination", "-FireflyDebugCrew",
                     "-FireflyDebugHideBadge"] {
            XCTAssertFalse(FirebaseTelemetryBootstrap.shouldAttachSink(
                arguments: ["Firefly", flag], isXCTestRuntimeLoaded: false, isRunningUnderXCTest: false),
                "\(flag) must refuse the sink")
        }
    }
}
