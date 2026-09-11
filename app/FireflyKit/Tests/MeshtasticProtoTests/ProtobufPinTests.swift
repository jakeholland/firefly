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

    func testSwiftAndNanopbGeneratorsPinTheSameProtobufCommit() throws {
        let nanopb = try pin(in: repoRoot.appending(path: "firmware/meshclient/tools/gen_nanopb.sh"))
        let swiftPin = try pin(in: repoRoot.appending(path: "app/tools/gen_swift_protos.sh"))
        XCTAssertFalse(nanopb.isEmpty)
        XCTAssertEqual(nanopb, swiftPin,
                       "protobuf pin drift: the puck's nanopb sources come from \(nanopb) "
                       + "but the app's Swift types come from \(swiftPin). Bump both, in one PR.")
    }
}
