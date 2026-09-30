# Windows → macOS High-Performance Screen Sharing
## Migration Plan: จาก VNC ไปสู่ Low-Latency Remote Desktop พร้อมรองรับ Apple High Performance Screen Sharing

> เป้าหมายของเอกสารนี้คือใช้เป็น Development Specification / AI Coding Prompt เพื่อปรับปรุงแอป Windows → macOS ที่ปัจจุบันใช้ VNC ให้มี FPS สูง, latency ต่ำ, ภาพคม, mouse/keyboard ตอบสนองเร็ว และให้ประสบการณ์ใกล้เคียง macOS → macOS Screen Sharing มากที่สุด โดยต้องไม่ทำลายความสามารถ VNC เดิมและต้องมี fallback ได้เสมอ

---

## 1. Product Goal

พัฒนา Remote Desktop ระหว่าง Windows และ macOS จากระบบเดิมที่ใช้ VNC ให้เป็นระบบแบบ multi-protocol ดังนี้

1. **High Performance Compatible Mode — ค่าเริ่มต้นสำหรับ Windows → Mac**
   - ใช้ ScreenCaptureKit บน macOS
   - ใช้ VideoToolbox hardware encoder
   - ใช้ low-latency UDP/QUIC transport
   - ใช้ hardware decoder บน Windows
   - render โดย Direct3D/DXGI
   - local cursor
   - adaptive bitrate / FPS / resolution
   - รองรับ 60 FPS เป็น baseline และเตรียม architecture สำหรับ 120 FPS

2. **VNC Compatibility Mode — fallback**
   - เก็บ VNC เดิมไว้
   - ใช้เมื่อ Mac รุ่นเก่า, permission ไม่พร้อม, hardware encoder ใช้ไม่ได้ หรือ network policy block UDP
   - ผู้ใช้สามารถบังคับเลือก VNC ได้

3. **Apple Native High Performance Screen Sharing — coexist / native support**
   - ต้องไม่รบกวนบริการ Screen Sharing ของ macOS
   - Mac host ต้องยังสามารถใช้ Apple Native High Performance Screen Sharing กับ Mac client ได้
   - ตรวจสอบและหลีกเลี่ยง port conflict
   - รองรับการตรวจ capability/configuration ของ host เท่าที่ public API/documentation ของ Apple อนุญาต

> **ข้อจำกัดสำคัญ:** Apple Native High Performance Screen Sharing ตามเอกสาร Apple ปัจจุบันรองรับ Apple-silicon Mac ↔ Apple-silicon Mac และใช้กลไกของ Apple เอง ไม่มี public Windows client API ที่ Apple ระบุให้ third-party Windows client เชื่อมตรงกับ native High Performance protocol ดังนั้น Windows client ของโครงการนี้ต้องใช้ High Performance Compatible Mode ของเราเอง ไม่ให้ reverse-engineer proprietary Apple protocol เป็น dependency ของ production

---

# 2. Success Criteria

ระบบถือว่าพร้อมใช้งานเมื่อผ่านเกณฑ์ต่อไปนี้

## 2.1 LAN Performance

### 1080p
- 1920×1080 @ 60 FPS
- average displayed FPS >= 58
- P95 end-to-end video latency <= 35 ms
- P95 input-to-visible-response <= 50 ms
- frame drop <= 1% บน Gigabit LAN
- cursor perceived latency <= 15 ms

### 1440p
- 2560×1440 @ 60 FPS
- average displayed FPS >= 58
- P95 E2E <= 40 ms

### 4K
- 3840×2160 @ 60 FPS
- average displayed FPS >= 55
- P95 E2E <= 50 ms
- adaptive bitrate ต้องลดคุณภาพก่อนเกิด queue buildup

### 120 FPS Experimental
- 1080p/1440p @ 120 FPS เมื่อ Mac capture, encoder, Windows decoder และ display รองรับ
- ต้องไม่บังคับ 120 FPS กับเครื่องที่ไม่รองรับ

---

# 3. Apple High Performance Screen Sharing Compatibility

Apple High Performance Screen Sharing มีคุณสมบัติสำคัญที่ใช้เป็น reference target:

- 30 หรือ 60 FPS
- low latency
- stereo audio
- 4:4:4 chroma subsampling
- HDR Video Reference Mode
- virtual display
- ต้องการ bandwidth สูงและ latency ต่ำ
- Apple ระบุประมาณ 75 Mbps สำหรับ single 4K display
- ใช้ UDP ports 5900, 5901, 5902
- Apple Native High Performance ต้องเป็น Apple-silicon Mac ทั้ง host/client และ macOS Sonoma 14+

ดังนั้น implementation ของเราต้อง:

### MUST
- ไม่ bind UDP 5900–5902 เป็น default
- ไม่ทำให้ `screensharingd` หรือ Remote Management ของ macOS หยุดทำงาน
- ตรวจพบว่า Apple Screen Sharing เปิดอยู่ได้
- ใช้ custom port สำหรับ protocol ของเรา
- ทำงานพร้อมกับ Apple Screen Sharing service ได้
- ใช้ feature negotiation แยก `apple-native-hp` กับ `custom-hp`

### SHOULD
- แสดงใน diagnostics:
  - Apple Screen Sharing enabled/disabled
  - Apple Native High Performance capability
  - Apple Silicon / Intel
  - macOS version
  - occupied ports
  - Screen Recording permission
  - Accessibility permission
  - hardware encoder capability

