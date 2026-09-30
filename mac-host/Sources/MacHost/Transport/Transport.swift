import Foundation
import Network

protocol DataTransport: AnyObject {
    /// Called on the transport queue for every inbound datagram.
    var onDatagram: ((Data) -> Void)? { get set }
    func start() throws
    /// Sends to the current peer (first accepted endpoint). Multi-client is V2.
    func send(_ data: Data)
    func stop()
    var peerConnected: Bool { get }
}

struct TransportStats {
    var packetsSent = 0
    var bytesSent = 0
    var packetsReceived = 0
    var bytesReceived = 0
    var sendErrors = 0
}

/// Dev-mode LAN transport: plain UDP datagrams on the custom HP port
/// (default 55443 — never 5900–5902, spec §3 MUST). Production path is
/// QUIC + TLS 1.3 via MsQuic (spec §19) behind this same interface; the wire
/// format and session logic do not change.
final class UDPTransport: DataTransport, @unchecked Sendable {
    let port: UInt16
    private var listener: NWListener?
    private var connection: NWConnection?
    private let queue = DispatchQueue(label: "mac-host.transport.udp") // Network Queue (§43)
    private let lock = NSLock()
    var onDatagram: ((Data) -> Void)?
    private(set) var stats = TransportStats()

    init(port: UInt16) {
        self.port = port
    }

    var peerConnected: Bool {
        lock.lock(); defer { lock.unlock() }
        return connection != nil
    }

    func start() throws {
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        guard let p = NWEndpoint.Port(rawValue: port) else {
            throw TransportError.invalidPort(port)
        }
        let listener: NWListener
        do {
            listener = try NWListener(using: params, on: p)
        } catch {
            throw TransportError.invalidPort(port)
        }
        listener.stateUpdateHandler = { [weak self] state in
            if case .failed(let error) = state {
                FileHandle.standardError.write(Data("transport failed: \(error)\n".utf8))
            }
        }
        listener.newConnectionHandler = { [weak self] conn in
            self?.accept(conn)
        }
        listener.start(queue: queue)
        self.listener = listener
    }

    private func accept(_ conn: NWConnection) {
        lock.lock()
        let replaced = connection
        connection = conn
        lock.unlock()
        if replaced !== conn { replaced?.cancel() }

        conn.stateUpdateHandler = { [weak self] state in
            guard case .ready = state else { return }
            self?.receiveLoop(conn)
        }
        conn.start(queue: queue)
    }

    private func receiveLoop(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                lock.lock()
                stats.packetsReceived += 1
                stats.bytesReceived += data.count
                lock.unlock()
                self.onDatagram?(data)
            }
            if error == nil {
                self.receiveLoop(conn)
            } else {
                lock.lock()
                if self.connection === conn { self.connection = nil }
                lock.unlock()
            }
        }
    }

    func send(_ data: Data) {
        lock.lock()
        let conn = connection
        lock.unlock()
        guard let conn else { return }
        conn.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let self else { return }
            self.lock.lock()
            if error == nil {
                self.stats.packetsSent += 1
                self.stats.bytesSent += data.count
            } else {
                self.stats.sendErrors += 1
            }
            self.lock.unlock()
        })
    }

    func stop() {
        lock.lock()
        let conn = connection
        connection = nil
        lock.unlock()
        conn?.cancel()
        listener?.cancel()
        listener = nil
    }
}

enum TransportError: LocalizedError {
    case invalidPort(UInt16)
    var errorDescription: String? {
        if case .invalidPort(let p) = self { return "Invalid UDP port: \(p)" }
        return "Transport error"
    }
}
