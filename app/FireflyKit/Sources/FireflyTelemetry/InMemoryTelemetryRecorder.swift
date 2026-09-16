//
//  InMemoryTelemetryRecorder.swift — A04: `AppDependencies.stub()`'s
//  telemetry recorder, and every unit test's own. Same stamping rule as
//  `TelemetryRecorder` (`TelemetryEvent.stamped(seq:sessionID:)`) and
//  the same allowlist enforcement, held in memory only — nothing here
//  touches disk, so a test can `await recorder.record(...)` and then
//  read `events` back synchronously afterward with no race.
//
import Foundation

public actor InMemoryTelemetryRecorder: TelemetryRecording, Sendable {
    public private(set) var events: [TelemetryEvent] = []
    private var seq: UInt64 = 0
    private let sessionID: String

    public init(sessionID: String = UUID().uuidString) {
        self.sessionID = sessionID
    }

    public func record(_ event: TelemetryEvent) async {
        seq += 1
        var stamped = event.stamped(seq: seq, sessionID: sessionID)
        stamped.attributes = TelemetryAttributeAllowlist.strip(stamped.attributes)
        events.append(stamped)
    }

    /// Test convenience: the most recent event whose name matches, or
    /// `nil` — most wiring tests only care about the LAST occurrence of
    /// an event, not the whole log.
    public func lastEvent(named name: String) -> TelemetryEvent? {
        events.last { $0.name == name }
    }
}
