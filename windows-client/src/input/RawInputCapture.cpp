// Implementation notes (skeleton — see header).
//
// initialize():
//   RAWINPUTDEVICE rid{ usUsagePage: 0x01, usUsage: 0x02 (mouse) / 0x06 (keyboard),
//                       dwFlags: RIDEV_INPUTSINK, hwndTarget: hwnd_ };
//   RegisterRawInputDevices — capture even when the window lacks focus.
//
// onWMInput():
//   GetRawInputData(RID_INPUT) → RAWMOUSE / RAWKEYBOARD.
//   Mouse: build MousePacket with normalized coords (cursor pos over the video
//     viewport ÷ viewport size), update local cursor window FIRST (spec §16),
//     then queue the packet. Do not interpolate or batch — 1 event = 1 packet;
//     coalescing (if needed later) must keep latest-state semantics.
//   Keyboard: mapVkToMacKeyCode → KeyPacket → send on the reliable stream
//     (UDP reliable-flagged 3× today; QUIC stream in production).
//
// resetKeyState(): send KeyUp for every key currently marked down (tracked in
// a 256-bit set) — called on disconnect and on focus loss (spec §49).
//
// mapVkToMacKeyCode(): fill the table from Apple's "Mac virtual key codes"
// (kVK_ANSI_*): e.g. 'C'=0x08, 'V'=0x09, Tab=0x30, Space=0x31, arrows
// 0x7B–0x7C, Command flags = CGEventFlagCommand (1<<20) etc. Windows Ctrl →
// Command for Mac-friendly mode (config §18: Windows native / Mac-friendly /
// Custom).

#include "RawInputCapture.h"

namespace rdp {

bool RawInputCapture::initialize(HWND hwnd) {
    // TODO(windows): RegisterRawInputDevices per notes.
    hwnd_ = hwnd;
    return false;
}

void RawInputCapture::shutdown() {}

void RawInputCapture::onWMInput(LPARAM lParam, WPARAM wParam) {
    // TODO(windows).
    (void)lParam; (void)wParam;
}

void RawInputCapture::setLocalCursor(HWND cursorWindow) {
    // TODO(windows): layered child window + software cursor bitmap from
    // CursorState packets (cursor_shape_id, hotspot, visibility — spec §16).
    (void)cursorWindow;
}

void RawInputCapture::resetKeyState() {}

uint16_t RawInputCapture::mapVkToMacKeyCode(WPARAM vk, bool* outIsCommand, bool* outIsOption) {
    // TODO(windows): full table (docs/protocol.md §6).
    (void)vk;
    if (outIsCommand) *outIsCommand = false;
    if (outIsOption) *outIsOption = false;
    return 0;
}

} // namespace rdp
