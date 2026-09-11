//
//  HeadingProviderTests.swift — the compass rule, pinned (docs/specs/
//  A01-companion-app.md, "Compass heading"): trueHeading preferred,
//  magneticHeading as the fallback, accuracy carried through so a
//  negative value still reads NOHDG downstream — and macOS gets the
//  permanent no-magnetometer answer, never a fabricated arrow.
//
import FireflyModel
import XCTest

final class HeadingProviderTests: XCTestCase {

    #if os(iOS)
    // MARK: - iOS: the pure trueHeading/magneticHeading selection rule

    func testPrefersTrueHeadingWhenValid() {
        let reading = HeadingProvider.reading(trueHeading: 275, magneticHeading: 10, accuracy: 5)
        XCTAssertEqual(reading.headingDegrees, 275)
        XCTAssertEqual(reading.accuracyDegrees, 5)
        XCTAssertTrue(reading.isValid)
    }

    /// CoreLocation's own convention: `trueHeading < 0` means "no valid
    /// true heading" (typically: no location fix to compute declination
    /// from yet) — fall back to magnetic, exactly per spec.
    func testFallsBackToMagneticHeadingWhenTrueHeadingInvalid() {
        let reading = HeadingProvider.reading(trueHeading: -1, magneticHeading: 42, accuracy: 3)
        XCTAssertEqual(reading.headingDegrees, 42)
        XCTAssertTrue(reading.isValid)
    }

    /// A negative accuracy must read as NOHDG downstream regardless of
    /// which heading source produced the reading — never a confidently
    /// wrong arrow.
    func testNegativeAccuracyIsCarriedThroughAsInvalid() {
        let reading = HeadingProvider.reading(trueHeading: 90, magneticHeading: 90, accuracy: -1)
        XCTAssertFalse(reading.isValid)
    }
    #endif

    #if os(macOS)
    // MARK: - macOS: permanently NOHDG, never a fabricated heading

    /// `HeadingProvider` on macOS IS `NoHeadingProvider` (a type alias,
    /// not a second implementation) — this pins that it behaves
    /// exactly like the shared seam's own stand-in: `nil`, forever.
    func testMacOSHeadingProviderIsPermanentlyNOHDG() async throws {
        let provider = HeadingProvider()
        let stream = provider.headings()
        let collected = await collectHeadings(from: stream, count: 1, windowSeconds: 1)
        XCTAssertEqual(collected.count, 1)
        if let first = collected.first {
            XCTAssertNil(first, "macOS has no magnetometer: every reading must be NOHDG, never fabricated")
        }
    }

    private func collectHeadings(from stream: AsyncStream<HeadingReading?>, count: Int, windowSeconds: TimeInterval) async -> [HeadingReading?] {
        await withTaskGroup(of: [HeadingReading?].self) { group in
            group.addTask {
                var out: [HeadingReading?] = []
                for await reading in stream {
                    out.append(reading)
                    if out.count >= count { break }
                }
                return out
            }
            group.addTask {
                try? await Task.sleep(nanoseconds: UInt64(windowSeconds * 1_000_000_000))
                return []
            }
            let first = await group.next() ?? []
            group.cancelAll()
            return first
        }
    }
    #endif
}