### MUST NOT
- reverse-engineer authentication/encryption/proprietary wire protocol ของ Apple เป็น requirement
- impersonate Apple Screen Sharing client
- disable System Integrity Protection
- inject private frameworks เพื่อให้ Windows เข้า Apple Native HP protocol

---

# 4. Target Architecture

```text
                      macOS Host
┌──────────────────────────────────────────────────────┐
│ Host App / Agent                                     │
│                                                      │
│ Session Manager                                      │
│ ├── Apple Screen Sharing coexistence detector        │
│ ├── VNC Server Adapter       ← legacy/fallback       │
│ └── High Performance Engine  ← default               │
│                                                      │
│ ScreenCaptureKit                                     │
│      ↓                                               │
│ CMSampleBuffer / IOSurface                           │
│      ↓                                               │
│ Metal preprocessing (optional)                       │
│      ↓                                               │
│ VideoToolbox HW Encoder                              │
│ H.264 / HEVC                                         │
│      ↓                                               │
│ Packetizer / FEC / pacing                            │
│      ↓                                               │
│ QUIC Datagram / UDP                                  │
│                                                      │
│ Input receiver → CGEvent                             │
│ Audio capture  → audio encoder                       │
│ Clipboard      ↔ reliable QUIC stream                │
└──────────────────────┬───────────────────────────────┘
                       │
                       │ encrypted low-latency session
                       │
┌──────────────────────▼───────────────────────────────┐
│ Windows Client                                       │
│                                                      │
│ Session Manager                                      │
│ ├── HP Client                                        │
│ └── VNC Client Adapter ← fallback                    │
│                                                      │
│ Network receiver                                     │
│      ↓                                               │
│ Jitter / reorder / loss recovery                     │
│      ↓                                               │
│ Media Foundation HW decoder                          │
│      ↓                                               │
│ D3D11 texture                                        │
│      ↓                                               │
│ DXGI Flip Model                                      │
│      ↓                                               │
│ Display                                              │
│                                                      │
│ Raw Input → input channel → macOS                    │
│ Local Cursor Renderer                                │
└──────────────────────────────────────────────────────┘
```

---

# 5. Migration Strategy จาก VNC เดิม

ห้าม rewrite ทั้งระบบในครั้งเดียว

ให้ refactor architecture เดิมก่อน แล้วค่อยเปลี่ยน media pipeline

## Phase 0 — Baseline VNC

ก่อนแก้ code ให้สร้าง benchmark ของระบบปัจจุบัน

เก็บ:

```text
connection_type
resolution
display_refresh_rate
capture_fps
received_fps
presented_fps
network_rtt_ms
input_rtt_ms
bandwidth_mbps
cpu_mac_percent
cpu_windows_percent
gpu_mac_percent
gpu_windows_percent
frame_decode_ms
frame_present_ms
dropped_frames
```

สร้างหน้า:

```text
Settings
└── Developer
    └── Performance Overlay
```

Overlay:

```text
FPS       31 / 60
RTT       8 ms
Capture   3.1 ms
Encode    4.4 ms
Network   5.2 ms
Decode    2.2 ms
Present   4.8 ms
E2E       27 ms
Bitrate   22 Mbps
Drop      0.4 %
Codec     H264 HW
Mode      HP
```

ต้องมี benchmark เดิมก่อน เพื่อพิสูจน์ว่าระบบใหม่เร็วขึ้นจริง

---

# 6. Refactor VNC Into Transport Abstraction

สร้าง interface กลางก่อน

```text
IRemoteSession
├── connect()
├── disconnect()
├── requestKeyFrame()
├── setResolution()
├── setFrameRate()
├── sendKeyboard()
├── sendMouse()
├── sendScroll()
├── sendClipboard()
├── getStatistics()
└── getCapabilities()
```

Implement:

```text
VncRemoteSession
HighPerformanceRemoteSession
```

UI ต้องไม่รู้ว่า backend เป็น VNC หรือ HP

ตัวอย่าง:

```text
RemoteSessionFactory
    AUTO
      ↓
Capability Probe
      ├── HP supported → HighPerformanceRemoteSession
      └── otherwise    → VncRemoteSession
```

---

# 7. Connection Modes

ให้ผู้ใช้เลือก:

```text
Connection Mode

● Auto (Recommended)
○ High Performance
○ Compatibility / VNC
```

Auto algorithm:

```pseudo
if host_supports_custom_hp
   and hw_encoder_available
   and hw_decoder_available
   and udp_or_quic_available:
       use HIGH_PERFORMANCE
else:
       use VNC
```

ถ้า HP fail:

```text
HP Connect
   ↓
QUIC timeout
   ↓
retry 1 time
   ↓
fall back to VNC
```

ห้าม reconnect loop ไม่รู้จบ

---

# 8. macOS Host — Screen Capture

## 8.1 ScreenCaptureKit

High Performance Engine ต้องใช้:

```text
ScreenCaptureKit
SCStream
SCStreamConfiguration
SCContentFilter
```

ไม่ใช้ screenshot polling

ไม่ใช้:

```text
CGWindowListCreateImage loop
screencapture CLI loop
CPU bitmap polling
```

Capture output:

```text
CMSampleBuffer
    ↓
CVPixelBuffer / IOSurface
```

