import Foundation

/// Metadata describing one encoded frame, used for packetization.
struct FrameMeta {
    var frameID: UInt32
    var keyframe: Bool
    var hasParamSets: Bool
    var codec: VideoCodec
    var width: Int
    var height: Int
    var fpsProfile: UInt8
    var captureTsUs: UInt64
    var encodeTsUs: UInt64
}

/// Splits an encoded frame (annex-B H.264) into datagram-sized packets.
/// Every packet carries the full VideoFrameHeader so the client can
/// reassemble out of order and drop stale frames (spec §10/§33/§34).
enum Packetizer {
    static let defaultMTU = 1200

    static func split(meta: FrameMeta, payload: Data, mtu: Int = defaultMTU,
                      sequenceBase: UInt32, sessionID: UInt32) -> [Data] {
        let chunkSize = max(1, mtu)
        let total = payload.count
        let packetCount = max(1, (total + chunkSize - 1) / chunkSize)
        precondition(packetCount <= UInt16.max, "frame too large for u16 packet count")

        var flags: UInt8 = 0
        if meta.keyframe { flags |= VideoFrameHeader.flagKeyframe }
        if meta.hasParamSets { flags |= VideoFrameHeader.flagHasParamSets }

        var packets: [Data] = []
        packets.reserveCapacity(packetCount)
        for i in 0..<packetCount {
            let start = i * chunkSize
            let end = min(total, start + chunkSize)
            let chunk = payload.subdata(in: start..<end)

            let fh = VideoFrameHeader(
                frameID: meta.frameID,
                packetID: UInt16(i),
                packetCount: UInt16(packetCount),
                flags: flags,
                codec: meta.codec,
                width: UInt16(meta.width),
                height: UInt16(meta.height),
                fpsProfile: meta.fpsProfile,
                captureTsUs: meta.captureTsUs,
                encodeTsUs: meta.encodeTsUs,
                payloadTotalLen: UInt32(total))
            let chunkData = fh.encode() + chunk

            var h = WireHeader(type: .videoFrame, sessionID: sessionID)
            h.sequence = sequenceBase &+ UInt32(i)
            h.payloadLength = UInt32(chunkData.count)
            packets.append(h.encode() + chunkData)
        }
        return packets
    }
}

/// Reassembles packetized frames on the receiving side (used by the Windows
/// client; implemented here so the wire format is regression-tested on the
/// host). Frames that never complete within `staleAfter` are dropped.
struct FrameReassembler {
    private struct Partial {
        var header: VideoFrameHeader
        var chunks: [UInt16: Data]
        var receivedBytes: Int
        var arrivedAtUs: UInt64
    }

    private var partials: [UInt32: Partial] = [:]
    private var lastCompletedFrameID: UInt32 = 0
    private let staleAfterUs: UInt64
    private(set) var droppedStaleFrames = 0

    init(staleAfterMs: UInt64 = 250) {
        self.staleAfterUs = staleAfterMs * 1_000
    }

    /// Ingest one wire packet. Returns (header, payload) when a frame completes.
    mutating func ingest(_ packet: Data) -> (VideoFrameHeader, Data)? {
        guard let h = WireHeader.decode(packet), h.type == .videoFrame,
              packet.count >= WireHeader.length + VideoFrameHeader.length else { return nil }
        let payload = packet.subdata(in: (packet.startIndex + WireHeader.length)..<packet.endIndex)
        guard let fh = VideoFrameHeader.decode([UInt8](payload)) else { return nil }
        let chunk = payload.dropFirst(VideoFrameHeader.length)

        let now = WireHeader.nowUs()
        dropStale(now: now)

        // Video is latest-frame-wins: an old frame is worthless (spec §65.2) —
        // discard anything older than the newest in-flight or completed frame.
        let newestInFlight = partials.values.map(\.header.frameID).max() ?? 0
        let newest = max(newestInFlight, lastCompletedFrameID)
        if newest > 0, fh.frameID < newest {
            droppedStaleFrames += 1
            partials[fh.frameID] = nil
            return nil
        }

        var p = partials[fh.frameID] ?? Partial(
            header: fh, chunks: [:], receivedBytes: 0, arrivedAtUs: now)
        if p.chunks[fh.packetID] == nil {
            p.chunks[fh.packetID] = Data(chunk)
            p.receivedBytes += chunk.count
        }
        partials[fh.frameID] = p

        guard p.chunks.count == Int(fh.packetCount),
              p.receivedBytes == Int(fh.payloadTotalLen) else { return nil }

        var data = Data()
        data.reserveCapacity(Int(fh.payloadTotalLen))
        for i in 0..<fh.packetCount { data.append(p.chunks[i] ?? Data()) }
        partials[fh.frameID] = nil
        lastCompletedFrameID = fh.frameID
        return (fh, data)
    }

    private mutating func dropStale(now: UInt64) {
        for (id, p) in partials where now > p.arrivedAtUs + staleAfterUs {
            partials[id] = nil
            droppedStaleFrames += 1
        }
    }
}

/// Encodes input/control packets onto the wire.
enum PacketFactory {
    static func mouse(_ m: MouseInput, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.inputMouse, payload: m.encode(), sessionID: sessionID, sequence: sequence)
    }
    static func key(_ k: KeyInput, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.inputKey, payload: k.encode(), sessionID: sessionID, sequence: sequence)
    }
    static func scroll(_ s: ScrollInput, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.inputScroll, payload: s.encode(), sessionID: sessionID, sequence: sequence)
    }
    static func control(_ op: ControlOp, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.control, payload: Data([op.rawValue]), sessionID: sessionID, sequence: sequence,
               flags: WireHeader.flagsReliable)
    }
    static func hello(_ json: Data, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.hello, payload: json, sessionID: sessionID, sequence: sequence,
               flags: WireHeader.flagsReliable)
    }
    static func capabilities(_ json: Data, sessionID: UInt32, sequence: UInt32) -> Data {
        packet(.capabilities, payload: json, sessionID: sessionID, sequence: sequence,
               flags: WireHeader.flagsReliable)
    }

    static func packet(_ type: PacketType, payload: Data, sessionID: UInt32,
                       sequence: UInt32, flags: UInt16 = 0) -> Data {
        var h = WireHeader(type: type, sessionID: sessionID)
        h.sequence = sequence
        h.flags = flags
        h.payloadLength = UInt32(payload.count)
        return h.encode() + payload
    }
}
