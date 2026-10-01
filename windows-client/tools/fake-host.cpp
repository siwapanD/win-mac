// fake-host — a Windows stand-in for `mac-host serve --mode hp`, so the whole
// Windows client pipeline (handshake → reassembly → MF decode → D3D present →
// input) can be exercised on one PC without a Mac.
//
// It speaks the same dev-UDP protocol as mac-host HighPerformanceRemoteSession:
//   hello → capabilities ×3, client capabilities → stream, Control/RequestKeyFrame
//   → next keyframe, heartbeat every 1 s (client echoes → RTT), input packets
//   decoded and logged the way CGEventInjector would receive them.
//
// Video: a 2-second synthetic clip (moving bar, colour bands, binary frame
// counter) is encoded once at startup with the Media Foundation H.264 encoder
// (no B-frames, GOP = 1 s, SPS/PPS on every IDR — like VideoToolbox in the
// host) and then looped at the requested fps. Timestamps are this PC's
// monotonic clock, so they are NOT comparable with a real Mac.
//
//   fake-host.exe [--bind 0.0.0.0] [--port 55443] [--width 1920] [--height 1080] [--fps 60]
//                 [--bitrate 12000000] [--loss <percent>] [--seconds <n>]

#include <initguid.h>
#include "src/input/MacKeyMap.h"
#include "src/protocol/wire.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>   // SIO_UDP_CONNRESET
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

using Microsoft::WRL::ComPtr;
using namespace rdp;

namespace {

struct Options {
    uint16_t port = kDefaultHPPort;
    std::string bind = "0.0.0.0";   // 127.0.0.1 = loopback only (no firewall prompt)
    int width = 1920, height = 1080, fps = 60;
    int bitrate = 12'000'000;
    double lossPercent = 0.0;
    int seconds = 0;
};

struct AccessUnit {
    std::vector<uint8_t> bytes;   // annex-B
    bool keyframe = false;
    bool hasParamSets = false;
};

// MARK: - synthetic clip

void fillFrame(std::vector<uint8_t>& nv12, int w, int h, int index, int count) {
    uint8_t* y = nv12.data();
    uint8_t* uv = nv12.data() + size_t(w) * h;
    for (int row = 0; row < h; ++row)
        for (int col = 0; col < w; ++col)
            y[size_t(row) * w + col] = uint8_t(40 + (col + row) * 160 / (w + h));
    // moving bar (motion → real P-frames)
    const int barW = w / 16, x0 = (index * (w - barW)) / (count - 1);
    for (int row = h / 4; row < h * 3 / 4; ++row)
        memset(y + size_t(row) * w + x0, 235, size_t(barW));
    // per-frame noise patch (bottom middle): incompressible detail pushes the
    // bitrate to a realistic level, so frames span many datagrams
    uint32_t s = 0x9E3779B9u ^ uint32_t(index) * 2654435761u;
    for (int row = h * 13 / 16; row < h * 15 / 16; ++row) {
        for (int col = w * 3 / 8; col < w * 5 / 8; ++col) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            y[size_t(row) * w + col] = uint8_t(16 + (s & 0xFF) * 219 / 255);
        }
    }
    // binary frame counter, 12 blocks top-left (white = 1)
    const int bs = (std::max)(16, w / 48);
    for (int bit = 0; bit < 12; ++bit) {
        const uint8_t v = (index >> (11 - bit)) & 1 ? 235 : 16;
        for (int row = bs / 2; row < bs / 2 + bs; ++row)
            memset(y + size_t(row) * w + bs / 2 + bit * (bs + 4), v, size_t(bs));
    }
    // chroma: reddish left third, neutral middle, bluish right third
    for (int row = 0; row < h / 2; ++row) {
        uint8_t* line = uv + size_t(row) * w;
        for (int col = 0; col < w / 2; ++col) {
            uint8_t u = 128, v = 128;
            if (col < w / 6) { u = 100; v = 200; }
            else if (col >= w / 3) { u = 200; v = 110; }
            line[col * 2] = u;
            line[col * 2 + 1] = v;
        }
    }
}

