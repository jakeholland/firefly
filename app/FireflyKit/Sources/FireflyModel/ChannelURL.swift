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
import Foundation
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
/// {SettingsStore,ChannelURL}.swift`), so this models JUST the one
/// field M1's channel import actually needs — `settings` (field 1) —
/// using the same `SwiftProtobuf.Message` machinery the generated types
/// use, so wire compatibility with a real `meshtastic.org/e/#…` payload
/// is exact. Field 2 (`lora_config`) is deliberately NOT modeled: M1
/// only ever reads an imported channel for display, and never
/// re-encodes an imported set for write-back (that is M3) — there is
/// nothing to preserve `lora_config` bytes for yet, and pretending
/// otherwise would be exactly the kind of invented completeness this
/// project's honesty rule is about.
public struct ChannelSet: Sendable, Equatable {
    public var settings: [ChannelSettings]
    public var unknownFields = SwiftProtobuf.UnknownStorage()

    // `SwiftProtobuf.Message` requires a bare `init()` witness — a
    // defaulted-parameter initializer alone does not satisfy it.
    public init() {
        self.settings = []
    }

    public init(settings: [ChannelSettings]) {
        self.settings = settings
    }
}

extension ChannelSet: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
    public static let protoMessageName: String = "meshtastic.ChannelSet"
    public static let _protobuf_nameMap = SwiftProtobuf._NameMap(bytecode: "\0\u{1}settings\0\u{1}lora_config\0")

    public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
        while let fieldNumber = try decoder.nextFieldNumber() {
            switch fieldNumber {
            case 1: try decoder.decodeRepeatedMessageField(value: &settings)
            // Field 2 (lora_config) — deliberately skipped, see the
            // type's own doc comment. `nextFieldNumber()` consumes its
            // bytes as part of finding the next tag, so this does not
            // desync the decoder.
            default: break
            }
        }
    }

    public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
        if !settings.isEmpty {
            try visitor.visitRepeatedMessageField(value: settings, fieldNumber: 1)
        }
        try unknownFields.traverse(visitor: &visitor)
    }

    public static func == (lhs: ChannelSet, rhs: ChannelSet) -> Bool {
        lhs.settings == rhs.settings
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
    /// about round-tripping what it CAN represent (everything except
    /// `lora_config` — see `ChannelSet`'s doc comment). Every entry is
    /// passed through `withExplicitPositionPrecision` first: this
    /// function is the one place M3's write-back is meant to reuse, so
    /// it never emits the dangerous "absent means full precision" shape
    /// itself, even though `parse` (reading, not writing) faithfully
    /// reports it when that is what the source URL actually said.
    public static func encode(_ channelSet: ChannelSet, addMode: Bool = false, host: String = "meshtastic.org") -> String {
        var safe = channelSet
        safe.settings = safe.settings.map { withExplicitPositionPrecision($0) }
        let data = (try? safe.serializedData()) ?? Data()
        let payload = Base64URL.encode(data)
        let query = addMode ? "?add=true" : ""
        return "https://\(host)/e/\(query)#\(payload)"
    }
}
