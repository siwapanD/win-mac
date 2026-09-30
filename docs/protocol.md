# Wire Protocol — v1 (custom HP)

Canonical reference for the byte layout shared by `mac-host/Sources/MacHost/Protocol/`
(Swift, implementation + regression tests) and `windows-client/src/protocol/wire.h`
(C++ mirror). **Change only through this document; update both implementations
in the same commit** (spec §32: every packet is versioned, major mismatch ⇒ refuse).

## 1. Transport

| | Now (dev) | Production |
|---|---|---|
| Carrier | UDP datagrams | QUIC v1 (RFC 9000) + TLS 1.3 via MsQuic |
| Ports | host listens **55443/udp** (configurable, never 5900–5902); VNC proxy **55444/tcp** | same numbering; QUIC streams replace the reliable-flag UDP copies |
| Encryption | none (LAN dev only) | TLS 1.3, device pairing — see docs/security.md |
| MTU per datagram | 1200 B video payload chunks | QUIC datagrams (RFC 9221) |

## 2. Packet header — 28 bytes, big-endian, every packet

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic `0x52445048` "RDPh" | mismatch ⇒ silently drop |
| 4 | 1 | protocolVersion = 1 | ≠1 ⇒ refuse/fallback (spec §32) |
| 5 | 1 | packetType | table §3 |
| 6 | 2 | flags | bit0 = reliable |
| 8 | 4 | sequence | per-sender monotonic |
| 12 | 4 | sessionID | random u32 per session |
| 16 | 8 | timestampUs | **monotonic clock only** (spec §33 — never wall clock) |
| 24 | 4 | payloadLength | |

## 3. Packet types

| # | Type | Direction | Payload |
|---|---|---|---|
| 1 | Hello | C→H | JSON `{"deviceName","clientVersion","mode"}` |
| 2 | Capabilities | both | JSON §5 |
| 3 | VideoFrame | H→C | VideoFrameHeader (§4) + annex-B chunk |
| 4 | Audio | H→C | reserved (Phase 10, Opus) |
| 5 | InputMouse | C→H | kind u8, buttons u8 (bit0 left, bit1 right, bit2 mid), x f32, y f32 (normalized 0…1) |
| 6 | InputKey | C→H | down u8, macKeyCode u16, flags u64 (CGEventFlags raw) — **client maps VK→mac** (spec §18) |
| 7 | InputScroll | C→H | dx f32, dy f32 (lines; positive dy = scroll down) |
| 8 | CursorState | H→C | reserved (client renders cursor locally, spec §16) |
| 9 | Clipboard | both | reserved (QUIC stream in production) |
| 10 | Control | C→H | op u8: 1 requestKeyFrame, 2 setFrameRate, 3 setResolution |
| 11 | Heartbeat | both | sender's monotonic timestampUs u64 — receiver **echoes the payload unchanged**; sender computes RTT = now − ts (feeds the adaptive controller, spec §21) |
| 12 | Stats | both | reserved (telemetry) |

Reliable-flagged packets are sent 3× in dev UDP mode; QUIC moves them to
streams 1 (control), 2 (keyboard), 3 (clipboard) per spec §19.

## 4. VideoFrame payload header — 36 bytes

frameId u32 | packetId u16 | packetCount u16 | flags u8 (bit0 keyframe, bit1 hasParamSets) | codec u8 (0=h264) | width u16 | height u16 | fpsProfile u8 | reserved u8 | captureTsUs u64 | encodeTsUs u64 | payloadTotalLen u32

The host splits one annex-B frame into ≤1200 B chunks; each chunk repeats the
full header so the client can reassemble out of order and discard stale
frames (latest-frame-wins, spec §10/§65). Reassembly rules are regression-
tested on the host (`PacketizerTests`): frame older than the newest in-flight
**or completed** frame ⇒ drop; partial frame older than 250 ms ⇒ drop.

`captureTsUs` is the host's monotonic µs at capture instant — a client on the
same clock domain (loopback test) computes capture→received latency directly;
cross-machine use compares offset-corrected deltas per session.

## 5. Capability negotiation (spec §31)

Host → client:
```json
{"protocolVersion":1,"platform":"macOS","architecture":"arm64",
 "capture":{"screenCaptureKit":true,"maxWidth":1920,"maxHeight":1080,"maxFps":120,"hdr":false},
 "video":{"h264":true,"hevc":false,"av1":false},
 "appleScreenSharing":{"enabled":true,"nativeHighPerformanceEligible":true}}
```
Client → host:
```json
{"protocolVersion":1,
 "decode":{"h264Hardware":true,"hevcHardware":false,"maxFps":60,"hdr":false},
 "display":{"refreshRate":144}}
```
Session profile: fps = min(host fps, client maxFps); resolution/bitrate tiers
per spec §22 (4K 40–80 Mbps, 1440p 20–40, 1080p 10–25 on LAN).

## 6. Keyboard mapping table (spec §18 — fill during Windows client Phase 8)

Client maps Windows VK → macOS virtual keycode (`kVK_ANSI_*`) with
Mac-friendly mode: Ctrl→Command, Alt→Option, Win→Command (e.g. Ctrl+C =
Cmd+C keycode 0x08 + CGEventFlagCommand `1<<20`). Physical scancodes are
preserved for shortcut accuracy.

## 7. Keyframe triggers (spec §12)

Client requests `Control/RequestKeyFrame` on: packet-loss burst, decoder
desync, resolution change, network recovery, session resume. Host GOP is
keyframe every 1–2 s at 60 fps, 1 s at 120 fps.
