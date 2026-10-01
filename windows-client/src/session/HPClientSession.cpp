// HP client session — see header. Mirrors the host side of the dev UDP
// transport (mac-host UDPTransport + HighPerformanceRemoteSession) and the
// Swift loopback harness (`mac-host client-test`).
//
// Loss handling (spec §34): video is never retransmitted. A missing frame is
// reported to the decode thread as `lostBefore`, which keeps decoding (small
// loss) and asks for a keyframe; requests are rate-limited so a loss burst
// costs one keyframe, not one per frame. A partial frame that times out
// (250 ms) triggers a keyframe request immediately, because on a static Mac
// screen no later frame may arrive to reveal the gap.

#include "HPClientSession.h"

#include <mswsock.h>   // SIO_UDP_CONNRESET
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <random>

#pragma comment(lib, "ws2_32.lib")

namespace rdp {

namespace {

constexpr uint64_t kHandshakeRetryUs = 1'000'000;
constexpr uint64_t kHostSilentUs = 3'000'000;
constexpr uint64_t kKeyframeRequestMinIntervalUs = 300'000;
constexpr uint64_t kKeyframeRequestMaxIntervalUs = 2'000'000;   // = host GOP at 60 fps
constexpr size_t kMaxFrameBytes = 16u << 20;

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string deviceName() {
    wchar_t buf[256]; DWORD len = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &len)) return utf8(buf);
    return "windows-client";
}

bool winsockReady() {
    static const bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

} // namespace

HPClientSession::~HPClientSession() { disconnect(); }

bool HPClientSession::connect(const std::string& host, uint16_t port, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        return false;
    };
    if (!winsockReady()) return fail("WSAStartup failed");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res); rc != 0 || !res) {
        return fail("cannot resolve host '" + host + "' (getaddrinfo " + std::to_string(rc) + ")");
    }
    // Prefer IPv4 (the dev host is reached by LAN IPv4 address in practice).
    for (int pass = 0; pass < 2 && sock_ == INVALID_SOCKET; ++pass) {
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            if ((pass == 0) != (ai->ai_family == AF_INET)) continue;
            SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            // UDP connect: fixes the peer for send() and filters inbound to the host only.
            if (::connect(s, ai->ai_addr, int(ai->ai_addrlen)) == 0) { sock_ = s; break; }
            closesocket(s);
        }
    }
    freeaddrinfo(res);
    if (sock_ == INVALID_SOCKET) return fail("cannot open UDP socket to " + host);

    int rcvBuf = 8 * 1024 * 1024;   // keyframe bursts arrive in one go (no pacing yet, spec §35)
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof rcvBuf);
    DWORD timeoutMs = 50;           // lets the rx loop run periodic work while idle
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);
    // An ICMP port-unreachable (host not running yet) must not poison recv().
    BOOL connReset = FALSE; DWORD bytes = 0;
    WSAIoctl(sock_, SIO_UDP_CONNRESET, &connReset, sizeof connReset, nullptr, 0, &bytes, nullptr, nullptr);

    std::random_device rd;
    do { sessionId_ = uint32_t(rd()); } while (sessionId_ == 0);
    hostLabel_ = host + ":" + portStr;

    running_ = true;
    rxThread_ = std::thread([this] { receiveLoop(); });
    inputThread_ = std::thread([this] { inputLoop(); });
    return true;
}

void HPClientSession::disconnect() {
    if (!running_.exchange(false)) return;
    inputCv_.notify_all();
    if (inputThread_.joinable()) inputThread_.join();   // drains queued key-ups first (spec §49)
    if (rxThread_.joinable()) rxThread_.join();
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
    handshakeDone_ = false;
}

uint64_t HPClientSession::usSinceLastPacket() const {
    const uint64_t last = lastPacketUs_.load();
    return last ? nowUs() - last : 0;
}

