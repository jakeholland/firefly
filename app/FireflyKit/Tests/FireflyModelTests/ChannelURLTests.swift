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

    // MARK: - makeChannelWritePlan: replace mode (BLOCKING 1 & 2, SHOULD-FIX 4)

    func testMakeChannelWritePlanReplaceModeAssignsIndexAndRole() throws {
        var primary = ChannelSettings()
        primary.name = "Firefly"
        primary.moduleSettings.positionPrecision = 32
        var secondary = ChannelSettings()
        secondary.name = "Ops"
        secondary.moduleSettings.positionPrecision = 24

        let result = ChannelImportResult(channelSet: ChannelSet(settings: [primary, secondary]), addMode: false)
        let plan = try result.makeChannelWritePlan()

        XCTAssertFalse(plan.addMode)
        let written = plan.request.channels.filter { $0.role != .disabled }
        XCTAssertEqual(written.count, 2)
        XCTAssertEqual(written[0].index, 0)
        XCTAssertEqual(written[0].role, .primary)
        XCTAssertEqual(written[0].settings.name, "Firefly")
        XCTAssertEqual(written[1].index, 1)
        XCTAssertEqual(written[1].role, .secondary)
        XCTAssertEqual(written[1].settings.name, "Ops")
    }

    /// BLOCKING 2 — a replace plan must explicitly DISABLE every slot it
    /// does not fill, up to `maxChannelSlots`, and disclose that in
    /// `disabledIndexes` — never leave old channels silently running.
    func testMakeChannelWritePlanReplaceModeDisablesEveryUnfilledSlot() throws {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        var lora = Config.LoRaConfig()
        lora.region = .us
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings], loraConfig: lora), addMode: false)

        // Even with a node that currently has channels 2-7 occupied,
        // replace ignores that entirely — it disables 1...7 regardless
        // of what is currently there (Meshtastic-Apple's own behaviour).
        let plan = try result.makeChannelWritePlan(occupiedIndexes: [0, 1, 2, 3, 4, 5, 6, 7])

        XCTAssertEqual(plan.disabledIndexes, Array(Int32(1)..<8))
        XCTAssertEqual(plan.untouchedIndexes, [], "replace accounts for every slot")
        XCTAssertEqual(plan.request.channels.count, 8, "1 written + 7 explicit disables")
        let disabled = plan.request.channels.filter { $0.role == .disabled }
        XCTAssertEqual(Set(disabled.map(\.index)), Set(Int32(1)..<8))
        XCTAssertTrue(disabled.allSatisfy { !$0.hasSettings })
    }

    /// SHOULD-FIX 4 — absent `moduleSettings` on write must default to
    /// the SAFE value (0), never 32 (full precision).
    func testMakeChannelWritePlanDefaultsMissingPrecisionToZeroNotThirtyTwo() throws {
        var settings = ChannelSettings()
        settings.name = "NoLimit" // no moduleSettings at all
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: false)
        let plan = try result.makeChannelWritePlan()

        let written = try XCTUnwrap(plan.request.channels.first { $0.role == .primary })
        XCTAssertTrue(written.settings.hasModuleSettings)
        XCTAssertEqual(written.settings.moduleSettings.positionPrecision, 0)
        XCTAssertFalse(plan.writtenChannels[0].precisionWasExplicit)
        XCTAssertEqual(plan.writtenChannels[0].positionPrecisionBits, 0)
    }

    /// An explicit precision in the imported link is used verbatim, not
    /// overridden by the safe default.
    func testMakeChannelWritePlanUsesExplicitPrecisionVerbatim() throws {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 16
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: false)
        let plan = try result.makeChannelWritePlan()

        let written = try XCTUnwrap(plan.request.channels.first { $0.role == .primary })
        XCTAssertEqual(written.settings.moduleSettings.positionPrecision, 16)
        XCTAssertTrue(plan.writtenChannels[0].precisionWasExplicit)
    }

    func testMakeChannelWritePlanCarriesTheImportedLoraConfigOnReplaceOnly() throws {
        var settings = ChannelSettings()
        settings.name = "Firefly"
        settings.moduleSettings.positionPrecision = 32
        var lora = Config.LoRaConfig()
        lora.region = .us
        lora.usePreset = true
        lora.modemPreset = .longFast
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings], loraConfig: lora), addMode: false)
        let plan = try result.makeChannelWritePlan()
        XCTAssertEqual(plan.request.loraConfig, lora)
    }

    /// Replace requires more than 8 channels to be rejected outright —
    /// the radio's channel table is a fixed array of 8.
    func testMakeChannelWritePlanRejectsMoreThanEightChannels() {
        let settings = (0..<9).map { i -> ChannelSettings in
            var s = ChannelSettings()
            s.name = "Ch\(i)"
            return s
        }
        let result = ChannelImportResult(channelSet: ChannelSet(settings: settings), addMode: false)
        XCTAssertThrowsError(try result.makeChannelWritePlan()) { error in
            guard case .tooManyChannels(9) = error as? ChannelWritePlanError else {
                return XCTFail("expected .tooManyChannels(9), got \(error)")
            }
        }
    }

    // MARK: - makeChannelWritePlan: add mode (BLOCKING 1)

    /// BLOCKING 1 — an "add" import must place its channel(s) into the
    /// lowest FREE SECONDARY slot(s) — never index 0, never an occupied
    /// index — and must not fabricate a LoRa config even if the URL
    /// (unusually) carried one.
    func testMakeChannelWritePlanAddModePlacesInLowestFreeSecondarySlots() throws {
        var settings = ChannelSettings()
        settings.name = "Ops"
        var lora = Config.LoRaConfig() // present but must be IGNORED for an add plan
        lora.region = .us
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings], loraConfig: lora), addMode: true)

        // Index 0 (primary) and 1 are occupied; the plan must skip both.
        let plan = try result.makeChannelWritePlan(occupiedIndexes: [0, 1])

        XCTAssertTrue(plan.addMode)
        XCTAssertEqual(plan.request.channels.count, 1)
        XCTAssertEqual(plan.request.channels[0].index, 2)
        XCTAssertEqual(plan.request.channels[0].role, .secondary)
        XCTAssertNil(plan.request.loraConfig, "add must never fabricate/forward a LoRa config")
        XCTAssertEqual(plan.disabledIndexes, [], "add mode never disables anything")
        XCTAssertEqual(Set(plan.untouchedIndexes), Set([0, 1, 3, 4, 5, 6, 7]))
    }

    /// BLOCKING 1 — an "add" import must NEVER be assigned index 0
    /// even when index 0 is reported free (an empty/partial local read,
    /// never a license to hand it the primary slot — Meshtastic-Apple's
    /// own reasoning, cross-checked).
    func testMakeChannelWritePlanAddModeNeverTargetsIndexZeroEvenWhenFree() throws {
        var settings = ChannelSettings()
        settings.name = "Ops"
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: true)

        let plan = try result.makeChannelWritePlan(occupiedIndexes: [])
        XCTAssertEqual(plan.request.channels[0].index, 1, "the lowest candidate is 1, never 0")
        XCTAssertTrue(plan.untouchedIndexes.contains(0), "index 0 must be reported untouched")
    }

    /// BLOCKING 1 — no free secondary slot at all must error, never
    /// silently overwrite index 0 or any existing PSK.
    func testMakeChannelWritePlanAddModeWithNoFreeSlotsThrows() {
        var settings = ChannelSettings()
        settings.name = "Ops"
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [settings]), addMode: true)
        let allOccupied = Set(Int32(0)..<8)
        XCTAssertThrowsError(try result.makeChannelWritePlan(occupiedIndexes: allOccupied)) { error in
            XCTAssertEqual(error as? ChannelWritePlanError, .noFreeChannelSlots)
        }
    }

    /// BLOCKING 1 — fewer free slots than the import needs must error
    /// with the exact counts, never silently truncate the import.
    func testMakeChannelWritePlanAddModeWithNotEnoughFreeSlotsThrows() {
        var a = ChannelSettings(); a.name = "A"
        var b = ChannelSettings(); b.name = "B"
        var c = ChannelSettings(); c.name = "C"
        let result = ChannelImportResult(channelSet: ChannelSet(settings: [a, b, c]), addMode: true)
        // Only index 1 is free (2...7 occupied) — needs 3, has 1.
        let occupied: Set<Int32> = [0, 2, 3, 4, 5, 6, 7]
        XCTAssertThrowsError(try result.makeChannelWritePlan(occupiedIndexes: occupied)) { error in
            XCTAssertEqual(error as? ChannelWritePlanError, .notEnoughFreeChannelSlots(needed: 3, available: 1))
        }
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
