import Foundation

/// Thread-safe cumulative counters shared across capture/encode/network queues.
final class PerfCounters {
    private let lock = NSLock()
    private var values: [String: Double] = [:]

    func add(_ key: String, _ amount: Double = 1) {
        lock.lock(); defer { lock.unlock() }
        values[key, default: 0] += amount
    }

    func get(_ key: String) -> Double {
        lock.lock(); defer { lock.unlock() }
        return values[key] ?? 0
    }

    func snapshot() -> [String: Double] {
        lock.lock(); defer { lock.unlock() }
        return values
    }
}

/// Sliding-window rate meter (FPS and Mbps), ticked once per second.
final class RateMeter {
    private let lock = NSLock()
    private var windowStart = Date()
    private var count = 0
    private var bytes = 0.0
    private(set) var lastRate: Double = 0
    private(set) var lastMbps: Double = 0

    func mark(bytes added: Int = 0) {
        lock.lock(); defer { lock.unlock() }
        count += 1
        bytes += Double(added)
    }

    @discardableResult
    func tick() -> Double {
        lock.lock(); defer { lock.unlock() }
        let dt = max(0.001, Date().timeIntervalSince(windowStart))
        lastRate = Double(count) / dt
        lastMbps = bytes * 8 / dt / 1_000_000
        windowStart = Date()
        count = 0
        bytes = 0
        return lastRate
    }
}

/// min/avg/max accumulator (milliseconds).
final class LatencyAccumulator {
    private let lock = NSLock()
    private var sum = 0.0
    private var minV = Double.greatestFiniteMagnitude
    private var maxV = 0.0
    private var n = 0.0

    func add(_ ms: Double) {
        lock.lock(); defer { lock.unlock() }
        sum += ms
        n += 1
        minV = Swift.min(minV, ms)
        maxV = Swift.max(maxV, ms)
    }

    func snapshot() -> (avg: Double, min: Double, max: Double, count: Int) {
        lock.lock(); defer { lock.unlock() }
        guard n > 0 else { return (0, 0, 0, 0) }
        return (sum / n, minV, maxV, Int(n))
    }
}
