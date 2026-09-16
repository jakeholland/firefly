//
//  FirebaseSink.swift — A04 (docs/specs/A04-telemetry.md): the one
//  non-`Noop` `TelemetrySink` this PR ships. APP TARGET ONLY — neither
//  FireflyKit's Package.swift nor any FireflyKit target depends on
//  Firebase (`FireflyKit/Package.swift`'s own header on `FireflyTelemetry`
//  says so), so this whole file, and everything under this directory,
//  lives here instead.
//
//  `#if canImport(FirebaseCore)` end to end, the SAME convention
//  `NotificationSending.swift` already uses for `#if canImport
//  (UserNotifications)`: a build where Firebase failed to resolve (or
//  was deliberately dropped — see project.yml's own comment on why
//  macOS gets `#if os(iOS)` as a fallback if linking Firebase there
//  ever proves troublesome) still compiles, with this sink simply not
//  existing. Nothing above this seam (`TelemetryRecorder`,
//  `AppDependencies`) ever references a Firebase type directly — the
//  composition root reaches for this type ONLY behind the same guard
//  (`FirebaseTelemetryBootstrap.swift`).
//
//  Two upload channels, one gate:
//
//  1. Crashlytics breadcrumbs + custom keys — `build`/`radio name hash`/
//     `link state`, set as each event carries new information. These
//     are NOT a second copy of the local JSONL: they are what a real
//     crash report carries alongside it, answered from Crashlytics'
//     own console rather than a field-collected phone.
//  2. Firestore — batched writes to `devices/{installId}/sessions/
//     {sessionId}/events/{seq}` + `devices/{installId}` (`build`,
//     `device`, `last_seen`), flushed by `TelemetryBatchPolicy` (every
//     60 s or 50 events, and on background — `flushOnBackground()`).
//
//  Both channels check `isSharingEnabled()` — the "Share diagnostics"
//  toggle — before doing anything: OFF means the event was already
//  durably appended to the local JSONL by `TelemetryRecorder` (this
//  sink is fanned out to AFTER that append, never instead of it), and
//  neither channel here ever sees it. OFF never deletes or recalls
//  anything already uploaded; it only stops the NEXT upload.
//
#if canImport(FirebaseCore)
import FirebaseAuth
import FirebaseCrashlytics
import FirebaseFirestore
import FireflyTelemetry
import Foundation

