//
//  ChannelURLTests.swift — channel-share URL parsing, and the
//  position_precision trap (docs/specs/A01-companion-app.md, Slice C
//  "Must add").
//
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
