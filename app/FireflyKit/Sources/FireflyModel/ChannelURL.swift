//
//  ChannelURL.swift — parsing (and, for the trap below, safe encoding)
//  of Meshtastic's `https://meshtastic.org/e/#…` channel-share URL
//  (docs/specs/A01-companion-app.md, "Reuse assessment" > Meshtastic-
//  Apple's "channel URL format", and Slice C).
//
//  M1 scope, stated explicitly so this file does not creep past it:
//  "M1 imports a channel by QR/URL and shows it. Writing channel config
//  back to a node is admin-message territory and is M3 at the
//  earliest" (A01, "Scope cuts"). So `parse` is the thing the Connect
//  screen actually calls; `encode` exists only to make the
//  position_precision trap testable end-to-end and to give M3 a single,
//  already-tested place to call from when it adds write-back.
//
//  M3 update: write-back has arrived (A01 M3: "Channel write-back
//  (admin messages) behind an explicit confirmation"), and it needs the
//  LoRa config (region/modem preset) a "replace" URL carries alongside
//  its channels — `ChannelSet.loraConfig` (field 2, below) and
//  `ChannelImportResult.makeChannelWriteRequest()` are that addition.
//  Nothing above this paragraph changed: `parse`/`encode` still round
//  trip exactly what they did in M1, this just stops skipping field 2.
//
import Foundation
import FireflyMesh
import MeshtasticProto
import SwiftProtobuf

// MARK: - ChannelSet

/// Meshtastic's `ChannelSet` message (`meshtastic/apponly.proto`) is
/// NOT part of the pinned generator's output: `app/tools/
/// gen_swift_protos.sh` generates from the pinned commit's core `.proto`
/// set, the same one the puck's nanopb sources come from
/// (`ProtobufPinTests`), and `apponly.proto` — being a phone-app-only
/// convenience wrapper the firmware itself never sends or receives —
/// is not in that set. Extending the generator is shared infra outside
/// this slice's file list (it is not `FireflyKit/Sources/FireflyModel/
/// {SettingsStore,ChannelURL}.swift`), so this models the two fields
/// this app actually needs — `settings` (field 1) and, since M3,
/// `lora_config` (field 2, the region/modem preset a "replace" URL
/// carries alongside its channels) — using the same
/// `SwiftProtobuf.Message` machinery the generated types use, so wire
/// compatibility with a real `meshtastic.org/e/#…` payload is exact.
public struct ChannelSet: Sendable, Equatable {
    public var settings: [ChannelSettings]
    /// M3: `lora_config`, modeled the same explicit-presence way
    /// `ChannelSettings.moduleSettings` already is — `hasLoraConfig` is
    /// `false`, not a zeroed struct, when the URL never carried one (an
    /// "add" import commonly doesn't; `ChannelURL.parse` must report
    /// that honestly rather than inventing a region nobody stated).
    fileprivate var _loraConfig: Config.LoRaConfig?
    public var loraConfig: Config.LoRaConfig {
        get { _loraConfig ?? Config.LoRaConfig() }
        set { _loraConfig = newValue }
    }
    /// Returns true if `loraConfig` has been explicitly set.
    public var hasLoraConfig: Bool { _loraConfig != nil }
    /// Clears the value of `loraConfig`. Subsequent reads from it will return its default value.
    public mutating func clearLoraConfig() { _loraConfig = nil }

    public var unknownFields = SwiftProtobuf.UnknownStorage()

    // `SwiftProtobuf.Message` requires a bare `init()` witness — a
    // defaulted-parameter initializer alone does not satisfy it.
    public init() {
        self.settings = []
    }

    public init(settings: [ChannelSettings], loraConfig: Config.LoRaConfig? = nil) {
        self.settings = settings
        self._loraConfig = loraConfig
    }
}

