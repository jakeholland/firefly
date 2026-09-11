//
//  ProtobufPinTests.swift — the app and the puck must speak the same
//  protobuf revision.
//
//  firmware/meshclient/tools/gen_nanopb.sh generates the puck's nanopb C
//  sources; app/tools/gen_swift_protos.sh generates these Swift types.
//  Both clone meshtastic/protobufs, and they MUST clone the same commit:
//  a phone that encodes a field the puck's generated decoder doesn't
//  know about fails in the field, at a festival, with no debugger.
//
//  The generator script refuses to run on drift. This test makes the
//  drift fail in CI too, for the case where somebody edits one script
//  and never runs either.
//
import XCTest

final class ProtobufPinTests: XCTestCase {
    private var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // MeshtasticProtoTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // FireflyKit
            .deletingLastPathComponent()   // app
            .deletingLastPathComponent()   // <repo root>
    }

    private func pin(in script: URL) throws -> String {
        let text = try String(contentsOf: script, encoding: .utf8)
        for line in text.split(separator: "\n") where line.hasPrefix("MESHTASTIC_PROTOBUFS_COMMIT=") {
            return line
                .replacingOccurrences(of: "MESHTASTIC_PROTOBUFS_COMMIT=", with: "")
                .replacingOccurrences(of: "\"", with: "")
                .trimmingCharacters(in: .whitespaces)
        }
        XCTFail("no MESHTASTIC_PROTOBUFS_COMMIT in \(script.path)")
        return ""
    }

    /// Pull the `PROTO_FILES=( ... )` bash array out of a generator
    /// script, one entry per line, comments (`# ...`) stripped.
    private func protoFiles(in script: URL) throws -> [String] {
        let text = try String(contentsOf: script, encoding: .utf8)
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        guard let start = lines.firstIndex(where: { $0.hasPrefix("PROTO_FILES=(") }) else {
            XCTFail("no PROTO_FILES=( in \(script.path)")
            return []
        }
        var files: [String] = []
        for line in lines[(start + 1)...] {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix(")") { break }
            let withoutComment = trimmed.split(separator: "#", maxSplits: 1)[0]
            let name = withoutComment.trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else { continue }
            files.append(String(name))
        }
        return files
    }

    func testSwiftAndNanopbGeneratorsPinTheSameProtobufCommit() throws {
        let nanopb = try pin(in: repoRoot.appending(path: "firmware/meshclient/tools/gen_nanopb.sh"))
        let swiftPin = try pin(in: repoRoot.appending(path: "app/tools/gen_swift_protos.sh"))
        XCTAssertFalse(nanopb.isEmpty)
        XCTAssertEqual(nanopb, swiftPin,
                       "protobuf pin drift: the puck's nanopb sources come from \(nanopb) "
                       + "but the app's Swift types come from \(swiftPin). Bump both, in one PR.")
    }

    /// `spec:78-80` says the `.proto` file list is "shared literally".
    /// This is the mechanical enforcement of that claim rather than an
    /// aspiration: the two arrays must list the same files, in the same
    /// order (N5).
    func testSwiftAndNanopbGeneratorsListTheSameProtoFiles() throws {
        let nanopb = try protoFiles(in: repoRoot.appending(path: "firmware/meshclient/tools/gen_nanopb.sh"))
        let swiftFiles = try protoFiles(in: repoRoot.appending(path: "app/tools/gen_swift_protos.sh"))
        XCTAssertFalse(nanopb.isEmpty, "could not parse PROTO_FILES out of gen_nanopb.sh - wrong path or format?")
        XCTAssertEqual(nanopb, swiftFiles,
                       "PROTO_FILES drift: gen_nanopb.sh lists \(nanopb) but gen_swift_protos.sh lists "
                       + "\(swiftFiles). Keep the two arrays identical, same order, same PR.")
    }
}