std::string HPClientSession::hostSummary() const {
    std::lock_guard<std::mutex> lock(hostInfoMutex_);
    if (hostCapsJson_.empty()) return {};
    char buf[256];
    snprintf(buf, sizeof buf, "%s/%s capture %lldx%lld maxFps=%lld h264=%s",
             jsonRawValue(hostCapsJson_, "platform").c_str(),
             jsonRawValue(hostCapsJson_, "architecture").c_str(),
             jsonInt(hostCapsJson_, "maxWidth", 0), jsonInt(hostCapsJson_, "maxHeight", 0),
             jsonInt(hostCapsJson_, "maxFps", 0),
             jsonBool(hostCapsJson_, "h264", false) ? "yes" : "no");
    return buf;
}

// MARK: - network thread

void HPClientSession::receiveLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::vector<uint8_t> buf(65536);
    sendHandshake();

    while (running_) {
        const int n = recv(sock_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        const uint64_t now = nowUs();
        if (n > 0) {
            lastPacketUs_ = now;
            stats_.packetsReceived++;
            stats_.bytesReceived += uint64_t(n);
            handleDatagram(buf.data(), size_t(n));
        } else if (n == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAECONNRESET && err != WSAEMSGSIZE) {
                if (!running_) break;
                Sleep(5);   // unexpected socket error: don't spin
            }
        }

        // Re-handshake until the host answers, and again if it goes silent
        // (host restarted → it waits for a fresh hello + capabilities).
        const uint64_t last = lastPacketUs_.load();
        const bool silent = last != 0 && now - last > kHostSilentUs;
        if (silent && handshakeDone_.exchange(false)) {
            printf("session: host silent for %.1f s — re-handshaking\n", double(now - last) / 1e6);
        }
        if (!handshakeDone_ && now - lastHandshakeSendUs_ > kHandshakeRetryUs) sendHandshake();

        dropStalePartials(now);
    }
}

void HPClientSession::handleDatagram(const uint8_t* data, size_t len) {
    auto h = WireHeader::decode(data, len);
    if (!h) return;   // wrong magic/version: silently drop (docs/protocol.md §2)
    const uint8_t* payload = data + kWireHeaderLength;
    const size_t plen = len - kWireHeaderLength;

    if (h->sessionId != hostSessionId_) {
        if (hostSessionId_ != 0) {
            printf("session: host session changed (%08x → %08x) — resetting video state\n",
                   hostSessionId_, h->sessionId);
            resetVideoState();
            pendingHostSessionChange_ = true;
        }
        hostSessionId_ = h->sessionId;
    }

    switch (h->type) {
    case PacketType::Capabilities: {
        std::string json(reinterpret_cast<const char*>(payload), plen);
        {
            std::lock_guard<std::mutex> lock(hostInfoMutex_);
            hostCapsJson_ = json;
        }
        if (!handshakeDone_.exchange(true)) {
            printf("handshake: host caps — %s\n", hostSummary().c_str());
            requestKeyFrame("session start");
        }
        break;
    }
    case PacketType::Heartbeat:
        // Echo protocol: the host sent its monotonic µs; returning the payload
        // unchanged lets it compute RTT for the adaptive controller (spec §21).
        if (plen >= 8) {
            sendPacket(PacketType::Heartbeat, std::vector<uint8_t>(payload, payload + 8));
            stats_.heartbeatsEchoed++;
        }
        break;
    case PacketType::VideoFrame:
        ingestVideo(*h, payload, plen);
        break;
    default:
        break;   // CursorState / Clipboard / Audio / Stats: reserved
    }
}

