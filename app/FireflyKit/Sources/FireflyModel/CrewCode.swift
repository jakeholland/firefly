//
//  CrewCode.swift — the crew code (`docs/specs/A02-crew-join.md`, §1.1–
//  §1.2): a 30-bit CSPRNG-drawn value rendered as `FIRE-` + six
//  Crockford-base32 symbols, and the parser that turns anything a human
//  typed, pasted, or scanned back into that canonical form.
//
//  This is slice A — pure, no I/O beyond the injected randomness source
//  below, no BLE, no radio. `CrewKey.swift` derives the PSK from the
//  canonical form this file produces; `CrewChannel.swift` builds the
//  `ChannelSettings`/`ChannelSet` from that key; `CrewLink.swift`
//  encodes/parses the `firefly://crew` deep link that carries a code
//  (and optionally a human name) between phones.
//
//  §1.9's fixture (`docs/specs/fixtures/A02-crew-codes.json`) is the
//  single source of truth for every byte this file and its siblings
//  produce — `CrewCodeTests` loads it directly rather than re-typing the
//  vectors, so the Swift and C (`firmware/core/tests/test_crewcode.c`)
//  suites can never drift from each other or from this file.
//
import Foundation

/// Crockford base32, Meshtastic/Firefly's own exclusions spelled out in
/// §1.1: no `I`/`L` (unreadable against `1` in the mono face, in the
/// dark), no `O` (unreadable against `0`), no `U` (kept out to match the
/// published Crockford alphabet exactly, and to keep accidental
/// obscenities out of a generated code). Exactly 32 symbols so each maps
/// to exactly 5 bits — the alphabet IS the encoding, not decoration on
/// top of it.
public enum CrewCodeAlphabet {
    public static let symbols: [Character] = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ")
    static let indexBySymbol: [Character: UInt32] = Dictionary(
        uniqueKeysWithValues: symbols.enumerated().map { ($1, UInt32($0)) })
}

/// Typed parse failures — §1.2: "a mistyped code is not silently wrong…
/// nothing decrypts". No case here ever produces a partial result; every
/// throw in `CrewCode.parse` happens before any code is constructed.
public enum CrewCodeError: Error, Equatable, Sendable {
    /// After stripping whitespace/dashes/the `FIRE` tag and applying
    /// Crockford aliasing, fewer or more than 6 characters remained.
    case invalidLength(Int)
    /// A remaining character is not in `CrewCodeAlphabet.symbols` after
    /// aliasing — this is how a bare `U` is rejected rather than
    /// silently accepted (§1.2 step 4: `U` is never aliased).
    case invalidCharacter(Character)
}

/// A canonical crew code: exactly `FIRE-` + six alphabet symbols
/// (§1.1's "This length is load-bearing" — `ChannelSettings.name`'s
/// 11-usable-byte budget, §1.3). The only way to construct one is
/// `parse(_:)` or `generate(using:)` — both funnel through the same
/// 6-symbol invariant, so a `CrewCode` value is always well-formed.
public struct CrewCode: Sendable, Equatable, Hashable {
    /// Exactly 6 characters, every one in `CrewCodeAlphabet.symbols`.
    public let symbols: String

    /// `FIRE-` + `symbols` — the exact 11 ASCII bytes written to
    /// `ChannelSettings.name` (§1.3) and shown on every screen.
    public var canonical: String { "FIRE-" + symbols }

    /// Only ever called with an already-validated 6-symbol string —
    /// `parse`/`generate` are the two public entry points and both
    /// establish the invariant before calling this.
    init(validatedSymbols: String) {
        self.symbols = validatedSymbols
    }

