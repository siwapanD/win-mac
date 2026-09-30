import AppKit
import ApplicationServices
import CoreGraphics

/// Injects remote input events via CGEvent (spec §17). Needs Accessibility
/// permission (checked by the inspector / onboarding §27).
///
/// Local-cursor policy (spec §16): the host never renders the remote cursor in
/// the video (showsCursor = false); cursor feel is the client's job.
final class CGEventInjector {
    let displayWidth: Int
    let displayHeight: Int

    var accessibilityGranted: Bool { AXIsProcessTrusted() }

    init() {
        displayWidth = CGDisplayPixelsWide(CGMainDisplayID())
        displayHeight = CGDisplayPixelsHigh(CGMainDisplayID())
    }

    func inject(mouse: MouseInput) {
        let point = CGPoint(x: CGFloat(mouse.x) * CGFloat(displayWidth),
                            y: CGFloat(mouse.y) * CGFloat(displayHeight))
        let rightDown = mouse.buttons & 0x02 != 0
        let eventType: CGEventType
        switch mouse.kind {
        case .move:
            eventType = mouse.buttons == 0
                ? .mouseMoved
                : (rightDown ? .rightMouseDragged : .leftMouseDragged)
        case .down:
            eventType = rightDown ? .rightMouseDown : .leftMouseDown
        case .up:
            eventType = rightDown ? .rightMouseUp : .leftMouseUp
        }
        let button: CGMouseButton = rightDown ? .right : .left
        CGEvent(mouseEventSource: nil, mouseType: eventType,
                mouseCursorPosition: point, mouseButton: button)?
            .post(tap: .cghidEventTap)
    }

    func inject(key: KeyInput) {
        guard let event = CGEvent(keyboardEventSource: nil,
                                  virtualKey: CGKeyCode(key.keyCode),
                                  keyDown: key.down) else { return }
        event.flags = CGEventFlags(rawValue: key.flags)
        event.post(tap: .cghidEventTap)
    }

    func inject(scroll: ScrollInput) {
        // Line-unit deltas; client sends positive dy = scroll down.
        let wheel1 = Int32(max(-100, min(100, -scroll.dy)))
        let wheel2 = Int32(max(-100, min(100, scroll.dx)))
        CGEvent(scrollWheelEvent2Source: nil, units: .line, wheelCount: 2,
                wheel1: wheel1, wheel2: wheel2, wheel3: 0)?.post(tap: .cghidEventTap)
    }
}
