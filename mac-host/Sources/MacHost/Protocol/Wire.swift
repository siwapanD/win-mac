import Foundation

// MARK: - Byte order helpers

struct ByteWriter {
    private(set) var data = Data()

    mutating func u8(_ v: UInt8) { data.append(v) }
    mutating func u16(_ v: UInt16) {
        data.append(UInt8(v >> 8 & 0xFF)); data.append(UInt8(v & 0xFF))
    }
    mutating func u32(_ v: UInt32) {
        data.append(UInt8(v >> 24 & 0xFF)); data.append(UInt8(v >> 16 & 0xFF))
        data.append(UInt8(v >> 8 & 0xFF)); data.append(UInt8(v & 0xFF))
    }
    mutating func u64(_ v: UInt64) {
        for shift in stride(from: 56, through: 0, by: -8) {
            data.append(UInt8(v >> UInt64(shift) & 0xFF))
        }
    }
    mutating func f32(_ v: Float) { u32(v.bitPattern) }
    mutating func bytes(_ d: Data) { data.append(d) }
}

struct ByteReader {
    let bytes: [UInt8]
    private(set) var offset = 0

    init(_ data: Data) { self.bytes = [UInt8](data) }
    init(bytes: [UInt8]) { self.bytes = bytes }

    var remaining: Int { bytes.count - offset }

    mutating func u8() -> UInt8? {
        guard remaining >= 1 else { return nil }
        defer { offset += 1 }
        return bytes[offset]
    }
    mutating func u16() -> UInt16? {
        guard remaining >= 2 else { return nil }
        defer { offset += 2 }
        return UInt16(bytes[offset]) << 8 | UInt16(bytes[offset + 1])
    }
    mutating func u32() -> UInt32? {
        guard remaining >= 4 else { return nil }
        defer { offset += 4 }
        var v: UInt32 = 0
        for i in 0..<4 { v = v << 8 | UInt32(bytes[offset + i]) }
        return v
    }
    mutating func u64() -> UInt64? {
        guard remaining >= 8 else { return nil }
        defer { offset += 8 }
        var v: UInt64 = 0
        for i in 0..<8 { v = v << 8 | UInt64(bytes[offset + i]) }
        return v
    }
    mutating func f32() -> Float? { u32().map(Float.init(bitPattern:)) }
    mutating func restData() -> Data? {
        guard remaining >= 0 else { return nil }
        defer { offset = bytes.count }
        return Data(bytes[offset...])
    }
}

// MARK: - Packet types

/// Packet type byte. Values are part of the wire contract (docs/protocol.md).
enum PacketType: UInt8 {
    case hello        = 1  // client → host: JSON greeting {deviceName, mode}
    case capabilities = 2  // both ways: JSON capability document (spec §31)
    case videoFrame   = 3  // host → client: VideoFrameHeader + annex-B chunk
    case audio        = 4  // reserved (Phase 10)
    case inputMouse   = 5
    case inputKey     = 6
    case inputScroll  = 7
    case cursorState  = 8  // host → client (spec §16)
    case clipboard    = 9  // reliable stream in production
    case control      = 10 // requestKeyFrame / setFrameRate / setResolution
    case heartbeat    = 11
    case stats        = 12
}

enum ControlOp: UInt8 {
    case requestKeyFrame = 1
    case setFrameRate    = 2
    case setResolution   = 3
}

enum VideoCodec: UInt8 {
    case h264 = 0
    case hevc = 1
    case av1  = 2
}

// MARK: - Wire header (spec §32)

/// Fixed 28-byte header on every packet:
/// magic | protocol_version | packet_type | flags | sequence | session_id |
/// monotonic timestamp_us | payload_length.
/// Latency measurement uses the monotonic clock only — never wall clock (spec §33).
struct WireHeader {
    static let length = 28
    static let magic: UInt32 = 0x5244_5048 // "RDPh"
    static let flagsReliable: UInt16 = 0x0001

