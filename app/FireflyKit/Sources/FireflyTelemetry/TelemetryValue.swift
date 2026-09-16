//
//  TelemetryValue.swift — A04 (docs/specs/A04-telemetry.md): the four
//  attribute value shapes a `TelemetryEvent` may carry. Closed on
//  purpose — string/int/double/bool and nothing else — so an attribute
//  can never silently become a raw coordinate pair, a message body, or
//  any other free-form payload the honesty rules forbid; see
//  `TelemetryAttributeAllowlist` for the second line of defense over
//  the KEYS these values are attached to.
//
import Foundation

/// One attribute's value. `Codable` so `TelemetryRecorder` can write it
/// straight to JSON Lines with no hand-rolled encoding, `Equatable` so
/// tests can assert on a whole event at once.
public enum TelemetryValue: Sendable, Equatable, Codable {
    case string(String)
    case int(Int)
    case double(Double)
    case bool(Bool)

    private enum CodingKeys: String, CodingKey {
        case type, value
    }

    private enum Kind: String, Codable {
        case string, int, double, bool
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        switch try container.decode(Kind.self, forKey: .type) {
        case .string: self = .string(try container.decode(String.self, forKey: .value))
        case .int: self = .int(try container.decode(Int.self, forKey: .value))
        case .double: self = .double(try container.decode(Double.self, forKey: .value))
        case .bool: self = .bool(try container.decode(Bool.self, forKey: .value))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .string(let value):
            try container.encode(Kind.string, forKey: .type)
            try container.encode(value, forKey: .value)
        case .int(let value):
            try container.encode(Kind.int, forKey: .type)
            try container.encode(value, forKey: .value)
        case .double(let value):
            try container.encode(Kind.double, forKey: .type)
            try container.encode(value, forKey: .value)
        case .bool(let value):
            try container.encode(Kind.bool, forKey: .type)
            try container.encode(value, forKey: .value)
        }
    }
}

extension TelemetryValue: ExpressibleByStringLiteral, ExpressibleByIntegerLiteral,
                           ExpressibleByFloatLiteral, ExpressibleByBooleanLiteral {
    public init(stringLiteral value: String) { self = .string(value) }
    public init(integerLiteral value: Int) { self = .int(value) }
    public init(floatLiteral value: Double) { self = .double(value) }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
}