bool hasNal(const std::vector<uint8_t>& b, uint8_t type) {
    for (size_t i = 0; i + 3 < b.size(); ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && (b[i + 3] & 0x1F) == type) return true;
    }
    return false;
}

void setCodecValue(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

bool encodeClip(const Options& o, std::vector<AccessUnit>* out) {
    ComPtr<IMFTransform> enc;
    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enc));
    if (FAILED(hr)) { fprintf(stderr, "H.264 encoder MFT unavailable (0x%08lx)\n", hr); return false; }

    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(enc.As(&api))) {
        setCodecValue(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodecValue(api.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(o.bitrate));
        setCodecValue(api.Get(), CODECAPI_AVEncMPVGOPSize, ULONG(o.fps));
        setCodecValue(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setCodecValue(api.Get(), CODECAPI_AVLowLatencyMode, 1);
    }

    ComPtr<IMFMediaType> outType, inType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(o.bitrate));
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, UINT32(o.width), UINT32(o.height));
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, UINT32(o.fps), 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    hr = enc->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) { fprintf(stderr, "encoder SetOutputType failed (0x%08lx)\n", hr); return false; }

    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, UINT32(o.width), UINT32(o.height));
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, UINT32(o.fps), 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = enc->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) { fprintf(stderr, "encoder SetInputType failed (0x%08lx)\n", hr); return false; }

    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    MFT_OUTPUT_STREAM_INFO info{};
    enc->GetOutputStreamInfo(0, &info);
    const DWORD outSize = (std::max<DWORD>)(info.cbSize, DWORD(o.width) * o.height * 2);
    const bool mftAllocates = info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    std::vector<uint8_t> seqHeader;
    auto drain = [&]() -> bool {
        for (;;) {
            ComPtr<IMFSample> sample;
            if (!mftAllocates) {
                ComPtr<IMFMediaBuffer> buf;
                MFCreateMemoryBuffer(outSize, &buf);
                MFCreateSample(&sample);
                sample->AddBuffer(buf.Get());
            }
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.pSample = sample.Get();
            DWORD status = 0;
            hr = enc->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) odb.pEvents->Release();
            if (mftAllocates && odb.pSample) sample.Attach(odb.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(enc->GetOutputAvailableType(0, 0, &t))) enc->SetOutputType(0, t.Get(), 0);
                continue;
            }
            if (FAILED(hr)) { fprintf(stderr, "encoder ProcessOutput failed (0x%08lx)\n", hr); return false; }
            if (seqHeader.empty()) {
                ComPtr<IMFMediaType> cur;
                UINT32 n = 0;
                if (SUCCEEDED(enc->GetOutputCurrentType(0, &cur)) &&
                    SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n > 0) {
                    seqHeader.resize(n);
                    cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHeader.data(), n, nullptr);
                }
            }
            ComPtr<IMFMediaBuffer> contiguous;
            sample->ConvertToContiguousBuffer(&contiguous);
            BYTE* p = nullptr; DWORD len = 0;
            contiguous->Lock(&p, nullptr, &len);
            AccessUnit au;
            au.bytes.assign(p, p + len);
            contiguous->Unlock();
            au.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) || hasNal(au.bytes, 5);
            if (au.keyframe && !hasNal(au.bytes, 7) && !seqHeader.empty())
                au.bytes.insert(au.bytes.begin(), seqHeader.begin(), seqHeader.end());   // SPS/PPS in-band, like the host
            au.hasParamSets = hasNal(au.bytes, 7);
            out->push_back(std::move(au));
        }
    };

    const int count = o.fps * 2;
    std::vector<uint8_t> nv12(size_t(o.width) * o.height * 3 / 2);
    const LONGLONG dur = 10'000'000LL / o.fps;
    for (int i = 0; i < count; ++i) {
        fillFrame(nv12, o.width, o.height, i, count);
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(DWORD(nv12.size()), &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength(DWORD(nv12.size()));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(i * dur);
        s->SetSampleDuration(dur);
        if (api && i % o.fps == 0) setCodecValue(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        hr = enc->ProcessInput(0, s.Get(), 0);
        if (FAILED(hr)) { fprintf(stderr, "encoder ProcessInput failed (0x%08lx)\n", hr); return false; }
        if (!drain()) return false;
    }
    enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (!drain()) return false;
    if (out->empty() || !(*out)[0].keyframe) {
        fprintf(stderr, "encoder output does not start with a keyframe\n");
        return false;
    }
    return true;
}

// MARK: - host

class FakeHost {
public:
    explicit FakeHost(const Options& o) : o_(o) {}

    bool run(const std::vector<AccessUnit>& clip);

private:
    void rxLoop();
    void streamLoop(const std::vector<AccessUnit>& clip);
    void send(PacketType type, const std::vector<uint8_t>& payload, uint16_t flags = 0, int copies = 1);
    void sendRaw(const std::vector<uint8_t>& pkt);
    void handle(const uint8_t* d, size_t n, const sockaddr_in& from);
    void logInput(PacketType type, const uint8_t* p, size_t n);

    Options o_;
    SOCKET sock_ = INVALID_SOCKET;
    std::mutex peerMutex_;
    sockaddr_in peer_{};
    bool havePeer_ = false;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> forceKey_{false};
    std::atomic<uint32_t> seq_{0};
    uint32_t sessionId_ = 0;

    std::atomic<uint64_t> bytesSent_{0}, framesSent_{0}, keyReqs_{0}, inputEvents_{0};
    std::atomic<uint64_t> rttUsLast_{0}, rttSamples_{0};
    uint64_t lastMoveLogUs_ = 0;
    std::mt19937 rng_{12345};
};

void FakeHost::send(PacketType type, const std::vector<uint8_t>& payload, uint16_t flags, int copies) {
    const auto pkt = buildPacket(type, payload, sessionId_, ++seq_, flags);
    for (int i = 0; i < copies; ++i) sendRaw(pkt);
}

void FakeHost::sendRaw(const std::vector<uint8_t>& pkt) {
    sockaddr_in to;
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        if (!havePeer_) return;
        to = peer_;
    }
    sendto(sock_, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof to);
    bytesSent_ += pkt.size();
}

