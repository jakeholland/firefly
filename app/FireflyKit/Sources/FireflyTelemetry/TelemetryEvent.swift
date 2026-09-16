//
//  TelemetryEvent.swift — A04 (docs/specs/A04-telemetry.md): one
//  telemetry occurrence.
//
//  A call site builds one of these with just a name, attributes and
//  (optionally) an explicit timestamp — `seq`/`sessionID` are NOT the
//  caller's to set. `TelemetryRecorder.record(_:)` stamps both onto the
//  copy it actually persists and fans out, exactly once, from the one
//  place that owns the monotonic counter and the process's session id
//  — the same reason `seq`/`sessionID` default to `0`/`""` here rather
//  than being required at every call site.
//
import Foundation

public struct TelemetryEvent: Sendable, Equatable, Codable {
    public var name: String
    public var timestamp: Date
    /// Monotonically increasing within one `TelemetryRecorder` — stamped
    /// by `record(_:)`, never set by a caller. `0` until stamped.
    public var seq: UInt64
    /// One process launch's identity — stamped by `record(_:)`. Empty
    /// until stamped.
    public var sessionID: String
    public var attributes: [String: TelemetryValue]

    public init(name: String, timestamp: Date = Date(), seq: UInt64 = 0, sessionID: String = "",
                attributes: [String: TelemetryValue] = [:]) {
        self.name = name
        self.timestamp = timestamp
        self.seq = seq
        self.sessionID = sessionID
        self.attributes = attributes
    }

    /// The stamped copy `record(_:)` actually persists — a pure
    /// function so `TelemetryRecorderTests`/`InMemoryTelemetryRecorder`
    /// can both use exactly the same stamping rule.
    func stamped(seq: UInt64, sessionID: String) -> TelemetryEvent {
        var copy = self
        copy.seq = seq
        copy.sessionID = sessionID
        return copy
    }
}
