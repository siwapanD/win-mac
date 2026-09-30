# VNC compatibility layer

From-scratch deployment — there is no legacy VNC implementation to keep, so
the fallback is staged:

## Stage 1 (built): RFB proxy

`mac-host/Sources/MacHost/Session/VncProxySession.swift` — TCP proxy
**55444 → 127.0.0.1:5900** relaying to macOS's built-in Screen Sharing
(RFB). Verified end-to-end on this Mac (RFB 003.889 handshake relays).

- Requires Apple Screen Sharing enabled; refuses with an explicit reason
  otherwise (spec §51 — never silent fallback).
- Needs no permissions on the host (least privilege, spec §58).
- The Windows client connects its existing VNC adapter to `mac:55444`.

## Stage 2 (future): self-contained RFB server

Own minimal RFB 3.8 server inside the host agent so VNC fallback also works
when Apple Screen Sharing is **disabled** (Apple Screen Sharing: Disabled /
Custom HP: available cases). Scope: RFB handshake, SetPixelFormat,
RawEncoding (BGRA from the same capture path) at 10–30 fps. deliberately
simple: fallback is for compatibility, not performance (spec §65.9).

VNC compatibility never binds 5900–5902 and never touches Apple's services
(spec §3 MUST).