void HPClientSession::ingestVideo(const WireHeader&, const uint8_t* payload, size_t len) {
    auto fh = VideoFrameHeader::decode(payload, len);
    if (!fh || fh->packetCount == 0 || fh->packetId >= fh->packetCount) return;
    if (fh->payloadTotalLen == 0 || fh->payloadTotalLen > kMaxFrameBytes) return;
    const uint8_t* chunk = payload + kVideoFrameHeaderLength;
    const size_t clen = len - kVideoFrameHeaderLength;
    const size_t total = fh->payloadTotalLen;

    const uint64_t now = nowUs();
    dropStalePartials(now);

    const uint32_t fid = fh->frameId;
    if (lastCompletedFrame_ != 0 && fid == lastCompletedFrame_) return;   // late duplicate
    const uint32_t newestInFlight = partials_.empty() ? 0 : partials_.rbegin()->first;
    const uint32_t newest = (std::max)(newestInFlight, lastCompletedFrame_);
    if (newest > 0 && fid < newest) {
        stats_.framesDroppedStale++;   // latest frame wins (spec §65.2)
        partials_.erase(fid);
        return;
    }

    // Chunk placement: every chunk but the last has the same size, so its
    // offset is id × len; the last chunk ends exactly at payloadTotalLen.
    size_t offset;
    if (fh->packetId + 1u == fh->packetCount) {
        if (clen > total) return;
        offset = total - clen;
    } else {
        offset = size_t(fh->packetId) * clen;
    }
    if (offset + clen > total) return;

    auto [it, inserted] = partials_.try_emplace(fid);
    Partial& p = it->second;
    if (inserted) {
        p.meta = *fh;
        p.data.resize(total);
        p.have.assign(fh->packetCount, false);
        p.arrivedAtUs = now;
    } else if (p.meta.packetCount != fh->packetCount || p.meta.payloadTotalLen != fh->payloadTotalLen) {
        return;   // inconsistent chunk for this frame id
    }
    if (p.have[fh->packetId]) return;
    std::memcpy(p.data.data() + offset, chunk, clen);
    p.have[fh->packetId] = true;
    p.receivedChunks++;
    p.receivedBytes += clen;
    if (p.receivedChunks != p.meta.packetCount || p.receivedBytes != total) return;

    CompletedFrame f;
    f.meta = p.meta;
    f.annexB = std::move(p.data);
    f.completedUs = now;
    f.hostSessionChanged = pendingHostSessionChange_;
    if (!pendingHostSessionChange_ && lastDeliveredFrame_ != 0 && fid > lastDeliveredFrame_ + 1) {
        f.lostBefore = fid - lastDeliveredFrame_ - 1;
    }
    pendingHostSessionChange_ = false;

    // Everything older than the completed frame is now worthless.
    partials_.erase(partials_.begin(), std::next(it));
    lastCompletedFrame_ = fid;
    lastDeliveredFrame_ = fid;

    if (f.meta.isKeyframe()) {   // requests are getting through: back to fast recovery
        keyframeSinceRequest_ = true;
        keyframeIntervalUs_ = kKeyframeRequestMinIntervalUs;
    }
    stats_.framesCompleted++;
    stats_.framesLost += f.lostBefore;
    if (f.meta.captureTsUs != 0 && f.meta.encodeTsUs >= f.meta.captureTsUs) {
        stats_.hostEncodeUsSum += f.meta.encodeTsUs - f.meta.captureTsUs;
        stats_.hostEncodeSamples++;
    }
    if (onFrame_) onFrame_(std::move(f));
}

void HPClientSession::dropStalePartials(uint64_t now) {
    bool lostNewer = false;
    for (auto it = partials_.begin(); it != partials_.end();) {
        if (now > it->second.arrivedAtUs + kStaleAfterUs) {
            if (it->first > lastCompletedFrame_) lostNewer = true;
            it = partials_.erase(it);
        } else {
            ++it;
        }
    }
    if (lostNewer) requestKeyFrame("incomplete frame timed out");
}

void HPClientSession::resetVideoState() {
    partials_.clear();
    lastCompletedFrame_ = 0;
    lastDeliveredFrame_ = 0;
}