extension ChannelSet: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
    public static let protoMessageName: String = "meshtastic.ChannelSet"
    public static let _protobuf_nameMap = SwiftProtobuf._NameMap(bytecode: "\0\u{1}settings\0\u{1}lora_config\0")

    public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
        while let fieldNumber = try decoder.nextFieldNumber() {
            switch fieldNumber {
            case 1: try decoder.decodeRepeatedMessageField(value: &settings)
            case 2: try decoder.decodeSingularMessageField(value: &_loraConfig)
            default: break
            }
        }
    }

    public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
        if !settings.isEmpty {
            try visitor.visitRepeatedMessageField(value: settings, fieldNumber: 1)
        }
        if let loraConfig = _loraConfig {
            try visitor.visitSingularMessageField(value: loraConfig, fieldNumber: 2)
        }
        try unknownFields.traverse(visitor: &visitor)
    }

    public static func == (lhs: ChannelSet, rhs: ChannelSet) -> Bool {
        lhs.settings == rhs.settings && lhs._loraConfig == rhs._loraConfig
    }
}

// MARK: - The position_precision trap

extension ChannelSettings {
    /// `true` when this entry has no `moduleSettings` submessage AT
    /// ALL — the exact state the spec's trap is about: on a WRITE,
    /// Meshtastic firmware fills this gap in as 32 (full precision)
    /// when `moduleSettings` is absent, silently leaking exact
    /// coordinates on a channel whose importer never said anything
    /// about precision. `ChannelURL.parse` never papers over this by
    /// quietly filling it in — it decodes exactly what the URL said and
    /// leaves it to the caller (the Connect screen) to render the
    /// warning explicitly.
    public var missingExplicitPositionPrecision: Bool { !hasModuleSettings }
}

extension ChannelURL {
    /// The one place `moduleSettings.positionPrecision` is ever written
    /// deliberately for a settings entry that arrived without an
    /// explicit one: never emit a channel with the submessage absent.
    /// M1 has no write-to-node path at all (M3, admin-message
    /// territory — see this file's header), so nothing calls this
    /// outside `encode`/tests yet; it exists so the trap has one
    /// enforced, tested implementation ready for M3 to reuse rather
    /// than being rediscovered the hard way, the same spirit as the
    /// FROMNUM-subscription-ACK gate this spec borrows elsewhere.
    public static func withExplicitPositionPrecision(
        _ settings: ChannelSettings, default defaultBits: UInt32 = 32
    ) -> ChannelSettings {
        var copy = settings
        if !copy.hasModuleSettings {
            copy.moduleSettings.positionPrecision = defaultBits
        }
        return copy
    }
}

// MARK: - Base64url

enum Base64URL {
    static func decode(_ s: String) -> Data? {
        var b = s.replacingOccurrences(of: "-", with: "+")
            .replacingOccurrences(of: "_", with: "/")
        let remainder = b.count % 4
        if remainder != 0 { b += String(repeating: "=", count: 4 - remainder) }
        return Data(base64Encoded: b)
    }

    static func encode(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
    }
}

// MARK: - ChannelURL

public enum ChannelURLError: Error, Equatable, Sendable {
    /// Not `https://meshtastic.org/e/…` or `meshtastic://e/…`.
    case unsupportedScheme
    /// No `#…` payload at all.
    case missingFragment
    /// The fragment is not valid base64url.
    case malformedBase64
    /// It decoded as bytes, but not as a `ChannelSet`.
    case malformedProtobuf(String)
}

public struct ChannelImportResult: Sendable, Equatable {
    public let channelSet: ChannelSet
    /// `?add=true`, in the query OR the fragment — Meshtastic-Apple's
    /// own accepted shapes (A01, "Reuse assessment"). M1 only DISPLAYS
    /// this; nothing downstream acts on it (there is no write path at
    /// all yet — see this file's header).
    public let addMode: Bool

    public init(channelSet: ChannelSet, addMode: Bool) {
        self.channelSet = channelSet
        self.addMode = addMode
    }
}

public enum ChannelURL {
    /// Parses a scanned or pasted channel link. Rejects a malformed
    /// payload outright — it never half-applies one (A01, Slice C
    /// "Must add"): every failure path below throws before touching
    /// `ChannelSet` decoding, and a `ChannelSet` decode failure is
    /// propagated, not swallowed into an empty result.
    public static func parse(_ raw: String) throws -> ChannelImportResult {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)

        guard let schemeRange = trimmed.range(of: "://") else { throw ChannelURLError.unsupportedScheme }
        let scheme = trimmed[trimmed.startIndex..<schemeRange.lowerBound].lowercased()
        let rest = trimmed[schemeRange.upperBound...]

