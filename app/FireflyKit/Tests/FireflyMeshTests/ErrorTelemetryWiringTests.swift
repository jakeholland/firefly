//
//  ErrorTelemetryWiringTests.swift — A04: `error {domain, code, where}`
//  at the connect()-path caught-error site (docs/specs/A04-telemetry.md
//  §6, "Known gaps" — now wired). Alongside the existing
//  `publish(.failed(...))` this app already made, never instead of it.
//
import FireflyMesh
import FireflyTelemetry
import XCTest

/// Throws on `connect()` every time — the one thing `LoopbackTransport`
/// (this package's own test double) cannot do, since its `connect()`
/// always succeeds. Everything else is unused by the test below and so
/// left minimally implemented.
private final class AlwaysFailsToConnectTransport: MeshTransport, @unchecked Sendable {
    let kind: TransportKind = .message
    struct ConnectFailure: Error, CustomStringConvertible {
        var description: String { "simulated transport connect failure" }
    }
    func events() -> AsyncStream<TransportEvent> { AsyncStream { $0.finish() } }
    func connect() async throws { throw ConnectFailure() }
    func disconnect() async {}
    func send(_ data: Data) async throws {}
    var isLinkReady: Bool { get async { false } }
}

final class ErrorTelemetryWiringTests: XCTestCase {
    /// The BLE-connect catch site: `MeshtasticClient.connect()`'s own
    /// `do { try await transport.connect() } catch { ... }`.
    func testTransportConnectFailureRecordsAnErrorEvent() async throws {
        let telemetry = InMemoryTelemetryRecorder()
        let client = MeshtasticClient(transport: AlwaysFailsToConnectTransport(), telemetry: telemetry)

        do {
            try await client.connect()
            XCTFail("connect() must rethrow the transport's failure")
        } catch {
            // Expected — the existing behaviour this PR does not change.
        }

        // The `error` event is recorded from a detached `Task`
        // (`MeshtasticClient.connect()`'s own A04 comment) — give it a
        // moment to land rather than racing it.
        var events: [TelemetryEvent] = []
        for _ in 0..<200 {
            events = await telemetry.events
            if events.contains(where: { $0.name == TelemetryEventName.error }) { break }
            try await Task.sleep(for: .milliseconds(5))
        }

        let errorEvent = try XCTUnwrap(events.first { $0.name == TelemetryEventName.error })
        XCTAssertEqual(errorEvent.attributes[TelemetryAttributeKey.domain], .string("ble"))
        XCTAssertEqual(errorEvent.attributes[TelemetryAttributeKey.whereKey],
                       .string("MeshtasticClient.connect.transport"))
        guard case .string(let code)? = errorEvent.attributes[TelemetryAttributeKey.errorCode] else {
            XCTFail("error event must carry an error_code attribute")
            return
        }
        XCTAssertEqual(code, "simulated transport connect failure",
                       "error_code must describe the actual thrown error, not a placeholder")
        // Regression guard for the collision this key's own doc comment
        // explains: `error_code` must never be silently stripped the way
        // a literal `code` key would be.
        XCTAssertTrue(TelemetryAttributeAllowlist.isClean(errorEvent.attributes))
    }
}
