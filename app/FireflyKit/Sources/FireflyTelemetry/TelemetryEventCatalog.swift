//
//  TelemetryEventCatalog.swift — A04 (docs/specs/A04-telemetry.md): the
//  ONE place every event name and attribute key is spelled. Every
//  emission call site (`BLETransport`, `MeshtasticClient`, `AppGraph`,
//  `CrewController`, `NotificationTapRouter`/`UNNotificationSending`)
//  references these constants rather than a literal string, and
//  `TelemetryEventCatalogTests`/the wiring-guard tests pin the exact
//  spelling against `docs/specs/A04-telemetry.md`'s own table — so the
//  doc and the code cannot quietly drift apart.
//
//  Namespacing follows the dotted-domain convention the spec itself
//  uses (`ble.*`, `app.*`, `notif.*`, `admin.*`/`crew.*`, `radio.*`,
//  `gps.*`, plus the bare `error`), never a flat list.
//
public enum TelemetryEventName {
    // MARK: - BLE / connectivity

    public static let bleScanStart = "ble.scan.start"
    public static let bleScanStop = "ble.scan.stop"
    public static let bleDiscovered = "ble.discovered"
    public static let bleConnectAttempt = "ble.connect.attempt"
    public static let bleConnected = "ble.connected"
    public static let bleHandshakePhase = "ble.handshake.phase"
    public static let bleReady = "ble.ready"
    public static let bleDisconnected = "ble.disconnected"
    public static let bleLadderScheduled = "ble.ladder.scheduled"
    public static let bleLadderFired = "ble.ladder.fired"
    public static let bleRestore = "ble.restore"
    public static let blePower = "ble.power"

    // MARK: - App lifecycle

    public static let appForeground = "app.foreground"
    public static let appBackground = "app.background"
    public static let appLaunch = "app.launch"
    public static let appTerminate = "app.terminate"

    // MARK: - Notifications

    public static let notifPosted = "notif.posted"
    public static let notifTapped = "notif.tapped"
    public static let notifAuthorization = "notif.authorization"

    // MARK: - Crew / admin

    public static let adminWrite = "admin.write"
    public static let crewJoin = "crew.join"
    public static let crewLeave = "crew.leave"
    public static let crewStart = "crew.start"
    public static let crewMemberSeen = "crew.member.seen"
    public static let crewMemberLost = "crew.member.lost"

    // MARK: - Radio

    /// Every 5 minutes while connected — see `AppGraph`'s own
    /// `observeRadioSnapshot()`.
    public static let radioSnapshot = "radio.snapshot"

    // MARK: - Position (buckets only — NEVER raw coordinates; see
    // `TelemetryAttributeAllowlist`)

    public static let gpsFix = "gps.fix"
    public static let gpsUplink = "gps.uplink"

    // MARK: - Errors

    /// One event name for every caught error on a connectivity path —
    /// `domain`/`code`/`where` (below) is what distinguishes them, not
    /// a proliferation of event names.
    public static let error = "error"
}

public enum TelemetryAttributeKey {
    // Shared
    public static let outcome = "outcome"
    public static let ms = "ms"

    // ble.discovered
    public static let name = "name"
    public static let rssi = "rssi"

    // ble.connect.attempt
    public static let trigger = "trigger"
    public static let attempt = "attempt"

    // ble.handshake.phase
    public static let phase = "phase"

    // ble.ready
    public static let msSinceAttempt = "ms_since_attempt"

    // ble.disconnected
    public static let reason = "reason"
    public static let sessionS = "session_s"
    public static let expected = "expected"

    // ble.ladder.scheduled
    public static let delayS = "delay_s"
    public static let step = "step"

    // ble.restore
    public static let action = "action"

    // ble.power
    public static let state = "state"

    // app.launch
    public static let build = "build"
    public static let device = "device"
    public static let os = "os"

    // notif.posted / notif.tapped
    public static let kind = "kind"

    // notif.authorization
    public static let status = "status"

    // admin.write
    public static let outcomeMs = "ms" // alias kept for readability at call sites; same as `ms`

    // crew.member.seen / crew.member.lost
    public static let idHash = "id_hash"
    public static let ageS = "age_s"

    // radio.snapshot
    public static let battPct = "batt_pct"
    public static let rssiLast = "rssi_last"
    public static let nodeCount = "node_count"

    // gps.fix
    public static let accuracyBucket = "accuracy_bucket"
    public static let source = "source"

    // error
    public static let domain = "domain"
    /// NOT `"code"` — deliberately. `TelemetryAttributeAllowlist
    /// .forbiddenKeys` forbids exactly that key (it is the crew CODE's
    /// own would-be key, `code`/`crew_code`/`join_code`/`invite_code`),
    /// so an `error` event that used the plain word would have its own
    /// diagnostic code silently stripped by the SAME guard that exists
    /// to protect it — found wiring the first real `error` call site
    /// (this key previously existed in the catalogue with no call site
    /// to expose the collision). `error_code` says the same thing and
    /// collides with nothing.
    public static let errorCode = "error_code"
    public static let whereKey = "where"
}

/// `ble.connect.attempt`'s `trigger` values (§A04's own catalogue).
public enum TelemetryTrigger: String, Sendable {
    case launch, auto, manual, ladder, restore
}
