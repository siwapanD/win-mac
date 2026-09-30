import Foundation
import CoreMedia
import CoreVideo

enum Commands {
    struct AppError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    static func int(_ s: String?, _ def: Int) -> Int { s.flatMap(Int.init) ?? def }

    // MARK: - inspect (spec §28/§45)

    static func inspect(_ flags: Flags) async throws {
        let inspector = AppleScreenSharingInspector()
        let diag = await inspector.diagnostics()
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        print(String(decoding: try encoder.encode(diag), as: UTF8.self))
        print("""
        ---
        Apple Screen Sharing:      \(diag.appleScreenSharingEnabled ? "Enabled" : "Disabled")
        Native HP Eligible:        \(diag.nativeHighPerformanceEligible ? "Yes" : "No")
        Custom High Performance:   \(diag.customHPPortFree ? "Available (UDP \(AppleScreenSharingInspector.defaultCustomHPPort) free)" : "Port \(AppleScreenSharingInspector.defaultCustomHPPort) busy")
        VNC (proxy):               \(diag.appleScreenSharingEnabled ? "Available" : "Needs macOS Screen Sharing enabled")
        High Performance ready:    \(diag.screenRecordingPermission && diag.hardwareH264Encoder ? "Yes" : "No — fix permissions / encoder above")
        """)
    }

    // MARK: - encode-test (VideoToolbox verification, no capture permission needed)

    static func encodeTest(_ flags: Flags) async throws {
        let width = int(flags["width"], 1920)
        let height = int(flags["height"], 1080)
        let fps = int(flags["fps"], 60)
        let seconds = Double(int(flags["seconds"], 5))
        let bitrate = int(flags["bitrate"], 12_000_000)
        let outPath = flags["out"] ?? "/tmp/mac-host-encode-test.h264"

        print("encode-test: \(width)×\(height) @ \(fps) fps for \(Int(seconds)) s, target \(bitrate / 1_000_000) Mbps")
        let encoder = try VideoToolboxEncoder(width: width, height: height, fps: fps, bitrate: bitrate)

        let semaphore = DispatchSemaphore(value: 2) // in-flight ≤ 2 (spec §10)
        let fileLock = NSLock()
        var annexBFile = Data()
        let encodeLatency = LatencyAccumulator()

        encoder.onEncodedFrame = { frame in
            if frame.captureTsUs > 0 {
                encodeLatency.add(Double(frame.encodeDoneUs &- frame.captureTsUs) / 1000.0)
            }
            fileLock.lock()
            annexBFile.append(frame.annexB)
            fileLock.unlock()
            semaphore.signal()
        }

        var pool: [CVPixelBuffer] = []
        for _ in 0..<4 {
            var pb: CVPixelBuffer?
            let st = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                                         kCVPixelFormatType_32BGRA, nil, &pb)
            guard st == kCVReturnSuccess, let pb else {
                throw VideoToolboxEncoder.EncoderError.createFailed(OSStatus(st))
            }
            pool.append(pb)
        }

        let totalFrames = Int(seconds * Double(fps))
        for i in 0..<totalFrames {
            let buf = pool[i % pool.count]
            fillTestPattern(buf, frameIndex: i, fps: fps)
            let pts = CMTime(value: CMTimeValue(i), timescale: CMTimeScale(fps))
            semaphore.wait()
            encoder.encode(buf, pts: pts, captureTsUs: WireHeader.nowUs())
        }

        let deadline = Date().addingTimeInterval(seconds + 10)
        while Date() < deadline, encoder.encodedFrames < totalFrames { usleep(20_000) }

