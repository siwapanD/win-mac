# windows-client

Windows client for the Windows → Mac high-performance remote desktop
(spec: `docs/spec-original.md`). Compiles **only on Windows** — built and
verified on macOS for the mac-host side first (see repo README for status).

## Build (Windows 10 1903+ / Windows 11, Visual Studio 2022, C++20)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Layout

| Path | Role (spec §) |
|---|---|
| `src/protocol/wire.h` | Wire-format mirror of mac-host `Wire.swift` — byte layouts must stay in sync (docs/protocol.md is canonical) |
| `src/session/HPClientSession.*` | UDP transport, hello/capabilities handshake, video reassembly (latest-frame-wins), keyframe requests |
| `src/decode/MediaFoundationDecoder.*` | H.264 → D3D11 GPU texture via MF/DXVA — no CPU bitmap path (§14) |
| `src/render/D3DRenderer.*` | DXGI Flip Model swapchain, video processor, latest-frame present, `frame_age_at_present_ms` metric (§15/§37) |
| `src/input/RawInputCapture.*` | WM_INPUT keyboard/mouse, local-cursor-first policy (§16), VK→mac keycode mapping (§18), key-state reset (§49) |
| `src/main.cpp` | Window + threading skeleton (network/decode/render/input threads, §43) |

## Status

- [x] Wire format mirrored + CMake project
- [x] Threading/architecture per spec §14–§18, §37, §43
- [ ] MF decoder implemented (skeleton notes document the exact MF calls)
- [ ] D3D flip-model renderer implemented
- [ ] Raw Input + local cursor overlay implemented
- [ ] Capability JSON reply + keyframe-request logic
- [ ] Performance overlay (Settings → Developer, spec §5)
- [ ] QUIC/MsQuic transport replacing UDP datagrams (spec §19)

Implementation notes in each `.cpp` describe the intended API calls
(MFTEnumEx hardware decoder, MFCreateDXGIDeviceManager, DXGIFactory2
CreateSwapChainForHwnd with FLIP_SEQUENTIAL + max frame latency 1,
RegisterRawInputDevices, etc.) so the next step is mechanical.
