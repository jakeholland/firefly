//
//  CrewKey.swift — HKDF-SHA256 derivation of a crew's 32-byte channel
//  PSK from its canonical code (`docs/specs/A02-crew-join.md`, §1.4).
//
//  RFC 5869 HKDF, both `salt` and `info` fixed, version-tagged ASCII
//  constants (never the crew's human name — §1.4's own "must not be"):
//  both sides of a join derive the SAME key from nothing but the code,
//  so `salt` cannot be a secret, and `info` exists only to domain-
//  separate this key from a future one derived from the same code.
//
//  Uses CryptoKit's `HKDF<SHA256>` directly per the spec's own
//  implementation note — no hand-rolled HMAC/expand on this side (the
//  C core vendors its own 40-line HMAC-SHA256 + expand instead, per
//  §9 Slice A, because `firmware/core` stays zero-dependency).
//
import CryptoKit
import Foundation

public enum CrewKey {
    /// 15 ASCII bytes, no NUL — §1.4. Version-tagged so a future
    /// `firefly-crew-v2` format can never collide with a v1 code.
    static let salt = Data("firefly-crew-v1".utf8)
    /// 19 ASCII bytes, no NUL — a CONSTANT, never the crew's human name
    /// (§1.4's own rejected alternative).
    static let info = Data("firefly-crew-psk-v1".utf8)
    /// AES256 needs exactly 32 bytes, one HKDF expand block.
    static let outputByteCount = 32

    /// The 32-byte channel PSK for `code` — deterministic, so two phones
    /// that only agree on the code (never having spoken to each other)
    /// derive byte-identical keys.
    public static func psk(for code: CrewCode) -> Data {
        let ikm = SymmetricKey(data: Data(code.canonical.utf8))
        let derived = HKDF<SHA256>.deriveKey(
            inputKeyMaterial: ikm, salt: salt, info: info, outputByteCount: outputByteCount)
        return derived.withUnsafeBytes { Data($0) }
    }
}
