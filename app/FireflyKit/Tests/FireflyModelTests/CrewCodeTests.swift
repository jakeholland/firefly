//
//  CrewCodeTests.swift — Slice A, AC1–AC5: `CrewCode`/`CrewKey`/
//  `CrewChannel`/`CrewLink` against the shared fixture
//  (`docs/specs/fixtures/A02-crew-codes.json`, `docs/specs/
//  A02-crew-join.md` §1.9). Every assertion below is byte-exact against
//  that file — nothing here is a re-typed copy of its numbers.
//
import FireflyMesh
import Foundation
import MeshtasticProto
import XCTest
@testable import FireflyModel

/// Loads and decodes the shared fixture once per test process.
private enum Fixture {
    struct Vector: Decodable {
        let input: String
        let note: String
        let canonical: String
        let psk_hex: String
        let psk_base64: String
        let channelset_hex: String
        /// §1.8 amendment (2026-09-14): the "Copy Meshtastic link"
        /// export's `ChannelSet` bytes — `channelset_hex` PLUS the
        /// fixture's `export_lora_config`. `channelset_hex` itself is
        /// unchanged: it is still exactly what Firefly's own join path
        /// writes (§1.5/§1.7), with no `lora_config`.
        let export_channelset_hex: String
        let meshtastic_url: String
        let deep_link: String?
    }
    struct Rejection: Decodable {
        let input: String
        let why: String
    }
    /// §1.8 amendment's canonical `lora_config` — the values the fixture
    /// says an exporting radio's CURRENT LoRa config holds, used to
    /// regenerate `export_channelset_hex`/`meshtastic_url` for every
    /// vector.
    struct ExportLoraConfig: Decodable {
        let use_preset: Bool
        let modem_preset: String
        let region: String
        let hop_limit: UInt32
        let tx_enabled: Bool
    }
    struct File: Decodable {
        let export_lora_config: ExportLoraConfig
        let vectors: [Vector]
        let rejections: [Rejection]
    }

    /// `#filePath` for THIS file is
    /// `.../app/FireflyKit/Tests/FireflyModelTests/CrewCodeTests.swift`;
    /// the fixture lives at `docs/specs/fixtures/A02-crew-codes.json`,
    /// five directories up from `Tests/FireflyModelTests`. Walking up
    /// from `#filePath` (rather than assuming a working directory) keeps
    /// this test correct under both `swift test` (run from
    /// `app/FireflyKit`) and Xcode's own test runner (a different CWD
    /// entirely).
    static let file: File = {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.appendPathComponent("docs/specs/fixtures/A02-crew-codes.json")
        let data = try! Data(contentsOf: url) // swiftlint:disable:this force_try
        return try! JSONDecoder().decode(File.self, from: data) // swiftlint:disable:this force_try
    }()
}

/// `Fixture.ExportLoraConfig`, translated to the real proto type —
/// `region`/`modem_preset` are strings in the fixture (matching the
/// C test's own string-enum convention) so this is the one place that
/// maps them, rather than every test doing it inline.
private func fixtureLoraConfig(_ raw: Fixture.ExportLoraConfig) -> Config.LoRaConfig {
    var lora = Config.LoRaConfig()
    lora.usePreset = raw.use_preset
    lora.hopLimit = raw.hop_limit
    lora.txEnabled = raw.tx_enabled
    switch raw.modem_preset {
    case "LONG_FAST": lora.modemPreset = .longFast
    default: XCTFail("unrecognised modem_preset \(raw.modem_preset) — extend fixtureLoraConfig")
    }
    switch raw.region {
    case "US": lora.region = .us
    default: XCTFail("unrecognised region \(raw.region) — extend fixtureLoraConfig")
    }
    return lora
}

final class CrewCodeTests: XCTestCase {
    // MARK: - A02_AC1 — parse/normalise, and typed rejections

    func testA02_AC1_acceptsEveryDocumentedSpelling() throws {
        XCTAssertEqual(try CrewCode.parse("FIRE-4K9M7X").canonical, "FIRE-4K9M7X")
        XCTAssertEqual(try CrewCode.parse("fire 4k9m7x").canonical, "FIRE-4K9M7X")
        XCTAssertEqual(try CrewCode.parse("4K9M7X").canonical, "FIRE-4K9M7X")
        XCTAssertEqual(try CrewCode.parse("FIRE-4KIM7X").canonical, "FIRE-4K1M7X")
    }

