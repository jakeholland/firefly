//
//  ThemeTests.swift — the phone's palette is the puck's palette.
//
//  Not "looks similar": the same hex values, read out of
//  firmware/app/theme/ff_theme.h at test time. Two products that drift
//  apart on colour stop reading as one product, and nobody notices until
//  they are side by side at a festival.
//
import FireflyModel
import XCTest

final class ThemeTests: XCTestCase {
    private var themeHeader: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // FireflyModelTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // FireflyKit
            .deletingLastPathComponent()   // app
            .deletingLastPathComponent()   // <repo root>
            .appending(path: "firmware/app/theme/ff_theme.h")
    }

    /// Pull `#define <name> 0xRRGGBB` out of the header.
    private func defines() throws -> [String: UInt32] {
        let text = try String(contentsOf: themeHeader, encoding: .utf8)
        var out: [String: UInt32] = [:]
        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard line.hasPrefix("#define ") else { continue }
            let parts = line.dropFirst("#define ".count).split(separator: " ", omittingEmptySubsequences: true)
            guard parts.count >= 2, parts[1].hasPrefix("0x") else { continue }
            guard let value = UInt32(parts[1].dropFirst(2), radix: 16) else { continue }
            out[String(parts[0])] = value
        }
        return out
    }

    func testCoreTokensMatchTheHeader() throws {
        let d = try defines()
        XCTAssertFalse(d.isEmpty, "could not parse ff_theme.h - wrong path?")
        XCTAssertEqual(d["FF_THEME_COLOR_BG"], FireflyTheme.bg)
        XCTAssertEqual(d["FF_THEME_COLOR_SURFACE"], FireflyTheme.surface)
        XCTAssertEqual(d["FF_THEME_COLOR_AMBER"], FireflyTheme.amber)
        XCTAssertEqual(d["FF_THEME_COLOR_STALE_AMBER"], FireflyTheme.staleAmber)
        XCTAssertEqual(d["FF_THEME_COLOR_LIVE_GREEN"], FireflyTheme.liveGreen)
        XCTAssertEqual(d["FF_THEME_COLOR_MUTED"], FireflyTheme.muted)
        XCTAssertEqual(d["FF_THEME_COLOR_INK"], FireflyTheme.ink)
    }

    func testCrewPaletteMatchesTheHeaderInOrder() throws {
        let d = try defines()
        let expected = [
            "FF_THEME_CREW_PINK", "FF_THEME_CREW_TEAL", "FF_THEME_CREW_VIOLET", "FF_THEME_CREW_GREEN",
            "FF_THEME_CREW_ORANGE", "FF_THEME_CREW_GOLD", "FF_THEME_CREW_BLUE", "FF_THEME_CREW_MAGENTA",
        ].map { d[$0] }
        XCTAssertEqual(expected, FireflyTheme.crew.map { Optional($0) })
    }

    /// `color_idx` comes off the wire; it must wrap, never trap.
    func testCrewColorWrapsForAnyIndex() {
        XCTAssertEqual(FireflyTheme.crewColor(index: 0), FireflyTheme.crew[0])
        XCTAssertEqual(FireflyTheme.crewColor(index: 8), FireflyTheme.crew[0])
        XCTAssertEqual(FireflyTheme.crewColor(index: -1), FireflyTheme.crew[7])
        XCTAssertEqual(FireflyTheme.crewColor(index: 255), FireflyTheme.crew[255 % 8])
    }
}
