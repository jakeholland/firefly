//
//  PicksCodecParseInputTests.swift — `PicksCodec.parsePicksInput` run
//  against settimes' OWN `parsePicksInput` on the same inputs.
//
//  Every expectation below is the literal value settimes'
//  `src/lib/picks.ts` returns for that exact string, captured by
//  bundling that module with esbuild and running it under node — not
//  by reasoning about what the regex "should" do. That matters
//  because the two implementations reach the same answers by
//  different routes (JavaScript's throwing `new URL` + `URLSearchParams`
//  vs `Foundation.URL`'s far more permissive parse + `URLComponents`),
//  so agreement is a measurement, not a restatement: a paste that the
//  website accepts must import on the phone, and one the website
//  rejects must not silently turn into picks here.
//
import FireflyModel
import XCTest

final class PicksCodecParseInputTests: XCTestCase {
    /// (input, what settimes' `parsePicksInput` returns for it).
    private static let settimesBehaviour: [(input: String, expected: String?)] = [
        ("", nil),
        ("   ", nil),
        ("abc123", "abc123"),
        ("abc.def", "abc.def"),
        ("ABC.DEF", "ABC.DEF"),
        ("abc..def", nil),
        (".abc", nil),
        ("abc.", nil),
        ("a b", nil),
        ("just some words", nil),
        ("picks=abc.def", "abc.def"),
        ("?picks=abc.def", "abc.def"),
        ("?picks=abc.def&x=1", "abc.def"),
        ("x=1&picks=abc.def", "abc.def"),
        ("https://settimes.kandiwooks.com/lost-lands/2026/fri?picks=abc.def", "abc.def"),
        ("https://settimes.kandiwooks.com/lost-lands/2026/fri", nil),
        ("https://settimes.kandiwooks.com/lost-lands/2026/fri?picks=", nil),
        ("https://x.test/a?now=1&picks=abc.def&z=1", "abc.def"),
        ("https://x.test/a?picks=abc%2Edef", "abc.def"),
        // A fragment is not a query: neither side reads it.
        ("https://x.test/a#picks=abc.def", nil),
        // JS's `new URL` accepts a scheme with no host; Foundation's
        // `host != nil` guard rejects it, and the bare-`picks=` scan
        // below catches it instead — same answer either way.
        ("mailto:a@b.com?picks=abc.def", "abc.def"),
        ("settimes.kandiwooks.com/lost-lands/2026/fri?picks=abc.def", "abc.def"),
        ("//settimes.kandiwooks.com/x?picks=abc.def", "abc.def"),
        ("  https://x.test/a?picks=abc.def  ", "abc.def"),
        // Both sides match the param name case-SENSITIVELY.
        ("PICKS=abc.def", nil),
        ("Picks=abc.def", nil),
        ("https://x.test/a?picks=abc.def&picks=zzz", "abc.def"),
        ("http://x.test?picks=abc.def", "abc.def"),
        ("abc-def", nil),
        ("abc_def", nil),
        ("abc def", nil),
        ("abc.def.ghi", "abc.def.ghi"),
        ("1230q9o.1jksh97", "1230q9o.1jksh97"),
        ("https://x.test/a?PICKS=abc", nil),
        ("foo:bar", nil),
        ("urn:isbn:123", nil),
        // A junk param value is returned as-is by both, and then
        // fails to resolve to any set in `decodePicks`.
        ("https://x.test/a?picks=abc def", "abc def"),
        ("café", nil),
        ("ünicode.abc", nil),
    ]

    func testMatchesSettimesParsePicksInputOnEveryInput() {
        for row in Self.settimesBehaviour {
            XCTAssertEqual(PicksCodec.parsePicksInput(row.input), row.expected,
                           "parsePicksInput(\(row.input.debugDescription))")
        }
    }

    /// The ONE known divergence, pinned here rather than left to be
    /// discovered: JavaScript's `URLSearchParams` decodes "+" in a
    /// query value to a space (the historical form-encoding rule),
    /// `URLComponents` does not. Harmless in practice — a real picks
    /// value is base36 and dots, so neither "abc+def" nor "abc def"
    /// resolves to a set — but asserted so a future change to this
    /// function has to notice it.
    func testPlusInAQueryValueIsTheOneDocumentedDivergenceFromSettimes() {
        // settimes returns "abc def" here.
        XCTAssertEqual(PicksCodec.parsePicksInput("https://x.test/a?picks=abc+def"), "abc+def")
    }
}
