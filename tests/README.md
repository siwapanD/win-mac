# Tests

- **Swift unit + regression tests** live inside the package:
  `mac-host/Tests/MacHostTests/` — wire header round-trip/refusal, frame
  packetization/reassembly (out-of-order, stale, loss), input packet
  round-trips, session state machine paths. Run: `cd mac-host && swift test`.
- **Windows side**: `windows-client/tools/smoke.ps1` — loopback smoke against
  `fake-host` (handshake, reassembly, DXVA decode, present ≥ 30 fps; `-Loss`
  simulates packet loss). Still to add: a C++ unit target validating wire.h
  against the same vectors as the Swift tests.
- **Network simulator** (spec §46): dummynet profiles — RTT {1,20,50,100} ms,
  loss {0,0.5,1,3,5} %, jitter 0–30 ms, bandwidth 5–100 Mbps; assert no
  unbounded queue growth. To be automated here once the Windows client runs.
- **Long sessions** (spec §47): 30 min / 2 h / 8 h / 24 h — memory/GPU leaks,
  FPS degradation, audio drift, clock drift, reconnects.
- **PR gate** (spec §62): unit tests + protocol tests + connection test +
  1080p60 smoke + VNC fallback test + disconnect/reconnect test; performance
  PRs attach before/after FPS, latency, CPU, bandwidth.
