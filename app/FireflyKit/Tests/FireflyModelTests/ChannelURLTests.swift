//
//  ChannelURLTests.swift — channel-share URL parsing, and the
//  position_precision trap (docs/specs/A01-companion-app.md, Slice C
//  "Must add").
//
import FireflyMesh
import FireflyModel
import MeshtasticProto
import XCTest

final class ChannelURLTests: XCTestCase {

    private func samplePayload(withModuleSettings: Bool) -> String {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.psk = Data([0x01, 0x02, 0x03, 0x04])
        if withModuleSettings {
            settings.moduleSettings.positionPrecision = 16
        }
        let set = ChannelSet(settings: [settings])
        let data = (try? set.serializedData()) ?? Data()
        return Base64URLTestHelper.encode(data)
    }

    // MARK: - Happy path

    func testHTTPSURLRoundTrips() throws {
        let payload = samplePayload(withModuleSettings: true)
        let result = try ChannelURL.parse("https://meshtastic.org/e/#\(payload)")
        XCTAssertEqual(result.channelSet.settings.count, 1)
        XCTAssertEqual(result.channelSet.settings[0].name, "Firefly")
        XCTAssertEqual(result.channelSet.settings[0].psk, Data([0x01, 0x02, 0x03, 0x04]))
        XCTAssertFalse(result.addMode)
    }

    func testMeshtasticSchemeURLParses() throws {
        let payload = samplePayload(withModuleSettings: true)
        let result = try ChannelURL.parse("meshtastic://e/#\(payload)")
        XCTAssertEqual(result.channelSet.settings.first?.name, "Firefly")
    }

    func testAddTrueInQueryIsRecognised() throws {
        let payload = samplePayload(withModuleSettings: true)
        let result = try ChannelURL.parse("https://meshtastic.org/e/?add=true#\(payload)")
        XCTAssertTrue(result.addMode)
        XCTAssertEqual(result.channelSet.settings.first?.name, "Firefly")
    }

    func testAddTrueInFragmentIsRecognised() throws {
        let payload = samplePayload(withModuleSettings: true)
        let result = try ChannelURL.parse("https://meshtastic.org/e/#\(payload)&add=true")
        XCTAssertTrue(result.addMode)
        XCTAssertEqual(result.channelSet.settings.first?.name, "Firefly")
    }

    func testLeadingAndTrailingWhitespaceIsTrimmed() throws {
        let payload = samplePayload(withModuleSettings: true)
        let result = try ChannelURL.parse("  https://meshtastic.org/e/#\(payload)\n")
        XCTAssertEqual(result.channelSet.settings.first?.name, "Firefly")
    }

    /// Base64url padding: a payload whose length isn't a multiple of 4
    /// (the common case — URL-safe base64 is usually shared unpadded)
    /// must still decode.
    func testUnpaddedBase64URLDecodes() throws {
        var settings = ChannelSettings()
        settings.name = "X" // short payload, likely to need padding
        let set = ChannelSet(settings: [settings])
        let data = (try? set.serializedData()) ?? Data()
        let unpadded = Base64URLTestHelper.encode(data)
        XCTAssertFalse(unpadded.hasSuffix("="), "test payload should already be unpadded")
        let result = try ChannelURL.parse("https://meshtastic.org/e/#\(unpadded)")
        XCTAssertEqual(result.channelSet.settings.first?.name, "X")
    }

    // MARK: - Rejection, not half-application

    func testUnsupportedSchemeIsRejected() {
        XCTAssertThrowsError(try ChannelURL.parse("http://example.com/e/#abcd")) { error in
            XCTAssertEqual(error as? ChannelURLError, .unsupportedScheme)
        }
    }

    func testMissingFragmentIsRejected() {
        XCTAssertThrowsError(try ChannelURL.parse("https://meshtastic.org/e/")) { error in
            XCTAssertEqual(error as? ChannelURLError, .missingFragment)
        }
    }

