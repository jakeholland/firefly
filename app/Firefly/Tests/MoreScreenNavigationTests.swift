//
//  MoreScreenNavigationTests.swift — `MoreScreen`'s own push-state rule
//  (docs/specs/A01-companion-app.md, "Navigation"). Run via `xcodebuild
//  test -only-testing:FireflyAppTests` (see project.yml) — same reason
//  as `ConnectSettingsViewModelTests`' own header comment: this is
//  app-target Swift, not part of the FireflyKit SwiftPM package
//  `swift test` covers.
//
//  Owner note (build 304, item 2): "tapping the More tab while Connect
//  is already pushed pushes another Connect onto the stack." The old
//  `PushRequest`/fresh-`UUID` wrapper always wrote a distinct value on
//  every `open(_:)` call regardless of what was already showing, which
//  combined badly with `autoOpen` never being consumed (this file does
//  not re-test that half — it lives entirely in `RootView`/`MoreScreen`
//  state this target's own `project.yml` comment says to keep out of
//  here). `MoreScreenNavigation.pushed(_:onto:)` is the fix for the
//  OTHER half: the actual push rule, factored out into its own
//  SwiftUI-free file specifically so it has a real test here instead of
//  only ever being exercised through XCUITest.
//
import XCTest

final class MoreScreenNavigationTests: XCTestCase {
    func testPushingANewRowReplacesWhateverWasThere() {
        XCTAssertEqual(MoreScreenNavigation.pushed(.connect, onto: []), [.connect])
        XCTAssertEqual(MoreScreenNavigation.pushed(.settings, onto: [.connect]), [.settings],
                        "a different row REPLACES the one pushed, never stacks on top of it")
    }

    /// The exact bug: re-"opening" the row already on top must be a
    /// complete no-op on `path` — not even a same-value rewrite — since
    /// `MoreScreen`'s `.task`/`.onChange` can call `open(_:)` again for
    /// a request that was already satisfied (a reappearance after the
    /// tab-reselect pop, `autoOpen` re-delivered before it is cleared,
    /// a macOS sidebar re-click on the row already showing).
    func testPushingTheRowAlreadyOnTopIsANoOp() {
        XCTAssertEqual(MoreScreenNavigation.pushed(.connect, onto: [.connect]), [.connect])
        XCTAssertEqual(MoreScreenNavigation.pushed(.settings, onto: [.settings]), [.settings])
    }

    func testPushingOntoAnEmptyPathAlwaysPushes() {
        for row: MoreScreenRow in [.connect, .settings, .system] {
            XCTAssertEqual(MoreScreenNavigation.pushed(row, onto: []), [row])
        }
    }

    /// Never more than one level deep — Connect/Settings/System are
    /// leaf destinations under More, so a push never grows `path` past
    /// a single element even across several different rows in a row.
    func testPathNeverGrowsPastOneElement() {
        var path: [MoreScreenRow] = []
        path = MoreScreenNavigation.pushed(.connect, onto: path)
        path = MoreScreenNavigation.pushed(.settings, onto: path)
        path = MoreScreenNavigation.pushed(.system, onto: path)
        XCTAssertEqual(path.count, 1)
        XCTAssertEqual(path, [.system])
    }
}
