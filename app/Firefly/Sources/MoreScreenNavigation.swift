//
//  MoreScreenNavigation.swift — the pure push-state logic behind
//  `MoreScreen`'s own `Row`/`path` (docs/specs/A01-companion-app.md,
//  "Navigation"; see `MoreScreen.swift`'s own header comment for the
//  "tapping More re-pushes Connect" bug this logic exists to fix).
//
//  Standalone top-level types, NOT nested in/extending `MoreScreen`
//  itself — `MoreScreen.swift` imports SwiftUI and, transitively
//  through `ConnectScreen`/`SettingsScreen`/`DiagnosticsScreen`, most of
//  this app's UI, and `FireflyAppTests`' own `project.yml` comment is
//  explicit that this target "never has to compile the SwiftUI screens
//  themselves... or their Theme+SwiftUI dependency." Putting this logic
//  in its own SwiftUI-free file (and aliasing it back as `MoreScreen
//  .Row`/`MoreScreen.pushed(_:onto:)` in `MoreScreen.swift`) is what
//  lets THIS file join that target's source list without pulling
//  `MoreScreen.swift` in behind it.
//
enum MoreScreenRow: Hashable {
    /// A02 §5 — the Crew page, appended (never inserted) to this list so
    /// every existing `MoreScreenRow` switch stays exhaustive-by-append
    /// rather than needing a reorder.
    case crew, connect, settings, system
}

enum MoreScreenNavigation {
    /// The push rule itself: replace whatever is pushed with `row`,
    /// UNLESS `row` is already the one on top — in which case leave
    /// `path` completely untouched (never a duplicate entry, and never
    /// a redundant write that could retrigger an observer for no
    /// visible change). This is the fix for "tapping More while Connect
    /// is already pushed pushes another Connect": the old
    /// `PushRequest`/fresh-`UUID` scheme always wrote a distinct value
    /// regardless of what was already showing.
    static func pushed(_ row: MoreScreenRow, onto path: [MoreScreenRow]) -> [MoreScreenRow] {
        path.last == row ? path : [row]
    }
}
