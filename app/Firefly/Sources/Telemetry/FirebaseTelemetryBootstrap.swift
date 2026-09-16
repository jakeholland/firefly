//
//  FirebaseTelemetryBootstrap.swift — A04 (docs/specs/A04-telemetry.md):
//  the composition-root glue between `AppDependencies.telemetry` (built
//  in `FireflyModel`, which cannot know Firebase exists) and
//  `FirebaseSink` (this directory, app target only).
//
//  Two calls, from two different places, on purpose:
//
//  * `configureIfNeeded()` — `FireflyApp.init()`, exactly ONCE per
//    process. `FirebaseApp.configure()` traps if called twice ("default
//    app has already been configured"), and `init()` is the one place
//    in this app guaranteed to run once.
//  * `attachSink(to:buildString:deviceString:)` — `AppRuntimeBundle
//    .build(dependencies:...)`, which the demo-mode switch calls again
//    on every enter/leave. Safe to call repeatedly: it is a no-op
//    whenever `dependencies.telemetry` is not the real, file-backed
//    `TelemetryRecorder` (`.stub()`'s `InMemoryTelemetryRecorder` does
//    not conform to `TelemetrySinkAttaching` at all — the cast below
//    simply fails) — which is exactly "never touch Firebase for the
//    demo stack", stated as a type fact rather than an `if isDemoMode`
//    check that could drift from the composition root's own rule.
//
import FireflyModel
import Foundation
#if canImport(FirebaseCore)
import FirebaseAuth
import FirebaseCore
import FirebaseCrashlytics
import FirebaseFirestore
import FireflyTelemetry
#endif

enum FirebaseTelemetryBootstrap {
    /// A04 review gap-close: `attachSink(to:buildString:deviceString:)`
    /// must never touch the network from an XCTest process, whatever
    /// `dependencies.telemetry` happened to be typed as — belt and
    /// braces alongside the existing TYPE-based gate (`.stub()`/
    /// `.demo()`'s `InMemoryTelemetryRecorder` does not conform to
    /// `TelemetrySinkAttaching` at all, so the cast in `attachSink`
    /// already fails for them). That type gate does not cover every
    /// platform: `AppDependencies.current()`'s own `#if
    /// targetEnvironment(simulator)` guard means `-FireflyDemo` is
    /// silently ignored on macOS (`DemoLaunch.isRequested()` is never
    /// even consulted there) and `.live()` — a REAL, attaching
    /// `TelemetryRecorder` — is used regardless of any launch argument,
    /// which is exactly the composition an app-hosted macOS UI test
    /// would run under.
    ///
    /// Deliberately does NOT gate on a bare `-FireflyDemo` argument on
    /// its own — only on actually running under XCTest, or on a
    /// `-FireflyDebug*` flag. The A04 review's own live-Firebase
    /// verification step launches the macOS build with exactly
    /// `-FireflyDemo` and nothing else, specifically so the demo stack
    /// can prove the sink reaches Firestore end to end with no radio
    /// required; gating on that argument alone would make that
    /// verification impossible to ever run again. A Simulator UI test
    /// launched with `-FireflyDemo` was already safe via the type-based
    /// gate above (`AppDependencies.demo()`'s `InMemoryTelemetryRecorder`);
    /// what this closes is the one case that gate does not reach.
    static func shouldAttachSink(arguments: [String] = CommandLine.arguments,
                                  isXCTestRuntimeLoaded: Bool = NSClassFromString("XCTestCase") != nil,
                                  isRunningUnderXCTest: Bool =
                                      ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil)
        -> Bool {
        guard !isXCTestRuntimeLoaded, !isRunningUnderXCTest else { return false }
        guard !arguments.contains(where: { $0.hasPrefix("-FireflyDebug") }) else { return false }
        return true
    }

    #if canImport(FirebaseCore)
    /// `FirebaseApp.configure()` reads `GoogleService-Info.plist` from
    /// the main bundle on its own — but calling it with NO plist present
    /// crashes the process ("Could not locate configuration file").
    /// This is the "must still tolerate its absence" half of A04's own
    /// ask: a build that ships with no plist (should one ever exist —
    /// none is expected, the real one is committed) configures nothing
    /// and every telemetry event still lands in the local JSONL exactly
    /// as if Firebase were never linked at all.
    private static func plistExists(bundle: Bundle = .main) -> Bool {
        bundle.path(forResource: "GoogleService-Info", ofType: "plist") != nil
    }

