# Security

Target requirements (spec §26) and current status. The dev transport is
plaintext — **never expose UDP 55443 beyond a trusted LAN** until QUIC/TLS lands.

## MUST → status

| Requirement (spec §26) | Status |
|---|---|
| TLS 1.3 | ☐ QUIC/MsQuic phase (V1 DoD) — UDP datagrams today |
| device pairing (6-digit/QR → trusted-device record) | ☐ designed, not built |
| session authentication | partial: wire magic + version + session ID; sender without `RDPh` magic is dropped |
| short-lived session keys | ☐ |
| encrypted clipboard / control channel | ☐ |
| replay protection | ☐ (monotonic sequence is recorded per packet; acceptance window comes with QUIC phase) |
| no plaintext passwords stored | ✓ none stored |
| no macOS login credentials through custom protocol | ✓ never sent |

## Pairing plan (spec §26)

1. Client shows 6-digit code derived from its ephemeral key + host's public key.
2. Mac confirms; both exchange device identities over the QUIC control stream.
3. Trusted-device record (device ID + public key + label) stored in Keychain
   (host) / DPAPI (client). Revoke from Mac side at any time (spec §58).

## Privacy (spec §58) — already enforced where marked

- Remote-session indicator + stop from the Mac: ☐ (before release)
- Permission least privilege: ✓ Screen Recording/Accessibility requested only
  when HP mode is selected; VNC proxy needs neither.
- No frames to disk by default: ✓ encode-test/capture-test write only when
  `--out` is passed, and only test patterns/synthetic content.
- Logs contain no clipboard content: ✓ no clipboard implementation yet; keep
  it that way — log lengths/types only.
- Logs contain no keystroke content: ✓ injector logs event types/counts only.
- Metrics storage (spec §57) must aggregate (session duration, FPS, P95s,
  fallback reason) and never store screen content.

## Apple coexistence (spec §3 MUST NOT)

No reverse-engineering of Apple's protocol, no impersonating Apple clients, no
SIP changes, no private frameworks, never binding 5900–5902, never killing
`ScreenSharing*` services. The inspector reads only public signals.
