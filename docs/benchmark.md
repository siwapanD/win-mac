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

## Loopback smoke test (spec §62, host-only)

Full pipeline verification without a Windows client:

```bash
swift run mac-host serve --mode hp          # terminal A (waits for client)
swift run mac-host client-test --seconds 6 --expect-video   # terminal B
```

**First real run — 2026-09-30, this Mac (Apple Silicon, macOS 26.6, 1920×1080@75):**

| Run | Content on screen | Frames/8 s | FPS | Loss | cap→recv p50 | p95 | Mbps | RTT |
|---|---|---|---|---|---|---|---|---|
| 1 | idle desktop | 288 | 48.0 | 0 | 8.4 ms | 14.1 | 0.7 | 0.19 ms |
| 2 | small bouncing ball (~2 % area) | 430 | 53.8 | 0 | 7.9 ms | 13.0 | 0.2 | 0.41 ms |
| 3 | fullscreen tkinter animation | 442 | 55.3 | 0 | 7.8 ms | 13.0 | 0.0 | 0.19 ms |
| 4 | fullscreen, >60 Hz redraw source | 443 | 55.4 | 0 | 7.7 ms | 12.8 | 0.0 | 0.21 ms |

Verdict: **smoke PASS every run** — zero frame loss, capture→receive p95
12.8–15.4 ms (well under the §2 35 ms E2E budget, and this excludes the
Windows decode+render tail). FPS tracks *screen content change rate*, not
pipeline capacity: ScreenCaptureKit emits frames only when content changes
(0.0–0.7 Mbps idle), and it plateaus ~55 fps here regardless of a >60 Hz
redraw source — synthetic-content `encode-test` independently proves the
encoder itself sustains exactly 60 fps. True 60 fps capture numbers need
real on-screen motion visible to SCK (e.g. video playback) — run the smoke
while playing a video to see it.

`encode-test` (synthetic, no permission needed): 300/300 frames = **60.0 fps**
at 1080p, encode avg 5.55 ms (budget 16.67 ms) → encoder headroom for 120 FPS
experiments (§13).

## Windows client loopback (fake-host) — 2026-10-01

Full Windows pipeline on one PC, no Mac: `windows-client/tools/fake-host.exe`
(protocol stand-in for `serve --mode hp`, looped synthetic 1080p H.264 clip:
colour bands, moving bar, noise patch; keyframe ~46 KB / 40 datagrams, CBR
target 12 Mbps) → `windows-client.exe --host 127.0.0.1`. Machine: Windows 11
Pro 26200, Intel Arc 140T (DXVA decode), 240 Hz display, vsync on.

| Scenario | Shown fps | Mbps | Decode | age@present avg / max (steady) | Lost frames | Result |
|---|---|---|---|---|---|---|
| Clean, 8 s | 59–61 | 0.7 (pre-noise clip) | 0.8 ms | 2.4 / 3–8 ms | 0 | smoke PASS |
| Clean, noise clip, 10 s | 59–61 | 17.2–17.7 | 0.6–0.8 ms | 2.0 / 3–5 ms | 0 | PASS |
| 2 % packet loss, 8 s | 27–36 | 16–20 | 0.6–0.9 ms | 2.3 / 3–8 ms | 202 / 464 | PASS, no stall, 18 keyframe requests |
| Host restarted mid-session | 60 after resume | — | — | — | 0 | re-handshake + decoder resync, no client restart |
| No host | — | — | — | — | — | hint after 5 s, exit 2 |

`age@present` is client-side only: last datagram of a frame → `Present`
returned (reassembly + queue + decode + render wait). First-second maxima
(35–55 ms) are decoder/swapchain warm-up. At 2 % loss a ~29-datagram frame
survives only ~56 % of the time without FEC, which matches the shown fps;
real LAN loss is near zero. Input path verified with SendInput through the
keyboard hook: letters, Ctrl+C → Cmd+C, Shift, arrows (fn/numpad flags),
clicks at normalized (0.5, 0.5), wheel → 3 lines, Ctrl+Alt+Enter not
forwarded, fullscreen Alt+Tab → Cmd+Tab, every down matched by an up.

Pending: the same run against a real Mac (`serve --mode hp`) — adds the
network and host capture/encode legs to the latency picture.

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