ต้องรักษา GPU-backed surface ให้นานที่สุด

---

# 9. Capture Profiles

สร้าง profile:

```text
AUTO
1080P_60
1440P_60
4K_60
1080P_120_EXPERIMENTAL
1440P_120_EXPERIMENTAL
QUALITY
LOW_BANDWIDTH
```

ตัวอย่าง configuration:

```swift
let configuration = SCStreamConfiguration()

configuration.width = targetWidth
configuration.height = targetHeight
configuration.minimumFrameInterval =
    CMTime(value: 1, timescale: CMTimeScale(targetFPS))

configuration.queueDepth = 3
configuration.showsCursor = false
```

หมายเหตุ:

- queueDepth ต้องต่ำเพื่อรักษา latency
- benchmark 3–5
- ห้ามเพิ่ม queue เพื่อซ่อนปัญหา encoder ช้า
- ถ้า encoder ช้า ให้ drop old frame

หลักการ:

```text
REMOTE DESKTOP:
latest frame > complete frame history
```

---

# 10. Frame Queue Policy

ห้ามใช้ unbounded queue

ใช้:

```text
Capture Queue       1–3
Encoder Queue       <= 2
Network Video Queue <= 2 frames
Decoder Queue       <= 1–2
Render Queue        latest frame
```

เมื่อ queue เต็ม:

```pseudo
drop(oldest_non_key_frame)
```

ไม่ทำ:

```pseudo
wait_until_every_old_frame_is_rendered()
```

เพราะจะทำให้ remote desktop ช้าลงเรื่อย ๆ

---

# 11. macOS Encoder

ใช้ VideoToolbox:

```text
VTCompressionSession
```

## Codec Priority

### Default
```text
H.264 Hardware Low Latency
```

### Optional
```text
HEVC Hardware
```

### Future
```text
AV1 Hardware when capability confirmed
```

---

# 12. Encoder Configuration

เป้าหมาย H.264:

```text
real-time = true
allow frame reordering = false
low latency = true
short GOP
hardware acceleration = required/preferred
```

Initial GOP:

```text
60 FPS → keyframe every 1–2 sec
120 FPS → keyframe every 1 sec
```

ต้อง request keyframe เมื่อ:

```text
packet loss burst
decoder desync
resolution changed
network recovery
session resume
```

---

# 13. Zero-Copy / Minimal-Copy Pipeline

Target:

```text
ScreenCaptureKit
      ↓
IOSurface
      ↓
VideoToolbox
```

หลีกเลี่ยง:

```text
IOSurface
 ↓
CPU memcpy
 ↓
RGB buffer
 ↓
another memcpy
 ↓
encoder
```

Performance test ต้องนับ copy:

```text
capture_to_encode_cpu_copy_count
```

Target:

```text
0 หรือ minimum เท่าที่ API บังคับ
```

---

# 14. Windows Decoder

ใช้:

```text
Media Foundation
D3D11
DXVA
```

Target pipeline:

```text
Network NAL units
       ↓
Media Foundation decoder
       ↓
D3D11 GPU texture
       ↓
DXGI swap chain
```

ห้าม decode เป็น CPU bitmap แล้ว copy เข้า UI framework ทุก frame

---

# 15. Rendering

ใช้:

```text
Direct3D 11/12
DXGI Flip Model
```

Video surface ต้องแยกจาก WinUI/WPF UI layer

```text
Window
├── Native video swapchain
├── Cursor overlay
├── Performance overlay
└── WinUI controls
```

Render loop ต้อง sync กับ display refresh โดยไม่สร้าง frame queue ใหม่

---

# 16. Cursor — สำคัญมาก

เพื่อให้ความรู้สึกใกล้ local machine:

### macOS
capture video:

```text
showsCursor = false
```

### Windows
render cursor locally

ส่งเฉพาะ:

```text
cursor_shape_id
cursor_x
cursor_y
hotspot_x
hotspot_y
visibility
timestamp
```

mouse input จาก Windows ต้อง update local cursor ทันที

ไม่ต้องรอ video frame กลับจาก Mac

Flow:

```text
Mouse moved
   ├── local cursor update immediately
   └── send mouse packet to Mac
```

นี่เป็นส่วนสำคัญมากในการลด perceived latency

---

# 17. Input Transport

Windows:

```text
Raw Input
```

macOS:

```text
CGEvent
```

แยก event เป็น:

```text
MouseMove
MouseButton
MouseWheel
KeyDown
KeyUp
ModifierState
UnicodeText
```

MouseMove สามารถใช้ unreliable latest-state channel

Keyboard/button state ใช้ reliable ordered channel

---

# 18. Keyboard Mapping

รองรับ mapping:

```text
Windows Ctrl ↔ macOS Control
Windows Alt  ↔ macOS Option
Windows key  ↔ macOS Command
```

เพิ่ม setting:

```text
Keyboard Mapping

○ Windows native
● Mac-friendly
○ Custom
```

Mac-friendly:

```text
Ctrl+C       → Command+C
Ctrl+V       → Command+V
Alt+Tab      → Command+Tab
Ctrl+Space   → configurable
```

ต้องรองรับ physical key code เพื่อ shortcut ที่แม่นยำ

---

# 19. Network Transport

## Default

ใช้:

```text
QUIC + TLS 1.3
```

แนะนำ library:

```text
MsQuic
```

