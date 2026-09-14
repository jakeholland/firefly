//
//  FireflyDebugNotifyLaunch.swift — `-FireflyDebugNotify
//  thread|find|flare|rally`, a DEBUG-only launch argument that schedules
//  ONE real local notification a few seconds after launch so a UI test
//  can background (or terminate) the app and then actually TAP it.
//
//  Why this exists: `xcrun simctl` cannot tap a notification, and the
//  notification-tap path (A03 §3.11.3 —
//  `UNUserNotificationCenterDelegate.userNotificationCenter(_:didReceive:)`
//  -> `DeepLinkRouter` -> `RootView`) is only reachable through a real
//  tap. An XCUITest can drive SpringBoard's banner; it cannot invent the
//  notification. This is the missing half.
//
//  Deliberately scoped/gated exactly like its two siblings in this
//  directory (`FireflyAutoConnectLaunch`, `FireflyDebugStartDestination
//  Launch`): a pure function over an injectable arguments array, so
//  parsing is testable with no process launch, living in the APP target,
//  and `#if DEBUG` so a Release/TestFlight/App Store build cannot have a
//  notification conjured by a stray command-line argument.
//
//  The plans themselves come from `NotificationPlan.plan(for:)` — the
//  SAME pure function the live inbound path uses — so a repro
//  notification carries the real category, thread identifier,
//  interruption level and `userInfo` deep link. A hand-rolled stand-in
//  would prove nothing about the path that crashed.
//
import FireflyModel
import Foundation

enum FireflyDebugNotifyLaunch {
    /// The demo node id these repro notifications claim to be from —
    /// `DemoCrew.taylor`'s own value, so a `-FireflyDemo` run's deep
    /// link names somebody the seeded roster actually holds. Outside
    /// demo mode it names nobody, which is itself a case worth being
    /// able to tap (`CoreRadarComputing.select(nodeID:)` is documented
    /// as a no-op for a node the roster does not hold).
    static let reproNodeID: UInt32 = 0x0A0A_0001

    /// `-FireflyDebugNotify <kind>` — one of `thread`, `find`/`flare`,
    /// `rally`. `nil` on every ordinary launch, and unconditionally
    /// `nil` in a non-`DEBUG` build.
    static func requestedKind(arguments: [String] = CommandLine.arguments) -> String? {
        #if DEBUG
        guard let index = arguments.firstIndex(of: "-FireflyDebugNotify"),
              index + 1 < arguments.count else { return nil }
        return arguments[index + 1]
        #else
        nil
        #endif
    }

    /// The plan a kind name asks for, or `nil` for a name this seam does
    /// not own — never a silently substituted default, so a typo in a
    /// test's launch arguments fails loudly (no notification at all)
    /// instead of quietly testing the wrong category.
    static func plan(for kind: String, nodeID: UInt32 = reproNodeID) -> NotificationPlan? {
        switch kind.lowercased() {
        case "thread", "dm", "message":
            return NotificationPlan.plan(for: .directMessage(
                from: nodeID, senderName: "Taylor", packetID: 1, text: "Where are you?"))
        case "crew":
            return NotificationPlan.plan(for: .crewMessage(
                from: nodeID, senderName: "Taylor", packetID: 2, text: "At the main stage."))
        case "find", "flare":
            return NotificationPlan.plan(for: .flare(
                from: nodeID, senderName: "Taylor", packetID: 3))
        case "rally":
            return NotificationPlan.plan(for: .rally(
                from: nodeID, senderName: "Taylor", packetID: 4,
                text: "MY SPOT — 210 m NE of you", isBroadcast: true))
        default:
            return nil
        }
    }
}
