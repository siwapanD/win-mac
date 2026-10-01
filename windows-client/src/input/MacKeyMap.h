// Physical Windows key → macOS virtual keycode (spec §18, docs/protocol.md §6).
//
// Mapping is by PHYSICAL position (scancode set 1, +0x100 when E0-extended),
// not by Windows VK: macOS keycodes (kVK_*) are positional too, so the Mac's
// own input source decides which character a key produces. A Thai/English
// layout switch therefore happens on the Mac, exactly as on a local keyboard.
//
// Ctrl / Alt / Win are NOT in this table — their Mac role depends on the
// keyboard-mapping mode and is resolved by InputCapture.
#pragma once

#include <cstdint>

namespace rdp::mac {

constexpr uint16_t kNone = 0xFFFF;

// Modifier keycodes (HIToolbox Events.h).
constexpr uint16_t kCommand      = 0x37;
constexpr uint16_t kRightCommand = 0x36;
constexpr uint16_t kShift        = 0x38;
constexpr uint16_t kRightShift   = 0x3C;
constexpr uint16_t kOption       = 0x3A;
constexpr uint16_t kRightOption  = 0x3D;
constexpr uint16_t kControl      = 0x3B;
constexpr uint16_t kRightControl = 0x3E;
constexpr uint16_t kCapsLock     = 0x39;
constexpr uint16_t kTab          = 0x30;

// CGEventFlags bits.
constexpr uint64_t kFlagAlphaShift  = 1ull << 16;
constexpr uint64_t kFlagShift       = 1ull << 17;
constexpr uint64_t kFlagControl     = 1ull << 18;
constexpr uint64_t kFlagOption      = 1ull << 19;
constexpr uint64_t kFlagCommand     = 1ull << 20;
constexpr uint64_t kFlagNumericPad  = 1ull << 21;
constexpr uint64_t kFlagSecondaryFn = 1ull << 23;

/// CGEventFlags bit carried while a modifier keycode is held.
inline uint64_t modifierFlag(uint16_t macKey) {
    switch (macKey) {
    case kCommand: case kRightCommand: return kFlagCommand;
    case kShift:   case kRightShift:   return kFlagShift;
    case kOption:  case kRightOption:  return kFlagOption;
    case kControl: case kRightControl: return kFlagControl;
    default: return 0;
    }
}

/// Extra flags real Apple hardware sets on non-character keys (arrows carry
/// NumericPad|Fn, F-keys and the navigation cluster carry Fn, keypad keys
/// carry NumericPad). Some apps check them, e.g. Shift+Arrow selection.
inline uint64_t intrinsicFlags(uint16_t macKey) {
    switch (macKey) {
    case 0x7B: case 0x7C: case 0x7D: case 0x7E:              // arrows
        return kFlagNumericPad | kFlagSecondaryFn;
    case 0x7A: case 0x78: case 0x63: case 0x76: case 0x60: case 0x61:
    case 0x62: case 0x64: case 0x65: case 0x6D: case 0x67: case 0x6F:  // F1–F12
    case 0x69: case 0x6B: case 0x71: case 0x6A: case 0x40: case 0x4F:
    case 0x50: case 0x5A:                                    // F13–F20
    case 0x72: case 0x73: case 0x74: case 0x75: case 0x77: case 0x79:  // Help/Home/PgUp/FwdDel/End/PgDn
        return kFlagSecondaryFn;
    case 0x41: case 0x43: case 0x45: case 0x47: case 0x4B: case 0x4C:
    case 0x4E: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
    case 0x56: case 0x57: case 0x58: case 0x59: case 0x5B: case 0x5C:  // keypad
        return kFlagNumericPad;
    default: return 0;
    }
}

/// Physical key id → macOS keycode. `vk` disambiguates the two scancode-0x45
/// keys (Pause vs NumLock). Returns kNone for keys with no Mac equivalent.
inline uint16_t keyCodeForPhysical(uint16_t phys, uint32_t vk) {
    if (vk == 0x13 /*VK_PAUSE*/)   return 0x71;   // F15 (Mac convention for Pause)
    if (vk == 0x90 /*VK_NUMLOCK*/) return 0x47;   // keypad Clear sits where NumLock is
    switch (phys) {
    case 0x001: return 0x35;  // Esc
    case 0x002: return 0x12;  // 1
    case 0x003: return 0x13;  // 2
    case 0x004: return 0x14;  // 3
    case 0x005: return 0x15;  // 4
    case 0x006: return 0x17;  // 5
    case 0x007: return 0x16;  // 6
    case 0x008: return 0x1A;  // 7
    case 0x009: return 0x1C;  // 8
    case 0x00A: return 0x19;  // 9
    case 0x00B: return 0x1D;  // 0
    case 0x00C: return 0x1B;  // -
    case 0x00D: return 0x18;  // =
    case 0x00E: return 0x33;  // Backspace → Delete
    case 0x00F: return 0x30;  // Tab
    case 0x010: return 0x0C;  // Q
    case 0x011: return 0x0D;  // W
    case 0x012: return 0x0E;  // E
    case 0x013: return 0x0F;  // R
    case 0x014: return 0x11;  // T
    case 0x015: return 0x10;  // Y
    case 0x016: return 0x20;  // U
    case 0x017: return 0x22;  // I
    case 0x018: return 0x1F;  // O
    case 0x019: return 0x23;  // P
    case 0x01A: return 0x21;  // [
    case 0x01B: return 0x1E;  // ]
    case 0x01C: return 0x24;  // Enter → Return
    case 0x01E: return 0x00;  // A
    case 0x01F: return 0x01;  // S
    case 0x020: return 0x02;  // D
    case 0x021: return 0x03;  // F
    case 0x022: return 0x05;  // G
    case 0x023: return 0x04;  // H
    case 0x024: return 0x26;  // J
    case 0x025: return 0x28;  // K
    case 0x026: return 0x25;  // L
    case 0x027: return 0x29;  // ;
    case 0x028: return 0x27;  // '
    case 0x029: return 0x32;  // `  (Thai layout: ๅ / language toggle on Windows)
    case 0x02A: return kShift;
    case 0x02B: return 0x2A;  // backslash
    case 0x02C: return 0x06;  // Z
    case 0x02D: return 0x07;  // X
    case 0x02E: return 0x08;  // C
    case 0x02F: return 0x09;  // V
    case 0x030: return 0x0B;  // B
    case 0x031: return 0x2D;  // N
    case 0x032: return 0x2E;  // M
    case 0x033: return 0x2B;  // ,
    case 0x034: return 0x2F;  // .
    case 0x035: return 0x2C;  // /
    case 0x036: return kRightShift;
    case 0x037: return 0x43;  // keypad *
    case 0x039: return 0x31;  // Space
    case 0x03A: return kCapsLock;
    case 0x03B: return 0x7A;  // F1
    case 0x03C: return 0x78;  // F2
    case 0x03D: return 0x63;  // F3
    case 0x03E: return 0x76;  // F4
    case 0x03F: return 0x60;  // F5
    case 0x040: return 0x61;  // F6
    case 0x041: return 0x62;  // F7
    case 0x042: return 0x64;  // F8
    case 0x043: return 0x65;  // F9
    case 0x044: return 0x6D;  // F10
    case 0x046: return 0x6B;  // Scroll Lock → F14
    case 0x047: return 0x59;  // keypad 7
    case 0x048: return 0x5B;  // keypad 8
    case 0x049: return 0x5C;  // keypad 9
    case 0x04A: return 0x4E;  // keypad -
    case 0x04B: return 0x56;  // keypad 4
    case 0x04C: return 0x57;  // keypad 5
    case 0x04D: return 0x58;  // keypad 6
    case 0x04E: return 0x45;  // keypad +
    case 0x04F: return 0x53;  // keypad 1
    case 0x050: return 0x54;  // keypad 2
    case 0x051: return 0x55;  // keypad 3
    case 0x052: return 0x52;  // keypad 0
    case 0x053: return 0x41;  // keypad .
    case 0x056: return 0x0A;  // ISO extra key (left of Z) → ISO Section
    case 0x057: return 0x67;  // F11
    case 0x058: return 0x6F;  // F12
    case 0x059: return 0x51;  // keypad =
    case 0x064: return 0x69;  // F13
    case 0x065: return 0x6B;  // F14
    case 0x066: return 0x71;  // F15
    case 0x067: return 0x6A;  // F16
    case 0x068: return 0x40;  // F17
    case 0x069: return 0x4F;  // F18
    case 0x06A: return 0x50;  // F19
    case 0x06B: return 0x5A;  // F20
    case 0x070: return 0x68;  // JIS Katakana/Hiragana → Kana
    case 0x073: return 0x5E;  // JIS Ro → Underscore
    case 0x079: return 0x68;  // JIS Henkan → Kana
    case 0x07B: return 0x66;  // JIS Muhenkan → Eisu
    case 0x07D: return 0x5D;  // JIS Yen
    case 0x07E: return 0x5F;  // keypad , (Brazil/JIS)
    // E0-extended
    case 0x11C: return 0x4C;  // keypad Enter
    case 0x120: return 0x4A;  // Mute
    case 0x12E: return 0x49;  // Volume Down
    case 0x130: return 0x48;  // Volume Up
    case 0x135: return 0x4B;  // keypad /
    case 0x137: return 0x69;  // Print Screen → F13
    case 0x147: return 0x73;  // Home
    case 0x148: return 0x7E;  // Up
    case 0x149: return 0x74;  // Page Up
    case 0x14B: return 0x7B;  // Left
    case 0x14D: return 0x7C;  // Right
    case 0x14F: return 0x77;  // End
    case 0x150: return 0x7D;  // Down
    case 0x151: return 0x79;  // Page Down
    case 0x152: return 0x72;  // Insert → Help
    case 0x153: return 0x75;  // Delete → Forward Delete
    case 0x15D: return 0x6E;  // Menu/Apps → Contextual Menu
    default: return kNone;
    }
}

/// Short human-readable name for logs (fake-host, diagnostics).
inline const char* keyName(uint16_t k) {
    static const char* const kNames[0x80] = {
        "A","S","D","F","H","G","Z","X","C","V","Section","B","Q","W","E","R",
        "Y","T","1","2","3","4","6","5","=","9","7","-","8","0","]","O",
        "U","[","I","P","Return","L","J","'","K",";","\\",",","/","N","M",".",
        "Tab","Space","`","Delete",nullptr,"Esc","RightCmd","Cmd","Shift","CapsLock","Option","Control","RightShift","RightOption","RightControl","Fn",
        "F17","KP.",nullptr,"KP*",nullptr,"KP+",nullptr,"KPClear","VolUp","VolDown","Mute","KP/","KPEnter",nullptr,"KP-","F18",
        "F19","KP=","KP0","KP1","KP2","KP3","KP4","KP5","KP6","KP7","F20","KP8","KP9","JISYen","JISUnderscore","JISKP,",
        "F5","F6","F7","F3","F8","F9","Eisu","F11","Kana","F13","F16","F14",nullptr,"F10","ContextMenu","F12",
        nullptr,"F15","Help","Home","PageUp","FwdDelete","F4","End","F2","PageDown","F1","Left","Right","Down","Up",nullptr,
    };
    if (k < 0x80 && kNames[k]) return kNames[k];
    return "?";
}

} // namespace rdp::mac
