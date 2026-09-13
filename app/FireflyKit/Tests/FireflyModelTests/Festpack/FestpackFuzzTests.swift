//
//  FestpackFuzzTests.swift — the festpack loader's own adversarial
//  sweep (hardening QA pass): a few hundred mutated and hand-built
//  packs through `FestpackParser.parse`, asserting it always RETURNS —
//  a `.success` or a named `.failure`, never a trap.
//
//  `MalformedBytesFuzzTests` (FireflyMeshTests) does this for the
//  radio's own `FromRadio` frames. This is the other untrusted input
//  the app ingests: a festpack.json fetched over the network or read
//  back off a disk cache written by a previous version. Both are
//  festival-critical — a phone that crashes on a malformed pack in a
//  field with no cell service cannot be recovered by a re-download.
//
//  Deterministic seed, never `SystemRandomNumberGenerator`: a failure
//  here must reproduce on every run, in CI and locally, the same rule
//  `MalformedBytesFuzzTests` states for itself.
//
import FireflyModel
import XCTest

/// splitmix64 — the same generator, for the same reason, as
/// `MalformedBytesFuzzTests`' own copy (that file's doc comment). Test
/// input generation only.
private struct SplitMix64: RandomNumberGenerator {
    private var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
}

final class FestpackFuzzTests: XCTestCase {

    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Festpack
            .deletingLastPathComponent() // FireflyModelTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // FireflyKit
            .deletingLastPathComponent() // app
            .deletingLastPathComponent() // <repo root>
    }

    private func realPack() throws -> Data {
        try Data(contentsOf: repoRoot.appending(
            path: "firmware/festpack/tests/fixtures/lost-lands-2026.festpack.json"))
    }

    /// The assertion IS that `parse` returns at all. Every case below
    /// runs it for effect; a trap fails the whole suite.
    @discardableResult
    private func parse(_ data: Data) -> Result<Festpack, FestpackParseError> {
        FestpackParser.parse(data)
    }

    // MARK: - Structured edge cases (the shapes a real pack can honestly take)

    /// Empty stages, zero sets, duplicate ids, zero-length names and
    /// non-ASCII text are all things a real fest-almanac pack can
    /// legitimately contain (or a half-written one can contain by
    /// accident). None of them may trap, and a pack that parses must
    /// not then trap on the derived values every screen reads.
    func testStructuralEdgeCasePacksParseOrFailCleanly() throws {
        let packs: [String] = [
            // no stages, no sets at all
            #"{"festpack":"0.1","festival":{"name":"Empty","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[],"schedule":[]}"#,
            // stages but zero sets
            #"{"festpack":"0.1","festival":{"name":"NoSets","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[{"id":"a","name":"A"}],"schedule":[]}"#,
            // duplicate stage ids
            #"{"festpack":"0.1","festival":{"name":"Dupes","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[{"id":"a","name":"A"},{"id":"a","name":"A again"}],"schedule":[{"stage":"a","artist":"X","day":"2026-09-18","start":"20:00"}]}"#,
            // zero-length names everywhere
            #"{"festpack":"0.1","festival":{"name":"","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[{"id":"","name":""}],"schedule":[{"stage":"","artist":"","day":"2026-09-18","start":"20:00"}]}"#,
            // non-ASCII / emoji / RTL
            #"{"festpack":"0.1","festival":{"name":"Löst Länds 🌲","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[{"id":"wüb","name":"WÜB 🔊"}],"schedule":[{"stage":"wüb","artist":"مهرجان","artist_note":"日本語のノート","day":"2026-09-18","start":"20:00"}]}"#,
            // a set naming a stage that does not exist
            #"{"festpack":"0.1","festival":{"name":"Orphan","year":2026,"start":"2026-09-18","end":"2026-09-20"},"stages":[{"id":"a","name":"A"}],"schedule":[{"stage":"nope","artist":"X","day":"2026-09-18","start":"20:00"}]}"#,
            // end before start
            #"{"festpack":"0.1","festival":{"name":"Backwards","year":2026,"start":"2026-09-20","end":"2026-09-18"},"stages":[],"schedule":[]}"#,
            // absurd year
            #"{"festpack":"0.1","festival":{"name":"Far","year":999999,"start":"2026-09-18","end":"2026-09-20"},"stages":[],"schedule":[]}"#,
            // negative / nonsense offsets and coordinates
            #"{"festpack":"0.1","festival":{"name":"Bad geo","year":2026,"start":"2026-09-18","end":"2026-09-20","utc_offset_min":-99999,"venue":{"lat":9999,"lon":-9999}},"stages":[],"schedule":[]}"#,
            // null-heavy
            #"{"festpack":"0.1","festival":{"name":null,"year":null,"start":null,"end":null},"stages":null,"schedule":null}"#,
            // empty object / empty array / bare scalars
            "{}", "[]", "null", "0", #""a string""#, "",
        ]

        var parsed = 0
        for json in packs {
            // Must return, never trap.
            if case .success(let pack) = parse(Data(json.utf8)) {
                parsed += 1
                // Every derived value a screen reads must also be total
                // on a degenerate pack — these are the accessors
                // LineupViewModel/MapViewModel call unconditionally.
                XCTAssertGreaterThanOrEqual(pack.dayCount, 1, "dayCount must never be 0 or negative for \(json.prefix(40))")
                _ = pack.timeZone
                _ = pack.date(forDayOfYear: pack.startDayOfYear)
                _ = pack.date(forDayOfYear: Int.min / 2)
                _ = pack.date(forDayOfYear: Int.max / 2)
                _ = pack.dayIndex(forDayOfYear: Int.min / 2)
                _ = pack.dayIndex(forDayOfYear: Int.max / 2)
                _ = pack.stage(withID: nil)
                _ = pack.stage(withID: "nope")
                for set in pack.sets { _ = pack.stage(withID: set.stageID) }
            }
        }
        // Guards this test against its own proxy failure: a version
        // key typo (or a schema bump) that made every pack above fail
        // to parse would leave the loop asserting nothing at all about
        // the derived accessors it exists to exercise. Measured, not
        // assumed — 10 of the 16 inputs above are structurally valid
        // packs, the other 6 are deliberately unparseable.
        XCTAssertGreaterThanOrEqual(parsed, 10,
            "too few edge-case packs parsed — this test would be asserting nothing about derived values")
    }

    // MARK: - Mutation fuzz over the real pack

    /// 300 single-byte mutations of the REAL Lost Lands pack. Byte
    /// flips in a structurally valid document are what a corrupted disk
    /// cache or a truncated download actually looks like — far more
    /// likely to reach deep into `fp_parse` than uniformly random bytes,
    /// which almost never get past the first token.
    func testMutatedRealPackNeverTraps() throws {
        let original = try realPack()
        var rng = SplitMix64(seed: 0xFE57_9ACC_5EED_0001)
        for _ in 0..<300 {
            var bytes = original
            let mutations = Int.random(in: 1...4, using: &rng)
            for _ in 0..<mutations {
                let index = Int.random(in: 0..<bytes.count, using: &rng)
                bytes[bytes.startIndex + index] = UInt8.random(in: 0...255, using: &rng)
            }
            parse(bytes)
        }
    }

    /// Truncation at every scale: a download cut off mid-flight, or a
    /// cache file that was being written when the phone died.
    func testTruncatedRealPackNeverTraps() throws {
        let original = try realPack()
        var rng = SplitMix64(seed: 0x7204_CA7E_5EED_0002)
        for _ in 0..<200 {
            let length = Int.random(in: 0...original.count, using: &rng)
            parse(original.prefix(length))
        }
    }

    /// Random bytes, the unstructured sweep — the same shape
    /// `MalformedBytesFuzzTests` runs for `FromRadio`.
    func testRandomBytesNeverTrap() {
        var rng = SplitMix64(seed: 0x4A4D_0FE5_7000_0003)
        for _ in 0..<300 {
            let length = Int.random(in: 0...4096, using: &rng)
            parse(Data((0..<length).map { _ in UInt8.random(in: 0...255, using: &rng) }))
        }
    }
}
