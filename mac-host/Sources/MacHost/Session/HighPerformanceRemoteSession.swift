import Foundation
import CoreMedia
import CoreVideo

/// The High Performance engine (spec §4): ScreenCaptureKit → VideoToolbox
/// H.264 HW → packetizer → UDP/QUIC, plus inbound input injection.
/// Auto mode reaches here only when the capability probe passed; any connect
/// failure surfaces to the factory for a single retry, then explicit VNC
/// fallback (spec §7/§51 — never a reconnect loop, never silent fallback).
final class HighPerformanceRemoteSession: IRemoteSession, @unchecked Sendable {
    enum SessionError: LocalizedError {
        case handshakeTimeout
        case permissionMissing(String)

        var errorDescription: String? {
            switch self {
            case .handshakeTimeout:
                return "Client handshake timeout (10 s) — no hello/capabilities received."
            case .permissionMissing(let what):
                return "\(what) permission missing — HP mode unavailable (spec §27)."
            }
        }
    }

    let modeName = "High Performance (custom HP)"

    private let port: UInt16
    private let width: Int
    private let height: Int
    private let fps: Int
    private let bitrate: Int

    private let transport: UDPTransport
    private let capture = ScreenCaptureKitCapture()
    private let injector: CGEventInjector
    private let inspector: AppleScreenSharingInspector
    private let stateMachine = SessionStateMachine()
    private var encoder: VideoToolboxEncoder?

    private let counters = PerfCounters()
    private let fpsMeter = RateMeter()
    private let mbpsMeter = RateMeter()
    private let encodeLatency = LatencyAccumulator()

    private let lock = NSLock()
    private var sessionID = UInt32.random(in: 1...UInt32.max)
    private var outgoingSequence: UInt32 = 0
    private var frameID: UInt32 = 0
    private var clientCaps: ClientCapabilities?
    private var inFlightFrames = 0          // encoder queue bound (spec §10: <= 2)
    private var gotClientCapabilities = false

    private var statsTimer: DispatchSourceTimer?
    private var lastRttMs: Double = 0
    private let rttAccumulator = LatencyAccumulator()
    private lazy var quality = AdaptiveQualityController(bitrate: bitrate, fps: fps)

    init(port: UInt16 = AppleScreenSharingInspector.defaultCustomHPPort,
         width: Int = 1920, height: Int = 1080, fps: Int = 60,
         bitrate: Int = 12_000_000,
         handshakeTimeout: TimeInterval = 10,
         inspector: AppleScreenSharingInspector = AppleScreenSharingInspector()) {
        self.port = port
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate
        self.handshakeTimeout = handshakeTimeout
        self.transport = UDPTransport(port: port)
        self.injector = CGEventInjector()
        self.inspector = inspector
    }

    private var handshakeTimeout: TimeInterval = 10

    // MARK: connect / disconnect

    func connect() async throws {
        // Screen Recording preflight happens at capture.start (streaming
        // phase) — the handshake/control plane must come up regardless so a
        // client can connect and receive an explicit capability story.

        stateMachine.transition(to: .authenticating)
        // Dev-mode auth: sender must speak our wire magic; pairing/TLS lands
        // with the MsQuic transport (docs/security.md).

        transport.onDatagram = { [weak self] data in self?.handlePacket(data) }
        try transport.start()

        stateMachine.transition(to: .negotiating)
        print("HP engine: listening on UDP \(port), waiting for client hello…")
        try await waitForClientCapabilities(timeout: handshakeTimeout)
        let caps = clientCaps
        print("HP engine: client negotiated (h264HW=\(caps?.decode.h264Hardware ?? false), " +
              "clientMaxFps=\(caps?.decode.maxFps ?? 0), display=\(caps?.display.refreshRate ?? 0) Hz)")

        // Pick the best common profile: min(host, client) fps.
        let negotiatedFPS = max(1, min(fps, caps?.decode.maxFps ?? fps))

        stateMachine.transition(to: .connectingHP)
        try startEngine(fps: negotiatedFPS)
        stateMachine.transition(to: .streamingHP)
        print("HP engine: streaming — \(width)×\(height) @ \(negotiatedFPS) fps, " +
              "target \(bitrate / 1_000_000) Mbps")
        startStatsLoop()
    }

    func disconnect() async {
        stopStatsLoop()
        await capture.stop()
        transport.stop()
        stateMachine.transition(to: .disconnected)
    }

