// Windows client HP session (spec §4 right column / §31 / §34):
// WinSock UDP socket → hello + capabilities → receive packetized video →
// reassemble (latest-frame-wins) → Media Foundation decode → D3D present.
// Input flows out via RawInputCapture; keyframe requests on packet-loss burst.
//
// STATUS: skeleton — see windows-client/README.md.
#pragma once

#include "src/protocol/wire.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace rdp {

struct ClientCapabilitiesJson {
    // Serialized per docs/protocol.md §5 (spec §31 Windows reply).
    bool h264Hardware = true;
    bool hevcHardware = false;
    int maxFps = 60;
    int displayRefreshRate = 60;
};

class HPClientSession {
public:
    using OnVideoFrame = std::function<void(const uint8_t* annexB, size_t len,
                                            const VideoFrameHeader& meta)>;

    bool connect(const char* host, uint16_t port);
    void disconnect();
    void receiveLoop();                 // network receive thread (spec §43)
    void requestKeyFrame();
    void sendHello();
    // Input packets are produced by RawInputCapture and sent through this
    // session: mouse (unreliable latest-state), key/scroll (reliable, 3×
    // until QUIC replaces UDP). Wire encoding: PacketFactory in mac-host.
    void sendInput(const uint8_t* wirePacket, size_t len);

private:
    struct Partial {
        VideoFrameHeader meta{};
        std::map<uint16_t, std::vector<uint8_t>> chunks;
        size_t receivedBytes = 0;
        uint64_t arrivedAtUs = 0;
    };

    // Same reassembly rules as the host's FrameReassembler (tested in Swift):
    // complete frames out of order, drop stale, drop partials older than 250 ms.
    bool ingestDatagram(const uint8_t* data, int len);

    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in serverAddr_{};
    uint32_t sessionId_ = 0;
    uint32_t txSequence_ = 0;
    uint32_t lastCompletedFrame_ = 0;
    std::map<uint32_t, Partial> partials_;
    ClientCapabilitiesJson caps_;
    OnVideoFrame onFrame_;
};

} // namespace rdp
