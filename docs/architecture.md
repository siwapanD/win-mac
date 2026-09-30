# Architecture

Windows → Mac high-performance remote desktop. Full product spec:
`docs/spec-original.md` (authoritative for goals/limits); this file describes
what is **built** and how it maps to the target architecture.

## Target (spec §4) → current implementation

```
 macOS host (built, Swift/Sources/MacHost)                Windows client (skeleton)
┌────────────────────────────────────────┐   ┌────────────────────────────────────┐
│ Session manager + state machine §38    │   │ Session manager                    │
│ ├── AppleScreenSharingInspector §28 ✓  │   │ ├── HPClientSession (skeleton)     │
│ ├── HighPerformanceRemoteSession  ✓    │   │ ├── VNC adapter (uses proxy below) │
│ └── VncProxySession (RFB proxy)   ✓    │   │ └── RemoteSessionFactory           │
│                                        │   │                                    │
│ ScreenCaptureKitCapture §8        ✓    │   │ Network receiver (todo)            │
│   → CMSampleBuffer/CVPixelBuffer  ✓    │   │   → jitter/reorder/loss recovery   │
│ VideoToolboxEncoder §11/§12       ✓    │   │ MediaFoundationDecoder §14    (sk) │
│   H.264 HW, realtime, no reordering ✓  │   │   → D3D11 texture                  │
│ Packetizer (1200 B chunks) §33    ✓    │   │ D3DRenderer §15/§37           (sk) │
│ UDPTransport (QUIC later) §19     ✓    │   │ RawInputCapture §17           (sk) │
│ CGEventInjector §17/§18           ✓    │   │ Local cursor §16              (sk) │
│ IRemoteSession §6                 ✓    │   │ IRemoteSession mirror              │
└────────────────────────────────────────┘   └────────────────────────────────────┘
          UDP 55443 (video+input)  ·  TCP 55444 (VNC proxy fallback)
```
✓ built & exercised · (sk) skeleton with implementation notes

## Components (mac-host)

- **ScreenCaptureKitCapture** — SCStream, 1080p60 default, `queueDepth 3`,
  `showsCursor false` (cursor is client-local, §16). Never polls bitmaps.
- **VideoToolboxEncoder** — H.264 hardware-required, RealTime, no frame
  reordering, GOP = 2 s @60, annex-B output with SPS/PPS inlined on keyframes
  (IDR detection via NAL-type scan — the NotSync attachment proved unreliable).
- **Queue policy (§10)** — in-flight encoder frames ≤ 2; overflow drops the
  newest capture (old frames are worthless, §65). No unbounded queues.
- **UDPTransport** — NWListener on 55443; QUIC/MsQuic replaces it behind the
  same `DataTransport` interface (V1 DoD item, see roadmap).
- **HighPerformanceRemoteSession** — handshake (hello → capability JSON →
  negotiated fps), streaming, control (keyframe requests), input injection.
  State machine transitions per §38, illegal ones rejected.
- **RemoteSessionFactory** — Auto probe: Screen Recording + hardware encoder
  ⇒ HP; otherwise explicit VNC reason. HP connect: one retry, then explicit
  VNC fallback; user-forced HP fails loudly instead (§7/§51).
- **VncProxySession** — interim fallback: TCP proxy to macOS built-in RFB
  :5900 (requires Apple Screen Sharing enabled). Self-contained RFB server is
  tracked in `vnc/README.md`.
- **AppleScreenSharingInspector** — Apple Silicon, macOS version, permissions,
  HW-encoder probe, port 5900 reachability (screensharingd/ScreenSharing*),
  custom-port bindability. Detects only; never disables Apple services (§28).

## Threading (§43)

Mac: capture SCK queue · VT encoder callback queue · transport queue ·
stats timer. No encode/network work on the main thread.

## Latency accounting

All timestamps use `CLOCK_MONOTONIC_RAW` µs (§33). Current metrics: FPS,
Mbps, capture→encode latency, in-flight drops, keyframes. Client adds
decode/present/frame_age (§37) when the Windows pipeline lands.
