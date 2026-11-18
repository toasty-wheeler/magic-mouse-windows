// keyboard.h - Apple keyboard remapping (after MacKeys, github.com/dooperdrew/mackeys-win).
//
// Pure logic, fed one key event at a time from the low-level keyboard hook; include
// <windows.h> first (VK_* codes). Windows never reports the Apple Fn key (kbdhid drops it),
// so Right Option stands in for it, as in MacKeys:
//   Fn + Left / Right / Up / Down    Home / End / Page Up / Page Down
//   Fn + Backspace / Return          Delete / Insert
//   Fn + Esc                         switch the top row between media keys and F-keys
//   F3 F4 F7..F12 (media first)      Task View, Start, previous / play / next, mute, volume
//   F13 F14 F15                      Print Screen, Scroll Lock, Pause
//   Command <-> Option               so Alt and Windows sit where a PC keyboard has them
//
// Each physical key remembers what its press turned into, so its release always undoes
// exactly that, even if Fn or the settings changed in between.

#pragma once
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <string>

struct KbConfig {
    bool fnKey = true;          // Right Option is Fn
    bool swapCmdOption = true;  // Command <-> Option
    bool mediaFirst = true;     // F-keys need Fn (like a Mac); off: media keys need Fn
    bool extraKeys = true;      // F13-F15 = Print Screen, Scroll Lock, Pause
};

struct KbEvent {
    uint8_t vk;
    bool up;
};

struct KbResult {
    bool swallow = false;       // drop the physical event
    bool toggleTopRow = false;  // Fn + Esc pressed
    int count = 0;              // keys to send, in order
    KbEvent events[4];
};

// Scan code of the Left Control Windows makes up before Right Alt on AltGr layouts.
static const uint32_t kKbAltGrCtrlScan = 0x21D;

class KbRemapper {
public:
    KbResult Process(const KbConfig& c, uint8_t vk, uint32_t scan, bool down) {
        KbResult r;
        bool rightAltUsed = c.fnKey || c.swapCmdOption;
        if (rightAltUsed && vk == VK_LCONTROL && scan == kKbAltGrCtrlScan) {
            r.swallow = true;
            return r;
        }
        if (c.fnKey && vk == VK_RMENU) {
            fnDown_ = down;
            r.swallow = true;
            return r;
        }

        if (!down) {
            uint16_t was = active_[vk];
            active_[vk] = kPassed;
            if (was == kPassed) return r;
            r.swallow = true;
            if (was != kSwallowed) Add(r, (uint8_t)was, true);
            return r;
        }

        if (active_[vk] != kPassed) {               // auto-repeat: repeat what the press became
            r.swallow = true;
            if (active_[vk] != kSwallowed) Add(r, (uint8_t)active_[vk], false);
            return r;
        }

        Mapping m = Map(c, vk, fnDown_);
        switch (m.kind) {
        case Pass:
            return r;
        case Key:
            active_[vk] = m.a;
            Add(r, m.a, false);
            break;
        case Combo:                                 // pressed and released at once
            active_[vk] = kSwallowed;
            Add(r, m.a, false);
            Add(r, m.b, false);
            Add(r, m.b, true);
            Add(r, m.a, true);
            break;
        case ToggleTopRow:
            active_[vk] = kSwallowed;
            r.toggleTopRow = true;
            break;
        }
        r.swallow = true;
        return r;
    }

    // Releases every key a remapped press is still holding down (settings changed, keyboard
    // gone, app closing). Returns the number of events written to out.
    int ReleaseAll(KbEvent* out, int max) {
        int n = 0;
        for (int vk = 0; vk < 256; vk++) {
            uint16_t was = active_[vk];
            active_[vk] = kPassed;
            if (was != kPassed && was != kSwallowed && n < max) out[n++] = { (uint8_t)was, true };
        }
        fnDown_ = false;
        return n;
    }

    bool FnDown() const { return fnDown_; }

private:
    enum Kind { Pass, Key, Combo, ToggleTopRow };
    struct Mapping {
        Kind kind;
        uint8_t a, b;
    };
    static const uint16_t kPassed = 0, kSwallowed = 0x100;

