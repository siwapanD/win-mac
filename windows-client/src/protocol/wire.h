// Wire format mirror of mac-host Sources/MacHost/Protocol/Wire.swift.
// THE BYTE LAYOUTS IN THIS FILE MUST STAY IN SYNC WITH THE SWIFT IMPLEMENTATION.
// See docs/protocol.md — it is the canonical reference.
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr size_t   kVideoChunkMTU = 1200;        // Packetizer.defaultMTU on the host

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

enum class MouseKind : uint8_t {
    Move = 0,
    Down = 1,
    Up   = 2,
};

// Mouse button bits (MouseInput.buttons). The host only injects left/right;
// a bare middle bit is injected as LEFT by CGEventInjector, so never send it.
constexpr uint8_t kMouseLeft  = 0x01;
constexpr uint8_t kMouseRight = 0x02;

/// Monotonic microseconds (spec §33 — never wall clock). QPC is the Windows
/// equivalent of the host's CLOCK_MONOTONIC_RAW; the two clock domains are
/// unrelated, so cross-machine values are only compared as deltas.
inline uint64_t nowUs() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart);
    }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const uint64_t t = uint64_t(c.QuadPart);
    return (t / freq) * 1'000'000 + (t % freq) * 1'000'000 / freq;
}

// Big-endian helpers (spec §32/§33: network byte order).
inline void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}
inline void putU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
inline void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
inline void putF32(std::vector<uint8_t>& b, float f) {
    uint32_t u; std::memcpy(&u, &f, 4); putU32(b, u);
}

inline uint16_t be16(const uint8_t* q) { return uint16_t(q[0] << 8 | q[1]); }
inline uint32_t be32(const uint8_t* q) {
    return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
}
inline uint64_t be64(const uint8_t* q) {
    uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | q[i]; return v;
}
inline float beF32(const uint8_t* q) {
    uint32_t u = be32(q); float f; std::memcpy(&f, &u, 4); return f;
}

struct WireHeader {
    uint8_t  protocolVersion = kProtocolVersion;
    PacketType type = PacketType::Hello;
    uint16_t flags = 0;
    uint32_t sequence = 0;
    uint32_t sessionId = 0;
    uint64_t timestampUs = 0;   // monotonic (QueryPerformanceCounter)
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
        if (be32(d) != kMagic) return std::nullopt;
        if (d[4] != kProtocolVersion) return std::nullopt;   // major mismatch → refuse (spec §32)
        if (d[5] < uint8_t(PacketType::Hello) || d[5] > uint8_t(PacketType::Stats)) return std::nullopt;
        WireHeader h;
        h.type = PacketType(d[5]);
        h.flags = be16(d + 6);
        h.sequence = be32(d + 8);
        h.sessionId = be32(d + 12);
        h.timestampUs = be64(d + 16);
        h.payloadLength = be32(d + 24);
        return h;
    }
};

struct VideoFrameHeader {
    static constexpr uint8_t kFlagKeyframe = 0x01;
    static constexpr uint8_t kFlagHasParamSets = 0x02;

    uint32_t frameId = 0; uint16_t packetId = 0; uint16_t packetCount = 0;
    uint8_t flags = 0; uint8_t codec = 0;          // 0 = H.264 annex-B
    uint16_t width = 0, height = 0; uint8_t fpsProfile = 0;
    uint64_t captureTsUs = 0, encodeTsUs = 0;
    uint32_t payloadTotalLen = 0;

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
        VideoFrameHeader h;
        h.frameId = be32(d); h.packetId = be16(d + 4); h.packetCount = be16(d + 6);
        h.flags = d[8]; h.codec = d[9];
        h.width = be16(d + 10); h.height = be16(d + 12);
        h.fpsProfile = d[14];
        h.captureTsUs = be64(d + 16); h.encodeTsUs = be64(d + 24);
        h.payloadTotalLen = be32(d + 32);
        if (h.codec > 2) return std::nullopt;   // VideoCodec(rawValue:) on the host
        return h;
    }
};

// MARK: - Payloads (PacketFactory in mac-host Packetizer.swift)

/// kind u8 | buttons u8 | x f32 | y f32 — coordinates normalized 0…1.
inline std::vector<uint8_t> mousePayload(MouseKind kind, uint8_t buttons, float x, float y) {
    std::vector<uint8_t> p;
    p.push_back(uint8_t(kind)); p.push_back(buttons);
    putF32(p, x); putF32(p, y);
    return p;
}

/// down u8 | macKeyCode u16 | CGEventFlags u64.
inline std::vector<uint8_t> keyPayload(bool down, uint16_t macKeyCode, uint64_t flags) {
    std::vector<uint8_t> p;
    p.push_back(down ? 1 : 0); putU16(p, macKeyCode); putU64(p, flags);
    return p;
}

/// dx f32 | dy f32 in lines. Positive dy = scroll down; positive dx = scroll
/// LEFT (the host passes dx straight into CGEvent wheel2, whose positive
/// direction is left). The host truncates to whole lines.
inline std::vector<uint8_t> scrollPayload(float dx, float dy) {
    std::vector<uint8_t> p;
    putF32(p, dx); putF32(p, dy);
    return p;
}

inline std::vector<uint8_t> heartbeatPayload(uint64_t senderTsUs) {
    std::vector<uint8_t> p;
    putU64(p, senderTsUs);
    return p;
}

inline std::vector<uint8_t> buildPacket(PacketType type, const std::vector<uint8_t>& payload,
                                        uint32_t sessionId, uint32_t sequence,
                                        uint16_t flags = 0) {
    WireHeader h;
    h.type = type;
    h.flags = flags;
    h.sequence = sequence;
    h.sessionId = sessionId;
    h.timestampUs = nowUs();
    h.payloadLength = uint32_t(payload.size());
    std::vector<uint8_t> out;
    out.reserve(kWireHeaderLength + payload.size());
    h.encode(out);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// MARK: - Minimal JSON helpers for the hello/capability documents (spec §31).
// The documents are tiny and fixed-shape; a full parser is not warranted.

inline std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += char(c);
        }
    }
    return o;
}

/// Raw value text after `"key":` (first occurrence), or empty.
inline std::string jsonRawValue(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return {};
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
    size_t end = at;
    if (end < json.size() && json[end] == '"') {
        end = json.find('"', end + 1);
        return end == std::string::npos ? std::string{} : json.substr(at + 1, end - at - 1);
    }
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') ++end;
    return json.substr(at, end - at);
}

inline long long jsonInt(const std::string& json, const char* key, long long def) {
    const std::string v = jsonRawValue(json, key);
    if (v.empty()) return def;
    char* endp = nullptr;
    const long long n = std::strtoll(v.c_str(), &endp, 10);
    return endp == v.c_str() ? def : n;
}

inline bool jsonBool(const std::string& json, const char* key, bool def) {
    const std::string v = jsonRawValue(json, key);
    if (v.rfind("true", 0) == 0) return true;
    if (v.rfind("false", 0) == 0) return false;
    return def;
}

} // namespace rdp