    func testA02_AC1_rejectsEveryVectorInRejections() {
        for rejection in Fixture.file.rejections {
            XCTAssertThrowsError(try CrewCode.parse(rejection.input), rejection.why)
        }
    }

    func testA02_AC1_uIsRejectedNeverAliased() {
        XCTAssertThrowsError(try CrewCode.parse("FIRE-4K9M7U")) { error in
            guard case CrewCodeError.invalidCharacter("U") = error else {
                return XCTFail("expected .invalidCharacter(\"U\"), got \(error)")
            }
        }
    }

    /// §1.2 step 3's own pinned example: the tag-then-alias order means
    /// a crew whose six symbols spell `F1RE9X` parses from its full
    /// spelling but the tagless spelling is rejected.
    func testA02_AC1_tagStrippingRunsBeforeAliasing() throws {
        XCTAssertEqual(try CrewCode.parse("FIRE-FIRE9X").canonical, "FIRE-F1RE9X")
        XCTAssertThrowsError(try CrewCode.parse("FIRE9X"))
    }

    // MARK: - A02_AC2 — PSK derivation, byte for byte

    func testA02_AC2_pskMatchesEveryVector() throws {
        for vector in Fixture.file.vectors {
            let code = try CrewCode.parse(vector.input)
            let psk = CrewKey.psk(for: code)
            XCTAssertEqual(psk.hexEncoded, vector.psk_hex, vector.note)
            XCTAssertEqual(psk.base64EncodedString(), vector.psk_base64, vector.note)
            XCTAssertEqual(psk.count, 32, vector.note)
        }
    }

    // MARK: - A02_AC3 — ChannelSet / meshtastic URL round trip

    func testA02_AC3_channelSetSerializesToVectorHex() throws {
        for vector in Fixture.file.vectors {
            let code = try CrewCode.parse(vector.input)
            let channelSet = CrewChannel.channelSet(for: code)
            let data = try channelSet.serializedData()
            XCTAssertEqual(data.hexEncoded, vector.channelset_hex, vector.note)

            let settings = channelSet.settings[0]
            XCTAssertTrue(settings.hasModuleSettings, vector.note)
            XCTAssertEqual(settings.moduleSettings.positionPrecision, 32, vector.note)
            XCTAssertFalse(channelSet.hasLoraConfig, vector.note)
        }
    }

    /// §1.8 amendment (2026-09-14, bench finding): "Copy Meshtastic
    /// link" now carries the exporting radio's CURRENT `lora_config` —
    /// `--seturl` and the official apps' URL import REPLACE the target
    /// radio's `lora_config` wholesale, so an absent one writes it
    /// deaf (region UNSET, use_preset false), measured on a Heltec V3.
    /// `export_channelset_hex`/`meshtastic_url` are the vectors for
    /// THIS, built from `channelset_hex` (still lora-free — §1.5/§1.7,
    /// Firefly's own join path is unaffected) plus the fixture's
    /// `export_lora_config`.
    func testA02_AC3_meshtasticURLMatchesVectorAndRoundTrips() throws {
        let lora = fixtureLoraConfig(Fixture.file.export_lora_config)
        for vector in Fixture.file.vectors {
            let code = try CrewCode.parse(vector.input)

            let exportSet = try CrewChannel.exportChannelSet(for: code, loraConfig: lora)
            let exportData = try exportSet.serializedData()
            XCTAssertEqual(exportData.hexEncoded, vector.export_channelset_hex, vector.note)
            XCTAssertTrue(exportSet.hasLoraConfig, vector.note)
            XCTAssertEqual(exportSet.loraConfig, lora, vector.note)

            let url = try CrewChannel.meshtasticURL(for: code, loraConfig: lora)
            XCTAssertEqual(url, vector.meshtastic_url, vector.note)

            let parsed = try ChannelURL.parse(url)
            XCTAssertEqual(parsed.channelSet, exportSet, vector.note)
            XCTAssertFalse(parsed.addMode, vector.note)
        }
    }

