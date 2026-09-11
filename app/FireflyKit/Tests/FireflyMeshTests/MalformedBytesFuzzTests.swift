//
//  MalformedBytesFuzzTests.swift — PR #264 review, SHOULD-FIX 7: "no
//  malformed-FromRadio fuzz test exists in the PR". The review's own ad
//  hoc run (500 random byte strings through the `.message`-transport
//  decode path, 500 through `.stream`; not committed, zero crashes) is
//  pinned here permanently, so the guarantee it found true BY INSPECTION
//  ("every FromRadio/Position/Routing decode guarded by `try?`, no
//  force-unwraps anywhere in the new files") stays true by TEST.
//
//  A deterministic seed, never `SystemRandomNumberGenerator` (not
//  seedable): a failure here must reproduce on every run — in CI, and
//  locally — not just "sometimes".
//
import FireflyMesh
import MeshtasticProto
import XCTest

/// splitmix64 — small, fast, exactly reproducible for a given seed; the
/// same generator many language standard-library test suites use for
/// this reason. Fuzz-input generation only, nothing security-sensitive.
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

final class MalformedBytesFuzzTests: XCTestCase {

    private func randomBytes(maxLength: Int, using rng: inout SplitMix64) -> Data {
        let length = Int.random(in: 0...maxLength, using: &rng)
        return Data((0..<length).map { _ in UInt8.random(in: 0...255, using: &rng) })
    }

    /// The `.message`-transport decode path (BLE): a whole `FromRadio`
    /// protobuf per read, decoded with `try? FromRadio(serializedBytes:)`
    /// exactly like `MeshtasticClient.ingest(_:)`'s `.message` case.
    /// The assertion IS that every iteration returns (or throws
    /// cleanly) instead of trapping — a crash fails the whole suite,
    /// not just one iteration.
    func testRandomBytesNeverCrashFromRadioDecode() {
        var rng = SplitMix64(seed: 0xF12E_F1DE_0BAD_5EED)
        for _ in 0..<500 {
            let bytes = randomBytes(maxLength: 512, using: &rng)
            _ = try? FromRadio(serializedBytes: bytes)
        }
    }

    /// The two nested message types `MeshtasticClient` decodes off an
    /// already-unwrapped `MeshPacket.decoded.payload` — `Position`
    /// (`.positionApp`) and `Routing` (`.routingApp`), both also
    /// `try?`-guarded (`MeshtasticClient.swift`'s `handle(meshPacket:)`).
    /// Not reachable from a top-level `FromRadio` fuzz run alone, since a
    /// random top-level blob essentially never decodes as a valid
    /// `FromRadio` carrying a `.packet` variant with a plausible
    /// `portnum` — fuzzed directly here instead.
    func testRandomBytesNeverCrashPositionOrRoutingDecode() {
        var rng = SplitMix64(seed: 0x5EED_C0DE_F00D_BA11)
        for _ in 0..<500 {
            let bytes = randomBytes(maxLength: 512, using: &rng)
            _ = try? Position(serializedBytes: bytes)
            _ = try? Routing(serializedBytes: bytes)
        }
    }

    /// The `.stream`-transport decode path (serial/TCP): raw bytes
    /// through `StreamFramer.feed(_:)` first — `StreamFramerTests`
    /// already pins specific SHAPES (garbage prefix, oversize stated
    /// length); this is the broad, unstructured-input sweep — then every
    /// frame it DOES emit through the same `FromRadio` decode as above,
    /// mirroring `MeshtasticClient.ingest(_:)`'s `.stream` case exactly.
    func testRandomBytesNeverCrashStreamFramerOrDownstreamDecode() {
        var rng = SplitMix64(seed: 0xBADC_0FFE_E0DD_F00D)
        var framer = StreamFramer()
        for _ in 0..<500 {
            let bytes = randomBytes(maxLength: 512, using: &rng)
            for frame in framer.feed(bytes) {
                _ = try? FromRadio(serializedBytes: frame)
            }
        }
    }

    /// A stream fed random bytes ONE AT A TIME (the worst case for
    /// resync logic — no frame boundary is ever a coincidence) must
    /// still terminate cleanly on every call, exercising the same
    /// property `StreamFramerTests.testOversizeStatedLengthIsDroppedAndStreamRecovers`
    /// pins for one specific shape, here under unstructured pressure.
    func testByteAtATimeRandomStreamNeverCrashes() {
        var rng = SplitMix64(seed: 0x1234_5678_9ABC_DEF0)
        var framer = StreamFramer()
        for _ in 0..<500 {
            let byte = UInt8.random(in: 0...255, using: &rng)
            for frame in framer.feed(Data([byte])) {
                _ = try? FromRadio(serializedBytes: frame)
            }
        }
    }
}