    private func waitForClientCapabilities(timeout: TimeInterval) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            lock.lock()
            let ready = gotClientCapabilities
            lock.unlock()
            if ready { return }
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        throw SessionError.handshakeTimeout
    }

    // MARK: engine wiring

    private func startEngine(fps negotiatedFPS: Int) throws {
        let enc = try VideoToolboxEncoder(
            width: width, height: height, fps: negotiatedFPS, bitrate: bitrate)

        enc.onEncodedFrame = { [weak self] frame in
            self?.handleEncodedFrame(frame)
        }
        encoder = enc

        capture.onFrame = { [weak self] pixelBuffer, pts in
            guard let self else { return }
            let captureTsUs = WireHeader.nowUs()
            self.lock.lock()
            let busy = self.inFlightFrames
            self.inFlightFrames += 1
            self.lock.unlock()
            if busy >= 2 {
                // Queue policy (spec §10): drop before the encoder backs up —
                // latest frame beats complete history (spec §65).
                self.lock.lock()
                self.inFlightFrames -= 1
                self.lock.unlock()
                self.counters.add("frames_dropped_encoder_queue")
                return
            }
            let force = enc.shouldForceKeyframeOnNextEncode
            enc.encode(pixelBuffer, pts: pts, forceKeyframe: force, captureTsUs: captureTsUs)
        }

        capture.onStopped = { [weak self] error in
            FileHandle.standardError.write(Data("capture stopped: \(error)\n".utf8))
            self?.stateMachine.transition(to: .networkDegraded)
        }

        Task {
            do {
                try await capture.start(width: width, height: height, fps: negotiatedFPS)
            } catch {
                FileHandle.standardError.write(Data("capture failed to start: \(error)\n".utf8))
                self.stateMachine.transition(to: .networkDegraded)
            }
        }
    }

    private func handleEncodedFrame(_ frame: EncodedFrame) {
        defer {
            lock.lock()
            inFlightFrames = max(0, inFlightFrames - 1)
            lock.unlock()
        }

        // capture→encoded latency from the correlated capture timestamp.
        if frame.captureTsUs > 0 {
            encodeLatency.add(Double(frame.encodeDoneUs &- frame.captureTsUs) / 1000.0)
        }

        lock.lock()
        frameID &+= 1
        let id = frameID
        outgoingSequence &+= 1
        let seq = outgoingSequence
        lock.unlock()

        let meta = FrameMeta(
            frameID: id, keyframe: frame.isKeyframe, hasParamSets: frame.hasParamSets,
            codec: .h264, width: width, height: height,
            fpsProfile: UInt8(min(255, fps)),
            captureTsUs: frame.captureTsUs,
            encodeTsUs: frame.encodeDoneUs)
        let packets = Packetizer.split(
            meta: meta, payload: frame.annexB,
            sequenceBase: seq, sessionID: sessionID)

        // Pacing (spec §35) is naive here: datagrams go out in one burst per
        // frame; the queue stays < 1 frame on LAN. MsQuic path replaces this.
        for p in packets {
            mbpsMeter.mark(bytes: p.count)
            transport.send(p)
        }
        fpsMeter.mark()
        counters.add("video_packets_sent", Double(packets.count))
        counters.add("video_bytes_sent", Double(frame.annexB.count))
        if frame.isKeyframe { counters.add("keyframes_sent") }
    }

    // MARK: inbound

    private func handlePacket(_ data: Data) {
        guard let header = WireHeader.decode(data) else {
            counters.add("packets_invalid_magic")
            return
        }
        let payload = data.count > WireHeader.length
            ? data.subdata(in: (data.startIndex + WireHeader.length)..<data.endIndex)
            : Data()

        switch header.type {
        case .hello:
            if let json = try? JSONDecoder().decode(HelloMessage.self, from: payload) {
                print("HP engine: hello from \(json.deviceName) (v\(json.clientVersion))")
                sendHostCapabilities()
            }
        case .capabilities:
            if let caps = try? JSONDecoder().decode(ClientCapabilities.self, from: payload) {
                lock.lock()
                clientCaps = caps
                gotClientCapabilities = true
                lock.unlock()
            }
        case .control:
            guard let op = payload.first.flatMap(ControlOp.init(rawValue:)) else { return }
            if op == .requestKeyFrame {
                counters.add("keyframe_requests")
                encoder?.shouldForceKeyframeOnNextEncode = true
            }
        case .inputMouse:
            if let m = MouseInput.decode(payload) { injector.inject(mouse: m) }
        case .inputKey:
            if let k = KeyInput.decode(payload) { injector.inject(key: k) }
        case .inputScroll:
            if let s = ScrollInput.decode(payload) { injector.inject(scroll: s) }
        case .heartbeat:
            // Echo protocol: payload = sender's monotonic µs (spec §21 RTT feed).
            var r = ByteReader(payload)
            if let senderTs = r.u64() {
                let rttUs = WireHeader.nowUs() &- senderTs
                lastRttMs = Double(rttUs) / 1000.0
                rttAccumulator.add(lastRttMs)
                counters.add("rtt_samples")
            }
        default:
            counters.add("packets_unexpected_type")
        }
    }

    private func sendHostCapabilities() {
        let caps = HostCapabilities(
            capture: .init(screenCaptureKit: true, maxWidth: width, maxHeight: height,
                           maxFps: 120, hdr: false),
            video: .init(h264: true, hevc: false, av1: false),
            appleScreenSharing: .init(enabled: inspector.hardwareH264EncoderAvailable(),
                                      nativeHighPerformanceEligible: inspector.isAppleSilicon))
        guard let json = try? JSONEncoder().encode(caps) else { return }
        lock.lock()
        outgoingSequence &+= 1
        let packet = PacketFactory.capabilities(json, sessionID: sessionID,
                                                sequence: outgoingSequence)
        lock.unlock()
        // Dev UDP has no reliability — reliable-flagged JSON goes out 3×.
        for _ in 0..<3 { transport.send(packet) }
    }

    // MARK: IRemoteSession (client-side senders are no-ops on the host)

    func requestKeyFrame() { encoder?.shouldForceKeyframeOnNextEncode = true }

    func setResolution(width newWidth: Int, height newHeight: Int) {
        // Dynamic resolution is V2 (adaptive quality, spec §36); recorded, not applied.
        counters.add("resolution_change_requests")
        print("HP engine: setResolution(\(newWidth)×\(newHeight)) queued — adaptive profile lands in Phase 9")
    }

    func setFrameRate(_ newFPS: Int) {
        counters.add("fps_change_requests")
        print("HP engine: setFrameRate(\(newFPS)) queued — adaptive profile lands in Phase 9")
    }

    func sendMouse(_ mouse: MouseInput) {}
    func sendKeyboard(keyCode: UInt16, down: Bool, flags: UInt64) {}
    func sendScroll(dx: Float, dy: Float) {}
    func sendClipboard(text: String) {}

    func statistics() -> [String: Double] {
        var s = counters.snapshot()
        s["fps"] = fpsMeter.lastRate
        s["mbps"] = mbpsMeter.lastMbps
        let e = encodeLatency.snapshot()
        s["encode_ms_avg"] = e.avg
        s["encode_ms_max"] = e.max
        return s
    }

    // MARK: stats loop

    private func startStatsLoop() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "mac-host.stats"))
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.fpsMeter.tick()
            self.mbpsMeter.tick()

            // Heartbeat keeps RTT fresh for the controller (spec §21).
            self.lock.lock()
            self.outgoingSequence &+= 1
            let beat = PacketFactory.heartbeat(WireHeader.nowUs(),
                                               sessionID: self.sessionID,
                                               sequence: self.outgoingSequence)
            self.lock.unlock()
            self.transport.send(beat)

            // Adaptive quality tick + live bitrate application (spec §21/§36).
            let e = self.encodeLatency.snapshot()
            let decision = self.quality.tick(
                rttMs: self.lastRttMs, lossPct: 0,
                encoderInFlight: self.lock.withLock { self.inFlightFrames },
                encodeMs: e.avg)
            if decision.bitrate != self.encoder?.currentBitrate {
                self.encoder?.setBitrate(decision.bitrate)
            }

            let s = self.statistics()
            print(String(format: "HP  fps=%.1f  bitrate=%d/%.1f Mbps  encode(avg/max)=%.1f/%.1f ms  rtt=%.1f ms  drops(EncoderQueue)=%.0f  keyframes=%.0f  [%@]",
                         s["fps"] ?? 0, decision.bitrate / 1_000_000, s["mbps"] ?? 0,
                         s["encode_ms_avg"] ?? 0, s["encode_ms_max"] ?? 0,
                         self.lastRttMs,
                         s["frames_dropped_encoder_queue"] ?? 0, s["keyframes_sent"] ?? 0,
                         decision.note))
        }
        timer.resume()
        statsTimer = timer
    }

    private func stopStatsLoop() {
        statsTimer?.cancel()
        statsTimer = nil
    }
}
