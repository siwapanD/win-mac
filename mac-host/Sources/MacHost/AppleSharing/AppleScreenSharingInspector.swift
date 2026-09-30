import AppKit
import ApplicationServices
import CoreGraphics
import Network
import VideoToolbox

/// Host diagnostics (spec §3/§45) with special attention to Apple Screen
/// Sharing coexistence (spec §28): we detect, we never disable or replace.
struct HostDiagnostics: Codable {
    var appleSilicon: Bool
    var macOSVersion: String
    var screenRecordingPermission: Bool
    var accessibilityPermission: Bool
    var hardwareH264Encoder: Bool
    var screenCaptureKitAvailable: Bool
    var displayResolution: String
    var displayRefreshRateHz: Double
    var customHPPortFree: Bool
    var appleScreenSharingEnabled: Bool
    var appleScreenSharingProcessRunning: Bool
    var nativeHighPerformanceEligible: Bool
}

final class AppleScreenSharingInspector {
    static let defaultCustomHPPort: UInt16 = 55443 // never 5900–5902 (spec §3 MUST)
    static let appleVNCPort: UInt16 = 5900

    var isAppleSilicon: Bool {
        ProcessInfo.processInfo.machineDescription?.hasPrefix("arm64") ?? false
    }

    var macOSVersionString: String {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        return "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)"
    }

    var screenRecordingPermission: Bool { CGPreflightScreenCaptureAccess() }
    var accessibilityPermission: Bool { AXIsProcessTrusted() }

    var screenCaptureKitAvailable: Bool {
        // SCStream exists since macOS 12.3; this binary links it directly.
        true
    }

    /// Probes for a hardware H.264 encode session (spec: hardware required).
    func hardwareH264EncoderAvailable() -> Bool {
        var session: VTCompressionSession?
        let spec: [CFString: Any] = [
            kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true
        ]
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault, width: 640, height: 360,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary,
            imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: { _, _, _, _, _ in }, refcon: nil,
            compressionSessionOut: &session)
        if status == noErr, let s = session {
            VTCompressionSessionInvalidate(s)
            return true
        }
        return false
    }

    func diagnostics() async -> HostDiagnostics {
        let appleSharingEnabled = await tcpReachable(
            host: "127.0.0.1", port: Self.appleVNCPort, timeoutMs: 500)
        let screensharingd = processRunning("screensharing")
        let customPortFree = Self.isUDPPortBindable(Self.defaultCustomHPPort)

        let mainDisplay = CGMainDisplayID()
        let w = CGDisplayPixelsWide(mainDisplay)
        let h = CGDisplayPixelsHigh(mainDisplay)
        let refresh = CGDisplayCopyDisplayMode(mainDisplay)?.refreshRate ?? 0

        let nativeEligible = isAppleSilicon && (appleSharingEnabled || screensharingd)

        return HostDiagnostics(
            appleSilicon: isAppleSilicon,
            macOSVersion: macOSVersionString,
            screenRecordingPermission: screenRecordingPermission,
            accessibilityPermission: accessibilityPermission,
            hardwareH264Encoder: hardwareH264EncoderAvailable(),
            screenCaptureKitAvailable: screenCaptureKitAvailable,
            displayResolution: "\(w)×\(h)",
            displayRefreshRateHz: refresh,
            customHPPortFree: customPortFree,
            appleScreenSharingEnabled: appleSharingEnabled,
            appleScreenSharingProcessRunning: screensharingd,
            nativeHighPerformanceEligible: nativeEligible)
    }

    // MARK: probes

    private func tcpReachable(host: String, port: UInt16, timeoutMs: Int) async -> Bool {
        guard let p = NWEndpoint.Port(rawValue: port) else { return false }
        return await withCheckedContinuation { continuation in
            let conn = NWConnection(host: NWEndpoint.Host(host), port: p, using: .tcp)
            let queue = DispatchQueue(label: "inspector.probe")
            let lock = NSLock()
            var done = false
            func finish(_ value: Bool) {
                lock.lock()
                let already = done
                done = true
                lock.unlock()
                guard !already else { return }
                conn.cancel()
                continuation.resume(returning: value)
            }
            conn.stateUpdateHandler = { state in
                switch state {
                case .ready: finish(true)
                case .failed, .cancelled: finish(false)
                default: break
                }
            }
            conn.start(queue: queue)
            queue.asyncAfter(deadline: .now() + .milliseconds(timeoutMs)) { finish(false) }
        }
    }

    /// Deterministic UDP bind probe via POSIX sockets (NWListener reports
    /// spurious failures in restricted environments).
    static func isUDPPortBindable(_ port: UInt16) -> Bool {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr = in_addr(s_addr: INADDR_ANY)
        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        return bindResult == 0
    }

    private func processRunning(_ nameSubstring: String) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        // macOS 26 hosts screen sharing via ScreenSharing* XPC services; -i -f
        // catches screensharingd, ScreenSharingSubscriber, etc.
        p.arguments = ["-if", nameSubstring]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do {
            try p.run()
            p.waitUntilExit()
            return p.terminationStatus == 0
        } catch {
            return false
        }
    }
}
