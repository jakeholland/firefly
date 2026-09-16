//
//  TelemetryBatchPolicy.swift — A04: the pure "should I flush now"
//  decision behind Firestore batching ("writes batched — flush every
//  60 s or 50 events, and on background"). Split out from `FirebaseSink`
//  (app-target-only, behind `#if canImport(FirebaseCore)`, so it cannot
//  be unit-tested without a linked Firebase SDK) so the actual DECISION
//  — not the Firestore write itself — is exercised here, deterministically,
//  with an injected `TelemetryClock`, the same way `HandshakeRetryClock`
//  lets `MeshtasticClient`'s retry-loop tests avoid a real wall-clock
//  wait.
//
import Foundation

public struct TelemetryBatchPolicy: Sendable, Equatable {
    public let maxEventCount: Int
    public let maxInterval: TimeInterval

    public init(maxEventCount: Int = 50, maxInterval: TimeInterval = 60) {
        self.maxEventCount = maxEventCount
        self.maxInterval = maxInterval
    }

    /// `pendingCount` — how many events are buffered right now.
    /// `oldestPendingEventAt` — when the OLDEST still-unflushed event was
    /// buffered (`nil` iff `pendingCount == 0`); this, not "time of last
    /// flush", is what "flush every 60 s" has to measure against, or a
    /// buffer that never quite reaches 50 events could sit unflushed
    /// indefinitely every time a fresh event resets a last-flush clock.
    /// `isBackgrounding` — true exactly when the app is about to leave
    /// the foreground: "and on background" overrides both thresholds,
    /// because a buffer that never reaches either one must still not be
    /// lost to a process the OS may not resume (see `TelemetryRecorder`'s
    /// own durability guarantee for the LOCAL half of that promise —
    /// this is the upload half).
    public func shouldFlush(pendingCount: Int, oldestPendingEventAt: Date?, now: Date,
                             isBackgrounding: Bool = false) -> Bool {
        guard pendingCount > 0 else { return false }
        if isBackgrounding { return true }
        if pendingCount >= maxEventCount { return true }
        guard let oldestPendingEventAt else { return false }
        return now.timeIntervalSince(oldestPendingEventAt) >= maxInterval
    }
}