    func testEmptyFragmentIsRejected() {
        XCTAssertThrowsError(try ChannelURL.parse("https://meshtastic.org/e/#")) { error in
            XCTAssertEqual(error as? ChannelURLError, .missingFragment)
        }
    }

    func testInvalidBase64IsRejected() {
        XCTAssertThrowsError(try ChannelURL.parse("https://meshtastic.org/e/#not-valid-b64!!!")) { error in
            XCTAssertEqual(error as? ChannelURLError, .malformedBase64)
        }
    }

    /// A truncated length-delimited field 1 — the tag says a message
    /// follows, the declared length says more bytes than actually
    /// exist — must fail decode rather than silently producing an
    /// empty or partial `ChannelSet`: "malformed payload rejected
    /// rather than half-applied" (A01, Slice C "Must add").
    func testTruncatedPayloadIsRejectedNotSwallowed() {
        // Tag 0x0A = field 1, wire type 2 (length-delimited); length
        // byte 0x05 claims 5 bytes follow, but only 1 actually does.
        let malformed = Data([0x0A, 0x05, 0x01])
        let payload = Base64URLTestHelper.encode(malformed)
        XCTAssertThrowsError(try ChannelURL.parse("https://meshtastic.org/e/#\(payload)")) { error in
            guard case .malformedProtobuf? = error as? ChannelURLError else {
                return XCTFail("expected .malformedProtobuf, got \(error)")
            }
        }
    }

    // MARK: - The position_precision trap

    func testDecodeNeverInventsModuleSettings() throws {
        let payload = samplePayload(withModuleSettings: false)
        let result = try ChannelURL.parse("https://meshtastic.org/e/#\(payload)")
        let settings = try XCTUnwrap(result.channelSet.settings.first)
        XCTAssertFalse(settings.hasModuleSettings, "parse must report exactly what the URL said")
        XCTAssertTrue(settings.missingExplicitPositionPrecision)
    }

    func testWithExplicitPositionPrecisionFillsTheGapExplicitly() throws {
        var settings = ChannelSettings()
        settings.name = "NoPrecision"
        XCTAssertFalse(settings.hasModuleSettings)

        let fixed = ChannelURL.withExplicitPositionPrecision(settings)
        XCTAssertTrue(fixed.hasModuleSettings)
        XCTAssertEqual(fixed.moduleSettings.positionPrecision, 32)
    }

    func testWithExplicitPositionPrecisionNeverOverwritesAnExplicitValue() {
        var settings = ChannelSettings()
        settings.moduleSettings.positionPrecision = 10
        let fixed = ChannelURL.withExplicitPositionPrecision(settings)
        XCTAssertEqual(fixed.moduleSettings.positionPrecision, 10)
    }

    /// `encode` is the one path meant for eventual write-back (M3) —
    /// it must never emit the dangerous "absent means full precision"
    /// shape, even for a channel `parse` faithfully decoded as having
    /// no `moduleSettings` at all.
    func testEncodeNeverEmitsAbsentModuleSettings() throws {
        let importedPayload = samplePayload(withModuleSettings: false)
        let imported = try ChannelURL.parse("https://meshtastic.org/e/#\(importedPayload)")
        XCTAssertTrue(imported.channelSet.settings[0].missingExplicitPositionPrecision)

        let reencodedURL = ChannelURL.encode(imported.channelSet)
        let reparsed = try ChannelURL.parse(reencodedURL)
        XCTAssertFalse(reparsed.channelSet.settings[0].missingExplicitPositionPrecision)
        XCTAssertEqual(reparsed.channelSet.settings[0].moduleSettings.positionPrecision, 32)
    }

