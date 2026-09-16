//
//  TelemetryRecording.swift — A04: the seam every call site (and
//  `AppDependencies`) actually holds. `Telemetry.shared`-free by
//  design — injected through `AppDependencies` like every other
//  dependency in this codebase (`store`, `client`, `location`, …), so
//  every emission call site takes this as a constructor parameter,
//  never reaches for a global.
//
import Foundation

public protocol TelemetryRecording: Sendable {
    /// Records one event. Implementations stamp `seq`/`sessionID`
    /// themselves (`TelemetryEvent.stamped(seq:sessionID:)`) — a caller
    /// only ever supplies `name`/`attributes`/(optionally) `timestamp`.
    func record(_ event: TelemetryEvent) async
}

/// The harmless default for a call site that predates telemetry (or a
/// test that does not care about it) — appended, optional-with-default,
/// to every constructor telemetry was wired into, so no existing call
/// site had to change. Distinct from `NoopSink`: this is a
/// `TelemetryRecording` (what a call site holds), not a
/// `TelemetrySink` (what a `TelemetryRecorder` fans out to).
public struct NoopTelemetryRecorder: TelemetryRecording, Sendable {
    public init() {}
    public func record(_ event: TelemetryEvent) async {}
}

/// `TelemetryRecorder`'s own export seam, split into its own protocol
/// (rather than folded into `TelemetryRecording`) so the Settings
/// "Export diagnostics" row and the app-target Firebase wiring
/// (`dependencies.telemetry as? any TelemetrySinkAttaching`) can each
/// ask for exactly the capability they need without every
/// `TelemetryRecording` conformer — including test doubles that will
/// never be asked to export or attach anything — having to implement
/// methods that make no sense for them.
public protocol TelemetryExporting: Sendable {
    /// Every JSON-lines file this recorder currently holds, oldest
    /// first, current file last — what "Export diagnostics" zips/
    /// concatenates and hands to the share sheet.
    func exportFiles() async -> [URL]
}

/// The seam a composition root uses to attach a sink AFTER
/// construction — `AppDependencies.live()` builds the real
/// `TelemetryRecorder` in `FireflyModel`, which cannot know about
/// `FirebaseSink` (app-target-only, behind `#if canImport(FirebaseCore)`).
/// The app target does `(dependencies.telemetry as? any
/// TelemetrySinkAttaching)?.addSink(FirebaseSink(...))` once, after
/// `AppDependencies.live()` returns, only when `GoogleService-Info.plist`
/// is actually in the bundle.
public protocol TelemetrySinkAttaching: Sendable {
    func addSink(_ sink: any TelemetrySink) async
}
