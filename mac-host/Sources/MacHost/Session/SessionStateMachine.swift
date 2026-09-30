import Foundation

/// Session state machine (spec §38). Illegal transitions are rejected and
/// logged, never crash the session.
enum SessionState: String {
    case disconnected
    case discovering
    case authenticating
    case negotiating
    case connectingHP
    case streamingHP
    case hpFailed
    case connectingVNC
    case streamingVNC
    case networkDegraded
    case recovered
}

final class SessionStateMachine: @unchecked Sendable {
    private let lock = NSLock()
    private(set) var state: SessionState = .disconnected
    var onChange: ((SessionState, SessionState) -> Void)?

    static let allowed: [SessionState: Set<SessionState>] = [
        .disconnected:    [.discovering, .authenticating, .negotiating, .connectingHP, .connectingVNC],
        .discovering:     [.authenticating, .disconnected],
        .authenticating:  [.negotiating, .disconnected],
        .negotiating:     [.connectingHP, .connectingVNC, .disconnected],
        .connectingHP:    [.streamingHP, .hpFailed, .disconnected],
        .streamingHP:     [.networkDegraded, .disconnected],
        .hpFailed:        [.connectingVNC, .disconnected],
        .connectingVNC:   [.streamingVNC, .disconnected],
        .streamingVNC:    [.disconnected],
        .networkDegraded: [.recovered, .streamingHP, .disconnected],
        .recovered:       [.streamingHP, .disconnected],
    ]

    @discardableResult
    func transition(to next: SessionState) -> Bool {
        lock.lock()
        let current = state
        guard Self.allowed[current]?.contains(next) == true else {
            lock.unlock()
            FileHandle.standardError.write(
                Data("illegal state transition \(current.rawValue) → \(next.rawValue)\n".utf8))
            return false
        }
        state = next
        lock.unlock()
        onChange?(current, next)
        return true
    }
}
