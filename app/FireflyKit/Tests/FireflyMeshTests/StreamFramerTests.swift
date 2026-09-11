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

    /// `frame()` must permit an empty payload — matches
    /// `mc_frame_encode`, which only rejects `payload_len >
    /// MC_MAX_FRAME`, never a zero length. Only the oversize bound is a
    /// real refusal.
    func testFrameRefusesOversizeButNotEmptyPayloads() {
        XCTAssertNotNil(StreamFramer.frame(Data()))
        XCTAssertNil(StreamFramer.frame(Data(repeating: 0, count: StreamFramer.maxPayload + 1)))
        XCTAssertNotNil(StreamFramer.frame(Data(repeating: 0, count: StreamFramer.maxPayload)))
    }

    // MARK: - Zero-length (degenerate) frame — pinned against
    // `mc_framer_feed`'s `expected == 0` case in
    // firmware/meshclient/src/mc_framing.c: "may be 0 for a degenerate
    // zero-length frame" is a VALID frame that completes immediately,
    // not garbage to resync past.

    /// `frame(Data())` must encode as exactly the 4-byte header with a
    /// declared length of 0 and nothing else — no payload bytes to
    /// follow, same shape `mc_frame_encode(out, cap, NULL-or-empty, 0)`
    /// produces in C.
    func testFrameEncodesEmptyPayloadAsFourByteHeaderOnly() throws {
        let framed = try XCTUnwrap(StreamFramer.frame(Data()))
        XCTAssertEqual(Array(framed), [0x94, 0xC3, 0x00, 0x00])
    }

    /// The degenerate zero-length frame completes immediately on the
    /// length-low byte — no body bytes are consumed waiting for it.
    func testZeroLengthFrameCompletesImmediatelyWithEmptyPayload() {
        var framer = StreamFramer()
        let out = framer.feed(Data([0x94, 0xC3, 0x00, 0x00]))
        XCTAssertEqual(out, [Data()])
    }

    /// A zero-length frame is not a resync event, and the framer must
    /// still correctly frame whatever immediately follows it — same
    /// "recovery" shape as `S03_AC1_frame_length_513_resyncs` in
    /// `firmware/meshclient/tests/test_meshclient.c`, but for a frame
    /// that legitimately completed rather than one that was dropped.
    func testZeroLengthFrameThenARealFrameBothComplete() throws {
        var stream = Data([0x94, 0xC3, 0x00, 0x00]) // degenerate frame
        stream.append(try XCTUnwrap(StreamFramer.frame(payload)))

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(stream), [Data(), payload])
    }

    /// Dribbling the zero-length frame in one byte at a time (AC1's
    /// byte-dribble guarantee) must still yield it exactly once.
    func testZeroLengthFrameDribbledOneByteAtATimeStillCompletesOnce() {
        var framer = StreamFramer()
        var out: [Data] = []
        for byte: UInt8 in [0x94, 0xC3, 0x00, 0x00] {
            out += framer.feed(Data([byte]))
        }
        XCTAssertEqual(out, [Data()])
    }

    // MARK: - Ported from firmware/meshclient/tests/test_meshclient.c's
    // AC1 framing section (`S03_AC1_frame_length_exactly_512_is_accepted`
    // / `S03_AC1_frame_length_513_resyncs`), pinning the exact
    // `MC_MAX_FRAME` boundary the Swift and C framers must agree on.

    /// Port of `S03_AC1_frame_length_exactly_512_is_accepted`: exactly
    /// `maxPayload` (512) bytes must be accepted, not treated as
    /// oversize — `mc_framing.c`'s own check is `expected >
    /// MC_MAX_FRAME`, strictly greater-than.
    func testFrameLengthExactlyMaxPayloadIsAcceptedAndRoundTrips() throws {
        let big = Data((0..<StreamFramer.maxPayload).map { UInt8(($0 * 37 + 11) & 0xFF) })
        let framed = try XCTUnwrap(StreamFramer.frame(big))
        XCTAssertEqual(Array(framed.prefix(4)), [0x94, 0xC3, 0x02, 0x00]) // 512 == 0x0200

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(framed), [big])
    }

    /// Port of `S03_AC1_frame_length_513_resyncs`: one byte over
    /// `maxPayload` must resync immediately after the length is read,
    /// before ever touching the body, and a real frame right after must
    /// still complete cleanly (none of the dropped header's bytes
    /// mistaken for payload of anything).
    func testFrameLength513ResyncsThenRecovers() throws {
        // 513 = 0x0201 — one past MC_MAX_FRAME/maxPayload.
        var stream = Data([0x94, 0xC3, 0x02, 0x01])
        stream.append(try XCTUnwrap(StreamFramer.frame(payload)))

        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(stream), [payload])
    }

    func testTwoBackToBackFramesInOneRead() throws {
        var buf = try XCTUnwrap(StreamFramer.frame(payload))
        buf.append(try XCTUnwrap(StreamFramer.frame(Data([0x99]))))
        var framer = StreamFramer()
        XCTAssertEqual(framer.feed(buf), [payload, Data([0x99])])
    }
}
