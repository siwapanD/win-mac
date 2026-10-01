// Input capture — see header for the policy.
//
// Stuck-key safety (spec §49): every key whose down was forwarded remembers
// the exact macOS keycode sent (sentMac_), so its up always releases the same
// Mac key even if the mapping changed mid-press (Alt+Tab → Command+Tab) or
// focus moved. A key-up is only hidden from Windows if its down was hidden,
// so Windows' own key state can never be left stuck either.

#include "InputCapture.h"
#include "MacKeyMap.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rdp {

namespace {

InputCapture* g_capture = nullptr;   // the LL hook has no user data pointer

enum class Role { None, Shift, Ctrl, Alt, Win };

Role roleOf(uint16_t phys) {
    switch (phys) {
    case 0x02A: case 0x036: return Role::Shift;
    case 0x01D: case 0x11D: return Role::Ctrl;
    case 0x038: case 0x138: return Role::Alt;
    case 0x15B: case 0x15C: return Role::Win;
    default: return Role::None;
    }
}

bool isRightSide(uint16_t phys) {
    return phys == 0x036 || phys == 0x11D || phys == 0x138 || phys == 0x15C;
}

constexpr uint16_t kModifierKeys[] = {0x02A, 0x036, 0x01D, 0x11D, 0x038, 0x138, 0x15B, 0x15C};

constexpr uint16_t kPhysEnter = 0x01C, kPhysKeypadEnter = 0x11C, kPhysTab = 0x00F,
                   kPhysEsc = 0x001, kPhysF4 = 0x03E;

} // namespace

InputCapture::~InputCapture() { uninstall(); }

bool InputCapture::install(HWND hwnd, KeyboardMode mode, bool verbose) {
    hwnd_ = hwnd;
    mode_ = mode;
    verbose_ = verbose;
    std::fill(std::begin(sentMac_), std::end(sentMac_), mac::kNone);
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines_, 0);
    SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &wheelChars_, 0);
    if (wheelLines_ == WHEEL_PAGESCROLL || wheelLines_ == 0) wheelLines_ = 3;
    if (wheelChars_ == 0) wheelChars_ = 3;

    g_capture = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    return hook_ != nullptr;
}

void InputCapture::uninstall() {
    if (hook_) { UnhookWindowsHookEx(hook_); hook_ = nullptr; }
    if (g_capture == this) g_capture = nullptr;
}

void InputCapture::setSinks(MouseSink m, KeySink k, ScrollSink s, VideoRectFn rect) {
    mouseSink_ = std::move(m);
    keySink_ = std::move(k);
    scrollSink_ = std::move(s);
    videoRect_ = std::move(rect);
}

// MARK: - keyboard

LRESULT CALLBACK InputCapture::lowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_capture) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        // Injected keys are handled like physical ones: on-screen keyboards and
        // remote tools controlling this PC (Parsec, etc.) deliver input that way.
        if (g_capture->handleKey(*k, up)) return 1;   // swallowed
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

uint16_t InputCapture::macForModifier(uint16_t phys) const {
    const bool right = isRightSide(phys);
    const bool mac = mode_ == KeyboardMode::MacFriendly;
    switch (roleOf(phys)) {
    case Role::Shift: return right ? mac::kRightShift : mac::kShift;
    case Role::Ctrl:
        return mac ? (right ? mac::kRightCommand : mac::kCommand)
                   : (right ? mac::kRightControl : mac::kControl);
    case Role::Win:
        return mac ? (right ? mac::kRightControl : mac::kControl)
                   : (right ? mac::kRightCommand : mac::kCommand);
    case Role::Alt:
        if (altAsCommand_) return right ? mac::kRightCommand : mac::kCommand;
        return right ? mac::kRightOption : mac::kOption;
    default: return mac::kNone;
    }
}

uint64_t InputCapture::heldModifierFlags() const {
    uint64_t flags = 0;
    for (uint16_t p : kModifierKeys)
        if (sentMac_[p] != mac::kNone) flags |= mac::modifierFlag(sentMac_[p]);
    return flags;
}

