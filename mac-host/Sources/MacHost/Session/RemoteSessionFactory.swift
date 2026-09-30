import Foundation

struct SessionFactoryError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

enum ConnectionMode: String {
    case auto
    case highPerformance = "hp"
    case vncCompatibility = "vnc"
}

/// Mode selection (spec §7/§51). Every decision carries an explicit reason;
/// fallback is never silent.
struct SessionDecision {
    let mode: ConnectionMode
    let reason: String
}

final class RemoteSessionFactory {
    private let inspector: AppleScreenSharingInspector

    init(inspector: AppleScreenSharingInspector = AppleScreenSharingInspector()) {
        self.inspector = inspector
    }

    /// AUTO probe (spec §7 pseudo-code, host side): HP needs Screen Recording +
    /// a hardware encoder (+ UDP, always available on LAN). The decoder side of
    /// the probe completes during capability negotiation.
    func decideAuto() async -> SessionDecision {
        let diag = await inspector.diagnostics()
        if !diag.screenRecordingPermission {
            return SessionDecision(
                mode: .vncCompatibility,
                reason: "Screen Recording permission missing → HP unavailable, using Compatibility (VNC). " +
                        "Grant access, then restart serve to use High Performance.")
        }
        if !diag.hardwareH264Encoder {
            return SessionDecision(
                mode: .vncCompatibility,
                reason: "No hardware H.264 encoder → HP unavailable, using Compatibility (VNC).")
        }
        return SessionDecision(mode: .highPerformance, reason: "All HP capabilities present.")
    }

    /// HP → (one retry) → VNC for auto mode only (spec §7 — no reconnect loop).
    /// An explicit HP request that fails surfaces the error instead of silently
    /// degrading to VNC.
    func runWithFallback(
        mode: ConnectionMode,
        makeHP: () async throws -> HighPerformanceRemoteSession,
        makeVNC: () -> VncProxySession
    ) async throws -> (IRemoteSession, String) {
        if mode != .vncCompatibility {
            var lastError: Error?
            for attempt in 1...2 {
                do {
                    let hp = try await makeHP()
                    try await hp.connect()
                    return (hp, "High Performance connected (attempt \(attempt))")
                } catch {
                    lastError = error
                    print("HP connect attempt \(attempt) failed: \(error.localizedDescription)")
                }
            }
            if mode == .auto {
                do {
                    let vnc = makeVNC()
                    try await vnc.connect()
                    return (vnc, "HP connect failed twice (last: " +
                            "\(lastError?.localizedDescription ?? "unknown")) → " +
                            "explicit fallback to Compatibility (VNC)")
                } catch {
                    throw SessionFactoryError(
                        message: "HP failed (\(lastError?.localizedDescription ?? "?")) and VNC fallback also failed: " +
                                 "\(error.localizedDescription)")
                }
            }
            throw lastError ?? SessionFactoryError(message: "HP connect failed")
        }

        let vnc = makeVNC()
        try await vnc.connect()
        return (vnc, "Compatibility (VNC) mode requested explicitly")
    }
}