    /// §1.8 amendment: exporting with an UNSET region must refuse
    /// rather than hand out a link that would write the importing
    /// radio deaf — defense-in-depth, the same stance
    /// `MeshtasticClient.setRegion` already takes on a write.
    func testA02_AC3_exportRefusesUnsetRegion() throws {
        let code = try CrewCode.parse(Fixture.file.vectors[0].input)
        var unset = fixtureLoraConfig(Fixture.file.export_lora_config)
        unset.region = .unset
        XCTAssertThrowsError(try CrewChannel.exportChannelSet(for: code, loraConfig: unset)) { error in
            XCTAssertEqual(error as? CrewChannel.ExportError, .regionUnset)
        }
        XCTAssertThrowsError(try CrewChannel.meshtasticURL(for: code, loraConfig: unset)) { error in
            XCTAssertEqual(error as? CrewChannel.ExportError, .regionUnset)
        }
    }

    // MARK: - A02_AC4 — generation: CSPRNG source, bijection, round trip

    /// (a) Generation reads from the injected randomness source, never
    /// from anything seeded/time/node-derived — proved by recording
    /// exactly what bytes the source was asked for and handing back a
    /// fixed answer, not by any statistical property of the output.
    func testA02_AC4a_generationDrawsFromInjectedSource() {
        final class RecordingSource: CrewCodeRandomnessSource, @unchecked Sendable {
            private let lock = NSLock()
            private var counts: [Int] = []
            var requestedCounts: [Int] { lock.lock(); defer { lock.unlock() }; return counts }
            func randomBytes(count: Int) -> [UInt8] {
                lock.lock(); counts.append(count); lock.unlock()
                return [0x00, 0x00, 0x00, 0x00] // -> value 0 -> FIRE-000000
            }
        }
        let source = RecordingSource()
        let code = CrewCode.generate(using: source)
        XCTAssertEqual(source.requestedCounts, [4])
        XCTAssertEqual(code.canonical, "FIRE-000000")

        struct FixedSource: CrewCodeRandomnessSource {
            let bytes: [UInt8]
            func randomBytes(count: Int) -> [UInt8] { Array(bytes.prefix(count)) }
        }
        // Top 2 bits of the first byte must be masked off (30 bits, not 32).
        let allOnes = CrewCode.generate(using: FixedSource(bytes: [0xFF, 0xFF, 0xFF, 0xFF]))
        XCTAssertEqual(allOnes.canonical, "FIRE-ZZZZZZ")
    }

    /// (b) The integer <-> code encoding is a bijection: both fixture
    /// endpoints, and an exhaustive per-position sweep proving every one
    /// of the 32 symbols is reachable at every one of the 6 positions
    /// and maps back to exactly its own 5 bits.
    func testA02_AC4b_integerCodeBijection() {
        XCTAssertEqual(CrewCode(value: 0).canonical, "FIRE-000000")
        XCTAssertEqual(CrewCode(value: (1 << 30) - 1).canonical, "FIRE-ZZZZZZ")
        XCTAssertEqual(CrewCode(value: 0).value, 0)
        XCTAssertEqual(CrewCode(value: (1 << 30) - 1).value, (1 << 30) - 1)

        for position in 0..<6 {
            let shift = UInt32(5 * (5 - position))
            for symbolIndex in UInt32(0)..<32 {
                let value = symbolIndex << shift
                let code = CrewCode(value: value)
                let symbolAtPosition = Array(code.symbols)[position]
                XCTAssertEqual(symbolAtPosition, CrewCodeAlphabet.symbols[Int(symbolIndex)],
                                "position \(position), symbol index \(symbolIndex)")
                // Round trip: decoding this single-symbol-set value back
                // recovers exactly that 5 bits at that position and zero
                // everywhere else.
                XCTAssertEqual(code.value, value)
            }
        }
    }