หนึ่ง connection:

```text
QUIC Session
├── Datagram → video
├── Datagram → cursor state
├── Datagram → mouse move
├── Stream 1 → control
├── Stream 2 → keyboard/button
├── Stream 3 → clipboard
├── Stream 4 → audio control
└── Stream 5 → file transfer future
```

Custom HP port:

```text
default UDP: 55443
configurable
```

ห้ามใช้ 5900–5902 เป็น default เพราะต้อง coexist กับ Apple Screen Sharing

---

# 20. Internet Mode

LAN:

```text
Direct QUIC
```

Internet:

```text
Direct QUIC with NAT traversal
        ↓ fail
WebRTC/ICE/STUN/TURN fallback
```

V1 สามารถเริ่มด้วย LAN-first ก่อน

แต่ architecture ต้องไม่ผูก IP local แบบ hard-coded

---

# 21. Congestion Control

เก็บทุก 250–500 ms:

```text
RTT
packet loss
jitter
send queue
encoder queue
decoder queue
render queue
available bitrate estimate
```

Adaptive controller:

```pseudo
if packet_loss > threshold
   or network_queue_growing:
       reduce_bitrate()

if still_bad:
       reduce_resolution()

if still_bad:
       reduce_fps()

if stable_for_N_seconds:
       slowly_increase_bitrate()
```

อย่าเพิ่ม bitrate กลับเร็วเกินไป

ใช้ hysteresis

---

# 22. Adaptive Quality Profiles

ตัวอย่าง initial target:

```text
LAN Excellent
4K60
40–80 Mbps

LAN/Wi-Fi Good
1440p60
20–40 Mbps

Internet Good
1080p60
10–25 Mbps

Constrained
900p60 / 1080p30
5–12 Mbps
```

ตัวเลขต้องปรับจาก benchmark จริง

---

# 23. Image Quality / 4:4:4

Apple High Performance ใช้ 4:4:4 เป็นหนึ่งใน reference features

สำหรับระบบเรา:

## V1
- ให้ H.264/HEVC 4:2:0 hardware path เป็น baseline
- optimize text ด้วย bitrate / sharp scaling / native resolution

## V2
ทดลอง:

```text
4:4:4 hardware path
```

เฉพาะเมื่อ encode + decode hardware ของทั้งสองฝั่งรองรับจริง

ห้ามบังคับ software 4:4:4 แล้วทำให้ latency พัง

## Optional Advanced Design

```text
Base video stream       → 4:2:0
Text/chroma enhancement → region based
Cursor                  → independent overlay
```

ใช้เฉพาะเมื่อ benchmark พิสูจน์ว่าคุ้มค่า

---

# 24. Audio

รองรับ stereo audio

macOS:

```text
ScreenCaptureKit / CoreAudio
```

Codec:

```text
Opus
```

Transport:

```text
QUIC datagram / RTP
```

Audio jitter buffer แยกจาก video

อย่าให้ audio packet loss block video

---

# 25. Clipboard

รองรับ:

```text
text
image
file metadata future
```

clipboard ใช้ reliable encrypted stream

ต้องมี setting:

```text
Clipboard Sync
[✓] Windows → Mac
[✓] Mac → Windows
```

---

# 26. Security

MUST:

```text
TLS 1.3
device pairing
session authentication
short-lived session keys
encrypted clipboard
encrypted control channel
replay protection
```

Pair ครั้งแรก:

```text
Windows Client
    ↓
6-digit / QR pairing
    ↓
Mac confirms
    ↓
exchange device identity
    ↓
trusted-device record
```

ห้ามเก็บ password แบบ plaintext

ห้ามส่ง macOS login credential ผ่าน custom protocol หากไม่จำเป็น

---

# 27. macOS Permissions

Host ต้องตรวจ:

```text
Screen Recording
Accessibility
Microphone (ถ้าใช้)
Local Network (ตาม deployment)
```

สร้าง onboarding:

```text
1. Screen Recording       ✓
2. Accessibility          ✓
3. Network                ✓
4. Hardware Encoder       ✓
5. Ready
```

ถ้า permission ขาด:

```text
High Performance unavailable
→ explain exact missing permission
→ allow VNC fallback where possible
```

---

# 28. Apple Screen Sharing Coexistence

สร้าง component:

```text
AppleScreenSharingInspector
```

หน้าที่:

```text
detect screen sharing service state
detect port occupancy
detect Apple Silicon
detect macOS version
report native HP eligibility
```

ห้าม:

```text
kill screensharingd
replace Apple service
change Apple ports silently
```

ถ้าพบ Apple service:

```text
Apple Screen Sharing: Enabled
Native High Performance: Eligible
Custom High Performance: Available
VNC: Available
```

---

# 29. Virtual Display

V1:

```text
Physical display capture
```

V2:

รองรับ virtual display อย่างถูกต้องผ่าน public macOS APIs ที่มีให้ใช้ ณ target OS

เป้าหมาย:

```text
1920×1080 HiDPI
2560×1440
3840×2160
```

ห้ามใช้ private API เป็น production dependency

---

# 30. HDR

V1:

```text
SDR
```

V2:

```text
HDR capture
HEVC Main10
Windows HDR render
color metadata
tone mapping fallback
```

ต้องมี capability negotiation:

```text
host_hdr
codec_10bit
client_hdr
display_hdr
```

