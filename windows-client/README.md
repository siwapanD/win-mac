# windows-client

Windows client for the Windows → Mac high-performance remote desktop
(spec: `docs/spec-original.md`). HP mode end to end: UDP 55443 handshake →
packet reassembly (latest frame wins) → Media Foundation H.264 decode on the
GPU (DXVA) → D3D11 flip-model present → keyboard/mouse back to the Mac.

## Requirements

- Windows 10 1903+ / Windows 11, x64
- GPU with H.264 hardware decode (any Intel/AMD/NVIDIA from the last decade).
  The client refuses to start on software decode (spec §14/§65.5); use VNC
  mode (TCP 55444) on such machines.
- Visual Studio 2022 or Build Tools 2022 with the C++ workload (CMake ≥ 3.24 is bundled)

## Build

From a **Developer PowerShell for VS 2022** (puts `cmake` on PATH):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs `build\Release\windows-client.exe` and `build\Release\fake-host.exe`.

## Connect to the Mac

On the Mac (Screen Recording + Accessibility granted to the terminal):

```bash
swift run mac-host serve --mode hp
```

Use `--mode hp`, not the default `auto`: auto mode waits only 10 s for a
client before falling back to VNC.

On Windows:

```powershell
.\build\Release\windows-client.exe --host 192.168.1.20
```

| Option | Meaning |
|---|---|
| `--host <addr>` | Mac IP or name (required) |
| `--port <n>` | HP UDP port, default 55443 |
| `--keymap mac\|native` | keyboard mapping, see below (default `mac`) |
| `--fullscreen` | start borderless fullscreen |
| `--no-vsync` | present immediately (tears when the GPU allows; lowest latency) |
| `--max-fps <n>` | fps advertised to the host (default: display refresh, clamped 60–120) |
| `--seconds <n>` / `--expect-video` | run n seconds, print a summary, exit non-zero on failure (smoke) |
| `--snapshot <file.bmp>` | save the 60th presented frame (diagnostics) |
| `--verbose-input` | log every key sent to the Mac |

The terminal shows one telemetry line per second (spec §45): received / shown
fps, Mbps, decode ms, `frame_age_at_present` avg/max (client side:
last packet → on screen), host encode ms, lost/stale/superseded frames,
keyframe requests. The window title shows fps · Mbps · age.

### Keyboard (spec §18)

Keys are mapped by **physical position** (scancode → macOS `kVK_*`), so the
Mac's own input source decides the character — Thai/English switching happens
on the Mac exactly as on a Mac keyboard.

| Windows | `--keymap mac` (default) | `--keymap native` |
|---|---|---|
| Ctrl | Command | Control |
| Win | Control | Command |
| Alt | Option | Option |
| Alt+Tab (fullscreen) | Command+Tab | Option+Tab |

- **Windowed:** Win, Alt+Tab, Alt+Esc, Ctrl+Esc and Alt+F4 stay with Windows
  (like mstsc). **Fullscreen:** everything goes to the Mac.
- **Ctrl+Alt+Enter** toggles fullscreen and is never sent to the Mac.
- Insert → Help, Print Screen → F13, Scroll Lock → F14, Pause → F15, Menu →
  contextual-menu key. Media keys stay with Windows.
- Focus loss, disconnect and exit release every key and button on the Mac
  (spec §49).

### Mouse and cursor (spec §16)

The Windows arrow cursor is the local cursor: it moves at hardware rate and
never waits for video. Positions are normalized to the letterboxed video
rect. Left/right buttons, wheel and horizontal wheel are forwarded (the host
injects no middle button). Wheel deltas accumulate into whole lines because
the host truncates fractions.

## Test without a Mac

`fake-host.exe` stands in for `mac-host serve --mode hp` on the same PC. It
speaks the same protocol, streams a looped synthetic H.264 clip (colour bands,
moving bar, binary frame counter, noise patch at ~12–17 Mbps), sends
heartbeats, honours keyframe requests, and logs every input packet the way
the Mac injector would receive it.

```powershell
.\tools\smoke.ps1            # PASS/FAIL, exit code 0/1
.\tools\smoke.ps1 -Loss 2    # with 2 % simulated video packet loss
```

Manually, in two terminals:

```powershell
.\build\Release\fake-host.exe --bind 127.0.0.1
.\build\Release\windows-client.exe --host 127.0.0.1 --verbose-input
```

`--bind 127.0.0.1` keeps fake-host off the LAN, so Windows Firewall does not ask.

## Layout

| Path | Role (spec §) |
|---|---|
| `src/protocol/wire.h` | Wire-format mirror of mac-host `Wire.swift` + payload builders (docs/protocol.md is canonical) |
| `src/session/HPClientSession.*` | UDP transport, hello/capabilities handshake + re-handshake, reassembly (latest-frame-wins), heartbeat echo, keyframe requests with backoff, input send thread |
| `src/decode/MediaFoundationDecoder.*` | Microsoft H.264 MFT + D3D11 device manager → NV12 GPU texture, low-latency mode, no CPU picture path (§14) |
| `src/render/D3DRenderer.*` | DXGI flip-model swapchain (max latency 1), video processor NV12→BGRA + letterbox, latest-frame slot, `frame_age_at_present` (§15/§37) |
| `src/input/InputCapture.*` | Keyboard hook + mouse messages, modifier modes, stuck-key reset (§16–§18, §49) |
| `src/input/MacKeyMap.h` | Scancode → macOS keycode table and CGEventFlags |
| `src/main.cpp` | Window, thread wiring (network / input / decode / render, §43), telemetry, smoke mode |
| `tools/fake-host.cpp` | Windows stand-in for the Mac host (test only) |
| `tools/smoke.ps1` | Loopback smoke test |

## Status

- [x] Wire format mirror + payload builders
- [x] UDP session: handshake, re-handshake after host silence, host restart detection
- [x] Reassembly with latest-frame-wins, stale-partial timeout, loss accounting
- [x] Keyframe requests: session start, loss, decoder error — rate-limited with backoff
- [x] MF hardware decode (DXVA) → D3D11 texture, zero-copy into the video processor
- [x] D3D11 flip-model renderer, max frame latency 1, drop-stale, letterbox, resize, borderless fullscreen
- [x] Keyboard (physical scancode → kVK, two modifier modes, Alt+Tab → Cmd+Tab) + mouse + wheel
- [x] Heartbeat echo (host RTT), telemetry line, smoke mode
- [x] Verified on Windows loopback against fake-host (see docs/benchmark.md)
- [ ] First run against a real Mac host
- [ ] Cursor shape from `CursorState` (reserved in protocol; Windows arrow for now)
- [ ] On-screen performance overlay (Settings → Developer, spec §5)
- [ ] Clipboard / audio (Phase 10)
- [ ] QUIC/MsQuic transport + pairing replacing plain UDP (spec §19, docs/security.md)

The dev UDP transport is **unencrypted and unauthenticated**: LAN only.
