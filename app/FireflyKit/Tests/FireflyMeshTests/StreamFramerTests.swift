//
//  StreamFramerTests.swift
//
//  The two behaviours docs/specs/S03-meshclient.md AC1 pins for the C
//  framer, asserted again for the Swift one, because the serial and TCP
//  transports depend on both and a framer that desyncs on garbage stays
//  broken for the rest of the session.
//
import FireflyMesh
import XCTest

final class StreamFramerTests: XCTestCase {

    private let payload = Data([0x08, 0x2A, 0x10, 0x01])

    func testFrameThenParseRoundTrips() throws {
        let framed = try XCTUnwrap(StreamFramer.frame(payload))
        XCTAssertEqual(Array(framed.prefix(4)), [0x94, 0xC3, 0x00, 0x04])

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(framed), [payload])
    }

    /// AC1: one byte per read must still yield the frame exactly once.
    func testByteDribbleYieldsFrameExactlyOnce() throws {
        let framed = try XCTUnwrap(StreamFramer.frame(payload))
        var framer = StreamFramer()
        var out: [Data] = []
        for byte in framed {
            out += framer.feed(Data([byte]))
        }
        XCTAssertEqual(out, [payload])
    }

    /// AC1: garbage before the magic must resync, not poison the stream.
    func testGarbagePrefixResyncs() throws {
        let framed = try XCTUnwrap(StreamFramer.frame(payload))
        var noisy = Data([0x00, 0xFF, 0x94, 0x11, 0x94, 0x94])
        noisy.append(framed)

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(noisy), [payload])
    }

    /// An untrusted length larger than the frame budget must be dropped,
    /// never used to size a buffer.
    func testOversizeStatedLengthIsDroppedAndStreamRecovers() throws {
        var bogus = Data([0x94, 0xC3, 0xFF, 0xFF])
        bogus.append(try XCTUnwrap(StreamFramer.frame(payload)))

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(bogus), [payload])
    }

    func testFrameRefusesEmptyAndOversizePayloads() {
        XCTAssertNil(StreamFramer.frame(Data()))
        XCTAssertNil(StreamFramer.frame(Data(repeating: 0, count: StreamFramer.maxPayload + 1)))
        XCTAssertNotNil(StreamFramer.frame(Data(repeating: 0, count: StreamFramer.maxPayload)))
    }

    func testTwoBackToBackFramesInOneRead() throws {
        var buf = try XCTUnwrap(StreamFramer.frame(payload))
        buf.append(try XCTUnwrap(StreamFramer.frame(Data([0x99]))))
        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(buf), [payload, Data([0x99])])
    }
}