        let afterHost: Substring
        switch scheme {
        case "https", "http":
            guard rest.lowercased().hasPrefix("meshtastic.org/e") else { throw ChannelURLError.unsupportedScheme }
            afterHost = rest.dropFirst("meshtastic.org/e".count)
        case "meshtastic":
            guard rest.lowercased().hasPrefix("e") else { throw ChannelURLError.unsupportedScheme }
            afterHost = rest.dropFirst("e".count)
        default:
            throw ChannelURLError.unsupportedScheme
        }

        guard let hashIndex = afterHost.firstIndex(of: "#") else { throw ChannelURLError.missingFragment }
        let beforeHash = afterHost[afterHost.startIndex..<hashIndex]
        var fragment = String(afterHost[afterHost.index(after: hashIndex)...])
        guard !fragment.isEmpty else { throw ChannelURLError.missingFragment }

        var addMode = beforeHash.contains("add=true")
        // `add=true` can also ride inside the fragment, after the
        // base64url payload, separated by `&` or `?` — both observed
        // shapes; the payload itself never contains either character.
        if let separator = fragment.firstIndex(where: { $0 == "&" || $0 == "?" }) {
            let tail = fragment[fragment.index(after: separator)...]
            if tail.contains("add=true") { addMode = true }
            fragment = String(fragment[fragment.startIndex..<separator])
        }
        guard !fragment.isEmpty else { throw ChannelURLError.missingFragment }

        guard let data = Base64URL.decode(fragment), !data.isEmpty else {
            throw ChannelURLError.malformedBase64
        }

        do {
            let channelSet = try ChannelSet(serializedBytes: data)
            return ChannelImportResult(channelSet: channelSet, addMode: addMode)
        } catch {
            throw ChannelURLError.malformedProtobuf(String(describing: error))
        }
    }

    /// The inverse of `parse`, used by `ChannelURLTests` to prove the
    /// position_precision trap end-to-end and to keep `parse` honest
    /// about round-tripping everything a `ChannelSet` carries, including
    /// `lora_config` since M3. Every entry is passed through
    /// `withExplicitPositionPrecision` first: this function is the one
    /// place M3's write-back reuses, so it never emits the dangerous
    /// "absent means full precision" shape itself, even though `parse`
    /// (reading, not writing) faithfully reports it when that is what
    /// the source URL actually said.
    public static func encode(_ channelSet: ChannelSet, addMode: Bool = false, host: String = "meshtastic.org") -> String {
        var safe = channelSet
        safe.settings = safe.settings.map { withExplicitPositionPrecision($0) }
        let data = (try? safe.serializedData()) ?? Data()
        let payload = Base64URL.encode(data)
        let query = addMode ? "?add=true" : ""
        return "https://\(host)/e/\(query)#\(payload)"
    }
}

// MARK: - M3: channel write-back

/// Thrown building a write plan for an "add" import that cannot honestly
/// be placed — PR #274 review, BLOCKING 1: "an 'add' import writes its
/// channels only into free non-zero slots (error if none free...)".
/// Mirrors Meshtastic-Apple's own two distinct refusals
/// (`AccessoryManager+ToRadio.swift`'s `saveChannelSet`): "No free
/// channel slots available" when NONE of 1...7 are free, "Not enough
/// free channel slots" when some are free but fewer than the import
/// needs. Neither ever touches index 0 or an existing PSK — the plan is
/// simply never built.
public enum ChannelWritePlanError: Error, Equatable, Sendable {
    case noFreeChannelSlots
    case notEnoughFreeChannelSlots(needed: Int, available: Int)
    /// Meshtastic-Apple's own "A Meshtastic radio supports up to 8
    /// channels" guard, cross-checked rather than assumed
    /// (`AccessoryManager+ToRadio.swift`'s `saveChannelSet`).
    case tooManyChannels(Int)
}

