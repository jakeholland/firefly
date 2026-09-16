//
//  TelemetryClock.swift — A04: the same injected-clock convention
//  `HandshakeRetryClock` (`FireflyMesh/MeshtasticClient.swift`) already
//  uses, for the same reason — `TelemetryBatchPolicyTests` needs to
//  drive "60 seconds have passed" without a real 60-second wait.
//
import Foundation

public protocol TelemetryClock: Sendable {
    func now() -> Date
}

public struct SystemTelemetryClock: TelemetryClock, Sendable {
    public init() {}
    public func now() -> Date { Date() }
}
