// Implementation notes (skeleton — see header):
//
// connect():
//   1. WSAStartup; socket(AF_INET, SOCK_DGRAM) — the client sends first so no
//      bind is needed; the host replies to our ephemeral port.
//   2. sendHello(): PacketType::Hello JSON {deviceName, clientVersion} on the
//      reliable path (flag set, 3× copies until QUIC lands — mirrors host).
//   3. Await host Capabilities JSON; reply with ClientCapabilitiesJson
//      (h264Hardware from IDXGI VideoDevice probing — spec §31).
//
// receiveLoop(): recvfrom → ingestDatagram. Bind the thread to
//   THREAD_PRIORITY_TIME_CRITICAL; no allocation on the hot path (use the
//   buffer pool — spec §44).
//
// ingestDatagram(): mirrors mac-host FrameReassembler —
//   WireHeader::decode (reject wrong magic/version), VideoFrameHeader::decode,
//   accumulate chunks; when chunks.count == packetCount && receivedBytes ==
//   payloadTotalLen → concatenate and call onFrame_. frameId < max(last
//   completed, newest in-flight) → drop (latest frame wins, spec §65.2).
//
// Loss handling (spec §34): small loss → drop that frame and continue;
// burst loss (e.g. ≥3 consecutive incomplete frames or a keyframe lost) →
// send ControlOp::RequestKeyFrame. FEC is optional/later.
//
// requestKeyFrame(): Control packet, reliable-flagged, 3×.

#include "HPClientSession.h"

namespace rdp {

bool HPClientSession::connect(const char* host, uint16_t port) {
    // TODO(windows): WinSock flow per notes. Compile target: VS2022 C++20.
    (void)host; (void)port;
    return false;
}

void HPClientSession::disconnect() {}

void HPClientSession::receiveLoop() {}

void HPClientSession::sendHello() {}

void HPClientSession::requestKeyFrame() {}

void HPClientSession::sendInput(const uint8_t* wirePacket, size_t len) {
    (void)wirePacket; (void)len;
}

bool HPClientSession::ingestDatagram(const uint8_t* data, int len) {
    (void)data; (void)len;
    return false;
}

} // namespace rdp
