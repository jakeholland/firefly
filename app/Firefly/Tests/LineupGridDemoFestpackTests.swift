//
//  LineupGridDemoFestpackTests.swift — UI-level guard against the
//  by-stage Grid regression fixed in this PR (`LineupGridView`'s
//  header row and gutter+body row drifting apart horizontally, and
//  the gutter drifting from the scrollable body vertically): loads
//  the REAL bundled demo festpack (`firmware/assets/demo/
//  firefly-fields.festpack.json`, via `DemoFestpackProvider` exactly
//  as `RootView`'s `-FireflyDemo` path does) rather than synthetic
//  fixture stages, and asserts every night's `LineupGridLayout` places
//  each block under the column for ITS OWN stage — column index and
//  stage identity are the two things `LineupGridView` renders in
//  lockstep (`stageHeaderRow`/`columnBody` both `ForEach(layout
//  .columns)` in the same order), so a layout-model regression here
//  would reproduce exactly the visual bug this PR fixed. Belongs
//  alongside `LineupGridLayoutTests` in intent (same invariant), but
//  lives in the app target so it exercises the ACTUAL shipped data —
//  JSON parsing, night folding, and stage-id matching included — not
//  hand-built fixture stages.
//
import FireflyModel
import Foundation
import XCTest

/// `FireflyAppTests` is a plain macOS unit-test bundle (`project.yml`) —
/// unlike the `Firefly` app target itself, it carries none of the
/// app's bundled resources, so `DemoFestpackProvider`'s real
/// `MainBundleFestpackLoader` (`Bundle.main`) would just be the test
/// runner's own bundle and never find `firefly-fields.festpack.json`.
/// This reads that SAME file straight off disk instead — the identical
/// "test process has no app bundle, so read `firmware/assets/*`
/// directly" seam `FestpackBundleLoading`'s own doc comment describes —
/// located relative to this source file's own path so it keeps working
/// however the worktree got checked out.
private struct DiskFestpackBundleLoader: FestpackBundleLoading {
    func festpackData(forResource name: String, extension ext: String) -> Data? {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // LineupGridDemoFestpackTests.swift -> Tests
            .deletingLastPathComponent() // Tests -> Firefly
            .deletingLastPathComponent() // Firefly -> app
            .deletingLastPathComponent() // app -> repo root
        let url = repoRoot.appendingPathComponent("firmware/assets/demo/\(name).\(ext)")
        return try? Data(contentsOf: url)
    }
}

final class LineupGridDemoFestpackTests: XCTestCase {
    func testEveryBlockInTheDemoFestpackLandsInItsOwnStagesColumn() async throws {
        let provider = DemoFestpackProvider(bundleLoader: DiskFestpackBundleLoader())
        await provider.refresh()
        let loaded = await provider.current()
        let festpack = try XCTUnwrap(loaded,
                                      "demo festpack (firefly-fields.festpack.json) failed to load from the app bundle")
        XCTAssertFalse(festpack.stages.isEmpty, "demo festpack has no stages to check columns against")

        let nights = Set(festpack.sets.map(\.nightDayOfYear)).sorted()
        XCTAssertFalse(nights.isEmpty, "demo festpack has no scheduled sets to check")

        for night in nights {
            let daySets = FestpackSchedule.daySets(in: festpack, night: night)
            let layout = LineupGridLayout.build(daySets: daySets, stages: festpack.stages)

            for column in layout.columns {
                for block in column.blocks {
                    if let stage = column.stage {
                        // A known-stage column must hold ONLY sets whose
                        // own `stageID` names that exact stage — this is
                        // precisely "column index matches stage index":
                        // `layout.columns` is built once, in a fixed
                        // order, and `LineupGridView` reads stage name/
                        // color from `column.stage` for the header while
                        // reading blocks from the SAME `column` for the
                        // body, so the two can only disagree here, at
                        // the model boundary, not in the view.
                        XCTAssertEqual(block.set.stageID, stage.id,
                                       "\(block.set.artist) (night \(night)) rendered under \(stage.name)'s column but its own stageID is \(block.set.stageID ?? "nil")")
                    } else {
                        // The "unknown stage" column: every set placed
                        // there must genuinely match no known stage —
                        // never a KNOWN stage's set mis-sorted into the
                        // fallback column.
                        XCTAssertFalse(festpack.stages.contains { $0.id == block.set.stageID },
                                       "\(block.set.artist) (night \(night)) landed in the unknown-stage column despite matching stage \(block.set.stageID ?? "nil")")
                    }
                }
            }
        }
    }
}