    /// Idempotent: `FirebaseApp.app()` is non-nil once `configure()` has
    /// run, so a second call (there should never be one, but `init()`
    /// re-running under a SwiftUI preview/XCTest host is exactly the
    /// kind of thing worth guarding rather than trusting) is a no-op
    /// rather than a trap.
    static func configureIfNeeded() {
        guard plistExists() else { return }
        guard FirebaseApp.app() == nil else { return }
        FirebaseApp.configure()
        // Firestore offline persistence ON — the whole point of this
        // feature for Lost Lands' poor cell coverage: a write made with
        // no signal queues locally and flushes once the phone has some,
        // with no code on this app's side needing to know which case it
        // is in. `cacheSettings`, not the deprecated `isPersistenceEnabled`
        // Bool, per Firebase's own current API.
        let settings = Firestore.firestore().settings
        settings.cacheSettings = PersistentCacheSettings()
        Firestore.firestore().settings = settings
    }

    /// Signs in anonymously (if not already) and attaches `FirebaseSink`
    /// to `dependencies.telemetry` — a no-op, safely, on any composition
    /// whose telemetry is not `TelemetrySinkAttaching` (`.stub()`/demo)
    /// or whose bundle carries no plist (`configureIfNeeded()` never ran,
    /// so `FirebaseApp.app()` is still `nil`).
    ///
    /// `installId` IS the anonymous Auth uid, not a separately-generated
    /// one: `firebase/firestore.rules` checks `request.auth.uid` against
    /// the `devices/{installId}` path segment directly, so identity and
    /// the write path's own security boundary are the SAME value by
    /// construction — there is no second id that could ever drift from
    /// the uid the rules actually check.
    static func attachSink(to dependencies: AppDependencies, buildString: String, deviceString: String) async {
        guard shouldAttachSink() else {
            TelemetryDebugLog.log("attachSink skipped — running under XCTest or a -FireflyDebug* launch argument")
            return
        }
        guard let sinkAttaching = dependencies.telemetry as? any TelemetrySinkAttaching else { return }
        guard FirebaseApp.app() != nil else { return }
        guard let uid = await signInAnonymouslyIfNeeded() else { return }
        TelemetryDebugLog.log("anonymous sign-in succeeded — uid=\(uid)")
        let sessionId = UUID().uuidString
        let store = dependencies.store
        let sink = FirebaseSink(installId: uid, sessionId: sessionId, buildString: buildString,
                                 deviceString: deviceString,
                                 isSharingEnabled: {
                                     // A04 — "Share diagnostics" read
                                     // fresh on every send/flush, never
                                     // cached at attach time: a toggle
                                     // flipped mid-festival must take
                                     // effect on the very next event,
                                     // not the next launch. `store` is a
                                     // reference type (`SettingsStoring:
                                     // AnyObject`), so this closure
                                     // always reads whatever the real
                                     // instance currently holds.
                                     store.shareDiagnosticsEnabled
                                 })
        await sinkAttaching.addSink(sink)
    }

    /// `Auth.auth().currentUser` persists locally across launches
    /// (Firebase Auth's own documented behaviour) — this only actually
    /// calls out to the network the FIRST time this install ever runs.
    private static func signInAnonymouslyIfNeeded() async -> String? {
        if let existing = Auth.auth().currentUser { return existing.uid }
        do {
            let result = try await Auth.auth().signInAnonymously()
            return result.user.uid
        } catch {
            let message = "[FirebaseTelemetryBootstrap] anonymous sign-in failed: \(error) " +
                "— telemetry stays local-only this launch\n"
            FileHandle.standardError.write(Data(message.utf8))
            return nil
        }
    }

    /// `FirebaseApp.crashlytics()`-adjacent: sets the two custom keys
    /// A04 names that are facts about the WHOLE session rather than one
    /// event (`build`/`radio name hash`) — `link_state` moves per-event,
    /// inside `FirebaseSink.recordCrashlyticsBreadcrumb(_:)` itself.
    /// Called once, right after `attachSink`, with whatever this launch
    /// already knows; `radioNameHash` is `nil` until a BLE peripheral is
    /// actually discovered, in which case this is a no-op for that key
    /// until a later event updates it through the sink instead.
    static func setInitialCrashlyticsKeys(buildString: String) {
        guard FirebaseApp.app() != nil else { return }
        Crashlytics.crashlytics().setCustomValue(buildString, forKey: "build")
    }
    #else
    static func configureIfNeeded() {}
    static func attachSink(to dependencies: AppDependencies, buildString: String, deviceString: String) async {}
    static func setInitialCrashlyticsKeys(buildString: String) {}
    #endif
}
