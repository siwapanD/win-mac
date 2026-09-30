# QUIC / MsQuic proof (spec §19)

`msquic-loopback-test.cpp` — single-process QUIC v1 + TLS 1.3 loopback using
libmsquic 2.6.1 (Homebrew). Build:

```bash
openssl req -x509 -newkey rsa:2048 -keyout /tmp/rdp-quic-key.pem \
  -out /tmp/rdp-quic-cert.pem -days 30 -nodes -subj "/CN=mac-host"
clang++ -std=c++20 -I/opt/homebrew/include msquic-loopback-test.cpp \
  -o msquic-loop -L/opt/homebrew/lib -lmsquic -Wl,-rpath,/opt/homebrew/lib
./msquic-loop
```

## Results on this Mac (macOS 26.6, arm64, 2026-09-30)

| Component | Result |
|---|---|
| QUIC v1 + TLS 1.3 handshake (self-signed PEM cert, client skips validation for dev) | ✅ PASS, every run |
| Reliable stream send/recv + echo (control plane path) | ✅ PASS, every run |
| RFC 9221 datagrams (video path) | ⚠️ **unstable on this bottle** — SIGBUS in `QuicDatagramFrameEncodeEx` on some runs, hang on others, partial delivery (1/3) on others |

## Gotchas found in libmsquic 2.6.1 (differs from upstream docs!)

- `QUIC_CERTIFICATE_FILE` field order is **PrivateKeyFile first, CertificateFile second**
  (upstream has it the other way) — use designated initializers.
- `ConfigurationOpen` / `ListenerStart` take a **single `const QUIC_BUFFER* const`** ALPN
  pointer, not an array-of-pointers.
- `DatagramReceiveEnabled` must be set in QUIC_SETTINGS on BOTH sides.
- Server-side connection callbacks attach via `MsQuic->SetCallbackHandler(conn, ...)`
  on the accepted handle.

## Decision (recorded for the HP engine)

- **Control plane (hello/capabilities/heartbeats/input/clipboard) moves to a QUIC
  stream now** — encrypted, reliable, exactly per spec §19 stream layout.
- **Video stays on raw UDP datagrams (port 55443)** for the moment: MsQuic 2.6.1's
  datagram path is not stable enough for the hot path, and QUIC streams would add
  head-of-line blocking that spec §65.7 forbids for media.
- Video-on-QUIC returns when: (a) MsQuic fixes/updates the macOS datagram path, or
  (b) we build MsQuic from source and verify, or (c) Network.framework exposes
  RFC 9221 datagrams. The `DataTransport` interface keeps this swappable.
