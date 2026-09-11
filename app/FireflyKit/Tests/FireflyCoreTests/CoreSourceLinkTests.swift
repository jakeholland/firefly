//
//  CoreSourceLinkTests.swift — the anti-drift guard for the symlink farm.
//
//  `app/tools/link_core_sources.sh` links every firmware/core source and
//  header into the FireflyCore target rather than copying them. A copy
//  would be a fork with a grace period; a symlink farm cannot drift in
//  CONTENT, but it can go STALE — somebody adds
//  firmware/core/src/ff_new.c, the CMake builds pick it up via
//  sources.cmake, and the app silently doesn't have it.
//
//  This is the app-side twin of `ff_check_sources_complete()` in
//  firmware/CMakeLists.txt: it fails naming the missing file.
//
import XCTest

final class CoreSourceLinkTests: XCTestCase {
    /// .../app/FireflyKit/Tests/FireflyCoreTests/<this file>
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // FireflyCoreTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // FireflyKit
            .deletingLastPathComponent()   // app
            .deletingLastPathComponent()   // <repo root>
    }

    private func names(in dir: URL, suffix: String) throws -> Set<String> {
        let items = try FileManager.default.contentsOfDirectory(atPath: dir.path)
        return Set(items.filter { $0.hasSuffix(suffix) })
    }

    func testEveryCoreSourceIsLinked() throws {
        let real = try names(in: repoRoot.appending(path: "firmware/core/src"), suffix: ".c")
        let linked = try names(in: repoRoot.appending(path: "app/FireflyKit/Sources/FireflyCore/src"), suffix: ".c")
        XCTAssertFalse(real.isEmpty, "firmware/core/src has no .c files - wrong repo root?")
        XCTAssertEqual(real, linked,
                       "core source drift. Run app/tools/link_core_sources.sh. "
                       + "Missing from the app: \(real.subtracting(linked).sorted()); "
                       + "stale links: \(linked.subtracting(real).sorted())")
    }

    func testEveryCoreAndPlatformHeaderIsLinked() throws {
        var real = try names(in: repoRoot.appending(path: "firmware/core/include"), suffix: ".h")
        real.formUnion(try names(in: repoRoot.appending(path: "firmware/platform/include"), suffix: ".h"))
        let linked = try names(in: repoRoot.appending(path: "app/FireflyKit/Sources/FireflyCore/include"), suffix: ".h")
        XCTAssertEqual(real, linked,
                       "core/platform header drift. Run app/tools/link_core_sources.sh. "
                       + "Missing from the app: \(real.subtracting(linked).sorted()); "
                       + "stale links: \(linked.subtracting(real).sorted())")
    }

    /// They must be LINKS. A copy would compile just as well and would
    /// be exactly the failure this whole arrangement exists to prevent.
    /// Covers `src/` (`.c`) AND `include/` (`.h`) — a copied HEADER
    /// passes silently if only sources are checked, and the header farm
    /// is the half that also spans `firmware/platform`, so it is the
    /// more surprising place to have drift (A01_AC2, S8).
    func testLinkedSourcesAreSymlinksNotCopies() throws {
        try assertAllSymlinks(dir: repoRoot.appending(path: "app/FireflyKit/Sources/FireflyCore/src"), suffix: ".c")
        try assertAllSymlinks(dir: repoRoot.appending(path: "app/FireflyKit/Sources/FireflyCore/include"), suffix: ".h")
    }

    private func assertAllSymlinks(dir: URL, suffix: String) throws {
        let items = try FileManager.default.contentsOfDirectory(atPath: dir.path).filter { $0.hasSuffix(suffix) }
        for item in items {
            let attrs = try FileManager.default.attributesOfItem(atPath: dir.appending(path: item).path)
            XCTAssertEqual(attrs[.type] as? FileAttributeType, .typeSymbolicLink,
                           "\(item) is a COPY of a firmware/core or firmware/platform file, not a link to it")
        }
    }
}