    static Mapping Map(const KbConfig& c, uint8_t vk, bool fn) {
        if (c.swapCmdOption) {
            switch (vk) {
            case VK_LWIN:  return { Key, VK_LMENU, 0 };
            case VK_LMENU: return { Key, VK_LWIN, 0 };
            case VK_RWIN:  return { Key, VK_RMENU, 0 };
            case VK_RMENU: return { Key, VK_RWIN, 0 };     // only reached when it isn't Fn
            }
        }
        if (c.extraKeys) {
            switch (vk) {
            case VK_F13: return { Key, VK_SNAPSHOT, 0 };
            case VK_F14: return { Key, VK_SCROLL, 0 };
            case VK_F15: return { Key, VK_PAUSE, 0 };
            }
        }
        if (fn) {
            switch (vk) {
            case VK_ESCAPE: return { ToggleTopRow, 0, 0 };
            case VK_LEFT:   return { Key, VK_HOME, 0 };
            case VK_RIGHT:  return { Key, VK_END, 0 };
            case VK_UP:     return { Key, VK_PRIOR, 0 };
            case VK_DOWN:   return { Key, VK_NEXT, 0 };
            case VK_BACK:   return { Key, VK_DELETE, 0 };
            case VK_RETURN: return { Key, VK_INSERT, 0 };
            }
        }
        // The top row's media legends apply without Fn when media keys come first, with Fn
        // otherwise; the other way round the key is just its F-key.
        if (fn != c.mediaFirst) {
            switch (vk) {
            case VK_F3:  return { Combo, VK_LWIN, VK_TAB };    // Mission Control: Task View
            case VK_F4:  return { Key, VK_LWIN, 0 };           // Launchpad: Start
            case VK_F7:  return { Key, VK_MEDIA_PREV_TRACK, 0 };
            case VK_F8:  return { Key, VK_MEDIA_PLAY_PAUSE, 0 };
            case VK_F9:  return { Key, VK_MEDIA_NEXT_TRACK, 0 };
            case VK_F10: return { Key, VK_VOLUME_MUTE, 0 };
            case VK_F11: return { Key, VK_VOLUME_DOWN, 0 };
            case VK_F12: return { Key, VK_VOLUME_UP, 0 };
            }
        }
        return { Pass, 0, 0 };
    }

    static void Add(KbResult& r, uint8_t vk, bool up) {
        if (r.count < (int)(sizeof(r.events) / sizeof(r.events[0]))) r.events[r.count++] = { vk, up };
    }

    bool fnDown_ = false;
    uint16_t active_[256] = {};     // per physical key: kPassed, kSwallowed or the VK its press sent
};

// Apple keyboards by product ID (USB VID 05AC, Bluetooth VID 004C), from MacKeys.
inline const wchar_t* AppleKeyboardName(uint32_t pid) {
    switch (pid) {
    case 0x026C: return L"Magic Keyboard with Numeric Keypad";
    case 0x029C: return L"Magic Keyboard with Numeric Keypad";
    case 0x02C1: return L"Magic Keyboard with Numeric Keypad";
    case 0x0267: return L"Magic Keyboard";
    case 0x029A: case 0x029F: return L"Magic Keyboard with Touch ID";
    case 0x029B: return L"Magic Keyboard";
    case 0x02C0: return L"Magic Keyboard";
    case 0x0255: case 0x0256: case 0x0230: return L"Apple Wireless Keyboard";
    case 0x0220: case 0x0221: case 0x020B: case 0x020C: return L"Apple Keyboard";
    }
    return nullptr;
}

inline bool IsAppleKeyboard(uint32_t vid, uint32_t pid) {
    return (vid == 0x05AC || vid == 0x004C) && AppleKeyboardName(pid) != nullptr;
}

// VID / PID from a device path or name: USB "VID_05AC&PID_026C", Bluetooth
// "VID&0001004c_PID&026c" (the vendor ID source in front of the vendor ID).
inline bool ParseVidPid(const wchar_t* path, uint32_t* vid, uint32_t* pid) {
    std::wstring s(path);
    for (wchar_t& ch : s) ch = (wchar_t)towupper(ch);
    size_t v = s.find(L"VID_"), p = s.find(L"PID_");
    if (v == std::wstring::npos) v = s.find(L"VID&");
    if (p == std::wstring::npos) p = s.find(L"PID&");
    if (v == std::wstring::npos || p == std::wstring::npos) return false;
    *vid = wcstoul(s.c_str() + v + 4, nullptr, 16) & 0xFFFF;
    *pid = wcstoul(s.c_str() + p + 4, nullptr, 16) & 0xFFFF;
    return true;
}
