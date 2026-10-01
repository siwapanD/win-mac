// Windows client HP session (spec §4 right column / §31 / §34):
// WinSock UDP socket → hello + capabilities → receive packetized video →
// reassemble (latest-frame-wins) → hand complete annex-B frames to the decode
// thread. Echoes host heartbeats (the host measures RTT). Input packets go
// out on a dedicated input thread (spec §43).
#pragma once

#include "src/protocol/wire.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rdp {

struct ClientCapabilities {
    // Serialized per docs/protocol.md §5 (spec §31 Windows reply).
    bool h264Hardware = true;
    bool hevcHardware = false;
    int maxFps = 60;
    int displayRefreshRate = 60;
};

/// One reassembled access unit, handed to the decode thread.
struct CompletedFrame {
    VideoFrameHeader meta{};
    std::vector<uint8_t> annexB;
    uint64_t completedUs = 0;        // client clock: last packet arrived
    uint32_t lostBefore = 0;         // frames missing between this and the previous delivered one
    bool hostSessionChanged = false; // host restarted: decoder must reset
};

class HPClientSession {
public:
    using OnVideoFrame = std::function<void(CompletedFrame&&)>;

    struct Stats {
        std::atomic<uint64_t> packetsReceived{0};
        std::atomic<uint64_t> bytesReceived{0};
        std::atomic<uint64_t> framesCompleted{0};
        std::atomic<uint64_t> framesLost{0};          // sequence gaps + stale partials
        std::atomic<uint64_t> framesDroppedStale{0};  // arrived after a newer frame
        std::atomic<uint64_t> keyframeRequests{0};
        std::atomic<uint64_t> heartbeatsEchoed{0};
        std::atomic<uint64_t> hostEncodeUsSum{0};     // host clock: encodeTs − captureTs
        std::atomic<uint64_t> hostEncodeSamples{0};
        std::atomic<uint64_t> inputPacketsSent{0};
    };

    HPClientSession() = default;
    ~HPClientSession();
    HPClientSession(const HPClientSession&) = delete;
    HPClientSession& operator=(const HPClientSession&) = delete;

    void setOnVideoFrame(OnVideoFrame cb) { onFrame_ = std::move(cb); }
    void setCapabilities(const ClientCapabilities& caps) { caps_ = caps; }

    /// Resolve host, open the socket, start network + input threads and the
    /// hello/capabilities handshake. Returns false with `error` on failure.
    bool connect(const std::string& host, uint16_t port, std::string* error);
    void disconnect();

    bool handshakeComplete() const { return handshakeDone_.load(); }
    /// µs since the last packet from the host (0 if nothing received yet).
    uint64_t usSinceLastPacket() const;
    std::string hostSummary() const;
    const Stats& stats() const { return stats_; }

    /// Control/RequestKeyFrame (spec §12/§34). Rate-limited; safe from any thread.
    void requestKeyFrame(const char* reason);

    // Input (spec §17). Queued to the input thread; mouse moves coalesce to the
    // latest position (unreliable latest-state channel).
    void sendMouse(MouseKind kind, uint8_t buttons, float x, float y);
    void sendKey(bool down, uint16_t macKeyCode, uint64_t flags);
    void sendScroll(float dx, float dy);

private:
    struct Partial {
        VideoFrameHeader meta{};
        std::vector<uint8_t> data;   // payloadTotalLen bytes, chunks placed by offset
        std::vector<bool> have;
        uint32_t receivedChunks = 0;
        size_t receivedBytes = 0;
        uint64_t arrivedAtUs = 0;
    };

    struct InputEvent {
        PacketType type;
        std::vector<uint8_t> payload;
        int copies = 1;
    };

    void receiveLoop();      // network receive thread (spec §43)
    void inputLoop();        // input send thread (spec §43)
    void handleDatagram(const uint8_t* data, size_t len);
    void ingestVideo(const WireHeader& h, const uint8_t* payload, size_t len);
    void dropStalePartials(uint64_t now);
    void resetVideoState();
    void sendHandshake();
    void sendPacket(PacketType type, const std::vector<uint8_t>& payload,
                    uint16_t flags = 0, int copies = 1);
    void enqueueInput(InputEvent&& ev, bool coalesceMove);

    SOCKET sock_ = INVALID_SOCKET;
    std::string hostLabel_;
    ClientCapabilities caps_;
    OnVideoFrame onFrame_;
    uint32_t sessionId_ = 0;
    std::atomic<uint32_t> txSequence_{0};

    std::thread rxThread_;
    std::thread inputThread_;
    std::atomic<bool> running_{false};

    // Handshake / liveness (rx thread owns writes except where atomic).
    std::atomic<bool> handshakeDone_{false};
    std::atomic<uint64_t> lastPacketUs_{0};
    uint64_t lastHandshakeSendUs_ = 0;
    uint32_t hostSessionId_ = 0;
    mutable std::mutex hostInfoMutex_;
    std::string hostCapsJson_;

    // Reassembly (rx thread only). Same rules as the host's FrameReassembler:
    // frame older than newest in-flight or completed → drop; partial older
    // than 250 ms → drop (latest frame wins, spec §65.2).
    std::map<uint32_t, Partial> partials_;
    uint32_t lastCompletedFrame_ = 0;
    uint32_t lastDeliveredFrame_ = 0;
    bool pendingHostSessionChange_ = false;
    static constexpr uint64_t kStaleAfterUs = 250'000;

    std::atomic<uint64_t> lastKeyframeRequestUs_{0};
    std::atomic<uint64_t> keyframeIntervalUs_{300'000};   // backs off to 2 s under heavy loss
    std::atomic<bool> keyframeSinceRequest_{true};

    // Input queue.
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::deque<InputEvent> inputQueue_;

    Stats stats_;
};

} // namespace rdp
