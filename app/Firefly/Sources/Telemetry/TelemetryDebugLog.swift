//
//  TelemetryDebugLog.swift — A04 review gap-close: the one place this
//  app confirms, from stderr, that the Firebase half of telemetry
//  actually did something — Anonymous Auth succeeding and a Firestore
//  batch being ACKNOWLEDGED (or failing), the two facts the owner asked
//  to be able to read back from a live field-test run without opening
//  the Firebase console. `[Telemetry]` prefix, DEBUG only — this is a
//  developer/owner verification aid, never a Release-build log line
//  (Crashlytics/Firestore are the Release-build channel; this is stderr
//  for whoever is watching a `open --stderr LOG` capture).
//
import Foundation

enum TelemetryDebugLog {
    static func log(_ message: @autoclosure () -> String) {
        #if DEBUG
        FileHandle.standardError.write(Data("[Telemetry] \(message())\n".utf8))
        #endif
    }
}
