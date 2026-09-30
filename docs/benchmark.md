# Benchmark

Rule (spec §41/§62): every phase ships with numbers — BUILD TEST BENCHMARK
DOCUMENT COMMIT. No optimization without before/after.

## Baseline: VNC (spec Phase 1)

Baseline is measured against the VNC proxy path (TCP 55444 → Apple RFB :5900)
plus, ideally, a stock VNC client — record before any HP tuning:

| Metric (per §5) | VNC baseline | HP (fill per run) |
|---|---|---|
| connection_type | vnc-proxy | hp-udp |
| resolution / refresh | | |
| capture_fps / received_fps / presented_fps | | |
| network_rtt_ms / input_rtt_ms | | |
| bandwidth_mbps | | |
| cpu/gpu mac % / windows % | | |
| frame_decode_ms / frame_present_ms | | |
| dropped_frames | | |

## Acceptance targets (spec §2)

| Profile | Target |
|---|---|
| 1080p60 LAN | FPS ≥ 58, P95 E2E ≤ 35 ms, input-to-visible ≤ 50 ms, drop ≤ 1 %, cursor ≤ 15 ms |
| 1440p60 | FPS ≥ 58, P95 E2E ≤ 40 ms |
| 4K60 | FPS ≥ 55, P95 E2E ≤ 50 ms, adaptive bitrate before queue buildup |
| 120 FPS experimental | only when capture+encode+decode+display all support |

## Host-side measurements available now

`mac-host encode-test` (no permission needed): 1920×1080@60, 5 s, in-flight ≤ 2:

```
encoded 300/300 frames (keyframes 2), ~2.31 MB annex-B
encode latency avg 5.55 ms  min 5.04 ms  max 41.35 ms   (first-call warmup)
```
Budget check (spec §42): encode 2–6 ms ✓. Output verified by ffprobe:
H.264 High 1920×1080, ffmpeg decode 0 errors.

`mac-host capture-test`: SCK → encode with per-second capture/encoded FPS,
Mbps, capture→encode ms. Requires Screen Recording permission for the
launching terminal app.

## Test matrix to run per milestone (spec §46–§49)

- Network sim: RTT {1,20,50,100} ms × loss {0,0.5,1,3,5} % × jitter 0–30 ms ×
  bandwidth 5–100 Mbps — verify no queue growth (`pfctl`/`dummynet` on the Mac).
- Long sessions: 30 min → 2 h → 8 h → 24 h — memory, GPU resources, FPS,
  clock drift, reconnects.
- Visual QA content: VS Code/small text, web scroll, YouTube, Mission Control,
  dark/light, Retina scaling, multi-display.
- Input QA: rapid typing, rollover, modifiers, Cmd shortcuts, drag, high-
  polling mouse, horizontal scroll, stuck-key reset on disconnect.

## Network simulator (todo with Windows client)

`tests/` will host the dummynet profiles and the automated 1080p60 smoke
(`1080p60 smoke test` required per media/network PR, spec §62).
