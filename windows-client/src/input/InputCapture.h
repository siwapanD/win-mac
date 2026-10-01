// Windows client input (spec §16/§17/§18/§49).
//
// Keyboard: a WH_KEYBOARD_LL hook sees every key with its physical scancode
// and can keep system shortcuts (Win, Alt+Tab, Ctrl+Esc) away from Windows
// while the session is fullscreen — Raw Input can observe those keys but not
// stop Windows from acting on them. Keys map by physical position to macOS
// keycodes (MacKeyMap.h).
//
// Mouse: absolute pointer from window messages, normalized against the
// letterboxed video rect. The Windows cursor itself is the local cursor
// (spec §16): it moves at hardware rate with zero network dependency while
// the Mac renders none (showsCursor = false).
//
// Behaviour while windowed (like mstsc "Windows key combinations: full screen
// only"): Win/Alt+Tab/Alt+Esc/Ctrl+Esc/Alt+F4 stay with Windows and modifiers
// are shared with Windows; fullscreen sends everything to the Mac.
// Ctrl+Alt+Enter toggles fullscreen and is never forwarded.
#pragma once

#include "src/protocol/wire.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>

namespace rdp {

enum class KeyboardMode {
    MacFriendly,     // Ctrl→Command, Win→Control, Alt→Option, Alt+Tab→Command+Tab (default, spec §18 ●)
    WindowsNative,   // Ctrl→Control, Alt→Option, Win→Command (positional)
};

constexpr UINT kMsgToggleFullscreen = WM_APP + 1;

class InputCapture {
public:
    using MouseSink = std::function<void(MouseKind, uint8_t buttons, float x, float y)>;
    using KeySink = std::function<void(bool down, uint16_t macKeyCode, uint64_t flags)>;
    using ScrollSink = std::function<void(float dx, float dy)>;
    using VideoRectFn = std::function<RECT()>;   // video dest rect, client coordinates

    ~InputCapture();

    bool install(HWND hwnd, KeyboardMode mode, bool verbose);
    void uninstall();
    void setSinks(MouseSink m, KeySink k, ScrollSink s, VideoRectFn rect);

    /// Forward input only while the HP stream is up.
    void setEnabled(bool on) { enabled_ = on; }
    void setFullscreen(bool on) { fullscreen_ = on; }

    /// Window procedure hook for WM_MOUSE*/WM_*BUTTON*/WM_MOUSE(H)WHEEL/
    /// WM_CAPTURECHANGED. Returns true when the message was consumed.
    bool onMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /// Key/button-state reset — no stuck keys on the Mac (spec §49). Called on
    /// focus loss and before disconnect.
    void resetKeyState();

private:
    static LRESULT CALLBACK lowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    bool handleKey(const KBDLLHOOKSTRUCT& k, bool up);
    uint16_t macForModifier(uint16_t phys) const;
    uint64_t heldModifierFlags() const;
    void forwardKey(bool down, uint16_t macKey);
    bool held(uint16_t phys) const { return physDown_[phys]; }
    void mouseAt(MouseKind kind, uint8_t buttons, LPARAM lParam);
    void releaseButtons();

    HWND hwnd_ = nullptr;
    HHOOK hook_ = nullptr;
    KeyboardMode mode_ = KeyboardMode::MacFriendly;
    bool verbose_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> fullscreen_{false};

    MouseSink mouseSink_;
    KeySink keySink_;
    ScrollSink scrollSink_;
    VideoRectFn videoRect_;

    // Per physical key (scancode | 0x100 for E0-extended). Hook runs on the UI thread only.
    uint16_t sentMac_[512];     // macOS keycode we sent a down for (kNone if not forwarded)
    bool swallowed_[512] = {};  // we hid this key's down from Windows → hide its up too
    bool physDown_[512] = {};
    bool altAsCommand_ = false; // Mac-friendly Alt+Tab: held Alt is acting as Command

    uint8_t buttons_ = 0;
    float lastX_ = 0.5f, lastY_ = 0.5f;
    float wheelAccumX_ = 0.f, wheelAccumY_ = 0.f;
    UINT wheelLines_ = 3, wheelChars_ = 3;
};

} // namespace rdp
