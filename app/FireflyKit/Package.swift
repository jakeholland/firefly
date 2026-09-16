// swift-tools-version: 6.0
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
// A fifth target, `FireflyTelemetry` (docs/specs/A04-telemetry.md),
// appended below `FireflyCore` for the field-test telemetry work: the
// event/value types, the durable JSON-lines recorder, the sink seam and
// the event-name catalogue. It has no dependency on `FireflyCore` and
// depends on nothing but `Foundation`/`CryptoKit`, so `FireflyMesh` AND
// `FireflyModel` can both depend on it (the reason it is not simply
// folded into `FireflyModel`: `BLETransport`/`MeshtasticClient` live in
// `FireflyMesh`, which — per this file's own dependency graph — cannot
// depend on `FireflyModel`).
//
import PackageDescription

// M3 (docs/specs/A01-companion-app.md, "the package builds clean under
// SWIFT_STRICT_CONCURRENCY: complete"): every target below also carries
// an explicit `-strict-concurrency=complete` swiftSetting. That is
// redundant with `swiftLanguageModes: [.v6]` (Swift 6 language mode IS
// complete concurrency checking) but is kept anyway so the setting is
// legible target-by-target in `swift build -v` output and survives a
// future, target-by-target rollback to `.v5` without silently losing
// checking on the targets that stay in six.
let strictConcurrency: [SwiftSetting] = [.unsafeFlags(["-strict-concurrency=complete"])]

let package = Package(
    name: "FireflyKit",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        .library(name: "FireflyKit", targets: ["FireflyCore", "MeshtasticProto", "FireflyMesh", "FireflyModel", "FireflyTelemetry"]),
        .library(name: "FireflyCore", targets: ["FireflyCore"]),
        .library(name: "MeshtasticProto", targets: ["MeshtasticProto"]),
        .library(name: "FireflyMesh", targets: ["FireflyMesh"]),
        .library(name: "FireflyModel", targets: ["FireflyModel"]),
        .library(name: "FireflyTelemetry", targets: ["FireflyTelemetry"]),
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
            exclude: ["GENERATED.md"],
            swiftSettings: strictConcurrency
        ),
        // A04 — the telemetry event/value types, the durable recorder
        // and the sink seam. `Foundation` + `CryptoKit` only (the latter
        // for `TelemetryHash`'s truncated SHA-256), so it sits BELOW
        // `FireflyMesh` in this stack, same tier as `FireflyCore` — see
        // this file's own header comment for why it cannot simply be
        // folded into `FireflyModel`.
        .target(
            name: "FireflyTelemetry",
            path: "Sources/FireflyTelemetry",
            swiftSettings: strictConcurrency
        ),
        .target(
            name: "FireflyMesh",
            // FireflyTelemetry appended (A04): BLETransport/MeshtasticClient
            // record connectivity events at their existing log points.
            dependencies: ["FireflyCore", "MeshtasticProto", "FireflyTelemetry"],
            path: "Sources/FireflyMesh",
            swiftSettings: strictConcurrency
        ),
        .target(
            name: "FireflyModel",
            // MeshtasticProto is slice C's addition (append-only per
            // A01's shared-file table): ChannelURL.swift decodes an
            // imported channel's protobuf bytes and needs the generated
            // ChannelSettings/ModuleSettings types to do it with the
            // same wire format the puck's nanopb sources use, rather
            // than hand-rolling field parsing. FireflyTelemetry appended
            // (A04): AppDependencies/AppGraph own the recorder and emit
            // app-lifecycle/crew/notification events.
            dependencies: ["FireflyCore", "FireflyMesh", "MeshtasticProto", "FireflyTelemetry"],
            path: "Sources/FireflyModel",
            swiftSettings: strictConcurrency
        ),

        // "FireflyModel" appended for slice B: Bridge*.swift tests
        // exercise the Bridge/* wrapper types, which live in
        // FireflyModel, not FireflyCore itself (docs/specs/A01-companion-app.md's
        // shared-file table — a dependency appended to the array, never
        // reordering another slice's entry).
        .testTarget(name: "FireflyCoreTests", dependencies: ["FireflyCore", "FireflyModel"], path: "Tests/FireflyCoreTests", swiftSettings: strictConcurrency),
        .testTarget(name: "MeshtasticProtoTests", dependencies: ["MeshtasticProto"], path: "Tests/MeshtasticProtoTests", swiftSettings: strictConcurrency),
        .testTarget(name: "FireflyMeshTests", dependencies: ["FireflyMesh"], path: "Tests/FireflyMeshTests", swiftSettings: strictConcurrency),
        .testTarget(name: "FireflyModelTests", dependencies: ["FireflyModel"], path: "Tests/FireflyModelTests", swiftSettings: strictConcurrency),
        // A04 — recorder durability, catalogue pinning, hashing,
        // batching/flush and the no-coordinates/no-text attribute guard.
        .testTarget(name: "FireflyTelemetryTests", dependencies: ["FireflyTelemetry"], path: "Tests/FireflyTelemetryTests", swiftSettings: strictConcurrency),

        // Serial + TCP hardware integration tests (slice F). NOT
        // CoreBluetooth, so unaffected by the TCC restriction that
        // forces the BLE hardware suite into app/FireflyHardwareTests
        // (B1) — this one is a normal SwiftPM test target, gated at
        // runtime by FIREFLY_HARDWARE=1 (+ FIREFLY_SERIAL_PORT for the
        // serial tests) and skipping cleanly without them:
        //
        //   FIREFLY_HARDWARE=1 swift test --filter Hardware
        //
        // (docs/specs/A01-companion-app.md, "Test strategy" + Slice F).
        .testTarget(
            name: "HardwareTests",
            dependencies: ["FireflyCore", "MeshtasticProto", "FireflyMesh", "FireflyModel"],
            path: "Tests/HardwareTests",
            swiftSettings: strictConcurrency),
    ],
    // M3: Swift 6 language mode package-wide (docs/specs/A01-companion-app.md's
    // M3 acceptance criterion). FireflyCore is C11, unaffected either way.
    swiftLanguageModes: [.v6]
)