void FakeHost::logInput(PacketType type, const uint8_t* p, size_t n) {
    inputEvents_++;
    if (type == PacketType::InputMouse && n >= 10) {
        const char* kinds[] = {"move", "down", "up"};
        const uint8_t kind = p[0] < 3 ? p[0] : 0;
        const float x = beF32(p + 2), y = beF32(p + 6);
        const uint64_t now = nowUs();
        if (kind == 0 && now - lastMoveLogUs_ < 250'000) return;   // moves: at most 4/s in the log
        lastMoveLogUs_ = now;
        printf("input: mouse %-4s buttons=0x%02x at (%.3f, %.3f) → host px (%d, %d)\n", kinds[kind], p[1], x, y,
               int(x * o_.width), int(y * o_.height));
    } else if (type == PacketType::InputKey && n >= 11) {
        const uint16_t code = be16(p + 1);
        const uint64_t f = be64(p + 3);
        std::string mods;
        if (f & mac::kFlagCommand) mods += "Cmd+";
        if (f & mac::kFlagControl) mods += "Ctrl+";
        if (f & mac::kFlagOption) mods += "Opt+";
        if (f & mac::kFlagShift) mods += "Shift+";
        printf("input: key %-4s 0x%02X %-12s flags %s%s\n", p[0] ? "down" : "up", code, mac::keyName(code),
               mods.empty() ? "-" : mods.c_str(), (f & (mac::kFlagNumericPad | mac::kFlagSecondaryFn)) ? " (fn/numpad)" : "");
    } else if (type == PacketType::InputScroll && n >= 8) {
        printf("input: scroll dx=%+.0f dy=%+.0f lines (CGEvent wheel1=%d wheel2=%d)\n", beF32(p), beF32(p + 4),
               int(-beF32(p + 4)), int(beF32(p)));
    }
}

void FakeHost::handle(const uint8_t* d, size_t n, const sockaddr_in& from) {
    auto h = WireHeader::decode(d, n);
    if (!h) return;
    const uint8_t* p = d + kWireHeaderLength;
    const size_t pn = n - kWireHeaderLength;

    switch (h->type) {
    case PacketType::Hello: {
        bool newPeer;
        {
            std::lock_guard<std::mutex> lock(peerMutex_);
            newPeer = !havePeer_ || peer_.sin_addr.s_addr != from.sin_addr.s_addr || peer_.sin_port != from.sin_port;
            peer_ = from;   // like NWListener accept(): the newest client replaces the old one
            havePeer_ = true;
        }
        if (newPeer) {
            char ip[64];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            const std::string json(reinterpret_cast<const char*>(p), pn);
            printf("host: hello from %s (%s:%u, v%s)\n", jsonRawValue(json, "deviceName").c_str(), ip,
                   ntohs(from.sin_port), jsonRawValue(json, "clientVersion").c_str());
        }
        char caps[512];
        snprintf(caps, sizeof caps,
                 "{\"protocolVersion\":1,\"platform\":\"fake-host\",\"architecture\":\"x86_64\","
                 "\"capture\":{\"screenCaptureKit\":false,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFps\":%d,\"hdr\":false},"
                 "\"video\":{\"h264\":true,\"hevc\":false,\"av1\":false},"
                 "\"appleScreenSharing\":{\"enabled\":false,\"nativeHighPerformanceEligible\":false}}",
                 o_.width, o_.height, o_.fps);
        send(PacketType::Capabilities, std::vector<uint8_t>(caps, caps + strlen(caps)), kFlagReliable, 3);
        break;
    }
    case PacketType::Capabilities: {
        const std::string json(reinterpret_cast<const char*>(p), pn);
        if (!streaming_.exchange(true)) {
            const long long clientFps = jsonInt(json, "maxFps", o_.fps);
            printf("host: client negotiated (h264HW=%s, clientMaxFps=%lld, display=%lld Hz) — streaming %dx%d @ %lld fps\n",
                   jsonBool(json, "h264Hardware", false) ? "true" : "false", clientFps,
                   jsonInt(json, "refreshRate", 0), o_.width, o_.height, (std::min<long long>)(o_.fps, clientFps));
        }
        forceKey_ = true;   // a (re)joining client needs an IDR
        break;
    }
    case PacketType::Control:
        if (pn >= 1 && p[0] == uint8_t(ControlOp::RequestKeyFrame)) {
            keyReqs_++;
            forceKey_ = true;
        }
        break;
    case PacketType::Heartbeat:
        if (pn >= 8) {
            const uint64_t ts = be64(p);
            rttUsLast_ = nowUs() - ts;
            rttSamples_++;
        }
        break;
    case PacketType::InputMouse: case PacketType::InputKey: case PacketType::InputScroll:
        logInput(h->type, p, pn);
        break;
    default:
        break;
    }
}

void FakeHost::rxLoop() {
    std::vector<uint8_t> buf(65536);
    while (running_) {
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int n = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n > 0) handle(buf.data(), size_t(n), from);
    }
}