void InputCapture::forwardKey(bool down, uint16_t macKey) {
    const uint64_t flags = heldModifierFlags() | mac::intrinsicFlags(macKey);
    if (verbose_) {
        printf("input: key %-4s mac 0x%02X %-12s flags=0x%06llx\n", down ? "down" : "up",
               macKey, mac::keyName(macKey), static_cast<unsigned long long>(flags));
    }
    if (keySink_) keySink_(down, macKey, flags);
}

bool InputCapture::handleKey(const KBDLLHOOKSTRUCT& k, bool up) {
    // AltGr's synthetic LCtrl arrives with scancode 0x21D — not a real key.
    if (k.scanCode & 0x200) return false;
    uint32_t sc = k.scanCode;
    bool ext = (k.flags & LLKHF_EXTENDED) != 0;
    if (sc == 0) {   // some virtual keyboards / remappers send VK only
        const UINT v = MapVirtualKeyW(k.vkCode, MAPVK_VK_TO_VSC_EX);
        sc = v & 0xFF;
        ext = (v & 0xFF00) == 0xE000;
    }
    if (sc == 0 || sc > 0xFF) return false;
    const uint16_t phys = uint16_t(sc | (ext ? 0x100 : 0));
    const Role role = roleOf(phys);

    if (up) {
        physDown_[phys] = false;
        const bool swallow = swallowed_[phys];
        swallowed_[phys] = false;
        if (sentMac_[phys] != mac::kNone) {
            const uint16_t m = sentMac_[phys];
            sentMac_[phys] = mac::kNone;   // cleared first: the up event carries no flag for itself
            forwardKey(false, m);
        }
        if (role == Role::Alt && !held(0x038) && !held(0x138)) altAsCommand_ = false;
        return swallow;
    }

    const bool repeat = physDown_[phys];
    physDown_[phys] = true;
    if (repeat) {
        // Autorepeat: synthetic CGEvents get no repeat from macOS, so forward
        // repeats of ordinary keys; modifiers never repeat on a Mac.
        if (sentMac_[phys] != mac::kNone && role == Role::None) forwardKey(true, sentMac_[phys]);
        return swallowed_[phys];
    }
    if (!enabled_ || GetForegroundWindow() != hwnd_) return false;

    const bool ctrl = held(0x01D) || held(0x11D);
    const bool alt = held(0x038) || held(0x138);
    const bool win = held(0x15B) || held(0x15C);

    if ((phys == kPhysEnter || phys == kPhysKeypadEnter) && ctrl && alt) {
        PostMessageW(hwnd_, kMsgToggleFullscreen, 0, 0);   // client hotkey, never forwarded
        swallowed_[phys] = true;
        return true;
    }

    const bool fullscreen = fullscreen_.load();
    if (!fullscreen && role == Role::None) {
        const bool windowsCombo = (phys == kPhysTab && alt) ||
                                  (phys == kPhysEsc && (alt || ctrl)) ||
                                  (phys == kPhysF4 && alt) || win;
        if (windowsCombo) return false;   // Windows handles it, the Mac never sees it
    }

    // Mac-friendly Alt+Tab → Command+Tab: re-press held Alts as Command
    // (only reachable fullscreen; windowed Alt+Tab belongs to Windows).
    if (mode_ == KeyboardMode::MacFriendly && phys == kPhysTab && alt && !ctrl && !altAsCommand_) {
        altAsCommand_ = true;
        for (uint16_t p : {uint16_t(0x038), uint16_t(0x138)}) {
            if (sentMac_[p] == mac::kNone) continue;
            const uint16_t old = sentMac_[p];
            sentMac_[p] = mac::kNone;
            forwardKey(false, old);
            sentMac_[p] = macForModifier(p);
            forwardKey(true, sentMac_[p]);
        }
    }

    const uint16_t m = role != Role::None ? macForModifier(phys) : mac::keyCodeForPhysical(phys, k.vkCode);
    if (m == mac::kNone) return false;   // no Mac equivalent (media keys etc.): leave to Windows

    sentMac_[phys] = m;
    forwardKey(true, m);
    // Windowed: modifiers stay shared with Windows so its shortcuts keep working.
    const bool swallow = fullscreen || role == Role::None;
    swallowed_[phys] = swallow;
    return swallow;
}