ถ้าขาดอย่างใดอย่างหนึ่ง:

```text
SDR fallback
```

---

# 31. Capability Negotiation

ตอน connect:

```json
{
  "protocolVersion": 1,
  "platform": "macOS",
  "architecture": "arm64",
  "capture": {
    "screenCaptureKit": true,
    "maxWidth": 3840,
    "maxHeight": 2160,
    "maxFps": 120,
    "hdr": true
  },
  "video": {
    "h264": true,
    "hevc": true,
    "av1": false
  },
  "appleScreenSharing": {
    "enabled": true,
    "nativeHighPerformanceEligible": true
  }
}
```

Windows reply:

```json
{
  "protocolVersion": 1,
  "decode": {
    "h264Hardware": true,
    "hevcHardware": true,
    "maxFps": 120,
    "hdr": false
  },
  "display": {
    "refreshRate": 144
  }
}
```

จากนั้น session manager เลือก best common profile

---

# 32. Protocol Versioning

ทุก packet มี:

```text
protocol_version
session_id
packet_type
sequence
timestamp
payload_length
```

ห้าม hard-code protocol โดยไม่มี version

Compatibility:

```text
major mismatch → refuse / fallback
minor mismatch → feature negotiation
```

---

# 33. Video Packet Metadata

อย่างน้อย:

```text
frame_id
packet_id
packet_count
capture_timestamp
encode_timestamp
keyframe
codec
width
height
fps_profile
```

ใช้ timestamp monotonic clock

ห้ามใช้ wall-clock อย่างเดียวในการวัด latency

---

# 34. Packet Loss Strategy

Video:

```text
ไม่ retransmit ทุก packet
```

ถ้า packet หาย:

```text
small loss → continue/drop frame
burst loss → request keyframe
```

Optional:

```text
FEC
```

Control/input:

```text
reliable where state must not be lost
```

---

# 35. Frame Pacing

Encoder output ห้าม burst ทั้ง frame ลง socket โดยไม่ pacing

Implement:

```text
packet pacing
congestion window
network queue limit
```

เป้าหมาย:

```text
network send queue < 1 frame
```

---

# 36. Quality Controller

สร้าง:

```text
AdaptiveQualityController
```

Inputs:

```text
RTT
loss
jitter
encode_ms
decode_ms
present_ms
send_queue_ms
decode_queue_ms
```

Outputs:

```text
bitrate
resolution
fps
codec profile
```

priority:

```text
1. preserve latency
2. preserve FPS
3. preserve readable text
4. maximize resolution
```

สำหรับ remote desktop ห้าม optimize PSNR แล้วแลกกับ latency สูง

---

# 37. Windows Presentation Policy

ต้องใช้:

```text
latest decoded frame
```

ถ้า frame มาช้า:

```text
drop stale frame
```

สร้าง metric:

```text
frame_age_at_present_ms
```

Target:

```text
< 25–35 ms LAN
```

---

# 38. Session State Machine

```text
DISCONNECTED
   ↓
DISCOVERING
   ↓
AUTHENTICATING
   ↓
NEGOTIATING
   ↓
CONNECTING_HP
   ↓
STREAMING_HP
```

Failure:

```text
CONNECTING_HP
   ↓
HP_FAILED
   ↓
CONNECTING_VNC
   ↓
STREAMING_VNC
```

Recovery:

```text
STREAMING_HP
   ↓
NETWORK_DEGRADED
   ↓
adaptive downgrade
   ↓
RECOVERED
```

ไม่ควร fallback VNC จาก packet loss ชั่วคราวทันที

---

# 39. Discovery

LAN discovery:

```text
Bonjour / mDNS
```

Advertise:

```text
service name
device name
protocol version
HP support
VNC support
port
```

ห้าม advertise secrets

---

# 40. Repository Structure

ตัวอย่าง:

```text
remote-desktop/
│
├── mac-host/
│   ├── app/
│   ├── capture/
│   │   └── ScreenCaptureKitCapture.swift
│   ├── encode/
│   │   └── VideoToolboxEncoder.swift
│   ├── input/
│   │   └── CGEventInjector.swift
│   ├── apple-sharing/
│   │   └── AppleScreenSharingInspector.swift
│   └── session/
│
├── windows-client/
│   ├── app/
│   ├── decode/
│   │   └── MediaFoundationDecoder.cpp
│   ├── render/
│   │   └── D3DRenderer.cpp
│   ├── input/
│   │   └── RawInputCapture.cpp
│   └── session/
│
├── core/
│   ├── protocol/
│   ├── transport/
│   ├── congestion/
│   ├── telemetry/
│   └── security/
│
├── vnc/
│   ├── VncRemoteSession.*
│   └── legacy/
│
├── tests/
│   ├── protocol/
│   ├── latency/
│   ├── network/
│   └── interoperability/
│
└── docs/
    ├── architecture.md
    ├── protocol.md
    ├── benchmark.md
    └── security.md
```

---

# 41. Implementation Phases

## Phase 1 — Instrument VNC

ทำ:

- FPS meter
- latency metrics
- input latency
- bandwidth
- CPU/GPU
- performance overlay

Deliverable:

```text
VNC baseline report
```

---

## Phase 2 — Abstract Existing VNC

ทำ:

```text
IRemoteSession
VncRemoteSession
SessionFactory
```

VNC functionality ต้องไม่ regression