void FakeHost::streamLoop(const std::vector<AccessUnit>& clip) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const uint64_t periodUs = 1'000'000 / uint64_t(o_.fps);
    uint64_t next = nowUs();
    uint64_t lastBeat = 0, lastStats = nowUs();
    uint64_t lastBytes = 0, lastFrames = 0;
    size_t index = 0;
    uint32_t frameId = 0;
    std::uniform_real_distribution<double> uni(0.0, 100.0);

    while (running_) {
        next += periodUs;
        const uint64_t now = nowUs();
        if (next > now) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG((next - now) * 10);   // relative, 100 ns
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        } else if (now - next > 100'000) {
            next = now;   // fell far behind (e.g. debugger): don't burst
        }

        if (streaming_) {
            if (forceKey_.exchange(false) && !clip[index].keyframe) {
                // "Encode" a keyframe: jump to the next IDR in the looped clip.
                size_t k = index;
                do { k = (k + 1) % clip.size(); } while (!clip[k].keyframe);
                index = k;
            }
            const AccessUnit& au = clip[index];
            index = (index + 1) % clip.size();

            VideoFrameHeader fh;
            fh.frameId = ++frameId;
            fh.packetCount = uint16_t((au.bytes.size() + kVideoChunkMTU - 1) / kVideoChunkMTU);
            fh.flags = uint8_t((au.keyframe ? VideoFrameHeader::kFlagKeyframe : 0) |
                               (au.hasParamSets ? VideoFrameHeader::kFlagHasParamSets : 0));
            fh.codec = 0;
            fh.width = uint16_t(o_.width);
            fh.height = uint16_t(o_.height);
            fh.fpsProfile = uint8_t((std::min)(255, o_.fps));
            fh.captureTsUs = nowUs();
            fh.encodeTsUs = fh.captureTsUs + 4'000;   // synthetic: clip is pre-encoded
            fh.payloadTotalLen = uint32_t(au.bytes.size());
            for (uint16_t i = 0; i < fh.packetCount; ++i) {
                fh.packetId = i;
                const size_t off = size_t(i) * kVideoChunkMTU;
                const size_t len = (std::min)(kVideoChunkMTU, au.bytes.size() - off);
                std::vector<uint8_t> payload;
                payload.reserve(kVideoFrameHeaderLength + len);
                fh.encode(payload);
                payload.insert(payload.end(), au.bytes.begin() + off, au.bytes.begin() + off + len);
                const auto pkt = buildPacket(PacketType::VideoFrame, payload, sessionId_, ++seq_);
                if (o_.lossPercent > 0 && uni(rng_) < o_.lossPercent) continue;   // simulated loss
                sendRaw(pkt);
            }
            framesSent_++;

            const uint64_t t = nowUs();
            if (t - lastBeat >= 1'000'000) {
                lastBeat = t;
                send(PacketType::Heartbeat, heartbeatPayload(t));
            }
        }

        const uint64_t t = nowUs();
        if (t - lastStats >= 1'000'000) {
            const double secs = double(t - lastStats) / 1e6;
            lastStats = t;
            if (streaming_) {
                printf("HP  fps=%.1f  %.1f Mbps  rtt=%.2f ms (%llu samples)  keyframe-requests=%llu  input-events=%llu\n",
                       double(framesSent_ - lastFrames) / secs, double(bytesSent_ - lastBytes) * 8 / 1e6 / secs,
                       double(rttUsLast_) / 1000.0, static_cast<unsigned long long>(rttSamples_.load()),
                       static_cast<unsigned long long>(keyReqs_.load()),
                       static_cast<unsigned long long>(inputEvents_.load()));
            }
            lastFrames = framesSent_;
            lastBytes = bytesSent_;
        }
    }
    CloseHandle(timer);
}