void InputCapture::resetKeyState() {
    // Ordinary keys first, modifiers last, so no shortcut fires on the way out.
    for (int pass = 0; pass < 2; ++pass) {
        for (uint16_t p = 0; p < 512; ++p) {
            if (sentMac_[p] == mac::kNone || (roleOf(p) != Role::None) != (pass == 1)) continue;
            const uint16_t m = sentMac_[p];
            sentMac_[p] = mac::kNone;
            forwardKey(false, m);
        }
    }
    altAsCommand_ = false;
    releaseButtons();
}

// MARK: - mouse

void InputCapture::mouseAt(MouseKind kind, uint8_t buttons, LPARAM lParam) {
    const int px = GET_X_LPARAM(lParam), py = GET_Y_LPARAM(lParam);
    RECT vr{};
    if (videoRect_) vr = videoRect_();
    const int w = vr.right - vr.left, h = vr.bottom - vr.top;
    if (w > 0 && h > 0) {
        lastX_ = std::clamp((float(px - vr.left) + 0.5f) / float(w), 0.f, 1.f);
        lastY_ = std::clamp((float(py - vr.top) + 0.5f) / float(h), 0.f, 1.f);
    }
    if (mouseSink_) mouseSink_(kind, buttons, lastX_, lastY_);
}

void InputCapture::releaseButtons() {
    for (uint8_t b : {kMouseLeft, kMouseRight}) {
        if (!(buttons_ & b)) continue;
        buttons_ &= uint8_t(~b);
        if (mouseSink_) mouseSink_(MouseKind::Up, b, lastX_, lastY_);
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
}

bool InputCapture::onMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CAPTURECHANGED:
        if (HWND(lParam) != hwnd_ && buttons_) releaseButtons();   // e.g. focus stolen mid-drag
        return false;
    case WM_MOUSEMOVE:
        if (!enabled_) return false;
        mouseAt(MouseKind::Move, buttons_, lParam);
        return true;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        if (!enabled_) return false;
        const uint8_t b = msg == WM_LBUTTONDOWN ? kMouseLeft : kMouseRight;
        if (!buttons_) SetCapture(hwnd_);   // keep the drag even outside the window
        buttons_ |= b;
        // The host picks left/right from this byte, so it names only the
        // button that changed.
        mouseAt(MouseKind::Down, b, lParam);
        return true;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        const uint8_t b = msg == WM_LBUTTONUP ? kMouseLeft : kMouseRight;
        if (!(buttons_ & b)) return false;
        buttons_ &= uint8_t(~b);
        mouseAt(MouseKind::Up, b, lParam);
        if (!buttons_ && GetCapture() == hwnd_) ReleaseCapture();
        return true;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if (!enabled_) return false;
        const float notches = float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
        // Host truncates to whole lines: accumulate high-resolution wheel /
        // precision-touchpad deltas and send integer lines only.
        float dx = 0.f, dy = 0.f;
        if (msg == WM_MOUSEWHEEL) {
            wheelAccumY_ += -notches * float(wheelLines_);   // wheel forward = scroll up = negative dy
            dy = std::trunc(wheelAccumY_);
            wheelAccumY_ -= dy;
        } else {
            wheelAccumX_ += -notches * float(wheelChars_);   // tilt right → negative dx (dx>0 = left)
            dx = std::trunc(wheelAccumX_);
            wheelAccumX_ -= dx;
        }
        if ((dx != 0.f || dy != 0.f) && scrollSink_) scrollSink_(dx, dy);
        return true;
    }
    default:
        return false;
    }
}

} // namespace rdp
