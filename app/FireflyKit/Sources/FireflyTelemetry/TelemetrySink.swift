//
//  TelemetrySink.swift — A04: where a recorded event goes besides the
//  local JSON-lines log. `TelemetryRecorder` fans every stamped event
//  out to zero or more of these; `FirebaseSink` (app target, behind
//  `#if canImport(FirebaseCore)`) is the only non-`Noop` conformer this
//  PR ships.
//
public protocol TelemetrySink: Sendable {
    func send(_ event: TelemetryEvent) async
    /// "…and on background" — `TelemetryBatchPolicy`'s third flush
    /// trigger, the one no sink can notice on its own (it has no
    /// scene-phase observation of its own — one composition root, one
    /// place that knows the app backgrounded). `TelemetryRecorder
    /// .notifyBackground()` calls this on every attached sink;
    /// defaulted to a no-op (below) so a sink with nothing to batch
    /// (`NoopSink`, any future sink that uploads immediately) need not
    /// implement it.
    func flushOnBackground() async
}

public extension TelemetrySink {
    func flushOnBackground() async {}
}

/// The harmless default — every `FireflyMesh`/`FireflyModel` call site
/// that constructs a transport/client directly (every existing test,
/// `.stub()`) gets this rather than a sink that does nothing THE SAME
/// WAY Firebase's absence does, so a build with no `GoogleService-Info.plist`
/// behaves identically to one that never linked Firebase at all.
public struct NoopSink: TelemetrySink, Sendable {
    public init() {}
    public func send(_ event: TelemetryEvent) async {}
}