bool FakeHost::run(const std::vector<AccessUnit>& clip) {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return false; }
    int sndBuf = 8 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndBuf), sizeof sndBuf);
    DWORD timeoutMs = 100;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    BOOL connReset = FALSE; DWORD bytes = 0;
    WSAIoctl(sock_, SIO_UDP_CONNRESET, &connReset, sizeof connReset, nullptr, 0, &bytes, nullptr, nullptr);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(o_.port);
    if (inet_pton(AF_INET, o_.bind.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid --bind address %s\n", o_.bind.c_str());
        return false;
    }
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        fprintf(stderr, "cannot bind UDP %s:%u (in use? WSA %d)\n", o_.bind.c_str(), o_.port, WSAGetLastError());
        return false;
    }
    std::random_device rd;
    do { sessionId_ = uint32_t(rd()); } while (sessionId_ == 0);

    printf("HP engine (fake): listening on UDP %u, waiting for client hello…\n", o_.port);
    std::thread rx([this] { rxLoop(); });
    std::thread tx([&] { streamLoop(clip); });
    if (o_.seconds > 0) {
        Sleep(DWORD(o_.seconds) * 1000);
    } else {
        for (;;) Sleep(1000);   // Ctrl+C ends the process
    }
    running_ = false;
    tx.join();
    rx.join();
    closesocket(sock_);
    printf("summary: frames sent %llu  keyframe requests %llu  input events %llu  rtt samples %llu  last rtt %.2f ms\n",
           static_cast<unsigned long long>(framesSent_.load()), static_cast<unsigned long long>(keyReqs_.load()),
           static_cast<unsigned long long>(inputEvents_.load()), static_cast<unsigned long long>(rttSamples_.load()),
           double(rttUsLast_) / 1000.0);
    return true;
}

