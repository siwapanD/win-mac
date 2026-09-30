import XCTest
@testable import MacHost

final class PacketizerTests: XCTestCase {
    private func meta(_ payloadLen: Int, keyframe: Bool = false) -> FrameMeta {
        FrameMeta(
            frameID: 1, keyframe: keyframe, hasParamSets: keyframe, codec: .h264,
            width: 1920, height: 1080, fpsProfile: 60,
            captureTsUs: 1_000, encodeTsUs: 2_000)
    }

    func testSplitAndReassemble() {
        let payload = Data((0..<50_000).map { UInt8($0 % 251) })
        let packets = Packetizer.split(meta: meta(payload.count, keyframe: true),
                                       payload: payload, sequenceBase: 100, sessionID: 7)
        XCTAssertTrue(packets.count > 40)

        var reasm = FrameReassembler()
        var result: (VideoFrameHeader, Data)?
        for p in packets.shuffled() { // out-of-order delivery must work (§34)
            if let r = reasm.ingest(p) { result = r }
        }
        XCTAssertEqual(result?.1, payload)
        XCTAssertEqual(result?.0.isKeyframe, true)
        XCTAssertEqual(result?.0.width, 1920)
        XCTAssertEqual(reasm.droppedStaleFrames, 0)
    }

    func testIncompleteFrameIsDroppedAsStale() {
        let payload = Data(repeating: 0xAB, count: 2_400) // 2 packets at 1200
        var packets = Packetizer.split(meta: meta(payload.count), payload: payload,
                                       sequenceBase: 1, sessionID: 1)
        packets.removeLast() // simulate burst loss → never completes
        var reasm = FrameReassembler(staleAfterMs: 0)
        for p in packets { _ = reasm.ingest(p) }
        usleep(1_000)
        _ = reasm.ingest(Packetizer.split(meta: meta(10), payload: Data([1, 2, 3]),
                                          sequenceBase: 9, sessionID: 1)[0])
        XCTAssertEqual(reasm.droppedStaleFrames, 1)
    }

    func testOlderFrameIsDiscardedOnceNewerArrives() {
        // latest frame wins (spec §65.2)
        let older = Packetizer.split(meta: FrameMeta(frameID: 5, keyframe: false, hasParamSets: false,
                                                     codec: .h264, width: 1920, height: 1080,
                                                     fpsProfile: 60, captureTsUs: 0, encodeTsUs: 0),
                                     payload: Data(repeating: 1, count: 3_600),
                                     sequenceBase: 1, sessionID: 1)
        let newer = Packetizer.split(meta: FrameMeta(frameID: 6, keyframe: false, hasParamSets: false,
                                                     codec: .h264, width: 1920, height: 1080,
                                                     fpsProfile: 60, captureTsUs: 0, encodeTsUs: 0),
                                     payload: Data(repeating: 2, count: 3_600),
                                     sequenceBase: 9, sessionID: 1)
        var reasm = FrameReassembler()
        for p in newer { _ = reasm.ingest(p) }
        for p in older { XCTAssertNil(reasm.ingest(p)) } // stale, worthless
        XCTAssertEqual(reasm.droppedStaleFrames, older.count)
    }

    func testControlAndCapabilitiesPacketsDecode() {
        let control = PacketFactory.control(.requestKeyFrame, sessionID: 3, sequence: 8)
        let header = WireHeader.decode(control)
        XCTAssertEqual(header?.type, .control)
        XCTAssertEqual((header?.flags ?? 0) & WireHeader.flagsReliable, WireHeader.flagsReliable)
        XCTAssertEqual(control.dropFirst(WireHeader.length).first,
                       ControlOp.requestKeyFrame.rawValue)

        let caps = ClientCapabilities(
            decode: .init(h264Hardware: true, hevcHardware: false, maxFps: 60, hdr: false),
            display: .init(refreshRate: 144))
        let json = try! JSONEncoder().encode(caps)
        let packet = PacketFactory.capabilities(json, sessionID: 3, sequence: 9)
        let decoded = try? JSONDecoder().decode(
            ClientCapabilities.self,
            from: packet.dropFirst(WireHeader.length))
        XCTAssertEqual(decoded?.decode.h264Hardware, true)
        XCTAssertEqual(decoded?.display.refreshRate, 144)
    }
}