    var protocolVersion: UInt8 = 1
    var type: PacketType
    var flags: UInt16 = 0
    var sequence: UInt32 = 0
    var sessionID: UInt32 = 0
    var timestampUs: UInt64 = WireHeader.nowUs()
    var payloadLength: UInt32 = 0

    init(type: PacketType, sessionID: UInt32 = 0) {
        self.type = type
        self.sessionID = sessionID
    }

    static func nowUs() -> UInt64 {
        var ts = timespec()
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts)
        return UInt64(ts.tv_sec) * 1_000_000 + UInt64(ts.tv_nsec) / 1_000
    }

    func encode() -> Data {
        var w = ByteWriter()
        w.u32(Self.magic)
        w.u8(protocolVersion)
        w.u8(type.rawValue)
        w.u16(flags)
        w.u32(sequence)
        w.u32(sessionID)
        w.u64(timestampUs)
        w.u32(payloadLength)
        return w.data
    }

    /// Decode from the front of `bytes`. Returns nil on magic/version mismatch
    /// or truncation (major version mismatch ⇒ refuse, spec §32).
    static func decode(_ bytes: [UInt8]) -> WireHeader? {
        guard bytes.count >= length else { return nil }
        var r = ByteReader(bytes: bytes)
        guard r.u32() == magic else { return nil }
        var h = WireHeader(type: .heartbeat)
        guard let version = r.u8(), let typeRaw = r.u8(),
              let flags = r.u16(), let seq = r.u32(),
              let session = r.u32(), let ts = r.u64(),
              let payloadLen = r.u32() else { return nil }
        guard version == 1, let type = PacketType(rawValue: typeRaw) else { return nil }
        h.protocolVersion = version
        h.type = type
        h.flags = flags
        h.sequence = seq
        h.sessionID = session
        h.timestampUs = ts
        h.payloadLength = payloadLen
        return h
    }

    static func decode(_ data: Data) -> WireHeader? {
        decode([UInt8](data))
    }
}

// MARK: - Video frame header (spec §33)

/// 36-byte header at the start of every `videoFrame` packet payload:
/// frame_id, packet_id, packet_count, keyframe flag, codec, dimensions,
/// fps profile, capture/encode monotonic timestamps, total payload length.
struct VideoFrameHeader {
    static let length = 36

    static let flagKeyframe: UInt8 = 0x01
    static let flagHasParamSets: UInt8 = 0x02 // SPS/PPS inline in this frame's annex-B

    var frameID: UInt32
    var packetID: UInt16
    var packetCount: UInt16
    var flags: UInt8
    var codec: VideoCodec
    var width: UInt16
    var height: UInt16
    var fpsProfile: UInt8
    var captureTsUs: UInt64
    var encodeTsUs: UInt64
    var payloadTotalLen: UInt32

    var isKeyframe: Bool { flags & Self.flagKeyframe != 0 }

    func encode() -> Data {
        var w = ByteWriter()
        w.u32(frameID)
        w.u16(packetID)
        w.u16(packetCount)
        w.u8(flags)
        w.u8(codec.rawValue)
        w.u16(width)
        w.u16(height)
        w.u8(fpsProfile)
        w.u8(0) // reserved
        w.u64(captureTsUs)
        w.u64(encodeTsUs)
        w.u32(payloadTotalLen)
        return w.data
    }

    static func decode(_ bytes: [UInt8]) -> VideoFrameHeader? {
        guard bytes.count >= length else { return nil }
        var r = ByteReader(bytes: bytes)
        guard let fid = r.u32(), let pid = r.u16(), let pc = r.u16(),
              let fl = r.u8(), let codecRaw = r.u8(),
              let wd = r.u16(), let ht = r.u16(),
              let fpsP = r.u8(), let _ = r.u8(),
              let cap = r.u64(), let enc = r.u64(),
              let total = r.u32() else { return nil }
        guard let codec = VideoCodec(rawValue: codecRaw) else { return nil }
        return VideoFrameHeader(
            frameID: fid, packetID: pid, packetCount: pc, flags: fl,
            codec: codec, width: wd, height: ht, fpsProfile: fpsP,
            captureTsUs: cap, encodeTsUs: enc, payloadTotalLen: total)
    }
}