---

## Phase 3 — Mac Capture Prototype

ทำ:

```text
ScreenCaptureKit
60 FPS
local preview
frame timestamps
```

Pass:

```text
1080p60 stable
CPU usage acceptable
no growing queue
```

---

## Phase 4 — Hardware Encoder

เพิ่ม:

```text
VideoToolbox H264
low latency
hardware encode
```

วัด:

```text
capture_ms
encode_ms
```

---

## Phase 5 — QUIC LAN Transport

สร้าง:

```text
MsQuic client/server
TLS
pairing
video datagram
control stream
```

ก่อนเพิ่ม adaptive quality ให้ได้:

```text
1080p60 LAN
```

---

## Phase 6 — Windows HW Decode

สร้าง:

```text
Media Foundation
D3D11 hardware decode
```

ตรวจสอบว่า decode surface อยู่ GPU

---

## Phase 7 — Direct GPU Render

เพิ่ม:

```text
DXGI flip swapchain
frame pacing
latest-frame policy
```

เป้าหมาย:

```text
smooth 60 FPS
```

---

## Phase 8 — Remote Input

เพิ่ม:

```text
Raw Input
CGEvent
local cursor
keyboard mapping
```

วัด input-to-photon latency

---

## Phase 9 — Adaptive Quality

เพิ่ม:

```text
bandwidth estimator
RTT
loss
dynamic bitrate
dynamic resolution
dynamic FPS
```

---

## Phase 10 — Audio / Clipboard

เพิ่ม:

```text
stereo audio
clipboard sync
```

---

## Phase 11 — Apple High Performance Coexistence

เพิ่ม:

```text
AppleScreenSharingInspector
port conflict detection
native HP capability indicator
```

ทดสอบ:

```text
Apple Mac → Mac HP still works
our Windows → Mac HP still works
VNC still works
```

---

## Phase 12 — 4K60

Tune:

```text
4K60
hardware encode
hardware decode
GPU render
adaptive bitrate
```

---

## Phase 13 — 120 FPS Experimental

เปิดเมื่อ capability ผ่าน

```text
ScreenCaptureKit cadence
VideoToolbox encode throughput
network
Windows decode throughput
120/144Hz display
```

ถ้า component ใดไม่ผ่าน:

```text
60 FPS
```

---

## Phase 14 — Internet Mode

เพิ่ม:

```text
NAT traversal
ICE/STUN/TURN or equivalent
relay
connection migration
```

---

# 42. Performance Budget

สำหรับ 60 FPS:

```text
1 frame = 16.67 ms
```

เป้าหมาย pipeline:

```text
Capture      1–4 ms
Encode       2–6 ms
Network      1–10 ms LAN
Decode       1–5 ms
Present      1–8 ms
```

pipeline สามารถ overlap กันได้

อย่ารวมแบบ serial อย่างเดียวในการวิเคราะห์

---

# 43. Threading

Mac:

```text
Capture Queue
Encode Queue
Network Queue
Input Queue
Telemetry Queue
```

Windows:

```text
Network Receive
Decode
Render
Input
Audio
Telemetry
```

ห้ามทำ encode/decode/network บน UI thread

---

# 44. Memory Management

สร้าง pool:

```text
frame buffers
packet buffers
network messages
```

หลีกเลี่ยง allocate/free ทุก frame

60 FPS × หลายชั่วโมง ต้องไม่มี memory growth

---

# 45. Diagnostics

หน้า Diagnostics:

```text
Connection
  Mode: High Performance
  Transport: QUIC
  Encryption: TLS 1.3
  RTT: 7.4 ms

Video
  Codec: H264 HW
  Resolution: 2560×1440
  FPS: 60
  Bitrate: 27.4 Mbps
  Capture: 2.4 ms
  Encode: 3.1 ms
  Decode: 1.8 ms
  Present: 4.2 ms
  Drop: 0.2 %

Mac
  Apple Silicon: Yes
  ScreenCaptureKit: Yes
  Hardware Encoder: Yes

Apple Screen Sharing
  Enabled: Yes
  Native HP Eligible: Yes
  Port Conflict: No
```

เพิ่มปุ่ม:

```text
Export diagnostics
```

ห้าม export password/token

---

# 46. Network Simulator Tests

CI / QA ต้องทดสอบ:

```text
RTT 1 ms
RTT 20 ms
RTT 50 ms
RTT 100 ms

Loss 0 %
Loss 0.5 %
Loss 1 %
Loss 3 %
Loss 5 %

Jitter 0–30 ms
Bandwidth 5–100 Mbps
```

ตรวจว่า queue ไม่โตไม่สิ้นสุด

---

# 47. Long Session Tests

ทดสอบ:

```text
30 min
2 hr
8 hr
24 hr
```

ตรวจ:

```text
memory leak
GPU resource leak
FPS degradation
audio drift
clock drift
queue growth
reconnect behavior
```

---

# 48. Visual QA

ทดสอบ content:

```text
VS Code / Terminal
small text
web scrolling
YouTube video
drag windows
Mission Control
animations
dark mode
light mode
Retina scaling
multiple displays
```

ให้ screenshot compare:

```text
local Mac
VNC
High Performance
```

---

# 49. Input QA

ทดสอบ:

```text
rapid typing
key rollover
modifier combinations
Command shortcuts
mouse drag
high polling mouse
trackpad-like scroll
horizontal scroll
multi-monitor coordinates
```

