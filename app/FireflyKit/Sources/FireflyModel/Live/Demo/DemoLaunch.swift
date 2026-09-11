//
//  DemoLaunch.swift — how demo mode gets turned on, and how a
//  screenshot/video script tells it which screen to open. Pure
//  functions over an injectable arguments/environment so a test can
//  assert the parsing without touching the real process's
//  `CommandLine`/`ProcessInfo` (S20's `--demo` flag, ported to the
//  app's own launch surface).
//
import Foundation

public enum DemoLaunch {
    /// `-FireflyDemo` on the command line, or `FIREFLY_DEMO=1` in the
    /// environment — either is enough. `AppDependencies.current()`
    /// only ever consults this from inside `#if targetEnvironment
    /// (simulator)` (that file's own comment): a real device ignores
    /// both, on purpose, so nothing can turn a bench Heltec into a
    /// fictional festival by accident.
    public static func isRequested(arguments: [String] = CommandLine.arguments,
                                    environment: [String: String] = ProcessInfo.processInfo.environment) -> Bool {
        arguments.contains("-FireflyDemo") || environment["FIREFLY_DEMO"] == "1"
    }

    /// `-FireflyDemoScreen <name>` — demo-only, and simpler than UI
    /// automation for the M1 screenshot set (S13's screenshot path):
    /// the app reads this once at launch and opens straight on the
    /// named screen instead of a script driving taps through a
    /// `TabView`/`NavigationStack` it doesn't otherwise have a seam
    /// into. Recognised names live with `RootView`, which is the only
    /// place that has to agree with a screenshot script about them.
    public static func requestedScreen(arguments: [String] = CommandLine.arguments) -> String? {
        guard let index = arguments.firstIndex(of: "-FireflyDemoScreen"), index + 1 < arguments.count else {
            return nil
        }
        return arguments[index + 1]
    }
}