        let e = encodeLatency.snapshot()
        try annexBFile.write(to: URL(fileURLWithPath: outPath))
        print(String(format: """
        encoded %d/%d frames (keyframes %d), %d bytes annex-B
        encode latency avg %.2f ms  min %.2f ms  max %.2f ms  (cap: in-flight ≤ 2)
        output: %s
        """,
        encoder.encodedFrames, totalFrames, encoder.keyframes, encoder.bytesOut,
        e.avg, e.min, e.max, outPath))
    }

    private static func fillTestPattern(_ pb: CVPixelBuffer, frameIndex: Int, fps: Int) {
        CVPixelBufferLockBaseAddress(pb, [])
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return }
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pb)
        let w = CVPixelBufferGetWidth(pb)
        let h = CVPixelBufferGetHeight(pb)
        let rows = base.assumingMemoryBound(to: UInt8.self)

        for row in 0..<h {
            memset(rows + row * bytesPerRow, CInt(40 + (row % 32) * 3), w * 4)
        }
        let range = max(1, w - 140)
        let barX = (frameIndex * max(1, w / max(1, fps))) % range
        for row in 0..<h {
            memset(rows + row * bytesPerRow + barX * 4, CInt(235), 120 * 4)
        }
        // fine grid: sharp content so the encoder has real work
        for row in Swift.stride(from: 0, to: h, by: 3) {
            for col in Swift.stride(from: 0, to: w, by: 3) {
                let p = rows + row * bytesPerRow + col * 4
                p[0] = 255; p[1] = 255; p[2] = 255
            }
        }
    }

    // MARK: - capture-test (ScreenCaptureKit → VideoToolbox, spec Phase 3/4 gate)

    static func captureTest(_ flags: Flags) async throws {
        let width = int(flags["width"], 1920)
        let height = int(flags["height"], 1080)
        let fps = int(flags["fps"], 60)
        let seconds = Double(int(flags["seconds"], 10))
        let bitrate = int(flags["bitrate"], 12_000_000)
        let outPath = flags["out"]

        try ScreenCaptureKitCapture.preflight()
        let encoder = try VideoToolboxEncoder(width: width, height: height, fps: fps, bitrate: bitrate)
        let capture = ScreenCaptureKitCapture()

        let fileLock = NSLock()
        var annexBFile = Data()
        let captureMeter = RateMeter()
        let encodeMeter = RateMeter()
        let mbpsMeter = RateMeter()
        let capToEnc = LatencyAccumulator()

        encoder.onEncodedFrame = { frame in
            if frame.captureTsUs > 0 {
                capToEnc.add(Double(frame.encodeDoneUs &- frame.captureTsUs) / 1000.0)
            }
            if outPath != nil {
                fileLock.lock()
                annexBFile.append(frame.annexB)
                fileLock.unlock()
            }
            encodeMeter.mark()
            mbpsMeter.mark(bytes: frame.annexB.count)
        }
        capture.onFrame = { pixelBuffer, pts in
            captureMeter.mark()
            encoder.encode(pixelBuffer, pts: pts, captureTsUs: WireHeader.nowUs())
        }

        print("capture-test: \(width)×\(height) @ \(fps) fps for \(Int(seconds)) s …")
        try await capture.start(width: width, height: height, fps: fps)

        for _ in 0..<Int(seconds) {
            sleep(1)
            captureMeter.tick()
            encodeMeter.tick()
            mbpsMeter.tick()
            print(String(format: "capture %5.1f fps | encoded %5.1f fps | %6.1f Mbps | capture→encode avg %.1f ms",
                         captureMeter.lastRate, encodeMeter.lastRate, mbpsMeter.lastMbps,
                         capToEnc.snapshot().avg))
        }

        await capture.stop()
        if let outPath {
            fileLock.lock()
            let data = annexBFile
            fileLock.unlock()
            try data.write(to: URL(fileURLWithPath: outPath))
            print("wrote \(outPath) (\(data.count) bytes)")
        }
        let e = capToEnc.snapshot()
        print(String(format: "totals: captured≈%.0f encoded=%d keyframes=%d bytes=%d | capture→encode avg %.1f ms max %.1f ms",
                     captureMeter.lastRate, encoder.encodedFrames, encoder.keyframes,
                     encoder.bytesOut, e.avg, e.max))
    }

    // MARK: - serve (spec §7 Auto algorithm + fallback)

    static func serve(_ flags: Flags) async throws {
        let modeRaw = flags["mode"] ?? "auto"
        guard let mode = ConnectionMode(rawValue: modeRaw) else {
            throw Commands.AppError(message: "invalid --mode \(modeRaw) (auto|hp|vnc)")
        }
        let port = UInt16(int(flags["port"], Int(AppleScreenSharingInspector.defaultCustomHPPort)))
        let vncPort = UInt16(int(flags["vnc-port"], 55444))
        let width = int(flags["width"], 1920)
        let height = int(flags["height"], 1080)
        let fps = int(flags["fps"], 60)
        let bitrate = int(flags["bitrate"], 12_000_000)

        let inspector = AppleScreenSharingInspector()
        let diag = await inspector.diagnostics()
        print("""
        host: \(diag.appleSilicon ? "Apple Silicon" : "Intel") · macOS \(diag.macOSVersion) · display \(diag.displayResolution) @ \(Int(diag.displayRefreshRateHz)) Hz
        permissions: screenRecording=\(diag.screenRecordingPermission) accessibility=\(diag.accessibilityPermission)
        encoder: h264HW=\(diag.hardwareH264Encoder) · Apple Screen Sharing enabled=\(diag.appleScreenSharingEnabled) (native HP eligible=\(diag.nativeHighPerformanceEligible))
        """)

        let factory = RemoteSessionFactory(inspector: inspector)
        // auto: bounded handshake so fallback can trigger; explicit hp: wait for client
        let handshakeTimeout: TimeInterval = mode == .auto ? 10 : 86_400

        let (session, reason) = try await factory.runWithFallback(
            mode: mode,
            makeHP: {
                HighPerformanceRemoteSession(port: port, width: width, height: height,
                                             fps: fps, bitrate: bitrate,
                                             handshakeTimeout: handshakeTimeout,
                                             inspector: inspector)
            },
            makeVNC: { VncProxySession(listenPort: vncPort) })
        print("serve: \(session.modeName) — \(reason)")
        print("serve: Ctrl+C to stop")

        signal(SIGINT, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGINT, queue: DispatchQueue.global())
        source.setEventHandler { exit(0) }
        source.resume()

        // Park forever. dispatchMain() must not be called from a Swift
        // concurrency cooperative thread — it kills the process.
        while true {
            try await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    // MARK: - client-test (loopback integration harness, spec §62 smoke test)

    /// Speaks the HP protocol like the Windows client: hello → capabilities →
    /// receive video (reassembled) → echo heartbeats (RTT). Against a local
    /// `serve --mode hp` this proves the whole host pipeline; with Screen
    /// Recording granted and `--expect-video` it is the 1080p60 smoke test.
    static func clientTest(_ flags: Flags) async throws {
        let host = flags["host"] ?? "127.0.0.1"
        let port = UInt16(int(flags["port"], 55443))
        let seconds = Double(int(flags["seconds"], 5))
        let expectVideo = flags["expect-video"] != nil

        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else {
            throw AppError(message: "socket() failed: \(String(cString: strerror(errno)))")
        }
        defer { close(fd) }
        var tv = timeval(tv_sec: 0, tv_usec: 200_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var rcvBuf: Int32 = 4 * 1024 * 1024 // burst headroom for a 12 Mbps stream
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        guard inet_pton(AF_INET, host, &addr.sin_addr) == 1 else {
            throw AppError(message: "invalid host: \(host)")
        }

        func send(_ d: Data) {
            d.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                withUnsafePointer(to: &addr) { a in
                    a.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                        _ = sendto(fd, raw.baseAddress, raw.count, 0, sa,
                                   socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
        }

        let sessionID = UInt32.random(in: 1...UInt32.max)
        let seqBox = LockedSequence()
        func nextSeq() -> UInt32 { seqBox.next() }

        // hello + capabilities, reliable-flagged 3× until the host replies
        let helloJSON = try JSONEncoder().encode(HelloMessage(
            deviceName: "loopback-client", clientVersion: "0.1", mode: "hp"))
        let capsJSON = try JSONEncoder().encode(ClientCapabilities(
            decode: .init(h264Hardware: true, hevcHardware: false, maxFps: 60, hdr: false),
            display: .init(refreshRate: 75)))
        func sendHandshake() {
            for _ in 0..<3 {
                send(PacketFactory.hello(helloJSON, sessionID: sessionID, sequence: nextSeq()))
                send(PacketFactory.capabilities(capsJSON, sessionID: sessionID, sequence: nextSeq()))
            }
        }
        sendHandshake()
        var lastHandshake = Date()

        var reasm = FrameReassembler()
        var gotHostCaps = false
        var rttSamples: [Double] = []
        var frames = 0
        var keyframes = 0
        var bytes = 0
        var firstFrameID: UInt32?
        var lastFrameID: UInt32 = 0
        var latencies: [Double] = []
        var buffer = [UInt8](repeating: 0, count: 65_536)
        let deadline = Date().addingTimeInterval(seconds)

        while Date() < deadline {
            if !gotHostCaps, Date().timeIntervalSince(lastHandshake) > 2 {
                sendHandshake()
                lastHandshake = Date()
            }
            let n = buffer.withUnsafeMutableBytes { raw in
                recvfrom(fd, raw.baseAddress, raw.count, 0, nil, nil)
            }
            guard n > 0 else { continue }
            let data = Data(buffer[0..<n])
            guard let h = WireHeader.decode(data) else { continue }
            let payload = data.count > WireHeader.length
                ? data.subdata(in: (data.startIndex + WireHeader.length)..<data.endIndex)
                : Data()

            switch h.type {
            case .capabilities:
                if !gotHostCaps,
                   let caps = try? JSONDecoder().decode(HostCapabilities.self, from: payload) {
                    gotHostCaps = true
                    print("handshake: host caps — sck=\(caps.capture.screenCaptureKit) " +
                          "h264=\(caps.video.h264) maxFps=\(caps.capture.maxFps) " +
                          "appleSS=\(caps.appleScreenSharing.enabled)")
                }
            case .heartbeat:
                var r = ByteReader(payload)
                if let ts = r.u64() {
                    rttSamples.append(Double(WireHeader.nowUs() &- ts) / 1000.0)
                    send(PacketFactory.heartbeat(ts, sessionID: sessionID, sequence: nextSeq()))
                }
            case .videoFrame:
                if let (fh, annexB) = reasm.ingest(data) {
                    frames += 1
                    bytes += annexB.count
                    if fh.isKeyframe { keyframes += 1 }
                    if firstFrameID == nil { firstFrameID = fh.frameID }
                    lastFrameID = max(lastFrameID, fh.frameID)
                    if fh.captureTsUs > 0 {
                        latencies.append(Double(WireHeader.nowUs() &- fh.captureTsUs) / 1000.0)
                    }
                }
            default:
                break
            }
        }

        guard gotHostCaps else {
            throw AppError(message: "handshake failed — no host capabilities " +
                                   "(is `mac-host serve --mode hp` running on \(host):\(port)?)")
        }
        if !rttSamples.isEmpty {
            let avg = rttSamples.reduce(0, +) / Double(rttSamples.count)
            print(String(format: "rtt: %d samples  avg %.2f ms  max %.2f ms",
                         rttSamples.count, avg, rttSamples.max() ?? 0))
        }
        if frames > 0 {
            let lost = Int(lastFrameID &- (firstFrameID ?? 0) &+ 1) - frames
            let lat = latencies.sorted()
            let p50 = lat.isEmpty ? 0 : lat[lat.count / 2]
            let p95 = lat.isEmpty ? 0 : lat[min(lat.count - 1, Int(Double(lat.count) * 0.95))]
            let mbps = Double(bytes * 8) / seconds / 1_000_000
            print(String(format: "video: %d frames (%d keyframes)  loss %d  cap→recv p50 %.1f ms  p95 %.1f ms  %.1f Mbps",
                         frames, keyframes, lost, p50, p95, mbps))
            if expectVideo {
                guard frames >= Int(seconds * 30) else {
                    throw AppError(message: "smoke failed: only \(frames) frames in \(Int(seconds)) s (need ≥ \(Int(seconds * 30)))")
                }
                guard lost == 0 else { throw AppError(message: "smoke failed: \(lost) frames lost on loopback") }
                print("smoke: PASS (loopback)")
            }
        } else if expectVideo {
            throw AppError(message: "expected video but received 0 frames — " +
                                   "host needs Screen Recording permission")
        } else {
            print("video: SKIP (0 frames — grant Screen Recording on the host for full smoke)")
        }
    }
}

/// Tiny sequence generator safe for use from multiple senders.
final class LockedSequence: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt32 = 0
    func next() -> UInt32 {
        lock.lock(); defer { lock.unlock() }
        value &+= 1
        return value
    }
}