    func testEncodeAddModeRoundTrips() throws {
        var settings = ChannelSettings()
        settings.name = "Crew"
        let set = ChannelSet(settings: [settings])
        let url = ChannelURL.encode(set, addMode: true)
        let result = try ChannelURL.parse(url)
        XCTAssertTrue(result.addMode)
        XCTAssertEqual(result.channelSet.settings.first?.name, "Crew")
    }

    // MARK: - M3: lora_config (field 2) and the write-request builder

    func testLoraConfigIsAbsentByDefault() {
        let set = ChannelSet(settings: [ChannelSettings()])
        XCTAssertFalse(set.hasLoraConfig, "an 'add' import commonly carries no LoRa config — never invent one")
    }

    func testLoraConfigRoundTripsThroughParseAndEncode() throws {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        var lora = Config.LoRaConfig()
        lora.usePreset = true
        lora.modemPreset = .longFast
        lora.region = .us
        let set = ChannelSet(settings: [settings], loraConfig: lora)

        XCTAssertTrue(set.hasLoraConfig)
        let url = ChannelURL.encode(set)
        let result = try ChannelURL.parse(url)
        XCTAssertTrue(result.channelSet.hasLoraConfig)
        XCTAssertEqual(result.channelSet.loraConfig.region, .us)
        XCTAssertEqual(result.channelSet.loraConfig.modemPreset, .longFast)
    }

    func testMakeChannelWriteRequestAssignsIndexAndRole() {
        var primary = ChannelSettings()
        primary.name = "Firefly"
        primary.moduleSettings.positionPrecision = 32
        var secondary = ChannelSettings()
        secondary.name = "Ops"
        secondary.moduleSettings.positionPrecision = 24

        let result = ChannelImportResult(channelSet: ChannelSet(settings: [primary, secondary]), addMode: false)
        let request = result.makeChannelWriteRequest()

        XCTAssertEqual(request.channels.count, 2)
        XCTAssertEqual(request.channels[0].index, 0)
        XCTAssertEqual(request.channels[0].role, .primary)
        XCTAssertEqual(request.channels[0].settings.name, "Firefly")
        XCTAssertEqual(request.channels[1].index, 1)
        XCTAssertEqual(request.channels[1].role, .secondary)
        XCTAssertEqual(request.channels[1].settings.name, "Ops")
    }

    /// The write-request builder must never emit the "absent means full
    /// precision" trap either — same rule `ChannelURL.encode` already
    /// enforces, reused here (`withExplicitPositionPrecision`).
    func testMakeChannelWriteRequestForcesExplicitPositionPrecision() {
        var settings = ChannelSettings()
        settings.name = "NoLimit" // no moduleSettings at all
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: false)
        let request = result.makeChannelWriteRequest()

        XCTAssertTrue(request.channels[0].settings.hasModuleSettings)
        XCTAssertEqual(request.channels[0].settings.moduleSettings.positionPrecision, 32)
    }

    /// An "add" import that carried no LoRa config must not fabricate
    /// one for write-back — `applyChannelSet` must never be asked to
    /// write a region/modem preset nobody stated.
    func testMakeChannelWriteRequestOmitsLoraConfigWhenTheImportDidNotCarryOne() {
        var settings = ChannelSettings()
        settings.name = "Ops"
        settings.moduleSettings.positionPrecision = 32
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: true)
        let request = result.makeChannelWriteRequest()
        XCTAssertNil(request.loraConfig)
    }

    func testMakeChannelWriteRequestCarriesTheImportedLoraConfig() {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        var lora = Config.LoRaConfig()
        lora.region = .us
        lora.usePreset = true
        lora.modemPreset = .longFast
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings], loraConfig: lora), addMode: false)
        let request = result.makeChannelWriteRequest()
        XCTAssertEqual(request.loraConfig, lora)
    }
}

/// A tiny, test-only mirror of `Base64URL` (internal to the module
/// under test) — building fixture payloads needs the same encoding
/// `ChannelURL.parse` expects to decode.
enum Base64URLTestHelper {
    static func encode(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
    }
}
