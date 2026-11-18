// kbtest.cpp - checks app\keyboard.h (Apple keyboard remapping) without a keyboard.
// Build: cl /nologo /EHsc /W4 /std:c++17 kbtest.cpp   Run: kbtest  (exit code = failures)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include "..\app\keyboard.h"

#pragma comment(lib, "user32.lib")

static int g_failures;

static std::string Describe(const KbResult& r) {
    std::string s = r.swallow ? "swallow" : "pass";
    if (r.toggleTopRow) s += " toggle";
    for (int i = 0; i < r.count; i++) {
        char b[16];
        sprintf_s(b, " %02X%s", r.events[i].vk, r.events[i].up ? "^" : "v");
        s += b;
    }
    return s;
}

// One key event; expect is Describe()'s output ("swallow 24v" = swallowed, Home down sent).
static void Step(KbRemapper& k, const KbConfig& c, const char* what, uint8_t vk, bool down, const char* expect,
                 uint32_t scan = 0) {
    std::string got = Describe(k.Process(c, vk, scan, down));
    bool ok = got == expect;
    if (!ok) g_failures++;
    printf("%s %-44s %s%s%s\n", ok ? "ok  " : "FAIL", what, got.c_str(), ok ? "" : "   expected ", ok ? "" : expect);
}

