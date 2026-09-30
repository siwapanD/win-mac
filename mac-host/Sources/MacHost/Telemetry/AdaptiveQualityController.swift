import Foundation

/// Adaptive quality controller (spec §21/§36). Inputs are pushed once per
/// stats tick (≈1 s; the spec's 250–500 ms cadence arrives with the QUIC
/// transport's pacing loop). Priority: latency > FPS > readable text >
/// resolution (spec §36) — degrade fast, recover slow with hysteresis
/// (spec §21: never ramp bitrate back quickly).
struct QualityDecision: Equatable {
    var bitrate: Int
    var fps: Int
    var note: String
}

final class AdaptiveQualityController {
    private let minBitrate: Int
    private let maxBitrate: Int
    private var currentBitrate: Int
    private var currentFPS: Int
    private var stableTicks = 0

    /// Triggers (spec §21/§42 budgets): in-flight 2 = encoder queue full,
    /// encode > 15 ms breaks the 2–6 ms budget at 60 fps, RTT/loss thresholds
    /// conservative until real client loss reports land (Phase 9 full).
    private let hardLossPct = 0.05
    private let softLossPct = 0.02
    private let hardRttMs = 100.0
    private let softRttMs = 50.0
    private let encodeBudgetMs = 15.0
    private let stableTicksToRecover = 10 // ≈10 s at 1 s ticks

    init(bitrate: Int, fps: Int, minBitrate: Int = 2_000_000, maxBitrate: Int? = nil) {
        self.currentBitrate = bitrate
        self.currentFPS = fps
        self.minBitrate = minBitrate
        self.maxBitrate = maxBitrate ?? bitrate
    }

    var bitrate: Int { currentBitrate }
    var fps: Int { currentFPS }

    @discardableResult
    func tick(rttMs: Double, lossPct: Double,
              encoderInFlight: Int, encodeMs: Double) -> QualityDecision {
        // 1. Hard degradation — latency first (spec §65.1).
        if lossPct >= hardLossPct || rttMs >= hardRttMs || encoderInFlight >= 2 {
            return drop(factor: 0.7, note: "hard (loss=\(lossPct) rtt=\(Int(rttMs))ms queue=\(encoderInFlight))")
        }
        // 2. Soft degradation.
        if lossPct >= softLossPct || rttMs >= softRttMs || encodeMs >= encodeBudgetMs {
            return drop(factor: 0.85, note: "soft (loss=\(lossPct) rtt=\(Int(rttMs))ms enc=\(Int(encodeMs))ms)")
        }
        // 3. Stable — recover slowly with hysteresis.
        stableTicks += 1
        if stableTicks >= stableTicksToRecover {
            stableTicks = 0
            let next = min(maxBitrate, Int(Double(currentBitrate) * 1.05))
            if next > currentBitrate {
                currentBitrate = next
                return QualityDecision(bitrate: next, fps: currentFPS,
                                       note: "recover +5% → \(next / 1_000_000) Mbps")
            }
        }
        return QualityDecision(bitrate: currentBitrate, fps: currentFPS, note: "stable")
    }

    private func drop(factor: Double, note: String) -> QualityDecision {
        stableTicks = 0
        let next = max(minBitrate, Int(Double(currentBitrate) * factor))
        if next == currentBitrate {
            return QualityDecision(bitrate: currentBitrate, fps: currentFPS, note: "floor " + note)
        }
        currentBitrate = next
        return QualityDecision(bitrate: next, fps: currentFPS,
                               note: "drop → \(next / 1_000_000) Mbps " + note)
    }
}
