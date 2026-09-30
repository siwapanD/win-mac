import XCTest
@testable import MacHost

final class SessionStateMachineTests: XCTestCase {
    func testHappyPathHP() {
        let sm = SessionStateMachine()
        XCTAssertTrue(sm.transition(to: .authenticating))
        XCTAssertTrue(sm.transition(to: .negotiating))
        XCTAssertTrue(sm.transition(to: .connectingHP))
        XCTAssertTrue(sm.transition(to: .streamingHP))
        XCTAssertEqual(sm.state, .streamingHP)
    }

    func testFallbackPath() {
        let sm = SessionStateMachine()
        XCTAssertTrue(sm.transition(to: .authenticating))
        XCTAssertTrue(sm.transition(to: .negotiating))
        XCTAssertTrue(sm.transition(to: .connectingHP))
        XCTAssertTrue(sm.transition(to: .hpFailed))
        XCTAssertTrue(sm.transition(to: .connectingVNC))
        XCTAssertTrue(sm.transition(to: .streamingVNC))
    }

    func testIllegalTransitionsRejected() {
        let sm = SessionStateMachine()
        XCTAssertFalse(sm.transition(to: .streamingHP)) // cannot skip the pipeline
        XCTAssertEqual(sm.state, .disconnected)
        XCTAssertTrue(sm.transition(to: .connectingHP))
        XCTAssertFalse(sm.transition(to: .streamingVNC)) // HP failure must pass through hpFailed
        XCTAssertTrue(sm.transition(to: .hpFailed))
    }

    func testRecoveryPath() {
        let sm = SessionStateMachine()
        sm.transition(to: .authenticating)
        sm.transition(to: .negotiating)
        sm.transition(to: .connectingHP)
        sm.transition(to: .streamingHP)
        XCTAssertTrue(sm.transition(to: .networkDegraded))
        XCTAssertTrue(sm.transition(to: .recovered))
        XCTAssertTrue(sm.transition(to: .streamingHP))
        XCTAssertTrue(sm.transition(to: .disconnected))
    }
}