int main() {
    KbConfig c;                         // defaults: Fn on Right Option, swap, media first, F13-F15
    {
        KbRemapper k;
        printf("-- Fn layer\n");
        Step(k, c, "Fn down", VK_RMENU, true, "swallow");
        Step(k, c, "Fn+Left down -> Home", VK_LEFT, true, "swallow 24v");
        Step(k, c, "Left repeat -> Home again", VK_LEFT, true, "swallow 24v");
        Step(k, c, "Fn up first", VK_RMENU, false, "swallow");
        Step(k, c, "Left up -> Home up (not Left up)", VK_LEFT, false, "swallow 24^");
        Step(k, c, "Left alone passes", VK_LEFT, true, "pass");
        Step(k, c, "Left up passes", VK_LEFT, false, "pass");
        Step(k, c, "Fn down", VK_RMENU, true, "swallow");
        Step(k, c, "Fn+Backspace -> Delete", VK_BACK, true, "swallow 2Ev");
        Step(k, c, "Backspace up", VK_BACK, false, "swallow 2E^");
        Step(k, c, "Fn+Return -> Insert", VK_RETURN, true, "swallow 2Dv");
        Step(k, c, "Return up", VK_RETURN, false, "swallow 2D^");
        Step(k, c, "Fn+Up -> Page Up", VK_UP, true, "swallow 21v");
        Step(k, c, "Up up", VK_UP, false, "swallow 21^");
        Step(k, c, "Fn+Esc toggles top row", VK_ESCAPE, true, "swallow toggle");
        Step(k, c, "Esc up swallowed", VK_ESCAPE, false, "swallow");
        Step(k, c, "Fn+F11 (media first) -> F11", VK_F11, true, "pass");
        Step(k, c, "F11 up", VK_F11, false, "pass");
        Step(k, c, "Fn up", VK_RMENU, false, "swallow");
    }
    {
        KbRemapper k;
        printf("-- top row, media first\n");
        Step(k, c, "F11 -> Volume Down", VK_F11, true, "swallow AEv");
        Step(k, c, "F11 up", VK_F11, false, "swallow AE^");
        Step(k, c, "F8 -> Play/Pause", VK_F8, true, "swallow B3v");
        Step(k, c, "F8 up", VK_F8, false, "swallow B3^");
        Step(k, c, "F3 -> Win+Tab at once", VK_F3, true, "swallow 5Bv 09v 09^ 5B^");
        Step(k, c, "F3 repeat does nothing", VK_F3, true, "swallow");
        Step(k, c, "F3 up swallowed", VK_F3, false, "swallow");
        Step(k, c, "F5 untouched", VK_F5, true, "pass");
        Step(k, c, "F5 up", VK_F5, false, "pass");
    }
    {
        KbRemapper k;
        KbConfig f = c;
        f.mediaFirst = false;
        printf("-- top row, F-keys first\n");
        Step(k, f, "F11 stays F11", VK_F11, true, "pass");
        Step(k, f, "F11 up", VK_F11, false, "pass");
        Step(k, f, "Fn down", VK_RMENU, true, "swallow");
        Step(k, f, "Fn+F11 -> Volume Down", VK_F11, true, "swallow AEv");
        Step(k, f, "F11 up", VK_F11, false, "swallow AE^");
        Step(k, f, "Fn up", VK_RMENU, false, "swallow");
    }
    {
        KbRemapper k;
        printf("-- modifiers and extra keys\n");
        Step(k, c, "Left Command -> Left Alt", VK_LWIN, true, "swallow A4v");
        Step(k, c, "Left Command up", VK_LWIN, false, "swallow A4^");
        Step(k, c, "Left Option -> Left Win", VK_LMENU, true, "swallow 5Bv");
        Step(k, c, "Left Option up", VK_LMENU, false, "swallow 5B^");
        Step(k, c, "Right Command -> Right Alt", VK_RWIN, true, "swallow A5v");
        Step(k, c, "Right Command up", VK_RWIN, false, "swallow A5^");
        Step(k, c, "AltGr's made-up Left Ctrl dropped", VK_LCONTROL, true, "swallow", kKbAltGrCtrlScan);
        Step(k, c, "real Left Ctrl passes", VK_LCONTROL, true, "pass", 0x1D);
        Step(k, c, "real Left Ctrl up", VK_LCONTROL, false, "pass", 0x1D);
        Step(k, c, "F13 -> Print Screen", VK_F13, true, "swallow 2Cv");
        Step(k, c, "F13 up", VK_F13, false, "swallow 2C^");
        Step(k, c, "F15 -> Pause", VK_F15, true, "swallow 13v");
        Step(k, c, "F15 up", VK_F15, false, "swallow 13^");
        Step(k, c, "A untouched", 'A', true, "pass");
        Step(k, c, "A up", 'A', false, "pass");
    }
    {
        KbRemapper k;
        KbConfig n = c;
        n.fnKey = false;
        printf("-- Fn off: Right Option is swapped instead\n");
        Step(k, n, "Right Option -> Right Win", VK_RMENU, true, "swallow 5Cv");
        Step(k, n, "Left: no Fn layer", VK_LEFT, true, "pass");
        Step(k, n, "Left up", VK_LEFT, false, "pass");
        Step(k, n, "Right Option up", VK_RMENU, false, "swallow 5C^");
        KbConfig off;
        off.fnKey = off.swapCmdOption = off.mediaFirst = off.extraKeys = false;
        printf("-- everything off\n");
        Step(k, off, "Right Option passes", VK_RMENU, true, "pass");
        Step(k, off, "AltGr Left Ctrl passes", VK_LCONTROL, true, "pass", kKbAltGrCtrlScan);
        Step(k, off, "F11 passes", VK_F11, true, "pass");
        Step(k, off, "Left Command passes", VK_LWIN, true, "pass");
    }
    {
        KbRemapper k;
        printf("-- release everything held\n");
        Step(k, c, "Fn down", VK_RMENU, true, "swallow");
        Step(k, c, "Fn+Down -> Page Down held", VK_DOWN, true, "swallow 22v");
        Step(k, c, "Left Command -> Left Alt held", VK_LWIN, true, "swallow A4v");
        KbEvent out[8];
        int n = k.ReleaseAll(out, 8);
        bool ok = n == 2 && out[0].vk == VK_NEXT && out[0].up && out[1].vk == VK_LMENU && out[1].up && !k.FnDown();
        if (!ok) g_failures++;
        printf("%s %-44s %d events\n", ok ? "ok  " : "FAIL", "ReleaseAll lets go of Page Down and Alt", n);
        Step(k, c, "Down up afterwards passes", VK_DOWN, false, "pass");
    }
    {
        printf("-- device IDs\n");
        bool ok = IsAppleKeyboard(0x004C, 0x026C) && IsAppleKeyboard(0x05AC, 0x0267) && !IsAppleKeyboard(0x004C, 0x0269) &&
                  !IsAppleKeyboard(0x045E, 0x026C);
        if (!ok) g_failures++;
        printf("%s %-44s\n", ok ? "ok  " : "FAIL", "keyboard IDs (not the mouse, not other vendors)");

        struct { const wchar_t* path; uint32_t vid, pid; } paths[] = {
            { L"\\\\?\\HID#VID_05AC&PID_026C&MI_01&Col01#8&2fa5df45&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}", 0x05AC, 0x026C },
            { L"\\\\?\\HID#{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&026c&Col01#9&1&0000#{884b96c3}", 0x004C, 0x026C },
            { L"\\\\?\\HID#{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&0267#9&1&0000#{884b96c3}", 0x05AC, 0x0267 },
        };
        for (const auto& p : paths) {
            uint32_t vid = 0, pid = 0;
            ok = ParseVidPid(p.path, &vid, &pid) && vid == p.vid && pid == p.pid && IsAppleKeyboard(vid, pid);
            if (!ok) g_failures++;
            printf("%s parse %04X:%04X from a %s path\n", ok ? "ok  " : "FAIL", p.vid, p.pid,
                   wcsstr(p.path, L"00001124") ? "Bluetooth" : "USB");
        }
        uint32_t vid, pid;
        ok = !ParseVidPid(L"\\\\?\\ACPI#PNP0303#4&1d401fb5&0", &vid, &pid);
        if (!ok) g_failures++;
        printf("%s %-44s\n", ok ? "ok  " : "FAIL", "a PS/2 keyboard has no VID/PID");
    }
    {
        printf("-- keyboards on this PC (information only)\n");
        uint32_t vid, pid;
        UINT n = 0;
        GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
        RAWINPUTDEVICELIST list[64];
        n = min(n, 64u);
        n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
        for (UINT i = 0; n != (UINT)-1 && i < n; i++) {
            if (list[i].dwType != RIM_TYPEKEYBOARD) continue;
            wchar_t name[512];
            UINT len = ARRAYSIZE(name);
            if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name, &len) == (UINT)-1) continue;
            bool has = ParseVidPid(name, &vid, &pid);
            printf("     %04X:%04X %-6s %.70ls\n", has ? vid : 0, has ? pid : 0,
                   has && IsAppleKeyboard(vid, pid) ? "APPLE" : "", name);
        }
    }
    printf("\n%d failure(s)\n", g_failures);
    return g_failures;
}
