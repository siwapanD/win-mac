import Foundation

/// Transport-agnostic session interface (spec §6). The UI layer must not know
/// whether the backend is VNC or High Performance. Host-side implementations
/// no-op the send* methods (input flows client → host via the receive path).
protocol IRemoteSession: AnyObject {
    var modeName: String { get }

    func connect() async throws
    func disconnect() async

    func requestKeyFrame()
    func setResolution(width: Int, height: Int)
    func setFrameRate(_ fps: Int)

    // Client-side senders; host implementations are no-ops.
    func sendMouse(_ mouse: MouseInput)
    func sendKeyboard(keyCode: UInt16, down: Bool, flags: UInt64)
    func sendScroll(dx: Float, dy: Float)
    func sendClipboard(text: String)

    func statistics() -> [String: Double]
}