// MARK: - Input packets (spec §17/§18)

enum MouseKind: UInt8 {
    case move = 0
    case down = 1
    case up   = 2
}

/// Mouse event. Coordinates are normalized 0…1 against the host display so the
/// injector can map them to host display points (multi-monitor mapping is V2).
struct MouseInput: Equatable {
    var kind: MouseKind
    /// bit0 = left, bit1 = right, bit2 = middle
    var buttons: UInt8
    var x: Float
    var y: Float

    func encode() -> Data {
        var w = ByteWriter()
        w.u8(kind.rawValue)
        w.u8(buttons)
        w.f32(x)
        w.f32(y)
        return w.data
    }

    static func decode(_ payload: Data) -> MouseInput? {
        var r = ByteReader(payload)
        guard let k = r.u8().flatMap(MouseKind.init(rawValue:)),
              let b = r.u8(), let x = r.f32(), let y = r.f32() else { return nil }
        return MouseInput(kind: k, buttons: b, x: x, y: y)
    }
}

/// Keyboard event. `keyCode` is a macOS virtual keycode — the Windows client
/// performs the VK→mac mapping (spec §18) before sending. `flags` is the raw
/// CGEventFlags bit pattern.
struct KeyInput: Equatable {
    var down: Bool
    var keyCode: UInt16
    var flags: UInt64

    func encode() -> Data {
        var w = ByteWriter()
        w.u8(down ? 1 : 0)
        w.u16(keyCode)
        w.u64(flags)
        return w.data
    }

    static func decode(_ payload: Data) -> KeyInput? {
        var r = ByteReader(payload)
        guard let d = r.u8(), let k = r.u16(), let f = r.u64() else { return nil }
        return KeyInput(down: d != 0, keyCode: k, flags: f)
    }
}

struct ScrollInput: Equatable {
    var dx: Float
    var dy: Float

    func encode() -> Data {
        var w = ByteWriter()
        w.f32(dx)
        w.f32(dy)
        return w.data
    }

    static func decode(_ payload: Data) -> ScrollInput? {
        var r = ByteReader(payload)
        guard let dx = r.f32(), let dy = r.f32() else { return nil }
        return ScrollInput(dx: dx, dy: dy)
    }
}

// MARK: - Capability negotiation (spec §31)

struct HostCapabilities: Codable {
    struct Capture: Codable {
        var screenCaptureKit: Bool
        var maxWidth: Int
        var maxHeight: Int
        var maxFps: Int
        var hdr: Bool
    }
    struct Video: Codable {
        var h264: Bool
        var hevc: Bool
        var av1: Bool
    }
    struct AppleScreenSharing: Codable {
        var enabled: Bool
        var nativeHighPerformanceEligible: Bool
    }

    var protocolVersion: Int = 1
    var platform: String = "macOS"
    var architecture: String = ProcessInfo.processInfo.machineDescription ?? "unknown"
    var capture: Capture
    var video: Video
    var appleScreenSharing: AppleScreenSharing
}

struct ClientCapabilities: Codable {
    struct Decode: Codable {
        var h264Hardware: Bool
        var hevcHardware: Bool
        var maxFps: Int
        var hdr: Bool
    }
    struct Display: Codable {
        var refreshRate: Int
    }

    var protocolVersion: Int = 1
    var decode: Decode
    var display: Display
}

extension ProcessInfo {
    var machineDescription: String? {
        var sysinfo = utsname()
        guard uname(&sysinfo) == 0 else { return nil }
        return withUnsafePointer(to: &sysinfo.machine) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: Int(_SYS_NAMELEN)) { cStr in
                String(cString: cStr)
            }
        }
    }
}

// MARK: - Hello

struct HelloMessage: Codable {
    var deviceName: String
    var clientVersion: String
    var mode: String // "hp" | "vnc-proxy"
}