ห้ามเกิด stuck key

ต้องมี key-state reset เมื่อ disconnect

---

# 50. Failure Handling

กรณี:

```text
Mac sleeps
Windows sleeps
Wi-Fi changes
Ethernet reconnects
VPN toggles
IP changes
decoder reset
encoder reset
display resolution changes
monitor unplugged
```

ต้อง recover โดยไม่ต้อง restart app ถ้าเป็นไปได้

---

# 51. VNC Fallback Rules

Fallback VNC เมื่อ:

```text
ScreenCaptureKit unavailable
Screen Recording denied
hardware encoder unavailable
QUIC blocked
HP handshake incompatible
client decoder unavailable
```

แสดง:

```text
Connected using Compatibility Mode (VNC)
Reason: UDP/QUIC unavailable
```

ห้าม fallback แบบเงียบจน debug ไม่ได้

---

# 52. User Settings

```text
Connection
  Auto
  High Performance
  VNC

Quality
  Auto
  Low Latency
  Balanced
  High Quality
  Custom

Frame Rate
  Auto
  30
  60
  120 Experimental

Resolution
  Dynamic
  1080p
  1440p
  4K
  Native

Codec
  Auto
  H264
  HEVC

Cursor
  Local Cursor ✓

Audio
  Stream Mac Audio ✓

Clipboard
  Sync Clipboard ✓
```

---

# 53. Low Latency Preset

```text
codec       H264
fps         60/120
queue       minimum
resolution  dynamic
cursor      local
bitrate     adaptive
B-frames    off
```

---

# 54. Quality Preset

```text
codec       HEVC when available
fps         60
resolution  native
bitrate     high
cursor      local
```

แต่ห้ามเพิ่ม queue หลายเฟรมเพื่อ quality

---

# 55. Auto Preset

Auto เลือกจาก:

```text
display size
display refresh
RTT
bandwidth
packet loss
encoder throughput
decoder throughput
battery/thermal state
```

---

# 56. Thermal Management

MacBook:

```text
thermal state
battery state
```

ถ้า thermal pressure สูง:

```text
4K60 → 1440p60
120 → 60 FPS
```

ต้องไม่ทำให้เครื่อง throttle แล้ว latency แย่กว่าเดิม

---

# 57. Metrics Storage

เก็บ aggregate local metrics:

```text
session duration
mode
average FPS
P95 E2E
P95 RTT
average bitrate
drops
reconnects
fallback reason
```

ไม่เก็บ screen content

---

# 58. Privacy

MUST:

- แสดง indicator ว่ากำลัง remote
- stop session ได้จาก Mac
- revoke paired device ได้
- permission least privilege
- ไม่เก็บ frame ลง disk โดย default
- log ห้ามมี clipboard content
- log ห้ามมี keystroke content

---

# 59. Definition of Done — V1

V1 พร้อม release เมื่อ:

- [ ] VNC เดิมยังใช้ได้
- [ ] Auto mode เลือก HP ได้
- [ ] ScreenCaptureKit capture ใช้งานจริง
- [ ] VideoToolbox H264 hardware encoding
- [ ] QUIC encrypted transport
- [ ] Windows hardware decoding
- [ ] D3D GPU rendering
- [ ] 1080p60 stable
- [ ] 1440p60 stable
- [ ] local cursor
- [ ] keyboard/mouse control
- [ ] clipboard text
- [ ] performance overlay
- [ ] graceful VNC fallback
- [ ] Apple Screen Sharing coexistence
- [ ] reconnect
- [ ] no queue growth
- [ ] no major memory leak in 8-hour test

---

# 60. Definition of Done — V2

- [ ] 4K60
- [ ] stereo audio
- [ ] HEVC
- [ ] adaptive bitrate
- [ ] dynamic resolution
- [ ] multi-monitor
- [ ] 120 FPS experimental
- [ ] HDR experimental
- [ ] Internet/NAT traversal
- [ ] virtual display
- [ ] enhanced text/chroma mode investigation
- [ ] 24-hour stability test

---

# 61. AI Development Instructions

ให้ AI coding agent ทำงานตามลำดับนี้โดยไม่ข้าม phase

```text
1. Inspect existing repository.
2. Identify current VNC server/client libraries.
3. Do NOT delete working VNC implementation.
4. Add benchmark instrumentation.
5. Introduce IRemoteSession abstraction.
6. Wrap existing VNC implementation.
7. Add HighPerformanceRemoteSession behind feature flag.
8. Build ScreenCaptureKit capture.
9. Add VideoToolbox hardware encoder.
10. Implement QUIC transport.
11. Implement Windows Media Foundation decoder.
12. Implement D3D/DXGI renderer.
13. Add input channel and local cursor.
14. Add capability negotiation.
15. Add adaptive quality.
16. Add Apple Screen Sharing coexistence diagnostics.
17. Run benchmark against VNC baseline.
18. Fix regressions.
19. Only then make HP mode default.
```

ทุก phase:

```text
BUILD
TEST
BENCHMARK
DOCUMENT
COMMIT
```

ห้ามแก้หลาย subsystem พร้อมกันโดยไม่มี benchmark

---

# 62. Required Tests Before Merge

ทุก PR ที่แตะ media/network:

```text
unit tests
protocol tests
connection test
1080p60 smoke test
VNC fallback test
disconnect/reconnect test
```