/// One channel this write plan actually writes — index, display name,
/// and whether it is the primary — used by the confirmation sheet to
/// state exactly what changes (PR #274 review, BLOCKING 2).
public struct ChannelPlanEntry: Sendable, Equatable {
    public let index: Int32
    public let name: String
    public let isPrimary: Bool
    /// The `position_precision` this entry actually writes — the
    /// imported URL's own value when it stated one, else the SAFE
    /// default 0 (PR #274 review, SHOULD-FIX 4).
    public let positionPrecisionBits: UInt32
    /// `false` when the imported URL never stated a precision for this
    /// channel — the confirmation sheet uses this to say so explicitly
    /// rather than rendering `positionPrecisionBits` as if the link had
    /// asked for it.
    public let precisionWasExplicit: Bool

    public init(index: Int32, name: String, isPrimary: Bool, positionPrecisionBits: UInt32, precisionWasExplicit: Bool) {
        self.index = index
        self.name = name
        self.isPrimary = isPrimary
        self.positionPrecisionBits = positionPrecisionBits
        self.precisionWasExplicit = precisionWasExplicit
    }
}

/// What `ChannelImportResult.makeChannelWritePlan(occupiedIndexes:)`
/// decides, and exactly what `applyChannelSet`'s `request` will do to
/// EVERY slot on the node — never just the ones it fills. PR #274
/// review, BLOCKING 1 & 2: an "add" import places its channels in free
/// SECONDARY slots only (never index 0, never an existing PSK) and
/// leaves everything else untouched; a "replace" import writes the set
/// from index 0 and explicitly disables every remaining slot up to
/// `maxChannelSlots`, so nothing from before the import can silently
/// keep running — and both are disclosed here, not just written.
public struct ChannelWritePlan: Sendable, Equatable {
    /// What `MeshtasticClientProtocol.applyChannelSet(_:)` actually
    /// sends: `writtenChannels`' entries (role primary/secondary) plus,
    /// for a "replace" plan only, one explicit `.disabled` `Channel`
    /// entry per index in `disabledIndexes`.
    public let request: ChannelWriteRequest
    public let writtenChannels: [ChannelPlanEntry]
    /// Every index this plan explicitly disables. Always empty for an
    /// "add" plan (BLOCKING 1: add never disables anything).
    public let disabledIndexes: [Int32]
    /// Every index `0..<maxChannelSlots` this plan neither writes nor
    /// disables. Always empty for a "replace" plan (BLOCKING 2: replace
    /// accounts for every slot); for an "add" plan this is index 0 plus
    /// whatever secondary slots were not chosen.
    public let untouchedIndexes: [Int32]
    public let addMode: Bool

    public init(request: ChannelWriteRequest, writtenChannels: [ChannelPlanEntry],
                disabledIndexes: [Int32], untouchedIndexes: [Int32], addMode: Bool) {
        self.request = request
        self.writtenChannels = writtenChannels
        self.disabledIndexes = disabledIndexes
        self.untouchedIndexes = untouchedIndexes
        self.addMode = addMode
    }
}

