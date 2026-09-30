# remote-desktop — Windows → Mac High-Performance Screen Sharing

Cross-platform high-performance remote desktop engine with a VNC
compatibility layer, coexisting with Apple Native High Performance Screen
Sharing (spec: [docs/spec-original.md](docs/spec-original.md)).

Windows client → macOS host over a custom low-latency protocol:
ScreenCaptureKit capture → VideoToolbox H.264 hardware encode → datagram
transport → Media Foundation hardware decode → D3D flip-model render, with
local cursor and explicit VNC fallback.

## Repository

```
mac-host/          Swift package (macOS 13+, built/tested on macOS 26 Apple Silicon)
  Sources/MacHost/{Capture,Encode,Input,AppleSharing,Session,Transport,Protocol,Telemetry}
  Tests/MacHostTests/            wire/packetizer/state-machine regression tests
windows-client/    C++20 CMake project (build on Windows — see its README)
core/protocol/     (protocol lives in docs/protocol.md + mirrored sources)
vnc/               VNC fallback strategy
tests/             network-sim / long-run plans
docs/              architecture · protocol · benchmark · security · spec-original
```

## Quick start (mac host)

```bash
cd mac-host
swift build && swift test

swift run mac-host inspect          # diagnostics JSON + coexistence summary
swift run mac-host encode-test      # HW H.264 encoder check (no permission needed)
swift run mac-host capture-test     # SCK→encode, per-second stats (needs Screen Recording)
swift run mac-host serve            # HP engine on UDP 55443, Auto→VNC fallback
swift run mac-host serve --mode vnc # VNC proxy on TCP 55444 → macOS Screen Sharing

# loopback integration harness (terminal A + B):
swift run mac-host serve --mode hp
swift run mac-host client-test --seconds 6 --expect-video   # 1080p60 smoke (spec §62)
```

First run needs **Screen Recording** (and later **Accessibility**) granted to
the terminal/app that launches `mac-host`: System Settings → Privacy &
Security → Screen Recording / Accessibility. `inspect` reports exactly what is
missing (spec §27/§45).

## Status vs first milestone (spec §68)

- [x] IRemoteSession abstraction + RemoteSessionFactory (Auto/HP/VNC, explicit fallback reasons)
- [x] ScreenCaptureKit 1080p60 capture path (SCK → CVPixelBuffer, queueDepth 3, cursor off)
- [x] VideoToolbox H.264 **hardware** encode — verified: 1080p60, 300/300 frames, encode avg 5.5 ms, ffprobe/ffmpeg 0 decode errors
- [x] Wire protocol v1 + packetizer/reassembler (unit-tested; latest-frame-wins)
- [x] UDP LAN transport on custom port 55443 (QUIC/MsQuic = V1 DoD next step)
- [x] CGEvent injection + keyframe-request control path
- [x] Heartbeat echo → RTT measurement (~0.4 ms loopback) feeding the adaptive controller
- [x] AdaptiveQualityController (§21/§36): degrade fast / recover +5 % with hysteresis, live VT bitrate — 5 unit tests
- [x] `client-test` loopback harness: handshake + RTT verified end-to-end on this Mac; becomes the 1080p60 smoke test once Screen Recording is granted
- [x] Apple Screen Sharing coexistence inspector (detect-only, verified on this Mac)
- [x] VNC fallback via RFB proxy — verified end-to-end handshake
- [ ] Windows client: MF decode → D3D render → Raw Input (skeleton + implementation notes ready)
- [ ] QUIC/TLS 1.3 + pairing (security roadmap in docs/security.md)
- [ ] 30-minute stability run with real Windows client (pending client build)

## Roadmap (spec §41 phases)

Phase 1–2 instrumentation/abstraction: done. Phase 3–4 capture+encode: done
(verified). Phase 5 QUIC: next. Phase 6–8 Windows decode/render/input:
skeleton → implement. Phase 9 adaptive quality → Phase 10 audio/clipboard →
Phase 11 coexistence hardening → Phase 12 4K60 → Phase 13 120 FPS → Phase 14
internet mode. Engineering rules: spec §65 (latest frame wins, hardware only,
no unbounded queues, measure everything).
