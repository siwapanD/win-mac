import Foundation
import Network

/// Interim VNC fallback for a from-scratch deployment (spec §51): a TCP proxy
/// between the Windows client's VNC adapter and macOS's built-in Screen Sharing
/// RFB server on 127.0.0.1:5900.
///
/// Requires Apple Screen Sharing to be enabled on the host. If it is not, the
/// proxy refuses with an explicit reason — fallback must never be silent.
/// A self-contained RFB server (no dependency on Apple's service) is tracked
/// in vnc/README.md.
final class VncProxySession: IRemoteSession, @unchecked Sendable {
    let modeName = "Compatibility (VNC proxy)"

    private let listenPort: UInt16
    private let vncPort: UInt16
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "mac-host.vnc.proxy")
    private var activePairs: [UUID: (NWConnection, NWConnection)] = [:]
    private let lock = NSLock()

    init(listenPort: UInt16 = 55444, vncPort: UInt16 = AppleScreenSharingInspector.appleVNCPort) {
        self.listenPort = listenPort
        self.vncPort = vncPort
    }

    enum ProxyError: LocalizedError {
        case appleScreenSharingUnavailable
        case listenFailed(UInt16)
        var errorDescription: String? {
            switch self {
            case .appleScreenSharingUnavailable:
                return "VNC fallback unavailable: macOS Screen Sharing (RFB :5900) is not " +
                       "enabled. Enable it in System Settings → General → Sharing → Screen Sharing."
            case .listenFailed(let port):
                return "VNC proxy could not listen on TCP \(port)."
            }
        }
    }

    func start() async throws {
        guard await isVNCAvailable() else { throw ProxyError.appleScreenSharingUnavailable }

        guard let p = NWEndpoint.Port(rawValue: listenPort) else {
            throw ProxyError.listenFailed(listenPort)
        }
        let listener: NWListener
        do {
            listener = try NWListener(using: .tcp, on: p)
        } catch {
            throw ProxyError.listenFailed(listenPort)
        }
        listener.newConnectionHandler = { [weak self] clientConn in
            self?.handleClient(clientConn)
        }
        listener.stateUpdateHandler = { state in
            if case .failed(let error) = state {
                FileHandle.standardError.write(
                    Data("VNC proxy listener failed: \(error)\n".utf8))
            }
        }
        listener.start(queue: queue)
        self.listener = listener
        print("VNC proxy: TCP \(listenPort) → 127.0.0.1:\(vncPort) (Apple Screen Sharing RFB)")
    }

    func stop() {
        listener?.cancel()
        listener = nil
        lock.withLock { activePairs.values }.forEach { $0.0.cancel(); $0.1.cancel() }
        lock.withLock { activePairs.removeAll() }
    }

    private func isVNCAvailable() async -> Bool {
        guard let p = NWEndpoint.Port(rawValue: vncPort) else { return false }
        return await withCheckedContinuation { continuation in
            let conn = NWConnection(host: "127.0.0.1", port: p, using: .tcp)
            let lock = NSLock()
            var done = false
            func finish(_ v: Bool) {
                lock.lock(); let already = done; done = true; lock.unlock()
                guard !already else { return }
                conn.cancel()
                continuation.resume(returning: v)
            }
            conn.stateUpdateHandler = { state in
                switch state {
                case .ready: finish(true)
                case .failed, .cancelled: finish(false)
                default: break
                }
            }
            conn.start(queue: queue)
            queue.asyncAfter(deadline: .now() + .milliseconds(800)) { finish(false) }
        }
    }

    private func handleClient(_ client: NWConnection) {
        guard let p = NWEndpoint.Port(rawValue: vncPort) else { return }
        let vnc = NWConnection(host: "127.0.0.1", port: p, using: .tcp)
        let id = UUID()
        // Start piping only when BOTH legs are ready — sending into a leg that
        // is not yet .ready fails the send and tears the pair down.
        let readyLock = NSLock()
        var readyCount = 0
        func markReady() {
            readyLock.lock()
            readyCount += 1
            let both = readyCount >= 2
            readyLock.unlock()
            guard both else { return }
            lock.withLock { activePairs[id] = (client, vnc) }
            pipe(from: client, to: vnc)
            pipe(from: vnc, to: client)
        }

        vnc.stateUpdateHandler = { state in
            switch state {
            case .ready: markReady()
            case .failed, .cancelled: client.cancel()
            default: break
            }
        }
        client.stateUpdateHandler = { state in
            switch state {
            case .ready: markReady()
            case .failed, .cancelled: vnc.cancel()
            default: break
            }
        }
        vnc.start(queue: queue)
        client.start(queue: queue)
    }

    private func pipe(from src: NWConnection, to dst: NWConnection) {
        src.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, _, error in
            if let data, !data.isEmpty {
                dst.send(content: data, completion: .contentProcessed { sendError in
                    if sendError != nil { src.cancel(); dst.cancel() }
                })
            }
            if error != nil {
                src.cancel(); dst.cancel()
                return
            }
            self?.pipe(from: src, to: dst)
        }
    }

    // MARK: IRemoteSession (proxy is byte-transparent)

    func connect() async throws { try await start() }
    func disconnect() async { stop() }
    func requestKeyFrame() {}
    func setResolution(width: Int, height: Int) {}
    func setFrameRate(_ fps: Int) {}
    func sendMouse(_ mouse: MouseInput) {}
    func sendKeyboard(keyCode: UInt16, down: Bool, flags: UInt64) {}
    func sendScroll(dx: Float, dy: Float) {}
    func sendClipboard(text: String) {}
    func statistics() -> [String: Double] {
        ["active_pairs": Double(lock.withLock { activePairs.count })]
    }
}