extension ChannelImportResult {
    /// Builds the plan `applyChannelSet(_:)` sends AND the confirmation
    /// sheet renders — the SAME plan, so what the user is shown is
    /// exactly what gets written (PR #274 review, BLOCKING 1 & 2).
    ///
    /// `occupiedIndexes` is the node's CURRENT channel table (every index
    /// whose role is not `.disabled`), read live off the radio just
    /// before this is called (`MeshtasticClientProtocol
    /// .currentChannelTable()`) — never a stale cache. It matters only
    /// for an "add" plan; a "replace" plan's targets/disables are fixed
    /// (0..<count written, the rest disabled) regardless of what is
    /// currently there, matching Meshtastic-Apple's own `saveChannelSet`.
    ///
    /// "Add": every imported channel becomes a SECONDARY in the lowest
    /// free indices of 1..<maxChannelSlots (index 0 / the primary is
    /// NEVER a candidate — Meshtastic-Apple's own comment: "An
    /// unoccupied index 0 here means the local cache is empty or
    /// partial — never a license to hand an imported channel the
    /// primary slot"). Throws `ChannelWritePlanError.noFreeChannelSlots`
    /// / `.notEnoughFreeChannelSlots` rather than ever touching index 0
    /// or an existing PSK. Never carries a LoRa config, even if the
    /// import carried one — Meshtastic-Apple's own `saveChannelSet` only
    /// ever sends `loraConfig` on the non-add path.
    ///
    /// "Replace": every imported channel is written starting at index 0
    /// (index 0 is the primary), and every slot from `settings.count` up
    /// to `maxChannelSlots` is explicitly DISABLED — an old channel this
    /// import does not mention is authoritatively turned off, not left
    /// running (Meshtastic-Apple's own `saveChannelSet`: "A replace has
    /// to say something about every slot, not just the ones it fills").
    ///
    /// Every written channel's `position_precision` uses the imported
    /// URL's value verbatim when present, else the SAFE default 0 ("do
    /// not share") — never 32 (full precision), the trap Meshtastic-Apple
    /// itself closes the same way in `makeChannel(_:at:)` (PR #274
    /// review, SHOULD-FIX 4).
    public func makeChannelWritePlan(occupiedIndexes: Set<Int32> = []) throws -> ChannelWritePlan {
        guard channelSet.settings.count <= Int(maxChannelSlots) else {
            throw ChannelWritePlanError.tooManyChannels(channelSet.settings.count)
        }
        let safeSettings = channelSet.settings.map { ChannelURL.withExplicitPositionPrecision($0, default: 0) }

        func planEntry(original: ChannelSettings, safe: ChannelSettings, index: Int32, isPrimary: Bool) -> ChannelPlanEntry {
            ChannelPlanEntry(
                index: index,
                name: safe.name.isEmpty ? "(default channel)" : safe.name,
                isPrimary: isPrimary,
                positionPrecisionBits: safe.moduleSettings.positionPrecision,
                precisionWasExplicit: original.hasModuleSettings)
        }

        if addMode {
            let free = (Int32(1)..<maxChannelSlots).filter { !occupiedIndexes.contains($0) }
            guard !free.isEmpty else { throw ChannelWritePlanError.noFreeChannelSlots }
            guard safeSettings.count <= free.count else {
                throw ChannelWritePlanError.notEnoughFreeChannelSlots(needed: safeSettings.count, available: free.count)
            }
            let targetIndexes = Array(free.prefix(safeSettings.count))

            let channels: [Channel] = zip(safeSettings, targetIndexes).map { settings, index in
                var channel = Channel()
                channel.index = index
                channel.role = .secondary // add mode NEVER assigns .primary — index 0 is never a target.
                channel.settings = settings
                return channel
            }
            let writtenChannels = zip(zip(channelSet.settings, safeSettings), targetIndexes).map { pair, index in
                planEntry(original: pair.0, safe: pair.1, index: index, isPrimary: false)
            }
            let writtenSet = Set(targetIndexes)
            let untouched = (Int32(0)..<maxChannelSlots).filter { !writtenSet.contains($0) }

            return ChannelWritePlan(
                request: ChannelWriteRequest(channels: channels, loraConfig: nil),
                writtenChannels: writtenChannels,
                disabledIndexes: [],
                untouchedIndexes: untouched,
                addMode: true)
        } else {
            let targetIndexes = safeSettings.indices.map { Int32($0) }
            let filled = Set(targetIndexes)

            var channels: [Channel] = zip(safeSettings, targetIndexes).map { settings, index in
                var channel = Channel()
                channel.index = index
                channel.role = index == 0 ? .primary : .secondary
                channel.settings = settings
                return channel
            }
            var disabledIndexes: [Int32] = []
            for index in Int32(0)..<maxChannelSlots where !filled.contains(index) {
                var disabled = Channel()
                disabled.index = index
                disabled.role = .disabled
                channels.append(disabled)
                disabledIndexes.append(index)
            }
            let writtenChannels = zip(zip(channelSet.settings, safeSettings), targetIndexes).map { pair, index in
                planEntry(original: pair.0, safe: pair.1, index: index, isPrimary: index == 0)
            }

            return ChannelWritePlan(
                request: ChannelWriteRequest(channels: channels, loraConfig: channelSet.hasLoraConfig ? channelSet.loraConfig : nil),
                writtenChannels: writtenChannels,
                disabledIndexes: disabledIndexes,
                untouchedIndexes: [],
                addMode: false)
        }
    }
}