bool parseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--port" && (v = next())) o->port = uint16_t(atoi(v));
        else if (a == "--bind" && (v = next())) o->bind = v;
        else if (a == "--width" && (v = next())) o->width = atoi(v) & ~1;
        else if (a == "--height" && (v = next())) o->height = atoi(v) & ~1;
        else if (a == "--fps" && (v = next())) o->fps = atoi(v);
        else if (a == "--bitrate" && (v = next())) o->bitrate = atoi(v);
        else if (a == "--loss" && (v = next())) o->lossPercent = atof(v);
        else if (a == "--seconds" && (v = next())) o->seconds = atoi(v);
        else return false;
    }
    return o->port && o->width >= 64 && o->height >= 64 && o->fps >= 1 && o->fps <= 240;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, &o)) {
        printf("usage: fake-host.exe [--bind 0.0.0.0] [--port 55443] [--width 1920] [--height 1080] [--fps 60]\n"
               "                     [--bitrate 12000000] [--loss <percent>] [--seconds <n>]\n");
        return 64;
    }
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);   // live log even when redirected to a file
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    printf("fake-host: encoding a %d-frame %dx%d clip (H.264 High, CBR %d Mbps, GOP %d, no B-frames)…\n",
           o.fps * 2, o.width, o.height, o.bitrate / 1'000'000, o.fps);
    std::vector<AccessUnit> clip;
    const uint64_t t0 = nowUs();
    if (!encodeClip(o, &clip)) return 3;
    size_t keys = 0, bytes = 0, keyBytes = 0;
    for (const auto& au : clip) {
        keys += au.keyframe;
        bytes += au.bytes.size();
        if (au.keyframe) keyBytes += au.bytes.size();
    }
    printf("fake-host: %zu access units (%zu keyframes ~%.0f KB / %zu datagrams each, %.1f KB avg) in %.1f s\n",
           clip.size(), keys, double(keyBytes) / double(keys) / 1024.0,
           (keyBytes / keys + kVideoChunkMTU - 1) / kVideoChunkMTU,
           double(bytes) / double(clip.size()) / 1024.0, double(nowUs() - t0) / 1e6);
    if (o.lossPercent > 0) printf("fake-host: simulating %.2f%% video packet loss\n", o.lossPercent);

    FakeHost host(o);
    const bool ok = host.run(clip);
    MFShutdown();
    CoUninitialize();
    return ok ? 0 : 1;
}
