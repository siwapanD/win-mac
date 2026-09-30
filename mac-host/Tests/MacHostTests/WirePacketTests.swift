import XCTest
@testable import MacHost

final class WirePacketTests: XCTestCase {
    func testHeaderRoundTrip() {
        var h = WireHeader(type: .videoFrame, sessionID: 0xDEAD_BEEF)
        h.sequence = 42
        h.flags = WireHeader.flagsReliable
        h.timestampUs = 123_456
        h.payloadLength = 777
        let data = h.encode()
        XCTAssertEqual(data.count, WireHeader.length)

        let decoded = WireHeader.decode(data)
        XCTAssertNotNil(decoded)
        XCTAssertEqual(decoded?.protocolVersion, 1)
        XCTAssertEqual(decoded?.type, .videoFrame)
        XCTAssertEqual(decoded?.flags, WireHeader.flagsReliable)
        XCTAssertEqual(decoded?.sequence, 42)
        XCTAssertEqual(decoded?.sessionID, 0xDEAD_BEEF)
        XCTAssertEqual(decoded?.timestampUs, 123_456)
        XCTAssertEqual(decoded?.payloadLength, 777)
    }

    func testHeaderRejectsGarbageAndTruncation() {
        XCTAssertNil(WireHeader.decode(Data(repeating: 0, count: WireHeader.length))) // wrong magic
        XCTAssertNil(WireHeader.decode(Data([0x52, 0x44]))) // truncated
        var good = WireHeader(type: .hello).encode()
        good[4] = 9 // unsupported major version → refuse (spec §32)
        XCTAssertNil(WireHeader.decode(good))
    }

    func testMonotonicTimestampProgresses() {
        let a = WireHeader.nowUs()
        usleep(2_000)
        let b = WireHeader.nowUs()
        XCTAssertGreaterThan(b, a)
    }

    func testFrameHeaderRoundTrip() {
        let fh = VideoFrameHeader(
            frameID: 9, packetID: 2, packetCount: 10,
            flags: VideoFrameHeader.flagKeyframe | VideoFrameHeader.flagHasParamSets,
            codec: .h264, width: 1920, height: 1080, fpsProfile: 60,
            captureTsUs: 111_222, encodeTsUs: 111_999, payloadTotalLen: 50_000)
        let decoded = VideoFrameHeader.decode([UInt8](fh.encode()))
        XCTAssertEqual(decoded?.frameID, 9)
        XCTAssertEqual(decoded?.packetID, 2)
        XCTAssertEqual(decoded?.packetCount, 10)
        XCTAssertTrue(decoded?.isKeyframe ?? false)
        XCTAssertEqual(decoded?.width, 1920)
        XCTAssertEqual(decoded?.height, 1080)
        XCTAssertEqual(decoded?.captureTsUs, 111_222)
        XCTAssertEqual(decoded?.payloadTotalLen, 50_000)
    }

    func testInputPacketRoundTrips() {
        let m = MouseInput(kind: .move, buttons: 0x01, x: 0.25, y: 0.75)
        XCTAssertEqual(MouseInput.decode(m.encode()), m)

        let k = KeyInput(down: true, keyCode: 7, flags: 1 << 20)
        XCTAssertEqual(KeyInput.decode(k.encode()), k)

        let s = ScrollInput(dx: -1.5, dy: 42)
        XCTAssertEqual(ScrollInput.decode(s.encode()), s)
    }
}
