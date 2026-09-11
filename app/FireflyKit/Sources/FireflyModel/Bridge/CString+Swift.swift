//
//  CString+Swift.swift — the one place a fixed C char array (`char
//  name[16]`, imported by Swift as an N-tuple, never a pointer or a
//  string) is turned into or from a Swift `String`.
//
//  docs/specs/A01-companion-app.md, "Data flow" rule 2: "No C pointer
//  escapes its owner... Fixed C char arrays import as tuples; the
//  bridge converts them with a `withUnsafeBytes` + `String(cString:)`
//  helper and never hands the tuple out." Every other Bridge/* file
//  calls into this one rather than repeating the unsafe-bytes dance at
//  each call site — one helper, one thing that can go wrong.
//
import Foundation

/// Not `public`: this is bridge-internal plumbing, never part of the
/// public API surface (which only ever sees the `String` on the far
/// side of `decode`).
enum FixedCString {
    /// Decode a fixed-size C char array (imported as an N-tuple) as
    /// UTF-8, stopping at the first NUL byte — or reading the whole
    /// buffer if there is none.
    ///
    /// `withUnsafeBytes(of:)` hands back exactly `MemoryLayout<T>.size`
    /// bytes — the tuple's own storage, nothing more, nothing borrowed
    /// from a neighboring field — so a buffer with NO NUL terminator at
    /// all (e.g. a hand-crafted test fixture that fills all 16 bytes)
    /// is read to its last real byte and never one byte past it.
    static func decode<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            let len = bytes.firstIndex(of: 0) ?? bytes.count
            return String(decoding: bytes[bytes.startIndex..<(bytes.startIndex + len)], as: UTF8.self)
        }
    }

    /// Write `string`'s UTF-8 bytes into a fixed C char array (an
    /// N-tuple), truncated to fit and zero-padded — truncated, not
    /// rejected, the same convention every core string-write documents
    /// for itself (e.g. `ff_meshname_sanitize`'s doc comment). Always
    /// leaves at least one trailing zero byte (the bound is
    /// `buf.count - 1`, never `buf.count`), so the result is always a
    /// valid NUL-terminated C string for anything on the core side that
    /// reads it that way, not just for `decode` above.
    static func encode<T>(_ string: String, into tuple: inout T) {
        withUnsafeMutableBytes(of: &tuple) { raw in
            let buf = raw.bindMemory(to: UInt8.self)
            guard buf.count > 0 else { return }
            for i in buf.indices { buf[i] = 0 }
            let capacity = buf.count - 1
            for (i, byte) in string.utf8.prefix(capacity).enumerated() { buf[i] = byte }
        }
    }
}

extension Character {
    /// `char initial` (a single C byte; `'\0'` means "unknown") decoded
    /// the same "absent means absent, never fabricated" way every other
    /// optional field in this bridge is.
    init?(ffInitial raw: Int8) {
        guard raw != 0 else { return nil }
        self.init(UnicodeScalar(UInt8(bitPattern: raw)))
    }
}
