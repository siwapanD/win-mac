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
        let callTsLock = NSLock()
        var callTs: [Int64: UInt64] = [:]
        let fileLock = NSLock()
        var annexBFile = Data()
        let encodeLatency = LatencyAccumulator()
        let encodeMeter = RateMeter()
        let mbpsMeter = RateMeter()

        encoder.onEncodedFrame = { frame in
            let done = WireHeader.nowUs()
            callTsLock.lock()
            let t0 = callTs.removeValue(forKey: frame.pts.value)
            callTsLock.unlock()
            if let t0 { encodeLatency.add(Double(done &- t0) / 1000.0) }
            fileLock.lock()
            annexBFile.append(frame.annexB)
            fileLock.unlock()
            encodeMeter.mark()
            mbpsMeter.mark(bytes: frame.annexB.count)
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
            callTsLock.lock()
            callTs[pts.value] = WireHeader.nowUs()
            callTsLock.unlock()
            encoder.encode(buf, pts: pts)
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
        _ = encodeMeter; _ = mbpsMeter
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

        let callTsLock = NSLock()
        var callTs: [Int64: UInt64] = [:]
        let fileLock = NSLock()
        var annexBFile = Data()
        let captureMeter = RateMeter()
        let encodeMeter = RateMeter()
        let mbpsMeter = RateMeter()
        let capToEnc = LatencyAccumulator()

        encoder.onEncodedFrame = { frame in
            let done = WireHeader.nowUs()
            callTsLock.lock()
            let capturedAt = callTs.removeValue(forKey: frame.pts.value)
            callTsLock.unlock()
            if let capturedAt { capToEnc.add(Double(done &- capturedAt) / 1000.0) }
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
            callTsLock.lock()
            callTs[pts.value] = WireHeader.nowUs()
            callTsLock.unlock()
            encoder.encode(pixelBuffer, pts: pts)
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
}