    /// §1.2 — normalises anything a human typed, pasted, or scanned.
    /// Order matters and is pinned by the spec (and by vector "F1RE9X",
    /// `CrewCodeTests.testTagStrippingRunsBeforeAliasing`):
    ///
    /// 1. Trim whitespace, uppercase.
    /// 2. Strip every space and `-`.
    /// 3. Strip one leading literal `FIRE`, BEFORE aliasing — so a
    ///    crew whose six symbols spell `F1RE9X` still parses from its
    ///    full spelling (`FIRE-FIRE9X` → strip tag → `FIRE9X` → alias →
    ///    `F1RE9X`), while `FIRE9X` typed with no tag at all is
    ///    (correctly) rejected as 2 remaining symbols, not guessed at.
    /// 4. Apply Crockford's decoding aliases: `I`→`1`, `L`→`1`, `O`→`0`.
    ///    `U` is never aliased — a `U` anywhere fails at step 5/6, on a
    ///    typed error, never silently mapped onto somebody else's crew.
    /// 5. Require exactly 6 characters remain, all in the alphabet.
    public static func parse(_ raw: String) throws -> CrewCode {
        var stripped = raw.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()
        stripped.removeAll { $0 == " " || $0 == "-" }
        if stripped.hasPrefix("FIRE") {
            stripped.removeFirst(4)
        }

        var aliased = ""
        aliased.reserveCapacity(stripped.count)
        for character in stripped {
            switch character {
            case "I", "L": aliased.append("1")
            case "O": aliased.append("0")
            default: aliased.append(character)
            }
        }

        guard aliased.count == 6 else { throw CrewCodeError.invalidLength(aliased.count) }
        for character in aliased where CrewCodeAlphabet.indexBySymbol[character] == nil {
            throw CrewCodeError.invalidCharacter(character)
        }
        return CrewCode(validatedSymbols: aliased)
    }

    /// The 30-bit integer this code encodes — MSB-first, 5 bits per
    /// symbol, §1.1. The inverse of `init(value:)` below; together they
    /// are AC4(b)'s bijection.
    public var value: UInt32 {
        symbols.reduce(UInt32(0)) { partial, character in
            (partial << 5) | (CrewCodeAlphabet.indexBySymbol[character] ?? 0)
        }
    }

    /// The bijective integer → code encoding, §1.1: MSB-first, 5 bits
    /// per symbol, 6 symbols = 30 bits. `value` must be `< 1 << 30`.
    public init(value: UInt32) {
        precondition(value < (1 << 30), "CrewCode.init(value:) takes a 30-bit value")
        var symbols = ""
        symbols.reserveCapacity(6)
        for shift in stride(from: 25, through: 0, by: -5) {
            let index = Int((value >> UInt32(shift)) & 0x1F)
            symbols.append(CrewCodeAlphabet.symbols[index])
        }
        self.init(validatedSymbols: symbols)
    }
}

// MARK: - Generation (AC4)

/// The seam AC4(a) requires: generation must draw from a real CSPRNG,
/// and that has to be provable WITHOUT sampling (§7's own rejected-draft
/// note on why a statistical test is the house proxy-check failure).
/// `SystemCrewRandomnessSource` is the only conformance that ships;
/// tests inject a recording/fixed source instead of asserting anything
/// about the numbers `SecRandomCopyBytes` actually returns.
public protocol CrewCodeRandomnessSource: Sendable {
    /// Exactly `count` fresh random bytes.
    func randomBytes(count: Int) -> [UInt8]
}

/// The one production conformance — `SecRandomCopyBytes`, Apple's CSPRNG,
/// never a seeded/time/node-derived generator (§1.1, AC4(a)).
public struct SystemCrewRandomnessSource: CrewCodeRandomnessSource {
    public init() {}

    public func randomBytes(count: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: count)
        let status = bytes.withUnsafeMutableBytes { buffer in
            SecRandomCopyBytes(kSecRandomDefault, count, buffer.baseAddress!)
        }
        precondition(status == errSecSuccess, "SecRandomCopyBytes failed with status \(status)")
        return bytes
    }
}

extension CrewCode {
    /// Mints a fresh code: 4 random bytes from `source`, masked to 30
    /// bits, MSB-first — never from a name, a timestamp, or a node id
    /// (§1.1). `source` defaults to the real CSPRNG in production; tests
    /// inject a fixed/recording source per AC4.
    public static func generate(using source: any CrewCodeRandomnessSource = SystemCrewRandomnessSource()) -> CrewCode {
        let bytes = source.randomBytes(count: 4)
        let raw = bytes.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        let value = raw & 0x3FFF_FFFF // low 30 bits
        return CrewCode(value: value)
    }
}
