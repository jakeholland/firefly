// swift-tools-version: 5.9
//
// FireflyKit — the Firefly companion app's non-UI half.
//
// Four targets, deliberately stacked so each one can be built and tested
// without the one above it (docs/specs/A01-companion-app.md):
//
//   FireflyCore      the puck's own pure-C11 domain logic, compiled IN
//                    PLACE from firmware/core via a symlink farm (see
//                    app/tools/link_core_sources.sh). Not a copy, not a
//                    port: the phone and the puck agree about crew
//                    freshness, radar geometry and inbox threading
//                    because they run the same object code.
//   MeshtasticProto  SwiftProtobuf types generated from the SAME pinned
//                    meshtastic/protobufs commit the puck's nanopb
//                    sources come from (app/tools/gen_swift_protos.sh).
//   FireflyMesh      transports (BLE / serial / TCP) + the Meshtastic
//                    client: framing, want_config handshake, nodeDB,
//                    routing-ack -> delivery state. Protocol-shaped, no
//                    UIKit/AppKit, no SwiftUI.
//   FireflyModel     the view models the SwiftUI shell observes. Depends
//                    on protocols, never on a concrete transport, so
//                    every screen is testable against a mock.
//
import PackageDescription

let package = Package(
    name: "FireflyKit",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        .library(name: "FireflyKit", targets: ["FireflyCore", "MeshtasticProto", "FireflyMesh", "FireflyModel"]),
        .library(name: "FireflyCore", targets: ["FireflyCore"]),
        .library(name: "MeshtasticProto", targets: ["MeshtasticProto"]),
        .library(name: "FireflyMesh", targets: ["FireflyMesh"]),
        .library(name: "FireflyModel", targets: ["FireflyModel"]),
    ],
    dependencies: [
        // Pinned to the runtime that matches protoc-gen-swift 1.38.0, the
        // generator app/tools/gen_swift_protos.sh records in
        // Sources/MeshtasticProto/GENERATED.md.
        .package(url: "https://github.com/apple/swift-protobuf.git", from: "1.38.0"),
    ],
    targets: [
        // firmware/core, compiled in place. `src/` and `include/` are
        // directories of per-file symlinks, regenerated (and checked for
        // drift) by app/tools/link_core_sources.sh — that script's header
        // explains why it is per-file and why firmware/platform's two
        // headers land in the same flat include directory.
        .target(
            name: "FireflyCore",
            path: "Sources/FireflyCore",
            sources: ["src"],
            publicHeadersPath: "include"
        ),
        .target(
            name: "MeshtasticProto",
            dependencies: [.product(name: "SwiftProtobuf", package: "swift-protobuf")],
            path: "Sources/MeshtasticProto",
            exclude: ["GENERATED.md"]
        ),
        .target(
            name: "FireflyMesh",
            dependencies: ["FireflyCore", "MeshtasticProto"],
            path: "Sources/FireflyMesh"
        ),
        .target(
            name: "FireflyModel",
            // MeshtasticProto is slice C's addition (append-only per
            // A01's shared-file table): ChannelURL.swift decodes an
            // imported channel's protobuf bytes and needs the generated
            // ChannelSettings/ModuleSettings types to do it with the
            // same wire format the puck's nanopb sources use, rather
            // than hand-rolling field parsing.
            dependencies: ["FireflyCore", "FireflyMesh", "MeshtasticProto"],
            path: "Sources/FireflyModel"
        ),

        .testTarget(name: "FireflyCoreTests", dependencies: ["FireflyCore"], path: "Tests/FireflyCoreTests"),
        .testTarget(name: "MeshtasticProtoTests", dependencies: ["MeshtasticProto"], path: "Tests/MeshtasticProtoTests"),
        .testTarget(name: "FireflyMeshTests", dependencies: ["FireflyMesh"], path: "Tests/FireflyMeshTests"),
        .testTarget(name: "FireflyModelTests", dependencies: ["FireflyModel"], path: "Tests/FireflyModelTests"),
    ]
)
