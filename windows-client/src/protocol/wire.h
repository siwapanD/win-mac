// Wire format mirror of mac-host Sources/MacHost/Protocol/Wire.swift.
// THE BYTE LAYOUTS IN THIS FILE MUST STAY IN SYNC WITH THE SWIFT IMPLEMENTATION.
// See docs/protocol.md — it is the canonical reference.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>
#include <string>

namespace rdp {

constexpr uint32_t kMagic = 0x52445048;          // "RDPh"
constexpr uint8_t  kProtocolVersion = 1;
constexpr size_t   kWireHeaderLength = 28;
constexpr size_t   kVideoFrameHeaderLength = 36;
constexpr uint16_t kFlagReliable = 0x0001;
constexpr uint16_t kDefaultHPPort = 55443;       // never 5900-5902 (coexist with Apple)

enum class PacketType : uint8_t {
    Hello        = 1,
    Capabilities = 2,
    VideoFrame   = 3,
    Audio        = 4,
    InputMouse   = 5,
    InputKey     = 6,
    InputScroll  = 7,
    CursorState  = 8,
    Clipboard    = 9,
    Control      = 10,
    Heartbeat    = 11,
    Stats        = 12,
};

enum class ControlOp : uint8_t {
    RequestKeyFrame = 1,
    SetFrameRate    = 2,
    SetResolution   = 3,
};

// Big-endian helpers (spec §32/§33: network byte order, monotonic timestamps).
inline void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}
inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
inline void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}

struct Reader {
    const uint8_t* p; size_t n; size_t off = 0;
    Reader(const uint8_t* data, size_t len) : p(data), n(len) {}
    bool take(size_t k) { if (n - off < k) return false; off += k; return true; }
    uint8_t  u8()  { return p[off - 1]; }
    uint16_t u16() { return uint16_t(p[off-2] << 8 | p[off-1]); }
    uint32_t u32() { uint32_t v = 0; for (size_t i = off - 4; i < off; ++i) v = v << 8 | p[i]; return v; }
    uint64_t u64() { uint64_t v = 0; for (size_t i = off - 8; i < off; ++i) v = v << 8 | p[i]; return v; }
};

struct WireHeader {
    uint8_t  protocolVersion = kProtocolVersion;
    PacketType type = PacketType::Hello;
    uint16_t flags = 0;
    uint32_t sequence = 0;
    uint32_t sessionId = 0;
    uint64_t timestampUs = 0;   // CLOCK_MONOTONIC-equivalent (QueryPerformanceCounter)
    uint32_t payloadLength = 0;

    void encode(std::vector<uint8_t>& out) const {
        putU32(out, kMagic);
        out.push_back(protocolVersion);
        out.push_back(uint8_t(type));
        putU16(out, flags);
        putU32(out, sequence);
        putU32(out, sessionId);
        putU64(out, timestampUs);
        putU32(out, payloadLength);
    }

    static std::optional<WireHeader> decode(const uint8_t* d, size_t len) {
        if (len < kWireHeaderLength) return std::nullopt;
        uint32_t magic; std::memcpy(&magic, d, 4);
        auto be32 = [](const uint8_t* q) { return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3]; };
        if (be32(d) != kMagic) return std::nullopt;
        if (d[4] != kProtocolVersion) return std::nullopt;   // major mismatch → refuse (spec §32)
        WireHeader h;
        h.type = PacketType(d[5]);
        h.flags = uint16_t(be32(d + 6) >> 16);
        h.sequence = be32(d + 8);
        h.sessionId = be32(d + 12);
        uint64_t ts = 0; for (int i = 0; i < 8; ++i) ts = ts << 8 | d[16 + i];
        h.timestampUs = ts;
        h.payloadLength = be32(d + 24);
        return h;
    }
};

struct VideoFrameHeader {
    static constexpr uint8_t kFlagKeyframe = 0x01;
    static constexpr uint8_t kFlagHasParamSets = 0x02;

    uint32_t frameId; uint16_t packetId; uint16_t packetCount;
    uint8_t flags; uint8_t codec;          // 0 = H.264 annex-B
    uint16_t width, height; uint8_t fpsProfile;
    uint64_t captureTsUs, encodeTsUs;
    uint32_t payloadTotalLen;

    bool isKeyframe() const { return flags & kFlagKeyframe; }

    void encode(std::vector<uint8_t>& out) const {
        putU32(out, frameId); putU16(out, packetId); putU16(out, packetCount);
        out.push_back(flags); out.push_back(codec);
        putU16(out, width); putU16(out, height);
        out.push_back(fpsProfile); out.push_back(0);
        putU64(out, captureTsUs); putU64(out, encodeTsUs);
        putU32(out, payloadTotalLen);
    }

    static std::optional<VideoFrameHeader> decode(const uint8_t* d, size_t len) {
        if (len < kVideoFrameHeaderLength) return std::nullopt;
        auto be16 = [](const uint8_t* q) { return uint16_t(q[0] << 8 | q[1]); };
        auto be32 = [](const uint8_t* q) { return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3]; };
        auto be64 = [](const uint8_t* q) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | q[i]; return v; };
        VideoFrameHeader h;
        h.frameId = be32(d); h.packetId = be16(d + 4); h.packetCount = be16(d + 6);
        h.flags = d[8]; h.codec = d[9];
        h.width = be16(d + 10); h.height = be16(d + 12);
        h.fpsProfile = d[14];
        h.captureTsUs = be64(d + 16); h.encodeTsUs = be64(d + 24);
        h.payloadTotalLen = be32(d + 32);
        return h;
    }
};

// Capability negotiation JSON documents live in docs/protocol.md (spec §31).

} // namespace rdp
