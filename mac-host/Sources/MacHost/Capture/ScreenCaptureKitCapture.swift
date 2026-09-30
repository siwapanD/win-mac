import CoreVideo
import CoreMedia
import ScreenCaptureKit

/// ScreenCaptureKit-based capture (spec §8): SCStream at a fixed target
/// resolution and cadence, cursor excluded from the video (the Windows client
/// renders it locally, spec §16). Output stays a GPU-backed CVPixelBuffer for
/// as long as the API allows — no CPU bitmap polling, no screenshot loops.
final class ScreenCaptureKitCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    enum CaptureError: LocalizedError {
        case screenRecordingPermissionDenied
        case noDisplay
        case startFailed(String)

        var errorDescription: String? {
            switch self {
            case .screenRecordingPermissionDenied:
                return """
                Screen Recording permission is missing. High Performance mode requires it.
                Grant it in System Settings → Privacy & Security → Screen Recording for the
                app that launched mac-host, then retry. (VNC fallback stays available.)
                """
            case .noDisplay:
                return "No display found to capture."
            case .startFailed(let detail):
                return "SCStream failed to start: \(detail)"
            }
        }
    }

    var onFrame: ((CVPixelBuffer, CMTime) -> Void)?
    var onStopped: ((Error) -> Void)?

    private var stream: SCStream?
    private let sampleQueue = DispatchQueue(label: "mac-host.capture.sck") // Capture Queue (§43)
    private(set) var configWidth = 0
    private(set) var configHeight = 0
    private(set) var configFPS = 0

    var isRunning: Bool { stream != nil }

    /// Preflight so the process never hangs on a permission prompt it cannot show.
    static func preflight() throws {
        guard CGPreflightScreenCaptureAccess() else {
            throw CaptureError.screenRecordingPermissionDenied
        }
    }

    func start(width: Int, height: Int, fps: Int) async throws {
        guard !isRunning else { return }
        try Self.preflight()

        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let display = content.displays.first else { throw CaptureError.noDisplay }

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let config = SCStreamConfiguration()
        config.width = width
        config.height = height
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        config.queueDepth = 3 // low queue depth keeps latency; never grow it to hide a slow encoder (§9)
        config.showsCursor = false
        config.pixelFormat = kCVPixelFormatType_32BGRA

        let stream = SCStream(filter: filter, configuration: config, delegate: self)
        try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: sampleQueue)
        self.stream = stream
        configWidth = width
        configHeight = height
        configFPS = fps

        do {
            try await stream.startCapture()
        } catch {
            self.stream = nil
            throw CaptureError.startFailed("\(error)")
        }
    }

    func stop() async {
        guard let stream else { return }
        self.stream = nil
        try? await stream.stopCapture()
    }

    // MARK: SCStreamOutput

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen, sampleBuffer.isValid,
              let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        onFrame?(pixelBuffer, pts)
    }

    // MARK: SCStreamDelegate

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        self.stream = nil
        onStopped?(error)
    }
}
