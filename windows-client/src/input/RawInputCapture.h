// Windows client: Raw Input keyboard/mouse capture (spec §17) + local cursor
// policy (spec §16 — the cursor must update locally IMMEDIATELY on mouse move,
// never waiting for a video frame from the Mac).
//
// STATUS: skeleton — compile-verified only on Windows. See README.md.
#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>

namespace rdp {

struct MousePacket {
    uint8_t kind;      // 0 move, 1 down, 2 up (MouseKind in wire.h)
    uint8_t buttons;   // bit0 left, bit1 right, bit2 middle
    float x, y;        // normalized 0..1 against host display
};

struct KeyPacket {
    bool down;
    uint16_t macKeyCode;  // mapped via KeyMap below BEFORE sending (spec §18)
    uint64_t flags;       // CGEventFlags raw bits
};

class RawInputCapture {
public:
    using SendMouse = std::function<void(const MousePacket&)>;
    using SendKey = std::function<void(const KeyPacket&)>;
    using SendScroll = std::function<void(float dx, float dy)>;

    /// RegisterRawInputDevices(RIDEV_INPUTSINK) on the video window.
    bool initialize(HWND hwnd);
    void shutdown();

    /// WM_INPUT handler. Must not do work inline — enqueue and send from the
    /// input thread (spec §43 Windows threading).
    void onWMInput(LPARAM lParam, WPARAM wParam);

    // Mouse-move fast path: update the local cursor overlay HERE, before the
    // packet is queued for the network (spec §16 flow).
    void setLocalCursor(HWND cursorWindow);

    /// Key-state reset on disconnect — no stuck keys (spec §49).
    void resetKeyState();

    /// Windows VK → macOS virtual keycode table (Mac-friendly mapping §18):
    /// Ctrl+C → Command+C, Alt+Tab → Command+Tab, Win key → Command, etc.
    /// Physical scancodes preserved for accurate shortcuts.
    static uint16_t mapVkToMacKeyCode(WPARAM vk, bool* outIsCommand, bool* outIsOption);

private:
    HWND hwnd_ = nullptr;
};

} // namespace rdp