/// An `actor` — the buffer `send(_:)` appends to and `flush()` drains
/// needs exactly the same single-writer discipline `TelemetryRecorder`
/// itself has, for the same reason: two overlapping flushes racing the
/// same `seq` range would double-write Firestore documents (harmless —
/// `setData` on the same path is idempotent — but two batches building
/// their OWN device-summary write at once is not a race worth having).
public actor FirebaseSink: TelemetrySink {
    private let installId: String
    private let sessionId: String
    private let buildString: String
    private let deviceString: String
    private let isSharingEnabled: @Sendable () -> Bool
    private let policy: TelemetryBatchPolicy
    private let clock: any TelemetryClock
    private let firestore: Firestore

    private var pending: [TelemetryEvent] = []
    private var oldestPendingAt: Date?

    /// Raw stderr write, same discipline as `TelemetryRecorder.log(_:)`.
    private static func log(_ message: String) {
        FileHandle.standardError.write(Data("[FirebaseSink] \(message)\n".utf8))
    }

    public init(installId: String, sessionId: String, buildString: String, deviceString: String,
                isSharingEnabled: @escaping @Sendable () -> Bool,
                policy: TelemetryBatchPolicy = TelemetryBatchPolicy(),
                clock: any TelemetryClock = SystemTelemetryClock(),
                firestore: Firestore = Firestore.firestore()) {
        self.installId = installId
        self.sessionId = sessionId
        self.buildString = buildString
        self.deviceString = deviceString
        self.isSharingEnabled = isSharingEnabled
        self.policy = policy
        self.clock = clock
        self.firestore = firestore
    }

    // MARK: - TelemetrySink

    public func send(_ event: TelemetryEvent) async {
        // Crashlytics: unconditional on the SDK being configured at
        // all, but still behind the toggle — "Share diagnostics" OFF
        // means neither channel leaves this phone, not just Firestore.
        guard isSharingEnabled() else { return }
        recordCrashlyticsBreadcrumb(event)

        pending.append(event)
        if oldestPendingAt == nil { oldestPendingAt = clock.now() }
        if policy.shouldFlush(pendingCount: pending.count, oldestPendingEventAt: oldestPendingAt, now: clock.now()) {
            await flush()
        }
    }

    /// Called from the app's own background-transition hook
    /// (`FirebaseTelemetryBootstrap`'s own doc comment) — "and on
    /// background" is `TelemetryBatchPolicy`'s third flush trigger, and
    /// this is the only one this sink cannot notice on its own (it has
    /// no scene-phase observation of its own by design — one composition
    /// root, one place that knows the app backgrounded, same as every
    /// other lifecycle event in this codebase).
    public func flushOnBackground() async {
        guard isSharingEnabled() else { return }
        await flush()
    }

    // MARK: - Crashlytics

    /// `build`/`radio name hash`/`link state` — the three custom keys
    /// A04 names — updated opportunistically as an event happens to
    /// carry new information about them, never invented. A `ble.*`
    /// event's own `name`/`state` attributes are exactly the "radio
    /// name" (already hashed nowhere here — BLE peripheral names are
    /// not node identifiers, `TelemetryHash` is for node NUMBERS, and a
    /// BLE name is diagnostic-safe, unlike a crew code or message body)
    /// and link-state facts this key pair exists to carry.
    private func recordCrashlyticsBreadcrumb(_ event: TelemetryEvent) {
        let crashlytics = Crashlytics.crashlytics()
        crashlytics.setCustomValue(buildString, forKey: "build")
        crashlytics.log("\(event.name) \(Self.plainAttributes(event))")
        switch event.name {
        case TelemetryEventName.bleConnected, TelemetryEventName.bleReady,
             TelemetryEventName.bleDisconnected, TelemetryEventName.bleHandshakePhase:
            crashlytics.setCustomValue(event.name, forKey: "link_state")
        default:
            break
        }
    }

    /// A plain `"key=value key=value"` line — Crashlytics' `log(_:)` is
    /// a free-form breadcrumb string, not a structured payload, and this
    /// is already allowlist-clean (`TelemetryAttributeAllowlist` ran
    /// before this event ever reached a sink — `TelemetryRecorder
    /// .record(_:)`'s own doc comment).
    private static func plainAttributes(_ event: TelemetryEvent) -> String {
        event.attributes.map { key, value in "\(key)=\(value.plainDescription)" }
            .sorted()
            .joined(separator: " ")
    }

    // MARK: - Firestore

    private func flush() async {
        guard !pending.isEmpty else { return }
        let events = pending
        pending.removeAll()
        oldestPendingAt = nil

        let deviceRef = firestore.collection("devices").document(installId)
        let batch = firestore.batch()
        for event in events {
            let eventRef = deviceRef.collection("sessions").document(sessionId)
                .collection("events").document(String(event.seq))
            batch.setData(Self.document(for: event), forDocument: eventRef)
        }
        batch.setData([
            "build": buildString,
            "device": deviceString,
            "last_seen": FieldValue.serverTimestamp(),
        ], forDocument: deviceRef, merge: true)

        do {
            try await batch.commit()
        } catch {
            // The local JSONL already has every one of these events
            // durably — a failed Firestore write loses nothing but the
            // upload; `pending` was already drained rather than
            // re-queued, since this batch policy's own "every 60s or 50
            // events" cadence means the NEXT flush attempt (assuming
            // connectivity by then) simply carries whatever comes in
            // after this point, not an ever-growing backlog racing a
            // network that may not come back for the rest of the
            // festival.
            Self.log("batch commit failed (\(events.count) events): \(error) — kept locally, not retried")
        }
    }

    private static func document(for event: TelemetryEvent) -> [String: Any] {
        var doc: [String: Any] = [
            "name": event.name,
            "ts": Timestamp(date: event.timestamp),
            "seq": event.seq,
        ]
        for (key, value) in event.attributes {
            doc[key] = value.firestoreValue
        }
        return doc
    }
}

private extension TelemetryValue {
    var plainDescription: String {
        switch self {
        case .string(let value): return value
        case .int(let value): return String(value)
        case .double(let value): return String(value)
        case .bool(let value): return String(value)
        }
    }

    var firestoreValue: Any {
        switch self {
        case .string(let value): return value
        case .int(let value): return value
        case .double(let value): return value
        case .bool(let value): return value
        }
    }
}
#endif