void HPClientSession::sendHandshake() {
    const std::string hello =
        "{\"deviceName\":\"" + jsonEscape(deviceName()) + "\",\"clientVersion\":\"0.2\",\"mode\":\"hp\"}";
    char caps[256];
    snprintf(caps, sizeof caps,
             "{\"protocolVersion\":1,\"decode\":{\"h264Hardware\":%s,\"hevcHardware\":%s,"
             "\"maxFps\":%d,\"hdr\":false},\"display\":{\"refreshRate\":%d}}",
             caps_.h264Hardware ? "true" : "false", caps_.hevcHardware ? "true" : "false",
             caps_.maxFps, caps_.displayRefreshRate);
    const std::vector<uint8_t> helloBytes(hello.begin(), hello.end());
    const std::vector<uint8_t> capsBytes(caps, caps + strlen(caps));
    // Dev UDP: reliable-flagged JSON goes out 3× (same as the host's caps reply).
    sendPacket(PacketType::Hello, helloBytes, kFlagReliable, 3);
    sendPacket(PacketType::Capabilities, capsBytes, kFlagReliable, 3);
    lastHandshakeSendUs_ = nowUs();
}

void HPClientSession::requestKeyFrame(const char* reason) {
    if (!running_) return;
    const uint64_t now = nowUs();
    uint64_t last = lastKeyframeRequestUs_.load();
    const uint64_t interval = keyframeIntervalUs_.load();
    if (last != 0 && now - last < interval) return;
    if (!lastKeyframeRequestUs_.compare_exchange_strong(last, now)) return;
    // Backoff: if no complete keyframe arrived since the previous request, the
    // loss rate is eating keyframes (the largest frames, most datagrams) —
    // asking faster would only add bitrate and make it worse (keyframe storm).
    if (last != 0 && !keyframeSinceRequest_.exchange(false))
        keyframeIntervalUs_ = (std::min)(interval * 2, kKeyframeRequestMaxIntervalUs);
    // Idempotent on the host (sets forceKeyframe), so the 3× copies are safe.
    sendPacket(PacketType::Control, {uint8_t(ControlOp::RequestKeyFrame)}, kFlagReliable, 3);
    stats_.keyframeRequests++;
    printf("session: keyframe requested (%s)\n", reason);
}

void HPClientSession::sendPacket(PacketType type, const std::vector<uint8_t>& payload,
                                 uint16_t flags, int copies) {
    if (sock_ == INVALID_SOCKET) return;
    const auto pkt = buildPacket(type, payload, sessionId_, ++txSequence_, flags);
    for (int i = 0; i < copies; ++i) {
        send(sock_, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0);
    }
}

// MARK: - input thread

void HPClientSession::sendMouse(MouseKind kind, uint8_t buttons, float x, float y) {
    // Button-up goes out twice: the host has no dedupe and no reliable channel
    // yet (QUIC stream 2 replaces this), and a lost up = a stuck drag. A second
    // up is a no-op on macOS; a second down would not be, so downs go once.
    enqueueInput({PacketType::InputMouse, mousePayload(kind, buttons, x, y),
                  kind == MouseKind::Up ? 2 : 1},
                 kind == MouseKind::Move);
}

void HPClientSession::sendKey(bool down, uint16_t macKeyCode, uint64_t flags) {
    enqueueInput({PacketType::InputKey, keyPayload(down, macKeyCode, flags), down ? 1 : 2}, false);
}

void HPClientSession::sendScroll(float dx, float dy) {
    enqueueInput({PacketType::InputScroll, scrollPayload(dx, dy), 1}, false);
}

void HPClientSession::enqueueInput(InputEvent&& ev, bool coalesceMove) {
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        if (!running_) return;
        if (coalesceMove && !inputQueue_.empty()) {
            InputEvent& back = inputQueue_.back();
            if (back.type == PacketType::InputMouse && back.payload[0] == uint8_t(MouseKind::Move)) {
                back = std::move(ev);   // latest-state: only the newest position matters
                return;
            }
        }
        inputQueue_.push_back(std::move(ev));
    }
    inputCv_.notify_one();
}

void HPClientSession::inputLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::deque<InputEvent> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(inputMutex_);
            inputCv_.wait(lock, [&] { return !inputQueue_.empty() || !running_; });
            if (inputQueue_.empty() && !running_) return;
            batch.swap(inputQueue_);
        }
        for (const InputEvent& ev : batch) {
            sendPacket(ev.type, ev.payload, 0, ev.copies);
            stats_.inputPacketsSent += uint64_t(ev.copies);
        }
        batch.clear();
    }
}

} // namespace rdp
