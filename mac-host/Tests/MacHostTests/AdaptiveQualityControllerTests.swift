import XCTest
@testable import MacHost

final class AdaptiveQualityControllerTests: XCTestCase {
    func testStableHoldsBitrate() {
        let c = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        for _ in 0..<9 {
            let d = c.tick(rttMs: 5, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
            XCTAssertEqual(d.bitrate, 12_000_000)
        }
    }

    func testHardConditionsDropFast() {
        let c = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        let d = c.tick(rttMs: 120, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
        XCTAssertEqual(d.bitrate, 8_400_000) // 0.7×, degrade fast (spec §21)

        let c2 = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        let d2 = c2.tick(rttMs: 5, lossPct: 0.06, encoderInFlight: 1, encodeMs: 4)
        XCTAssertEqual(d2.bitrate, 8_400_000)

        let c3 = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        let d3 = c3.tick(rttMs: 5, lossPct: 0, encoderInFlight: 2, encodeMs: 4)
        XCTAssertEqual(d3.bitrate, 8_400_000) // encoder queue full (spec §10)
    }

    func testSoftConditionsDropGently() {
        let c = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        let d = c.tick(rttMs: 60, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
        XCTAssertEqual(d.bitrate, 10_200_000) // 0.85×

        let c2 = AdaptiveQualityController(bitrate: 12_000_000, fps: 60)
        let d2 = c2.tick(rttMs: 5, lossPct: 0, encoderInFlight: 1, encodeMs: 20)
        XCTAssertEqual(d2.bitrate, 10_200_000) // encode over budget (§42)
    }

    func testRecoveryIsSlowWithHysteresis() {
        let c = AdaptiveQualityController(bitrate: 10_000_000, fps: 60, maxBitrate: 12_000_000)
        // 9 stable ticks must not move the bitrate…
        for _ in 0..<9 {
            c.tick(rttMs: 5, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
        }
        XCTAssertEqual(c.bitrate, 10_000_000)
        // …the 10th recovers +5 %…
        let d = c.tick(rttMs: 5, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
        XCTAssertEqual(d.bitrate, 10_500_000)
        // …and a single bad tick resets the recovery window.
        c.tick(rttMs: 120, lossPct: 0, encoderInFlight: 1, encodeMs: 4)
        XCTAssertEqual(c.bitrate, 10_500_000 * 7 / 10, accuracy: 100_000)
    }

    func testNeverExceedsMaxOrDropsBelowMin() {
        let c = AdaptiveQualityController(bitrate: 11_900_000, fps: 60,
                                          minBitrate: 2_000_000, maxBitrate: 12_000_000)
        for _ in 0..<40 { c.tick(rttMs: 1, lossPct: 0, encoderInFlight: 1, encodeMs: 2) }
        XCTAssertLessThanOrEqual(c.bitrate, 12_000_000)

        let c2 = AdaptiveQualityController(bitrate: 2_200_000, fps: 60, minBitrate: 2_000_000)
        for _ in 0..<20 { c2.tick(rttMs: 200, lossPct: 0.10, encoderInFlight: 2, encodeMs: 30) }
        XCTAssertGreaterThanOrEqual(c2.bitrate, 2_000_000)
    }
}