    /// (c) `parse(generate())` round-trips, and every generated code is
    /// 6 alphabet symbols.
    func testA02_AC4c_generateParseRoundTrips() throws {
        struct CountingSource: CrewCodeRandomnessSource {
            let value: UInt32
            func randomBytes(count: Int) -> [UInt8] {
                let v = value
                return [UInt8((v >> 24) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)]
            }
        }
        for raw: UInt32 in [0, 1, 12345, 999_999, (1 << 30) - 2, (1 << 30) - 1] {
            let generated = CrewCode.generate(using: CountingSource(value: raw))
            XCTAssertEqual(generated.symbols.count, 6)
            for symbol in generated.symbols {
                XCTAssertTrue(CrewCodeAlphabet.symbols.contains(symbol))
            }
            let reparsed = try CrewCode.parse(generated.canonical)
            XCTAssertEqual(reparsed, generated)
        }
    }

    // MARK: - A02_AC5 — deep link round trip

    func testA02_AC5_encodeMatchesVector1ByteExact() throws {
        let vector = Fixture.file.vectors[0]
        guard let deepLink = vector.deep_link else { return XCTFail("vector 1 must carry a deep_link") }
        let code = try CrewCode.parse(vector.input)
        XCTAssertEqual(CrewLink.encode(code: code, name: "Camp Firefly"), deepLink)
    }

    func testA02_AC5_encodeMatchesEveryVectorCarryingOne() throws {
        for vector in Fixture.file.vectors {
            guard let deepLink = vector.deep_link else { continue }
            let code = try CrewCode.parse(vector.input)
            XCTAssertEqual(CrewLink.encode(code: code, name: "Camp Firefly"), deepLink, vector.note)
        }
    }

    func testA02_AC5_parseRoundTripsVector1() throws {
        let vector = Fixture.file.vectors[0]
        guard let deepLink = vector.deep_link else { return XCTFail("vector 1 must carry a deep_link") }
        let parsed = try CrewLink.parse(deepLink)
        XCTAssertEqual(parsed.code.canonical, vector.canonical)
        XCTAssertEqual(parsed.name, "Camp Firefly")
    }

    func testA02_AC5_rejectsUnknownVersion() {
        XCTAssertThrowsError(try CrewLink.parse("firefly://crew?v=2&code=FIRE-4K9M7X")) { error in
            guard case CrewLinkError.unsupportedVersion("2") = error else {
                return XCTFail("expected .unsupportedVersion(\"2\"), got \(error)")
            }
        }
    }

    func testA02_AC5_rejectsMissingCode() {
        XCTAssertThrowsError(try CrewLink.parse("firefly://crew?v=1")) { error in
            guard case CrewLinkError.missingCode = error else {
                return XCTFail("expected .missingCode, got \(error)")
            }
        }
    }

    func testA02_AC5_rejectsMalformedCode() {
        XCTAssertThrowsError(try CrewLink.parse("firefly://crew?v=1&code=NOT-A-CODE")) { error in
            guard case CrewLinkError.invalidCode = error else {
                return XCTFail("expected .invalidCode, got \(error)")
            }
        }
    }

    func testA02_AC5_clampsLongNameRatherThanRejecting() throws {
        let longName = String(repeating: "A", count: 40)
        let link = try CrewLink.parse("firefly://crew?v=1&code=FIRE-4K9M7X&name=" + longName)
        XCTAssertEqual(link.name?.count, CrewLink.maxNameLength)
    }

    // MARK: - Scan classification (§3.1)

    func testScanClassification_recognizesAllThreeShapesAndRejectsJunk() throws {
        let vector = Fixture.file.vectors[0]
        guard let deepLink = vector.deep_link else { return XCTFail("vector 1 must carry a deep_link") }

        guard case .crewLink = CrewScanPayload.classify(deepLink) else {
            return XCTFail("expected .crewLink")
        }
        guard case .bareCode(let code) = CrewScanPayload.classify("FIRE-4K9M7X") else {
            return XCTFail("expected .bareCode")
        }
        XCTAssertEqual(code.canonical, "FIRE-4K9M7X")
        guard case .meshtasticChannelLink = CrewScanPayload.classify(vector.meshtastic_url) else {
            return XCTFail("expected .meshtasticChannelLink")
        }
        guard case .unrecognized = CrewScanPayload.classify("not a crew code at all") else {
            return XCTFail("expected .unrecognized")
        }
    }
}

private extension Data {
    var hexEncoded: String { map { String(format: "%02x", $0) }.joined() }
}