critical performance PR:

```text
before FPS
after FPS

before latency
after latency

before CPU
after CPU

before bandwidth
after bandwidth
```

ต้องแนบผลกับ PR

---

# 63. Non-Goals

V1 ไม่ต้อง:

```text
reverse-engineer Apple proprietary HP protocol
replace macOS Screen Sharing service
support private Apple frameworks
support every codec
implement cloud relay before LAN stable
support 8K
```

Focus:

```text
Windows → Mac
1080p60 / 1440p60
low latency
stable input
good text
VNC fallback
```

---

# 64. Recommended Technology Stack

## macOS

```text
Swift
ScreenCaptureKit
VideoToolbox
Metal
CoreGraphics / CGEvent
CoreAudio
```

## Shared/Core

```text
C++20 or Rust
MsQuic
TLS 1.3
FlatBuffers / Protobuf / compact custom binary protocol
```

## Windows

```text
C++20
WinUI 3
Media Foundation
D3D11/D3D12
DXGI
WASAPI
Raw Input
```

UI framework ไม่ควรอยู่ใน hot video path

---

# 65. Important Engineering Rules

1. Latency สำคัญกว่าไม่มี frame drop
2. Old frame ไม่มีคุณค่าใน interactive remote desktop
3. ห้าม queue frame ไม่จำกัด
4. ใช้ hardware encode/decode
5. หลีกเลี่ยง CPU copy
6. cursor ต้อง local
7. network media path ต้องไม่ติด TCP head-of-line blocking
8. bitrate ต้อง adaptive
9. VNC ต้องอยู่เป็น fallback
10. Apple Native High Performance ต้อง coexist
11. ใช้ public APIs ใน production
12. ทุก optimization ต้องวัดก่อน/หลัง

---

# 66. Final Target Experience

เมื่อผู้ใช้ Windows เชื่อม Mac:

```text
Open App
   ↓
Discover Mac
   ↓
Pair
   ↓
Auto negotiate
   ↓
High Performance
   ↓
1080p/1440p/4K @ 60 FPS
   ↓
local-feeling cursor
   ↓
hardware video pipeline
   ↓
adaptive network quality
```

ผู้ใช้ไม่ควรต้องรู้ว่า backend เปลี่ยนจาก VNC เป็น custom HP protocol

ถ้า HP ใช้ไม่ได้:

```text
Auto fallback → VNC
```

ถ้า Mac client เชื่อมผ่าน Apple Screen Sharing:

```text
Apple Native High Performance ยังคงใช้งานได้ตามปกติ
```

---

# 67. Reference Facts Used for This Design

- Apple High Performance Screen Sharing รองรับ 30/60 FPS, stereo audio, HDR reference mode และ 4:4:4 chroma subsampling
- Apple ระบุ bandwidth ประมาณ 75 Mbps สำหรับ single 4K display
- Apple Native HP ต้องใช้ Apple-silicon Mac ทั้งสองฝั่งและ macOS Sonoma 14+
- Apple Native HP ใช้ UDP 5900, 5901 และ 5902
- ScreenCaptureKit ถูกออกแบบสำหรับ high-performance screen capture/screen sharing และสามารถส่ง frame/audio เป็น CMSampleBuffer
- ScreenCaptureKit รองรับการตั้ง resolution, frame rate, pixel format และเปลี่ยน configuration ระหว่าง stream ได้
- Apple มี Device Management configuration สำหรับ Screen Sharing host เช่น PortBase, MaximumVirtualDisplays และ PreventHighPerformanceConnections

อ้างอิงหลักที่ทีมควรตรวจสอบกับ documentation เวอร์ชันล่าสุดก่อน release:

- Apple Support — Use High Performance screen sharing
- Apple Support — Screen sharing type options on Mac
- Apple Developer — ScreenCaptureKit
- Apple Developer — Meet ScreenCaptureKit
- Apple Developer — Take ScreenCaptureKit to the next level
- Apple Developer — ScreenSharingHostSettings
- Microsoft Learn — Media Foundation / D3D11 video decoding
- Microsoft MsQuic documentation

---

# 68. First Development Milestone

Milestone แรกที่ต้องทำให้เสร็จก่อน feature อื่น:

```text
Existing VNC
     +
IRemoteSession abstraction
     +
ScreenCaptureKit 1080p60 capture
     +
VideoToolbox H264 HW encode
     +
QUIC LAN
     +
Media Foundation HW decode
     +
D3D render
     +
Raw Input / CGEvent
     +
Local Cursor
```

Acceptance:

```text
Windows → Mac
1920×1080
60 FPS
stable 30 minutes
P95 latency <= 35–50 ms on wired LAN
no increasing frame queue
no major memory growth
VNC fallback works
Apple Screen Sharing remains enabled
```

เมื่อ milestone นี้ผ่าน ค่อยเดินต่อ:

```text
1440p60
→ 4K60
→ Audio
→ Adaptive Quality
→ 120 FPS
→ HDR
→ Internet Mode
```

---

## End State

ระบบปลายทางต้องไม่ถูกมองว่าเป็น “VNC ที่เร็วขึ้น” แต่เป็น:

> **Cross-platform high-performance remote desktop engine ที่มี VNC compatibility layer และสามารถ coexist กับ Apple Native High Performance Screen Sharing ได้**

