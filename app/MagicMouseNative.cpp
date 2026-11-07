// MagicMouseNative.cpp - Stage 1 tray app for the Magic Mouse 2009 (A1296): smooth
// scrolling, tap / two-finger tap clicks, flick Back / Forward, and a live touch view.
//
// Reads raw multi-touch frames (report 0x29) from mmfilter.sys through \\.\MagicMouseNative
// (shared\mmioctl.h) and runs them through the gesture engine (engine.h). Output goes out
// through SendInput: high-resolution wheel / horizontal wheel in sub-notch steps (fractions
// of WHEEL_DELTA) with inertia, and button clicks.
//
// Two-finger click: the physical button reaches Windows through mouhid as a left / right
// click, and the stock report has no middle button, so a low-level mouse hook swaps that
// click for middle down / up while two fingers are on the surface.
//
// The window (double-click the tray icon) shows the touches live and holds all settings;
// they're saved in HKCU\Software\MagicMouseNative, or in MagicMouseNative.ini next to the
// exe if that file exists (the portable version).
//
// Build: build.bat (x64 Native Tools / Developer Command Prompt for VS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <bluetoothapis.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <atomic>
#include <string>
#include <vector>
#include "engine.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t kAppName[]   = L"MagicMouseNative";
static const wchar_t kRegKey[]    = L"Software\\MagicMouseNative";
static const wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const UINT    WM_TRAY      = WM_APP + 1;
static const UINT    WM_CONNECTED = WM_APP + 2;   // wParam: 1 = driver open, 0 = not
static const UINT    WM_BATTERY   = WM_APP + 3;   // g_battery changed
static const UINT    WM_TRAYDOT   = WM_APP + 4;   // wParam: 1 button held, 0 released, 2 a tap clicked
static const UINT    WM_MIDDLEBUTTON = WM_APP + 5;   // wParam: 1 press, 0 release (from the mouse hook)
static const UINT    WM_MOUSESTATE = WM_APP + 6;     // g_mouseState changed
static const UINT    WM_RESTORECURSOR = WM_APP + 7;  // after a pinch: wParam x, lParam y
static const DWORD   kStatusPollMs   = 2000;        // driver status (mouse state, battery)
static const DWORD   kBatteryPollMs  = 60000;
static const DWORD   kBatteryRetryMs = 10000;
static const int     kBatteryLow     = 10;        // % that triggers the low-battery notice
static const DWORD   kGlideTickMs = 8;
static const int     kFramesPerRead = 32;
static const ULONGLONG kFingersFreshMs = 150;     // touch data this recent counts for clicks
static const ULONGLONG kGestureShowMs  = 1500;

// ---------------------------------------------------------------------------------------
// Settings (written by the UI thread, read by the engine thread and the mouse hook)

// Two-finger tap time limit: the slider's range.
static const int kTwoTapFastestMs = 300, kTwoTapSlowestMs = 600;

// The mouse reports being lifted off / put back on the desk in report 0x61: 01 lifted, 00 down.
static const UCHAR kReportLift = 0x61;
static std::atomic<bool> g_lifted{false};    // the mouse is off the desk (sensor not tracking)

struct Settings {
    std::atomic<bool> enabled{true};
    std::atomic<bool> tapToClick{true};
    std::atomic<bool> twoFingerTap{true};
    std::atomic<bool> twoFingerScroll{true};
    std::atomic<bool> twoFingerMiddle{true};
    std::atomic<bool> flickNav{true};
    std::atomic<bool> invertV{false};
    std::atomic<bool> invertH{false};
    std::atomic<bool> horizontal{true};
    std::atomic<bool> inertia{true};
    std::atomic<bool> hiRes{true};
    std::atomic<bool> flickInvert{false};
    std::atomic<bool> leftHanded{false};
    std::atomic<bool> ignoreLifted{false};
    std::atomic<bool> gameMode{false};
    std::atomic<bool> tapDrag{true};
    std::atomic<int>  speed{1};
    std::atomic<int>  glide{1};
    std::atomic<int>  twoTapMs{kTwoTapFastestMs};

    EngineConfig Snapshot() const {
        EngineConfig c;
        c.enabled = enabled;
        c.tapToClick = tapToClick;
        c.twoFingerTap = twoFingerTap;
        c.twoFingerScroll = twoFingerScroll;
        c.flickNav = flickNav;
        c.invertV = invertV;
        c.invertH = invertH;
        c.horizontal = horizontal;
        c.inertia = inertia;
        c.hiRes = hiRes;
        c.speed = speed;
        c.glide = glide;
        c.twoTapMs = twoTapMs;
        c.flickInvert = flickInvert;
        c.leftHanded = leftHanded;
        c.ignoreTouches = ignoreLifted && g_lifted;
        c.gameMode = gameMode;
        c.tapDrag = tapDrag;
        return c;
    }
};
static Settings g_set;

// Registry names for the on/off settings, in the order the window lists them. A row with an
// invert option shows a checkbox for it while the switch is on.
struct BoolSetting {
    std::atomic<bool>* value;
    const wchar_t* reg;
    const wchar_t* label;
    const wchar_t* detail;
    std::atomic<bool>* invert = nullptr;
    const wchar_t* invertReg = nullptr;
};
static const BoolSetting kEnabledSetting = { &g_set.enabled, L"Enabled", L"Gestures", L"All on or off" };
static const BoolSetting kGestureSettings[] = {
    { &g_set.tapToClick,      L"TapToClick",      L"Tap to click",       L"Right side is a right click" },
    { &g_set.twoFingerTap,    L"TwoFingerTap",    L"Two-finger tap",     L"Middle click" },
    { &g_set.twoFingerMiddle, L"TwoFingerMiddle", L"Two-finger click",   L"Hold the middle button" },
    { &g_set.flickNav,        L"FlickNav",        L"Flick to navigate",  L"\x2192 Back, \x2190 Forward", &g_set.flickInvert, L"FlickInvert" },
    { &g_set.tapDrag,         L"TapDrag",         L"Double-tap and hold", L"Keeps the button down to drag" },
    { &g_set.gameMode,        L"GameMode",        L"Game mode",          L"Hold one button, tap the other" },
    { &g_set.leftHanded,      L"LeftHanded",      L"Left-handed",        L"Swap buttons and tap sides" },
};
static const BoolSetting kScrollSettings[] = {
    { &g_set.invertV,         L"InvertV",         L"Invert vertical",    L"Reverse up and down" },
    { &g_set.horizontal,      L"Horizontal",      L"Horizontal scroll",  L"Sideways swipes scroll", &g_set.invertH, L"InvertH" },
    { &g_set.twoFingerScroll, L"TwoFingerScroll", L"Two-finger scroll",  L"Twice as fast" },
    { &g_set.inertia,         L"Inertia",         L"Inertia",            L"Keep gliding after a flick" },
    { &g_set.hiRes,           L"HiRes",           L"Smooth wheel",       L"Off sends whole notches" },
    { &g_set.ignoreLifted,    L"IgnoreLifted",    L"Ignore when lifted", L"No gestures off the desk" },
};

// Window settings (UI thread only).
enum ThemeMode { THEME_AUTO, THEME_LIGHT, THEME_DARK };
static int  g_themeMode = THEME_AUTO;   // automatic follows Windows' app theme
static bool g_onTop;

// Settings store: HKCU\Software\MagicMouseNative, or for a portable copy (one with
// MagicMouseNative.ini next to the exe) that file, so nothing is left on the PC.
static std::wstring g_ini;      // the INI's path when portable
static const wchar_t kIniSection[] = L"Settings";

static void FindPortableIni() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring ini(p, n);
    ini = ini.substr(0, ini.find_last_of(L'\\') + 1) + kAppName + L".ini";
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) g_ini = ini;
}

static DWORD RegGetDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}

static DWORD StoreGet(const wchar_t* name, DWORD def) {
    if (!g_ini.empty()) return GetPrivateProfileIntW(kIniSection, name, (INT)def, g_ini.c_str());
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}

static void StorePut(const wchar_t* name, DWORD v) {
    if (!g_ini.empty()) {
        WritePrivateProfileStringW(kIniSection, name, std::to_wstring(v).c_str(), g_ini.c_str());
        return;
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v));
}

// Binary values; in the INI as hex. Returns the bytes read (0 if none).
static DWORD StoreGetBinary(const wchar_t* name, void* data, DWORD size) {
    if (g_ini.empty()) return RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_BINARY, nullptr, data, &size) == ERROR_SUCCESS ? size : 0;
    std::wstring hex(size * 2 + 2, L'\0');
    DWORD len = GetPrivateProfileStringW(kIniSection, name, L"", hex.data(), (DWORD)hex.size(), g_ini.c_str());
    DWORD n = 0;
    for (; n < size && 2 * n + 1 < len; n++) ((BYTE*)data)[n] = (BYTE)wcstoul(hex.substr(2 * n, 2).c_str(), nullptr, 16);
    return n;
}

static void StorePutBinary(const wchar_t* name, const void* data, DWORD size) {
    if (g_ini.empty()) {
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_BINARY, data, size);
        return;
    }
    std::wstring hex;
    wchar_t b[3];
    for (DWORD i = 0; i < size; i++) {
        swprintf_s(b, L"%02X", ((const BYTE*)data)[i]);
        hex += b;
    }
    WritePrivateProfileStringW(kIniSection, name, hex.c_str(), g_ini.c_str());
}

static void LoadSettings() {
    *kEnabledSetting.value = StoreGet(kEnabledSetting.reg, 1) != 0;
    for (const BoolSetting* list : { kGestureSettings, kScrollSettings }) {
        int n = list == kGestureSettings ? (int)ARRAYSIZE(kGestureSettings) : (int)ARRAYSIZE(kScrollSettings);
        for (int i = 0; i < n; i++) {
            *list[i].value = StoreGet(list[i].reg, *list[i].value) != 0;
            if (list[i].invert) *list[i].invert = StoreGet(list[i].invertReg, *list[i].invert) != 0;
        }
    }
    g_set.speed = (int)min(StoreGet(L"Speed", 1), 2UL);
    g_set.glide = (int)min(StoreGet(L"Glide", 1), 2UL);
    g_set.twoTapMs = (int)max((DWORD)kTwoTapFastestMs, min(StoreGet(L"TwoTapMs", kTwoTapFastestMs), (DWORD)kTwoTapSlowestMs));
    g_themeMode = (int)min(StoreGet(L"Theme", THEME_AUTO), (DWORD)THEME_DARK);
    g_onTop = StoreGet(L"AlwaysOnTop", 0) != 0;
}

static void SaveSettings() {
    auto put = StorePut;
    put(kEnabledSetting.reg, *kEnabledSetting.value);
    for (const BoolSetting* list : { kGestureSettings, kScrollSettings }) {
        int n = list == kGestureSettings ? (int)ARRAYSIZE(kGestureSettings) : (int)ARRAYSIZE(kScrollSettings);
        for (int i = 0; i < n; i++) {
            put(list[i].reg, *list[i].value);
            if (list[i].invert) put(list[i].invertReg, *list[i].invert);
        }
    }
    put(L"Speed", (DWORD)g_set.speed);
    put(L"Glide", (DWORD)g_set.glide);
    put(L"TwoTapMs", (DWORD)g_set.twoTapMs);
    put(L"Theme", (DWORD)g_themeMode);
    put(L"AlwaysOnTop", g_onTop);
}

static std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// ---------------------------------------------------------------------------------------
// Elevation. Windows drops input that a normal app sends to an administrator window (UIPI),
// so taps and scrolling only reach apps like an elevated OBS when this app is elevated too.

static bool g_elevated;

static bool ProcessElevated(HANDLE process) {
    HANDLE token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return true;   // can't look: higher integrity
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

// True if the window under the cursor belongs to an elevated process we can't send input to.
// Called on the engine thread only (it caches the last window).
static bool TargetBlocked() {
    static HWND lastRoot;
    static bool lastBlocked;
    if (g_elevated) return false;
    POINT pt;
    GetCursorPos(&pt);
    HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (!root || root == lastRoot) return root && lastBlocked;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    lastRoot = root;
    lastBlocked = p && ProcessElevated(p);
    if (p) CloseHandle(p);
    return lastBlocked;
}

// Runs a command line without a console window; returns its exit code (-1 if it didn't run).
static DWORD RunHidden(std::wstring cmd) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// ---------------------------------------------------------------------------------------
// Start with Windows: a Run entry, or (when elevated) a logon task that starts the app
// elevated without a UAC prompt each time.

static const wchar_t kTaskName[] = L"MagicMouseNative";

static bool RunEntryExists() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static bool TaskExists() { return RunHidden(std::wstring(L"schtasks /Query /TN ") + kTaskName) == 0; }

static bool StartsWithWindows() { return RunEntryExists() || TaskExists(); }

static void SetRunEntry(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" /tray";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        if (ch == L'&') out += L"&amp;";
        else if (ch == L'<') out += L"&lt;";
        else if (ch == L'>') out += L"&gt;";
        else out += ch;
    }
    return out;
}

// schtasks' own switches can't lift the default 72-hour run limit, so the task is
// registered from XML.
static bool CreateLogonTask() {
    wchar_t domain[256] = L"", user[256] = L"", temp[MAX_PATH];
    GetEnvironmentVariableW(L"USERDOMAIN", domain, ARRAYSIZE(domain));
    GetEnvironmentVariableW(L"USERNAME", user, ARRAYSIZE(user));
    std::wstring account = XmlEscape(std::wstring(domain) + L"\\" + user);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>MagicMouseNative (elevated, so gestures reach administrator windows)</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + account + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" + account + L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(ExePath()) + L"</Command><Arguments>/tray</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n";

    GetTempPathW(MAX_PATH, temp);
    std::wstring path = std::wstring(temp) + L"MagicMouseNative_task.xml";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    fwrite(L"\xFEFF", sizeof(wchar_t), 1, f);       // UTF-16 BOM
    fwrite(xml.c_str(), sizeof(wchar_t), xml.size(), f);
    fclose(f);
    bool ok = RunHidden(std::wstring(L"schtasks /Create /F /TN ") + kTaskName + L" /XML \"" + path + L"\"") == 0;
    DeleteFileW(path.c_str());
    return ok;
}

// Returns false if it couldn't do what was asked.
static bool SetStartsWithWindows(bool on) {
    if (on && g_elevated) {
        if (!CreateLogonTask()) return false;
        SetRunEntry(false);
        return true;
    }
    if (on) {
        SetRunEntry(true);
        return true;
    }
    SetRunEntry(false);
    return !TaskExists() || RunHidden(std::wstring(L"schtasks /Delete /F /TN ") + kTaskName) == 0;
}

// ---------------------------------------------------------------------------------------
// What the touch view shows (written by the engine thread and the hook, read by the UI)

struct View {
    Frame frame{};
    ULONGLONG frameTick = 0;
    bool scrolling = false, gliding = false;
    double glideSpeed = 0;              // Engine::GlideFingerSpeed()
    bool pinching = false;
    ULONGLONG flickTick = 0;            // a flick fired Back / Forward: the light burst
    int flickDir = 0;                   // +1 the finger went right, -1 left
    ULONGLONG tapTick = 0;              // a tap clicked (tray icon dot)
    wchar_t gesture[96] = {};
    ULONGLONG gestureTick = 0;
};
static HWND    g_tray;                 // the tray icon's window (also receives app messages)
static SRWLOCK g_viewLock = SRWLOCK_INIT;
static View    g_view;
static std::atomic<int>       g_fingers{0};
static std::atomic<ULONGLONG> g_fingersTick{0};

static void NoteGesture(const wchar_t* text) {
    AcquireSRWLockExclusive(&g_viewLock);
    wcscpy_s(g_view.gesture, text);
    g_view.gestureTick = GetTickCount64();
    ReleaseSRWLockExclusive(&g_viewLock);
}

static void SendMouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
}

// ---------------------------------------------------------------------------------------
// Engine thread: reads frames from the driver, ticks inertia between frames

static const wchar_t kBlockedNote[] = L"Blocked: that window runs as administrator";

// Pinch: the injected touch points start kPinchSpan px apart (at 96 dpi) and spread with the
// fingers' spread to the power kPinchGain (fingers move little on the mouse).
static const double kPinchSpan = 240, kPinchMinSpan = 24, kPinchGain = 1.8;
// Jitter: how quickly the zoom / pan follow the fingers (0..1 per frame), and the dead zones
// that hold them still: 1.5 % in spread, 4 px of pan (at 96 dpi). Pan: screen px per touch unit.
static const double kPinchSmoothing = 0.35, kPinchZoomDeadZone = 0.015, kPinchPanDeadZone = 4, kPinchPanPerUnit = 0.5;
static double g_pinchDpi = 1.0;
// While a pinch's touch points are down the physical mouse's movement is held back (the mouse
// hook drops it), so the pointer doesn't fight the touch points; afterwards it goes back.
static std::atomic<bool> g_pinchActive{false};
static std::atomic<ULONGLONG> g_pinchTick{0};      // last pinch update: the hold lapses if they stop

// Taps light the tray icon's dot; flicks send a burst of light out of the mouse.
static void NoteClick(MouseButton b) {
    AcquireSRWLockExclusive(&g_viewLock);
    if (b == MouseButton::Back || b == MouseButton::Forward) {
        g_view.flickTick = GetTickCount64();
        g_view.flickDir = (b == MouseButton::Back) != g_set.flickInvert ? 1 : -1;
    } else {
        g_view.tapTick = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&g_viewLock);
    if (b != MouseButton::Back && b != MouseButton::Forward) PostMessageW(g_tray, WM_TRAYDOT, 2, 0);
}

// Left / right taps go through the driver as the mouse's own button reports, so they reach
// every window, elevated ones included. The stock report has no middle or X buttons (or
// wheel), so those still use SendInput.
static bool DriverSend(DWORD code, const void* in, DWORD inLen) {
    HANDLE h = CreateFileW(MM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = DeviceIoControl(h, code, (void*)in, inLen, nullptr, 0, &got, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

static bool DriverClick(MouseButton b) {
    if (b != MouseButton::Left && b != MouseButton::Right) return false;
    MM_INJECT in{ 2, { (UCHAR)(b == MouseButton::Left ? MM_BUTTON_LEFT : MM_BUTTON_RIGHT), 0 } };
    return DriverSend(IOCTL_MM_INJECT_BUTTONS, &in, sizeof(in));
}

// Left-handed mode swaps the mouse's own buttons in the driver (for this mouse only).
static void SendDriverOptions() {
    ULONG options = g_set.leftHanded ? MM_OPTION_SWAP_BUTTONS : 0;
    DriverSend(IOCTL_MM_SET_OPTIONS, &options, sizeof(options));
}

struct SendInputOutput : EngineOutput {
    void Wheel(int delta) override { Scroll(MOUSEEVENTF_WHEEL, delta); }
    void HWheel(int delta) override { Scroll(MOUSEEVENTF_HWHEEL, delta); }
    // Pinch: two touch points injected on the screen around the pointer, spreading and closing
    // with the fingers and moving with them, so browsers and other touch-aware apps zoom and
    // pan natively (the page itself, smoothly, around the pointer) as with a trackpad pinch.
    // The fingers' small tremors are smoothed out and a dead zone holds the touch points still,
    // so a held zoom doesn't shake. Touch feedback is off.
    void Pinch(PinchPhase phase, double scale, double panX, double panY) override {
        static const bool ready = InitializeTouchInjection(2, TOUCH_FEEDBACK_NONE) != FALSE;
        if (!ready) return;
        if (phase == PinchPhase::Begin) {
            GetCursorPos(&center_);
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(center_, MONITOR_DEFAULTTONEAREST), &mi);
            screen_ = mi.rcMonitor;
            zoom_ = Steady{};
            panX_ = Steady{};
            panY_ = Steady{};
            active_ = true;
            g_pinchActive = true;
            NoteGesture(TargetBlocked() ? kBlockedNote : L"Pinch to zoom");
        }
        if (!active_) return;
        double zoom = zoom_.Follow(std::log(max(0.05, scale)), kPinchZoomDeadZone);
        double px = panX_.Follow(panX * kPinchPanPerUnit * g_pinchDpi, kPinchPanDeadZone * g_pinchDpi);
        double py = panY_.Follow(panY * kPinchPanPerUnit * g_pinchDpi, kPinchPanDeadZone * g_pinchDpi);
        double span = kPinchSpan * g_pinchDpi * std::exp(zoom * kPinchGain);
        span = max(kPinchMinSpan * g_pinchDpi, min(span, (double)(screen_.right - screen_.left) * 0.9));
        LONG cx = center_.x + (LONG)px, cy = max(screen_.top + 2, min(screen_.bottom - 3, center_.y + (LONG)py));
        POINTER_FLAGS flags = phase == PinchPhase::Begin  ? POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                            : phase == PinchPhase::Update ? POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                                                          : POINTER_FLAG_UP;
        POINTER_TOUCH_INFO contacts[2] = {};
        for (int i = 0; i < 2; i++) {
            POINTER_TOUCH_INFO& c = contacts[i];
            LONG x = cx + (LONG)((i ? 0.5 : -0.5) * span);
            x = max(screen_.left + 2, min(screen_.right - 3, x));
            c.pointerInfo.pointerType = PT_TOUCH;
            c.pointerInfo.pointerId = i;
            c.pointerInfo.pointerFlags = flags;
            c.pointerInfo.ptPixelLocation = { x, cy };
            c.touchFlags = TOUCH_FLAG_NONE;
            c.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
            c.rcContact = { x - 2, cy - 2, x + 2, cy + 2 };
            c.orientation = 90;
            c.pressure = 32000;
        }
        InjectTouchInput(2, contacts);
        g_pinchTick = GetTickCount64();
        if (phase == PinchPhase::End) {
            active_ = false;
            g_pinchActive = false;
            // Windows may have moved the pointer to a touch point: put it back (after the
            // touch up has been processed).
            PostMessageW(g_tray, WM_RESTORECURSOR, (WPARAM)center_.x, (LPARAM)center_.y);
        }
    }

    // Steadies a jittery value: eased towards the input, then held until it has moved more
    // than the dead zone (and then trailing it by the dead zone, so nothing jumps).
    struct Steady {
        double smooth = 0, held = 0;
        bool started = false;
        double Follow(double input, double deadZone) {
            if (!started) {
                smooth = held = input;
                started = true;
            }
            smooth += (input - smooth) * kPinchSmoothing;
            if (smooth - held > deadZone) held = smooth - deadZone;
            else if (held - smooth > deadZone) held = smooth + deadZone;
            return held;
        }
    };
    Steady zoom_, panX_, panY_;    POINT center_{};
    RECT screen_{};
    bool active_ = false;
    static void Scroll(DWORD flag, int delta) {
        SendMouse(flag, (DWORD)delta);
        // Looking up the window under the pointer can wait on that window's app, so it's
        // done after sending, and only now and then.
        static ULONGLONG lastCheck;
        ULONGLONG now = GetTickCount64();
        if (now - lastCheck >= 500) {
            lastCheck = now;
            if (TargetBlocked()) NoteGesture(kBlockedNote);
        }
    }
    // Held buttons go through SendInput: the driver's injected reports would be undone by the
    // mouse's next own report.
    void Button(MouseButton b, bool isDown) override {
        static const DWORD down[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
        static const DWORD up[]   = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
        int i = (int)b;
        if (i > 2) return;
        SendMouse(isDown ? down[i] : up[i]);
        if (isDown) {
            NoteClick(b);
            NoteGesture(TargetBlocked() ? kBlockedNote : b == MouseButton::Left ? L"Holding left" : L"Holding right");
        }
    }
    void Click(MouseButton b) override {
        static const DWORD down[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                      MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN };
        static const DWORD up[]   = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                      MOUSEEVENTF_XUP, MOUSEEVENTF_XUP };
        static const wchar_t* const names[] = { L"Tap: left click", L"Tap: right click",
                                                L"Two-finger tap: middle click", L"Back", L"Forward" };
        int i = (int)b;
        NoteClick(b);
        wchar_t flick[48];
        if (b == MouseButton::Back || b == MouseButton::Forward) {       // the arrow follows the swipe
            bool right = (b == MouseButton::Back) != g_set.flickInvert;
            swprintf_s(flick, L"Flick %s : %s", right ? L"\x2192" : L"\x2190", names[i]);
        }
        const wchar_t* name = b == MouseButton::Back || b == MouseButton::Forward ? flick : names[i];
        if (DriverClick(b)) {
            NoteGesture(name);
            return;
        }
        DWORD data = b == MouseButton::Back ? XBUTTON1 : b == MouseButton::Forward ? XBUTTON2 : 0;
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = down[i];
        in[1].mi.dwFlags = up[i];
        in[0].mi.mouseData = in[1].mi.mouseData = data;
        SendInput(2, in, sizeof(INPUT));
        NoteGesture(TargetBlocked() ? kBlockedNote : name);
    }
};

static HANDLE g_stop;

static void SetConnected(bool on) { PostMessageW(g_tray, WM_CONNECTED, on, 0); }

static void PublishView(const Engine& engine, const Frame* f) {
    AcquireSRWLockExclusive(&g_viewLock);
    if (f) {
        g_view.frame = *f;
        g_view.frameTick = GetTickCount64();
    }
    g_view.scrolling = engine.Scrolling();
    g_view.gliding = engine.Gliding();
    g_view.glideSpeed = engine.GlideFingerSpeed();
    g_view.pinching = engine.Pinching();
    ReleaseSRWLockExclusive(&g_viewLock);
}

static DWORD WINAPI EngineThread(LPVOID) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    SendInputOutput out;
    Engine engine((double)freq.QuadPart, out);
    std::vector<MM_FRAME> frames(kFramesPerRead);
    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool stopping = false, pressed = false;

    while (!stopping) {
        HANDLE h = CreateFileW(MM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            SetConnected(false);
            g_lifted = false;
            if (WaitForSingleObject(g_stop, 2000) == WAIT_OBJECT_0) break;
            continue;
        }
        SetConnected(true);
        SendDriverOptions();            // the driver forgets them when it reloads

        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = ioEvent;
            ResetEvent(ioEvent);
            DWORD got = 0;
            if (!DeviceIoControl(h, IOCTL_MM_READ_FRAMES, nullptr, 0, frames.data(),
                                 (DWORD)(frames.size() * sizeof(MM_FRAME)), &got, &ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            for (;;) {
                HANDLE waits[2] = { ioEvent, g_stop };
                DWORD r = WaitForMultipleObjects(2, waits, FALSE, engine.NeedsTick() ? kGlideTickMs : INFINITE);
                if (r == WAIT_OBJECT_0) break;
                if (r == WAIT_TIMEOUT) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    engine.Tick(now.QuadPart, g_set.Snapshot());
                    PublishView(engine, nullptr);
                    continue;
                }
                stopping = true;
                CancelIoEx(h, &ov);
                break;
            }
            if (!GetOverlappedResult(h, &ov, &got, TRUE) || stopping) break;
            EngineConfig cfg = g_set.Snapshot();
            for (DWORD i = 0; i < got / sizeof(MM_FRAME); i++) {
                Frame f;
                if (frames[i].Report[0] == kReportLift && frames[i].Length >= 2) {
                    g_lifted = frames[i].Report[1] != 0;     // 1 lifted off the desk, 0 put back down
                    PublishView(engine, nullptr);
                    continue;
                }
                if (!DecodeFrame(frames[i], &f)) continue;
                if (f.dx || f.dy) g_lifted = false;     // the sensor is tracking: it's on the desk
                engine.OnFrame(f, cfg);
                if ((f.buttons != 0) != pressed) {
                    pressed = f.buttons != 0;
                    PostMessageW(g_tray, WM_TRAYDOT, pressed, 0);
                }
                g_fingers = engine.Fingers();
                g_fingersTick = GetTickCount64();
                PublishView(engine, &f);
            }
        }
        engine.Cancel();    // driver gone (mouse removed / filter unloaded): end any pinch, then reopen
        CloseHandle(h);
    }
    CloseHandle(ioEvent);
    return 0;
}

// ---------------------------------------------------------------------------------------
// Two-finger click = middle button (low-level mouse hook, runs on the UI thread)

static HHOOK g_hook;
static UINT  g_middleFor;   // WM_LBUTTONUP / WM_RBUTTONUP that releases the middle button

// Only a physical mouse's clicks are swapped: not injected input, and not touch or pen input
// Windows turns into mouse input (marked with MI_WP_SIGNATURE), such as the pinch's touches.
static bool FromRealMouse(const MSLLHOOKSTRUCT* m) {
    if (m->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) return false;
    return (m->dwExtraInfo & 0xFFFFFF00) != 0xFF515700;
}

// The hook only decides; the middle button is sent from the message loop (WM_MIDDLEBUTTON),
// because input sent from inside a low-level hook re-enters it and can deadlock against the
// engine thread's own input.
static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && FromRealMouse((MSLLHOOKSTRUCT*)lp)) {
        if (wp == WM_MOUSEMOVE && g_pinchActive && GetTickCount64() - g_pinchTick < 300) return 1;   // hold still while pinching
        bool down = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN;
        if (down && !g_middleFor && g_set.enabled && g_set.twoFingerMiddle && g_fingers >= 2 &&
            GetTickCount64() - g_fingersTick < kFingersFreshMs) {
            g_middleFor = wp == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_RBUTTONUP;
            PostMessageW(g_tray, WM_MIDDLEBUTTON, 1, 0);
            return 1;
        }
        if (g_middleFor && wp == g_middleFor) {
            g_middleFor = 0;
            PostMessageW(g_tray, WM_MIDDLEBUTTON, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// Every mouse event in the system (the scrolling we send included) waits for the hook to
// return, so it runs on a thread of its own: never behind the UI thread's painting.
static DWORD WINAPI HookThread(LPVOID) {
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    UnhookWindowsHookEx(g_hook);
    return 0;
}

// ---------------------------------------------------------------------------------------
// Battery: feature report 0x47 (Battery Strength, 0-100 %). Read by the filter, or from the
// mouse collection: Windows owns that collection, but feature requests work on a
// zero-access handle.

static std::atomic<int> g_battery{-1};      // -1 = unknown

static int ReadBattery() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return -1;

    int level = -1;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; level < 0 && SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> raw(need);
        auto det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)raw.data();
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, det, need, nullptr, nullptr)) continue;

        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{ sizeof(at) };
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (HidD_GetAttributes(h, &at) && at.VendorID == 0x05AC && (at.ProductID == 0x030D || at.ProductID == 0x0310) &&
            HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.FeatureReportByteLength >= 2) {
                std::vector<BYTE> buf(caps.FeatureReportByteLength);
                buf[0] = 0x47;
                if (HidD_GetFeature(h, buf.data(), (ULONG)buf.size())) level = min((int)buf[1], 100);
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(devs);
    return level;
}

static bool DriverCall(DWORD code, void* out, DWORD outLen);

// The filter reads the battery itself over the control channel (HidBth answers feature
// requests with "busy" while the filter runs, or with 0 while the mouse reconnects); ask
// Windows directly only when the filter isn't there.
// The mouse's state for the window title: asleep, the Magic Mouse drops its Bluetooth link
// (Windows still lists it, so it looks connected) and the driver sees its channels close.
enum MouseState { MOUSE_SEARCHING, MOUSE_ASLEEP, MOUSE_AWAKE };
static std::atomic<int> g_mouseState{MOUSE_SEARCHING};

// Polls the driver every kStatusPollMs for the mouse's state and battery level (the driver
// reads the battery itself; Windows is asked directly only without the driver).
static DWORD WINAPI StatusThread(LPVOID) {
    ULONGLONG lastFallback = 0;
    do {
        MM_STATUS s{};
        bool ok = DriverCall(IOCTL_MM_GET_STATUS, &s, sizeof(s));
        int state = !ok || !s.Attached ? MOUSE_SEARCHING : !s.InterruptOpen ? MOUSE_ASLEEP : MOUSE_AWAKE;
        if (state != g_mouseState.exchange(state)) PostMessageW(g_tray, WM_MOUSESTATE, 0, 0);
        if (state != MOUSE_AWAKE) g_lifted = false;     // its last lift report may be stale on waking
        int level = s.Battery;              // -1 until the driver's first read
        if (!ok) {
            ULONGLONG now = GetTickCount64();
            if (now - lastFallback < (g_battery >= 0 ? kBatteryPollMs : kBatteryRetryMs)) continue;
            lastFallback = now;
            level = ReadBattery();
        }
        if (level != g_battery.exchange(level)) PostMessageW(g_tray, WM_BATTERY, 0, 0);
    } while (WaitForSingleObject(g_stop, kStatusPollMs) == WAIT_TIMEOUT);
    return 0;
}

// Status and commands go through their own handle so they never wait on the read.
static bool DriverCall(DWORD code, void* out, DWORD outLen) {
    HANDLE h = CreateFileW(MM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = DeviceIoControl(h, code, nullptr, 0, out, outLen, &got, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

static void ShowStatus(HWND hwnd) {
    MM_STATUS s{};
    if (!DriverCall(IOCTL_MM_GET_STATUS, &s, sizeof(s))) {
        MessageBoxW(hwnd, L"Can't reach mmfilter (\\\\.\\MagicMouseNative).\n\n"
                          L"Is the driver installed and the mouse paired?", kAppName, MB_ICONWARNING);
        return;
    }
    wchar_t text[512];
    swprintf_s(text,
        L"Driver interface version %lu, mice attached: %lu\n\n"
        L"Control channel: %s\nInterrupt channel: %s\nMulti-touch: %s (D7 01 sent %lu times, last status 0x%08lX)\n\n"
        L"Touch frames: %lu (dropped %lu)\nRead errors: %lu\nOther reports: %lu (last ID 0x%02lX)\n"
        L"Tap clicks sent by the driver: %lu reports",
        s.Version, s.Attached,
        s.ControlOpen ? L"open" : L"not seen", s.InterruptOpen ? L"open" : L"not seen",
        s.TouchActive ? L"on" : L"off", s.EnableAttempts, (unsigned long)s.LastEnableStatus,
        s.FramesTotal, s.FramesDropped, s.ReadErrors, s.OtherReports, s.LastOtherId, s.Injected);
    MessageBoxW(hwnd, text, L"MagicMouseNative status", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------------------
// Window: one custom-drawn surface (GDI+). A header with the title, status and the always-
// on-top and theme buttons; Gestures switches on the left and Scrolling switches on the
// right of the live touch view, whose touches light up the edge of the mouse they're
// nearest (ambilight); app controls in the footer. Layout is in 96-dpi units.

namespace gp = Gdiplus;

static const int kMinX = -1100, kMaxX = 1258, kMinY = -1589, kMaxY = 2047;   // touch coordinates
static const int kClientW = 940, kClientH = 720;
static const int kMargin = 24, kSideW = 272, kHeaderH = 76, kFooterH = 72;
static const int kRowH = 58, kSwitchW = 46, kSwitchH = 28;
static double g_dpi = 1.0;

static float S(double v) { return (float)(v * g_dpi); }

struct Palette {
    gp::Color bg, card, text, subtext, line, hover, switchOff, segment, segmentOn, body, bodyEdge, shadow;
};
static const Palette kLightPalette = {
    gp::Color(255, 242, 242, 247), gp::Color(215, 255, 255, 255), gp::Color(255, 28, 28, 30),
    gp::Color(255, 110, 110, 115), gp::Color(255, 229, 229, 234), gp::Color(255, 236, 236, 241),
    gp::Color(255, 214, 214, 219), gp::Color(255, 229, 229, 234), gp::Color(255, 255, 255, 255),
    gp::Color(255, 255, 255, 255), gp::Color(255, 209, 209, 214), gp::Color(40, 0, 0, 0),
};
static const Palette kDarkPalette = {
    gp::Color(255, 28, 28, 30), gp::Color(200, 44, 44, 46), gp::Color(255, 242, 242, 247),
    gp::Color(255, 152, 152, 157), gp::Color(255, 58, 58, 60), gp::Color(255, 58, 58, 60),
    gp::Color(255, 72, 72, 74), gp::Color(255, 58, 58, 60), gp::Color(255, 99, 99, 102),
    gp::Color(255, 44, 44, 46), gp::Color(255, 72, 72, 74), gp::Color(90, 0, 0, 0),
};
static const gp::Color kAccent(255, 10, 132, 255), kGreen(255, 52, 199, 89), kWhite(255, 255, 255, 255);

static bool g_dark;                     // theme in effect (g_themeMode resolved)
static const Palette& Pal() { return g_dark ? kDarkPalette : kLightPalette; }

static gp::Color Mix(gp::Color a, gp::Color b, float t) {
    auto m = [t](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * t + 0.5f); };
    return gp::Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}

static gp::Color WithAlpha(gp::Color c, int alpha) {
    return gp::Color((BYTE)max(0, min(255, alpha)), c.GetR(), c.GetG(), c.GetB());
}

struct Fonts { gp::Font *title, *body, *caption, *section, *big, *icon, *glyph; };
static Fonts g_fonts;

static void CreateFonts() {
    g_fonts.title   = new gp::Font(L"Segoe UI Semibold", S(20), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.body    = new gp::Font(L"Segoe UI", S(14), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.caption   = new gp::Font(L"Segoe UI", S(12), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.section = new gp::Font(L"Segoe UI Semibold", S(12), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.big     = new gp::Font(L"Segoe UI Semibold", S(17), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.icon    = new gp::Font(L"Segoe MDL2 Assets", S(15), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.glyph   = new gp::Font(L"Segoe MDL2 Assets", S(10), gp::FontStyleRegular, gp::UnitPixel);
}

static void DeleteFonts() {
    delete g_fonts.title; delete g_fonts.body; delete g_fonts.caption;
    delete g_fonts.section; delete g_fonts.big; delete g_fonts.icon; delete g_fonts.glyph;
}

// ---- drawing helpers

static void RoundPath(gp::GraphicsPath& p, gp::RectF r, float radius) {
    float d = min(radius * 2, min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(gp::Graphics& g, gp::RectF r, float radius, gp::Color c) {
    gp::GraphicsPath p;
    RoundPath(p, r, radius);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void StrokeRound(gp::Graphics& g, gp::RectF r, float radius, gp::Color c, float width) {
    gp::GraphicsPath p;
    RoundPath(p, r, radius);
    gp::Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

static void Label(gp::Graphics& g, const wchar_t* s, const gp::Font* f, gp::Color c, gp::RectF r,
                     gp::StringAlignment h = gp::StringAlignmentNear, gp::StringAlignment v = gp::StringAlignmentCenter) {
    gp::StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    gp::SolidBrush b(c);
    g.DrawString(s, -1, f, r, &fmt, &b);
}

// Soft round glow: color at the centre fading to transparent at radius.
static void FillGlow(gp::Graphics& g, float cx, float cy, float radius, gp::Color c) {
    if (c.GetA() == 0 || radius < 1) return;
    gp::GraphicsPath p;
    p.AddEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
    gp::PathGradientBrush b(&p);
    b.SetCenterColor(c);
    gp::Color edge = WithAlpha(c, 0);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    g.FillPath(&b, &p);
}

// ---- interactive elements (rebuilt by every paint, used for hit testing)

enum ElemKind { E_TOGGLE, E_SEGMENT, E_BUTTON, E_PIN, E_THEME, E_THEME_ITEM, E_SLIDER, E_DEVICE, E_CHECK };
enum ButtonId { BTN_STATUS, BTN_REENABLE, BTN_ELEVATE };
enum SegmentId { SEG_SPEED, SEG_GLIDE };
struct Elem { ElemKind kind; gp::RectF rc; int id; int sub; };

static const int kGestureCount = (int)ARRAYSIZE(kGestureSettings);
static const int kScrollCount = (int)ARRAYSIZE(kScrollSettings);
static const int kSlotStartup = kGestureCount + kScrollCount;      // toggle slots: gestures, scrolling, startup,
static const int kSlotEnabled = kSlotStartup + 1;                 // the master switch

static std::vector<Elem> g_elems;
static int   g_hover = -1, g_pressed = -1;
static bool  g_themeOpen;
static bool  g_startup;                 // cached StartsWithWindows() (it runs schtasks)
static float g_knob[kSlotEnabled + 1];  // animated switch positions, 0 off .. 1 on

static HWND g_window;
static UINT g_taskbarCreated;
static bool g_connected;

static bool ToggleValue(int slot) {
    if (slot < kGestureCount) return *kGestureSettings[slot].value;
    if (slot < kSlotStartup) return *kScrollSettings[slot - kGestureCount].value;
    if (slot == kSlotEnabled) return g_set.enabled;
    return g_startup;
}

static int AddElem(ElemKind kind, gp::RectF rc, int id, int sub = 0) {
    g_elems.push_back({ kind, rc, id, sub });
    return (int)g_elems.size() - 1;
}

static bool Hovered(int elem) { return elem >= 0 && elem == g_hover; }

static void DrawSwitch(gp::Graphics& g, gp::RectF r, float t) {
    const Palette& p = Pal();
    FillRound(g, r, r.Height / 2, Mix(p.switchOff, kGreen, t));
    float d = r.Height - S(4);
    float x = r.X + S(2) + t * (r.Width - d - S(4));
    gp::SolidBrush shadow(gp::Color(50, 0, 0, 0)), knob(kWhite);
    g.FillEllipse(&shadow, x, r.Y + S(3), d, d);
    g.FillEllipse(&knob, x, r.Y + S(2), d, d);
}

static const wchar_t kGlyphSwap[] = L"\xE8AB", kGlyphTick[] = L"\xE73E";

// Invert checkbox, graphic only (a swap symbol and a box); fades in with the row's switch.
static void DrawInvertCheck(gp::Graphics& g, gp::RectF box, int slot, bool checked, float fade) {
    const Palette& p = Pal();
    int alpha = (int)(255 * fade);
    gp::RectF hit(box.X - S(24), box.Y - S(6), box.Width + S(30), box.Height + S(12));
    int e = fade > 0.5f ? AddElem(E_CHECK, hit, slot) : -1;
    if (Hovered(e)) FillRound(g, hit, S(8), WithAlpha(p.hover, alpha));
    Label(g, kGlyphSwap, g_fonts.glyph, WithAlpha(checked ? kAccent : p.subtext, alpha),
          gp::RectF(box.X - S(20), box.Y, S(16), box.Height), gp::StringAlignmentCenter);
    if (checked) {
        FillRound(g, box, S(5), WithAlpha(kAccent, alpha));
        Label(g, kGlyphTick, g_fonts.glyph, WithAlpha(kWhite, alpha), box, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    } else {
        StrokeRound(g, box, S(5), WithAlpha(p.subtext, alpha), S(1.5));
    }
}

// A switch row; settings with an invert option get its checkbox while the switch is on.
static void DrawToggleRow(gp::Graphics& g, gp::RectF r, int slot, const wchar_t* title, const wchar_t* detail,
                          const BoolSetting* setting = nullptr) {
    const Palette& p = Pal();
    int e = AddElem(E_TOGGLE, r, slot);
    if (Hovered(e)) FillRound(g, gp::RectF(r.X + S(4), r.Y + S(3), r.Width - S(8), r.Height - S(6)), S(8), p.hover);
    float t = g_knob[slot];
    bool invert = setting && setting->invert;
    float textW = r.Width - S(16) - S(kSwitchW) - S(28) - (invert ? S(44) * t : 0);
    Label(g, title, g_fonts.body, p.text, gp::RectF(r.X + S(16), r.Y + S(9), textW, S(20)));
    Label(g, detail, g_fonts.caption, p.subtext, gp::RectF(r.X + S(16), r.Y + S(30), textW, S(18)));
    float sx = r.X + r.Width - S(16) - S(kSwitchW);
    DrawSwitch(g, gp::RectF(sx, r.Y + (r.Height - S(kSwitchH)) / 2, S(kSwitchW), S(kSwitchH)), t);
    if (invert && t > 0.02f) {
        DrawInvertCheck(g, gp::RectF(sx - S(32), r.Y + (r.Height - S(20)) / 2, S(20), S(20)), slot, *setting->invert, t);
    }
}

static float DrawSectionLabel(gp::Graphics& g, float x, float y, float w, const wchar_t* label) {
    Label(g, label, g_fonts.section, Pal().subtext, gp::RectF(x + S(4), y, w, S(20)));
    return y + S(24);
}

// Two-finger tap time limit slider, dragged with the mouse (see WindowProc).
static const int kSliderRowH = 40;
static gp::RectF g_sliderTrack;         // where the track was last drawn, for dragging
static bool g_dragSlider;

static void SetTwoTapFromX(float x) {
    float t = max(0.0f, min(1.0f, (x - g_sliderTrack.X) / max(1.0f, g_sliderTrack.Width)));
    int ms = kTwoTapFastestMs + (int)(t * (kTwoTapSlowestMs - kTwoTapFastestMs) / 10 + 0.5f) * 10;
    g_set.twoTapMs = ms;
}

static void DrawTapSlider(gp::Graphics& g, gp::RectF r) {
    const Palette& p = Pal();
    bool on = g_set.twoFingerTap;
    int e = AddElem(E_SLIDER, r, 0);
    gp::Color text = on ? p.subtext : WithAlpha(p.subtext, 110);
    Label(g, L"Tap window", g_fonts.caption, text, gp::RectF(r.X + S(16), r.Y, S(72), r.Height));
    wchar_t value[16];
    swprintf_s(value, L"%.2f s", g_set.twoTapMs / 1000.0);
    Label(g, value, g_fonts.caption, text, gp::RectF(r.X + r.Width - S(16) - S(44), r.Y, S(44), r.Height),
          gp::StringAlignmentFar);
    g_sliderTrack = gp::RectF(r.X + S(94), r.Y + r.Height / 2 - S(2), r.Width - S(94) - S(68), S(4));
    float t = (float)(g_set.twoTapMs - kTwoTapFastestMs) / (kTwoTapSlowestMs - kTwoTapFastestMs);
    float kx = g_sliderTrack.X + t * g_sliderTrack.Width, ky = r.Y + r.Height / 2;
    FillRound(g, g_sliderTrack, S(2), p.switchOff);
    FillRound(g, gp::RectF(g_sliderTrack.X, g_sliderTrack.Y, kx - g_sliderTrack.X, g_sliderTrack.Height), S(2),
              on ? kAccent : p.line);
    float kr = (Hovered(e) || g_dragSlider) && on ? S(10) : S(9);
    gp::SolidBrush shadow(gp::Color(50, 0, 0, 0)), knob(on ? kWhite : Mix(kWhite, p.line, 0.5f));
    g.FillEllipse(&shadow, kx - kr, ky - kr + S(1), kr * 2, kr * 2);
    g.FillEllipse(&knob, kx - kr, ky - kr, kr * 2, kr * 2);
}

// A card of switch rows; sliderAfter puts the tap-window slider under that row (-1: none).
static float DrawToggleCard(gp::Graphics& g, float x, float y, float w, const BoolSetting* settings, int count,
                            int firstSlot, int sliderAfter = -1) {
    const Palette& p = Pal();
    gp::RectF card(x, y, w, S(kRowH) * count + (sliderAfter >= 0 ? S(kSliderRowH) : 0));
    FillRound(g, card, S(12), p.card);
    gp::Pen line(p.line, S(1));
    float ry = y;
    for (int i = 0; i < count; i++) {
        if (i) g.DrawLine(&line, x + S(16), ry, x + w, ry);
        DrawToggleRow(g, gp::RectF(x, ry, w, S(kRowH)), firstSlot + i, settings[i].label, settings[i].detail, &settings[i]);
        ry += S(kRowH);
        if (i == sliderAfter) {
            DrawTapSlider(g, gp::RectF(x, ry - S(8), w, S(kSliderRowH)));
            ry += S(kSliderRowH);
        }
    }
    return y + card.Height;
}

static void DrawSegmented(gp::Graphics& g, gp::RectF r, int id, const wchar_t* const* labels, int selected) {
    const Palette& p = Pal();
    FillRound(g, r, S(9), p.segment);
    float w = r.Width / 3;
    for (int i = 0; i < 3; i++) {
        gp::RectF seg(r.X + w * i, r.Y, w, r.Height);
        int e = AddElem(E_SEGMENT, seg, id, i);
        gp::RectF pill(seg.X + S(2), seg.Y + S(2), seg.Width - S(4), seg.Height - S(4));
        if (i == selected) {
            FillRound(g, gp::RectF(pill.X, pill.Y + S(1), pill.Width, pill.Height), S(7), p.shadow);
            FillRound(g, pill, S(7), p.segmentOn);
        } else if (Hovered(e)) {
            FillRound(g, pill, S(7), WithAlpha(p.segmentOn, 110));
        }
        Label(g, labels[i], i == selected ? g_fonts.section : g_fonts.caption, p.text, seg,
                 gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    }
}

static void DrawButton(gp::Graphics& g, gp::RectF r, int id, const wchar_t* text, bool primary) {
    const Palette& p = Pal();
    int e = AddElem(E_BUTTON, r, id);
    gp::Color fill = primary ? kAccent : p.card;
    if (Hovered(e)) fill = primary ? Mix(kAccent, kWhite, 0.15f) : p.hover;
    FillRound(g, r, S(8), fill);
    if (!primary) StrokeRound(g, r, S(8), p.line, S(1));
    Label(g, text, g_fonts.caption, primary ? kWhite : p.text, r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

// ---- touch view with ambilight

// Touch colours by the order fingers landed; the first finger turns green while scrolling.
static const gp::Color kTouchColors[] = {
    gp::Color(255, 10, 132, 255), gp::Color(255, 191, 90, 242), gp::Color(255, 255, 159, 10),
    gp::Color(255, 255, 55, 95), gp::Color(255, 100, 210, 255),
};
static const gp::Color kBatteryGreen(255, 52, 199, 89), kBatteryYellow(255, 255, 204, 0), kBatteryRed(255, 255, 59, 48);
static const int kBatteryLowLevel = 35, kBatteryCriticalLevel = 15;    // %: yellow, red

static gp::Color TouchColor(int order, bool scrolling) {
    return scrolling && order == 0 ? kGreen : kTouchColors[order % ARRAYSIZE(kTouchColors)];
}

static gp::Color BatteryColor(int level) {
    return level < kBatteryCriticalLevel ? kBatteryRed : level < kBatteryLowLevel ? kBatteryYellow : kBatteryGreen;
}

// One per finger, for both the touch dot and its light. It outlives the finger: pressing the
// button makes the mouse report fingers as briefly lifting, so a glow rides out a short gap
// (kGlowGraceMs) and then fades out instead of vanishing.
struct Glow {
    int id, order;              // order: colour slot, kept until the glow has faded
    float x, y, a;              // position (touch coordinates), strength 0..1
    float major, minor;         // contact size
    float r, g, b;              // current colour; r < 0 until first set
    bool live;                  // finger down in the latest frame
    ULONGLONG seen;             // last time the finger was down
};
static std::vector<Glow> g_glows;
static const ULONGLONG kGlowGraceMs = 150;

static gp::Color GlowColor(const Glow& gl, int alpha = 255) {
    return gp::Color((BYTE)max(0, min(255, alpha)), (BYTE)max(0.0f, gl.r), (BYTE)gl.g, (BYTE)gl.b);
}

// Eases each finger's glow in, follows it, and fades it out after the finger lifts. A finger
// that lands takes the first colour no other finger on the surface is using.
static void UpdateGlows(const View& v, bool fresh) {
    ULONGLONG now = GetTickCount64();
    for (Glow& gl : g_glows) gl.live = false;
    for (int i = 0; fresh && i < v.frame.count; i++) {
        const Touch& t = v.frame.touch[i];
        if (!IsDown(t)) continue;
        Glow* gl = nullptr;
        for (Glow& x : g_glows) if (x.id == t.id) gl = &x;
        if (!gl) {
            g_glows.push_back({ t.id, -1, (float)t.x, (float)t.y, 0, (float)t.major, (float)t.minor, -1, 0, 0, false, now });
            gl = &g_glows.back();
        }
        gl->x += (t.x - gl->x) * 0.5f;
        gl->y += (t.y - gl->y) * 0.5f;
        gl->major += (t.major - gl->major) * 0.4f;
        gl->minor += (t.minor - gl->minor) * 0.4f;
        float target = 0.55f + 0.45f * min(1.0f, t.major / 160.0f);
        gl->a += (target - gl->a) * 0.3f;
        gl->live = true;
        gl->seen = now;
    }
    for (Glow& gl : g_glows) {
        if (!gl.live || gl.order >= 0) continue;
        for (int order = 0; gl.order < 0; order++) {
            bool used = false;
            for (const Glow& other : g_glows) used |= other.live && other.order == order;
            if (!used) gl.order = order;
        }
    }
    for (Glow& gl : g_glows) {
        // Ease the colour (e.g. blue to green when scrolling starts) over ~0.2 s; a new
        // glow (r < 0) starts on it.
        gp::Color c = TouchColor(max(gl.order, 0), v.scrolling);
        float k = gl.r < 0 ? 1.0f : 0.15f;
        if (gl.r < 0) gl.r = c.GetR();
        gl.r += (c.GetR() - gl.r) * k;
        gl.g += (c.GetG() - gl.g) * k;
        gl.b += (c.GetB() - gl.b) * k;
        if (!gl.live && now - gl.seen > kGlowGraceMs) gl.a *= 0.9f;
    }
    for (size_t i = g_glows.size(); i-- > 0;) {
        if (!g_glows[i].live && g_glows[i].a < 0.02f) g_glows.erase(g_glows.begin() + i);
    }
}

// Where the mouse's top surface goes in the centre column: front at the top, with room
// around it for the glow and the readout below.
struct TouchLayout {
    gp::RectF area, body;
    double scale;
    float X(double x) const { return body.X + (float)((x - kMinX) * scale); }
    float Y(double y) const { return body.Y + (float)((y - kMinY) * scale); }
};

static TouchLayout LayoutTouchView(gp::RectF area) {
    float glowRoom = S(34), labelH = S(4), textH = S(70);
    double spanX = kMaxX - kMinX, spanY = kMaxY - kMinY;
    double scale = min((area.Width - 2 * glowRoom) / spanX, (area.Height - textH - labelH - 2 * glowRoom) / spanY);
    float bw = (float)(spanX * scale), bh = (float)(spanY * scale);
    return { area, gp::RectF(area.X + (area.Width - bw) / 2, area.Y + labelH + glowRoom, bw, bh), scale };
}

// Drawn first, under everything, as if lit from behind the mouse: the light follows the
// mouse's outline and fades with distance from it. Each finger lights the stretch of edge
// it's nearest (more strongly the closer it is to that edge), fading around the outline;
// a low battery lights the whole outline (yellow, red, gently pulsing).
//
// The mouse is a vertical capsule: a centre segment from (cx, top) to (cx, bottom) and a
// radius. For any point, the nearest point on that segment gives both its distance from the
// outline and the outward direction there, which is what the light is shaped by. The field
// is computed at 1/kAmbientScale resolution and smoothed up, so it has no edges anywhere.
static const int   kAmbientScale = 8;
static const float kAmbientSpread = 1.0f;       // radians around the outline (1 sigma)
static const float kAmbientReach = 230;         // 96-dpi px for the light to fall to 1/e
static const float kAmbientAlpha = 0.8f;
static const float kRingReach = 70;

// A pressed button lights that whole side instead, in the colour opposite the background.
static gp::Color ContrastColor() { return g_dark ? kWhite : gp::Color(255, 0, 0, 0); }

static void DrawAmbient(gp::Graphics& g, const TouchLayout& L, float w, float h, float clickL, float clickR,
                        float flick, int flickDir, gp::Color flickColor) {
    struct Source { float angle, along, strength, twoSpread2, twoAlong2; gp::Color c; };
    const gp::RectF& body = L.body;
    float cx = body.X + body.Width / 2, radius = body.Width / 2;
    float top = body.Y + radius, bottom = body.Y + body.Height - radius;
    float fingerAlong = body.Height * 0.45f, sideAlong = body.Height, sideSpread = 1.25f;
    float fingerSpread2 = 2 * kAmbientSpread * kAmbientSpread, fingerAlong2 = 2 * fingerAlong * fingerAlong;

    Source sources[MM_MAX_TOUCHES + 3];
    int count = 0;
    float middle = body.Y + body.Height / 2;
    if (clickL > 0.01f) sources[count++] = { 3.14159265f, middle, clickL, 2 * sideSpread * sideSpread, 2 * sideAlong * sideAlong, ContrastColor() };
    if (clickR > 0.01f) sources[count++] = { 0.0f, middle, clickR, 2 * sideSpread * sideSpread, 2 * sideAlong * sideAlong, ContrastColor() };
    if (flick > 0.01f) {                    // a flick: the side it ended on pulses
        sources[count++] = { flickDir > 0 ? 0.0f : 3.14159265f, middle, flick * 1.3f, 2 * sideSpread * sideSpread,
                             2 * sideAlong * sideAlong, Mix(flickColor, kWhite, 0.15f) };
    }
    for (const Glow& gl : g_glows) {
        if (count == ARRAYSIZE(sources)) break;
        float tx = L.X(gl.x), ty = L.Y(gl.y);
        float sy = max(top, min(bottom, ty));
        float dx = tx - cx, dy = ty - sy, len = sqrtf(dx * dx + dy * dy);
        if (len < 1) {                          // on the centre line: light the nearer end
            dx = 0;
            dy = ty < body.Y + body.Height / 2 ? -1.0f : 1.0f;
        }
        float closeness = max(0.0f, min(1.0f, len / radius));
        sources[count++] = { atan2f(dy, dx), sy, gl.a * (0.35f + 0.65f * closeness), fingerSpread2, fingerAlong2, GlowColor(gl) };
    }
    int level = g_battery;
    bool ring = level >= 0 && level < kBatteryLowLevel;
    if (!count && !ring) return;
    float ringStrength = ring ? 0.75f + 0.25f * sinf((float)(GetTickCount64() % 100000) / 1000.0f * 2.2f) : 0;
    gp::Color ringColor = ring ? BatteryColor(level) : kWhite;

    int bw = (int)(w / kAmbientScale) + 1, bh = (int)(h / kAmbientScale) + 1;
    gp::Bitmap field(bw, bh, PixelFormat32bppARGB);
    gp::BitmapData data;
    gp::Rect all(0, 0, bw, bh);
    if (field.LockBits(&all, gp::ImageLockModeWrite, PixelFormat32bppARGB, &data) != gp::Ok) return;
    float reach = S(kAmbientReach), ringReach = S(kRingReach);
    for (int y = 0; y < bh; y++) {
        UINT32* row = (UINT32*)((BYTE*)data.Scan0 + y * data.Stride);
        float py = (y + 0.5f) * kAmbientScale;
        float sy = max(top, min(bottom, py));
        for (int x = 0; x < bw; x++) {
            float px = (x + 0.5f) * kAmbientScale;
            float dx = px - cx, dy = py - sy;
            float d = max(0.0f, sqrtf(dx * dx + dy * dy) - radius);    // distance outside the outline
            float angle = atan2f(dy, dx);
            float sum = 0, r = 0, gr = 0, b = 0;
            for (int i = 0; i < count; i++) {
                float da = angle - sources[i].angle;
                if (da > 3.14159265f) da -= 6.2831853f;
                if (da < -3.14159265f) da += 6.2831853f;
                float ds = sy - sources[i].along;
                float k = sources[i].strength * expf(-da * da / sources[i].twoSpread2 - ds * ds / sources[i].twoAlong2 - d / reach);
                sum += k;
                r += k * sources[i].c.GetR();
                gr += k * sources[i].c.GetG();
                b += k * sources[i].c.GetB();
            }
            if (ring) {
                float k = ringStrength * expf(-d / ringReach);
                sum += k;
                r += k * ringColor.GetR();
                gr += k * ringColor.GetG();
                b += k * ringColor.GetB();
            }
            if (sum < 0.004f) {
                row[x] = 0;
                continue;
            }
            UINT32 a = (UINT32)(min(1.0f, sum) * kAmbientAlpha * 255);
            row[x] = (a << 24) | ((UINT32)(r / sum) << 16) | ((UINT32)(gr / sum) << 8) | (UINT32)(b / sum);
        }
    }
    field.UnlockBits(&data);

    g.SetInterpolationMode(gp::InterpolationModeHighQualityBilinear);
    g.DrawImage(&field, gp::RectF(0, 0, (float)(bw * kAmbientScale), (float)(bh * kAmbientScale)));
}

// Hyperspeed: while the page glides vertically, streaks of light race across the background
// in the direction of the swipe, faster, longer and brighter the faster the glide; they ease
// in, and slow and fade as the glide stops.
struct Streak { float x, y, length, speed, alpha, width; };
static std::vector<Streak> g_streaks;
static float g_hyper;                   // 0..1, eased towards the glide speed
static float g_hyperDir = 1;            // +1 streaks move down, -1 up
static ULONGLONG g_hyperTick;
static const int   kStreaks = 140;
static const float kHyperFullSpeed = 4000;     // wheel units/s for full effect
static const float kStreakSpeed = 2600;        // 96-dpi px/s at full effect

// Lifted mouse: the touch view fades to 30 % and blurs; slowly when lifted, quickly when put
// back down. 0 on the desk .. 1 lifted.
static float g_lift;
static const float kLiftedOpacity = 0.3f;

static void EaseLift() {
    float target = g_lifted ? 1.0f : 0.0f;
    g_lift += (target - g_lift) * (target > g_lift ? 0.05f : 0.35f);
    if (fabsf(target - g_lift) < 0.01f) g_lift = target;
}

static void DrawImageFaded(gp::Graphics& g, gp::Image& img, gp::RectF dest, float alpha) {
    if (alpha <= 0.004f) return;
    gp::ColorMatrix m = { { { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, alpha, 0 }, { 0, 0, 0, 0, 1 } } };
    gp::ImageAttributes attrs;
    attrs.SetColorMatrix(&m);
    g.DrawImage(&img, dest, 0, 0, (float)img.GetWidth(), (float)img.GetHeight(), gp::UnitPixel, &attrs);
}

static void DrawTouchView(gp::Graphics& g, const TouchLayout& L, const View& v, bool fresh, float clickL, float clickR);

// Draws the touch view through a layer that blurs and fades with g_lift.
static void DrawTouchViewLayered(gp::Graphics& g, const TouchLayout& L, const View& v, bool fresh, float clickL, float clickR) {
    if (g_lift <= 0) {
        DrawTouchView(g, L, v, fresh, clickL, clickR);
        return;
    }
    gp::RectF area = L.area;
    int w = max(1, (int)area.Width), h = max(1, (int)area.Height);
    gp::Bitmap layer(w, h, PixelFormat32bppPARGB);
    {
        gp::Graphics lg(&layer);
        lg.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        lg.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        lg.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        lg.TranslateTransform(-area.X, -area.Y);
        DrawTouchView(lg, L, v, fresh, clickL, clickR);
    }
    gp::Bitmap blurred(max(1, w / 10), max(1, h / 10), PixelFormat32bppPARGB);   // blur: shrink, then stretch
    {
        gp::Graphics sg(&blurred);
        sg.SetInterpolationMode(gp::InterpolationModeHighQualityBilinear);
        sg.DrawImage(&layer, gp::RectF(0, 0, (float)blurred.GetWidth(), (float)blurred.GetHeight()));
    }
    float opacity = 1 - (1 - kLiftedOpacity) * g_lift;
    g.SetInterpolationMode(gp::InterpolationModeHighQualityBilinear);
    DrawImageFaded(g, layer, area, opacity * (1 - g_lift));
    DrawImageFaded(g, blurred, area, opacity * g_lift);
}

// Flick accepted: the glow on the side of the mouse the swipe ended on pulses twice.
static const ULONGLONG kFlickFlashMs = 800;

// 0..1 flick pulse strength now: two bumps over kFlickFlashMs.
static float FlickPulse(const View& v) {
    ULONGLONG since = GetTickCount64() - v.flickTick;
    if (!v.flickTick || since >= kFlickFlashMs) return 0;
    float t = since / (float)kFlickFlashMs;
    return powf(fabsf(sinf(6.2831853f * t)), 1.5f) * (1 - 0.3f * t);
}
// Physical button glow strengths, 0..1: quick to light, slow to fade.
static float g_clickL, g_clickR;

static void EaseClick(float* level, bool pressed) {
    *level += ((pressed ? 1.0f : 0.0f) - *level) * (pressed ? 0.45f : 0.12f);
    if (*level < 0.01f) *level = 0;
}

static float Random01() { return (float)rand() / RAND_MAX; }

static void UpdateHyperspeed(const View& v) {
    ULONGLONG now = GetTickCount64();
    float dt = g_hyperTick ? min(0.05f, (now - g_hyperTick) / 1000.0f) : 0;
    g_hyperTick = now;
    float target = min(1.0f, (float)fabs(v.glideSpeed) / kHyperFullSpeed);
    if (target > 0.02f) g_hyperDir = v.glideSpeed > 0 ? 1.0f : -1.0f;
    g_hyper += (target - g_hyper) * (target > g_hyper ? 0.25f : 0.08f);
    if (g_hyper < 0.01f) g_hyper = 0;

    for (Streak& s : g_streaks) {
        s.y += g_hyperDir * S(kStreakSpeed) * s.speed * g_hyper * dt;
    }
}

static void DrawHyperspeed(gp::Graphics& g, float w, float h, gp::Color color) {
    if (g_hyper <= 0) return;
    if (g_streaks.empty()) {
        for (int i = 0; i < kStreaks; i++) {
            g_streaks.push_back({ Random01() * w, Random01() * h, S(40 + 140 * Random01()), 0.5f + Random01(),
                                  0.25f + 0.75f * Random01(), S(1 + 1.6 * Random01()) });
        }
    }
    gp::Color tint = Mix(color, kWhite, 0.45f);
    for (Streak& s : g_streaks) {
        float length = s.length * (0.4f + 1.4f * g_hyper);
        // Wrap round once a streak has fully left the window.
        if (g_hyperDir > 0 && s.y - length > h) { s.y = -Random01() * h * 0.3f; s.x = Random01() * w; }
        if (g_hyperDir < 0 && s.y + length < 0) { s.y = h + Random01() * h * 0.3f; s.x = Random01() * w; }
        float tail = s.y - g_hyperDir * length;
        int alpha = (int)(170 * g_hyper * s.alpha);
        if (alpha < 3 || fabsf(tail - s.y) < 1) continue;
        gp::LinearGradientBrush brush(gp::PointF(s.x, tail), gp::PointF(s.x, s.y), WithAlpha(tint, 0), WithAlpha(tint, alpha));
        gp::Pen pen(&brush, s.width);
        pen.SetStartCap(gp::LineCapRound);
        pen.SetEndCap(gp::LineCapRound);
        g.DrawLine(&pen, s.x, tail, s.x, s.y);
    }
}

static float g_zoneX = Engine::kRightZoneX;     // where the zone line is drawn (touch x), animated

static void DrawTouchView(gp::Graphics& g, const TouchLayout& L, const View& v, bool fresh, float clickL, float clickR) {
    const Palette& p = Pal();
    const gp::RectF& body = L.body;
    float zx = L.X(g_zoneX);

    gp::GraphicsPath shape;
    RoundPath(shape, body, body.Width / 2);
    gp::SolidBrush bodyBrush(p.body);
    g.FillPath(&bodyBrush, &shape);

    // A pressed button lights its side of the surface, brightest at the outer edge.
    g.SetClip(&shape);
    gp::Color contrast = ContrastColor();
    if (clickL > 0.01f) {
        gp::RectF half(body.X, body.Y, zx - body.X, body.Height);
        gp::LinearGradientBrush b(gp::PointF(half.X - 1, 0), gp::PointF(half.X + half.Width + 1, 0),
                                  WithAlpha(contrast, (int)(120 * clickL)), WithAlpha(contrast, (int)(30 * clickL)));
        g.FillRectangle(&b, half);
    }
    if (clickR > 0.01f) {
        gp::RectF half(zx, body.Y, body.X + body.Width - zx, body.Height);
        gp::LinearGradientBrush b(gp::PointF(half.X - 1, 0), gp::PointF(half.X + half.Width + 1, 0),
                                  WithAlpha(contrast, (int)(30 * clickR)), WithAlpha(contrast, (int)(120 * clickR)));
        g.FillRectangle(&b, half);
    }
    g.ResetClip();
    gp::Pen edge(p.bodyEdge, S(1.5));
    g.DrawPath(&edge, &shape);

    gp::Pen zone(p.line, S(1.2));
    zone.SetDashStyle(gp::DashStyleDash);
    g.DrawLine(&zone, zx, body.Y + S(30), zx, body.Y + body.Height - S(30));
    // Button labels low in each side, in the text colour (white on dark, black on light);
    // left-handed, the small zone on the left is the right button.
    bool lefty = g_set.leftHanded;
    float labelY = body.Y + body.Height * 0.70f;
    Label(g, lefty ? L"Right" : L"Left", g_fonts.section, p.text, gp::RectF(body.X, labelY, zx - body.X, S(20)),
          gp::StringAlignmentCenter);
    Label(g, lefty ? L"Left" : L"Right", g_fonts.section, p.text, gp::RectF(zx, labelY, body.X + body.Width - zx, S(20)),
          gp::StringAlignmentCenter);

    // Touches, from their glows so they fade out smoothly: sized by the contact's axes
    // (reported in 4-unit steps), coloured by order.
    for (const Glow& gl : g_glows) {
        float fade = min(1.0f, gl.a / 0.55f);
        if (fade < 0.02f) continue;
        float rx = max(S(7), (float)(gl.minor * 2 * L.scale)), ry = max(S(7), (float)(gl.major * 2 * L.scale));
        float x = L.X(gl.x), y = L.Y(gl.y);
        FillGlow(g, x, y, max(rx, ry) * 1.8f, GlowColor(gl, (int)(100 * fade)));
        gp::SolidBrush fill(GlowColor(gl, (int)(255 * fade)));
        g.FillEllipse(&fill, x - rx, y - ry, rx * 2, ry * 2);
    }
    int down = 0;
    for (int i = 0; fresh && i < v.frame.count; i++) down += IsDown(v.frame.touch[i]);

    // Readout under the mouse.
    wchar_t line[128];
    int buttons = fresh ? v.frame.buttons : 0;
    if (lefty) buttons = ((buttons & 1) << 1) | ((buttons & 2) >> 1);
    swprintf_s(line, L"Fingers %d   \x00B7   Buttons %s   \x00B7   Motion %d, %d", down,
               buttons == 0 ? L"\x2014" : buttons == 1 ? L"L" : buttons == 2 ? L"R" : L"L R",
               fresh ? v.frame.dx : 0, fresh ? v.frame.dy : 0);
    float ty = body.Y + body.Height + S(28);
    Label(g, line, g_fonts.caption, p.subtext, gp::RectF(L.area.X, ty, L.area.Width, S(20)), gp::StringAlignmentCenter);
    ULONGLONG now = GetTickCount64();
    const wchar_t* gesture = nullptr;
    if (now - v.gestureTick < kGestureShowMs) gesture = v.gesture;
    else if (v.gliding) gesture = L"Gliding";
    else if (v.pinching) gesture = L"Pinch to zoom";
    else if (v.scrolling) gesture = L"Scrolling";
    else if (!g_connected) gesture = L"Waiting for the driver";
    else if (!fresh) gesture = L"Touch the mouse";
    if (gesture) Label(g, gesture, g_fonts.big, p.text, gp::RectF(L.area.X - S(40), ty + S(22), L.area.Width + S(80), S(28)),
                       gp::StringAlignmentCenter);
}

// Battery bar for the header: rounded outline with a cap, filled to the level in green,
// yellow below 35 %, red below 15 %, with the percentage inside.
static std::wstring BatteryEstimate(int level);

static void DrawBattery(gp::Graphics& g, float centerX, float top) {
    const Palette& p = Pal();
    int level = g_battery;
    float w = S(84), h = S(32), cap = S(4);
    gp::RectF r(centerX - (w + cap) / 2, top, w, h);
    gp::Color outline = WithAlpha(p.subtext, 170);
    StrokeRound(g, r, S(10), outline, S(1.6));
    FillRound(g, gp::RectF(r.X + w + S(2), r.Y + h * 0.33f, cap, h * 0.34f), S(1.5), outline);
    if (level >= 0) {
        gp::RectF inner(r.X + S(3), r.Y + S(3), r.Width - S(6), r.Height - S(6));
        FillRound(g, gp::RectF(inner.X, inner.Y, max(S(8), inner.Width * level / 100.0f), inner.Height), S(7),
                  WithAlpha(BatteryColor(level), 210));
    }
    wchar_t text[16];
    if (level >= 0) swprintf_s(text, L"%d%%", level);
    else wcscpy_s(text, L"\x2014");
    Label(g, text, g_fonts.section, p.text, r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    Label(g, BatteryEstimate(level).c_str(), g_fonts.caption, p.subtext, gp::RectF(centerX - S(130), top + h + S(3), S(260), S(18)),
          gp::StringAlignmentCenter);
}

// ---- header buttons and theme menu

static const wchar_t kGlyphPin[] = L"\xE718", kGlyphPinned[] = L"\xE840", kGlyphSun[] = L"\xE706",
                     kGlyphMoon[] = L"\xE708", kGlyphChevron[] = L"\xE70D", kGlyphCheck[] = L"\xE73E";
static const wchar_t* const kThemeNames[] = { L"Automatic", L"Light", L"Dark" };

static void DrawIconButton(gp::Graphics& g, gp::RectF r, ElemKind kind, const wchar_t* glyph, const wchar_t* label, bool on) {
    const Palette& p = Pal();
    int e = AddElem(kind, r, 0);
    FillRound(g, r, S(10), Hovered(e) || on ? p.hover : p.card);
    StrokeRound(g, r, S(10), p.line, S(1));
    gp::Color c = on ? kAccent : p.text;
    if (!label) {
        Label(g, glyph, g_fonts.icon, c, r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        return;
    }
    Label(g, glyph, g_fonts.icon, c, gp::RectF(r.X + S(10), r.Y, S(20), r.Height), gp::StringAlignmentCenter);
    Label(g, label, g_fonts.caption, p.text, gp::RectF(r.X + S(34), r.Y, r.Width - S(58), r.Height));
    Label(g, kGlyphChevron, g_fonts.glyph, p.subtext, gp::RectF(r.X + r.Width - S(26), r.Y, S(16), r.Height),
             gp::StringAlignmentCenter);
}

static void DrawThemeMenu(gp::Graphics& g, gp::RectF anchor) {
    const Palette& p = Pal();
    float w = S(170), rowH = S(36);
    gp::RectF menu(anchor.X + anchor.Width - w, anchor.Y + anchor.Height + S(6), w, rowH * 3 + S(8));
    FillRound(g, gp::RectF(menu.X, menu.Y + S(3), menu.Width, menu.Height), S(12), p.shadow);
    FillRound(g, menu, S(12), WithAlpha(p.card, 255));
    StrokeRound(g, menu, S(12), p.line, S(1));
    for (int i = 0; i < 3; i++) {
        gp::RectF row(menu.X + S(4), menu.Y + S(4) + rowH * i, menu.Width - S(8), rowH);
        int e = AddElem(E_THEME_ITEM, row, 0, i);
        if (Hovered(e)) FillRound(g, row, S(8), p.hover);
        const wchar_t* glyph = i == THEME_LIGHT ? kGlyphSun : i == THEME_DARK ? kGlyphMoon : (g_dark ? kGlyphMoon : kGlyphSun);
        Label(g, glyph, g_fonts.icon, p.text, gp::RectF(row.X + S(8), row.Y, S(22), row.Height), gp::StringAlignmentCenter);
        Label(g, kThemeNames[i], g_fonts.body, p.text, gp::RectF(row.X + S(38), row.Y, row.Width - S(70), row.Height));
        if (i == g_themeMode) {
            Label(g, kGlyphCheck, g_fonts.icon, kAccent, gp::RectF(row.X + row.Width - S(30), row.Y, S(22), row.Height),
                     gp::StringAlignmentCenter);
        }
    }
}

// ---- the mouse's Bluetooth name (the window title; click to rename)

static ULONGLONG g_mouseAddress;        // Bluetooth address of the Magic Mouse, 0 until found
static std::wstring g_deviceName = L"Magic Mouse";
static gp::RectF g_titleText;           // where the name was drawn: the rename box goes there
static HWND  g_renameEdit;
static HFONT g_renameFont;
static HBRUSH g_renameBrush;

// The paired mouse's address: BthPort keeps a key per device, named by address, with its IDs.
static ULONGLONG FindMouseAddress() {
    HKEY devices;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices",
                      0, KEY_READ, &devices) != ERROR_SUCCESS) return 0;
    ULONGLONG address = 0;
    wchar_t name[64];
    for (DWORD i = 0; !address; i++) {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(devices, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(devices, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        DWORD vid = RegGetDword(k, L"VID", 0), pid = RegGetDword(k, L"PID", 0);
        if (vid == 0x05AC && (pid == 0x030D || pid == 0x0310)) address = wcstoull(name, nullptr, 16);
        RegCloseKey(k);
    }
    RegCloseKey(devices);
    return address;
}

static bool FindMouseInfo(BLUETOOTH_DEVICE_INFO* info) {
    if (!g_mouseAddress) g_mouseAddress = FindMouseAddress();
    if (!g_mouseAddress) return false;
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{ sizeof(search) };
    search.fReturnAuthenticated = search.fReturnRemembered = search.fReturnConnected = search.fReturnUnknown = TRUE;
    *info = { sizeof(*info) };
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, info);
    if (!find) return false;
    bool found = false;
    do {
        found = info->Address.ullLong == g_mouseAddress;
    } while (!found && BluetoothFindNextDevice(find, info));
    BluetoothFindDeviceClose(find);
    return found;
}

static void RefreshDeviceName() {
    BLUETOOTH_DEVICE_INFO info;
    if (FindMouseInfo(&info) && info.szName[0]) g_deviceName = info.szName;
}

static void EndRename(bool commit) {
    if (!g_renameEdit) return;
    wchar_t text[BLUETOOTH_MAX_NAME_SIZE] = L"";
    GetWindowTextW(g_renameEdit, text, ARRAYSIZE(text));
    HWND edit = g_renameEdit;
    g_renameEdit = nullptr;             // before DestroyWindow: losing focus calls back here
    DestroyWindow(edit);
    std::wstring name(text);
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (commit && !name.empty() && name != g_deviceName) {
        BLUETOOTH_DEVICE_INFO info;
        if (FindMouseInfo(&info)) {
            wcsncpy_s(info.szName, name.c_str(), _TRUNCATE);
            DWORD error = BluetoothUpdateDeviceRecord(&info);
            if (error == ERROR_SUCCESS) g_deviceName = name;
            else MessageBoxW(g_window, (L"Windows wouldn't rename the mouse (error " + std::to_wstring(error) + L").").c_str(),
                             kAppName, MB_ICONWARNING);
        }
    }
    InvalidateRect(g_window, nullptr, FALSE);
}

static LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        EndRename(wp == VK_RETURN);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;    // no beep
    if (msg == WM_KILLFOCUS) EndRename(true);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void BeginRename() {
    if (g_renameEdit) return;
    if (!g_renameFont) g_renameFont = CreateFontW(-(int)S(20), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                                  CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_renameEdit = CreateWindowExW(0, L"EDIT", g_deviceName.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   (int)g_titleText.X, (int)g_titleText.Y + (int)S(1), (int)S(300), (int)S(26),
                                   g_window, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_renameEdit, WM_SETFONT, (WPARAM)g_renameFont, FALSE);
    SendMessageW(g_renameEdit, EM_LIMITTEXT, BLUETOOTH_MAX_NAME_SIZE - 1, 0);
    SetWindowSubclass(g_renameEdit, RenameEditProc, 1, 0);
    SendMessageW(g_renameEdit, EM_SETSEL, 0, -1);
    SetFocus(g_renameEdit);
}

static const wchar_t kGlyphBluetooth[] = L"\xE702", kGlyphEdit[] = L"\xE70F";

// Searching: "Searching" with three dots filling in, one every kSearchDotMs.
static const ULONGLONG kSearchDotMs = 400;

static void DrawDeviceTitle(gp::Graphics& g, float x, float y) {
    const Palette& p = Pal();
    int state = g_mouseState;
    if (state != MOUSE_AWAKE) {             // no name to show (or edit): Sleep / Searching...
        bool asleep = state == MOUSE_ASLEEP;
        Label(g, asleep ? kGlyphMoon : kGlyphBluetooth, g_fonts.icon, asleep ? p.subtext : WithAlpha(kAccent, 160),
              gp::RectF(x, y, S(22), S(28)), gp::StringAlignmentCenter);
        wchar_t text[32];
        int dots = (int)((GetTickCount64() / kSearchDotMs) % 4);
        swprintf_s(text, L"%s%.*s", asleep ? L"Sleep" : L"Searching", asleep ? 0 : dots, L"...");
        Label(g, text, g_fonts.title, asleep ? p.subtext : p.text, gp::RectF(x + S(26), y, S(300), S(28)));
        return;
    }
    gp::RectF measured;
    g.MeasureString(g_deviceName.c_str(), -1, g_fonts.title, gp::PointF(0, 0), &measured);
    float w = min(measured.Width, S(300));
    gp::RectF all(x - S(6), y - S(2), S(30) + w + S(32), S(32));
    int e = g_renameEdit ? -1 : AddElem(E_DEVICE, all, 0);
    if (Hovered(e)) FillRound(g, all, S(8), p.hover);
    Label(g, kGlyphBluetooth, g_fonts.icon, kAccent, gp::RectF(x, y, S(22), S(28)), gp::StringAlignmentCenter);
    g_titleText = gp::RectF(x + S(26), y, w + S(4), S(28));
    if (!g_renameEdit) Label(g, g_deviceName.c_str(), g_fonts.title, p.text, g_titleText);
    if (Hovered(e)) {                   // shows it can be edited
        Label(g, kGlyphEdit, g_fonts.glyph, p.subtext, gp::RectF(all.X + all.Width - S(24), y, S(16), S(28)),
              gp::StringAlignmentCenter);
    }
}

// ---- battery estimate: the trend over recent readings, kept across restarts

struct BatterySample { ULONGLONG time; int level; };    // time: seconds (FILETIME / 10^7)
static std::vector<BatterySample> g_batteryHistory;
static const size_t kBatterySamples = 48;
static const ULONGLONG kBatterySampleEvery = 3600;      // s: at least hourly, or on a change

static ULONGLONG NowSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
}

static void LoadBatteryHistory() {
    std::vector<BatterySample> samples(kBatterySamples);
    DWORD size = StoreGetBinary(L"BatteryHistory", samples.data(), (DWORD)(samples.size() * sizeof(BatterySample)));
    if (size) {
        samples.resize(size / sizeof(BatterySample));
        g_batteryHistory = samples;
    }
}

static void LearnBatteryRate(double spanHours, double drop);

static void RecordBattery(int level) {
    if (level < 0) return;
    ULONGLONG now = NowSeconds();
    if (!g_batteryHistory.empty()) {
        const BatterySample& last = g_batteryHistory.back();
        if (level >= last.level + 10) g_batteryHistory.clear();     // new batteries
        else if (level == last.level && now - last.time < kBatterySampleEvery) return;
    }
    if (!g_batteryHistory.empty())
        LearnBatteryRate((now - g_batteryHistory.front().time) / 3600.0, g_batteryHistory.front().level - level);
    g_batteryHistory.push_back({ now, level });
    if (g_batteryHistory.size() > kBatterySamples) g_batteryHistory.erase(g_batteryHistory.begin());
    StorePutBinary(L"BatteryHistory", g_batteryHistory.data(), (DWORD)(g_batteryHistory.size() * sizeof(BatterySample)));
}

// Battery life estimate, available straight away: it starts from a typical drain rate
// (or the rate learned from the previous batteries) and moves towards the measured rate as
// readings come in. The starting rate counts like kPriorWeightHours of observation; hours
// with no drop count too (the battery is lasting better than assumed).
static const double kTypicalHoursPerPercent = 10;      // ~6 weeks on fresh AAs
static const double kPriorWeightHours = 24;
static const double kConfidentAfterHours = 24;         // "About" instead of "Roughly"
static double g_learnedHoursPerPercent;                // from the previous batteries, 0 if none

static void LoadBatteryRate() {
    g_learnedHoursPerPercent = StoreGet(L"BatteryTenthHoursPerPercent", 0) / 10.0;
}

// Remembers this battery's measured rate once there's enough of it, for the next batteries.
static void LearnBatteryRate(double spanHours, double drop) {
    if (spanHours < 48 || drop < 3) return;
    g_learnedHoursPerPercent = spanHours / drop;
    StorePut(L"BatteryTenthHoursPerPercent", (DWORD)(g_learnedHoursPerPercent * 10));
}

static std::wstring BatteryEstimate(int level) {
    if (level < 0) return L"";
    double priorHoursPerPercent = g_learnedHoursPerPercent > 0 ? g_learnedHoursPerPercent : kTypicalHoursPerPercent;
    double spanHours = 0, drop = 0;
    if (!g_batteryHistory.empty()) {
        spanHours = (NowSeconds() - g_batteryHistory.front().time) / 3600.0;
        drop = max(0, g_batteryHistory.front().level - level);
    }
    double percentPerHour = (drop + kPriorWeightHours / priorHoursPerPercent) / (spanHours + kPriorWeightHours);
    double hours = level / percentPerHour;
    const wchar_t* how = spanHours < kConfidentAfterHours ? L"Roughly" : L"About";
    wchar_t text[64];
    if (hours < 36) swprintf_s(text, L"%s %.0f hours left", how, max(1.0, hours));
    else if (hours < 24 * 14) swprintf_s(text, L"%s %.0f days left", how, hours / 24);
    else if (hours < 24 * 60) swprintf_s(text, L"%s %.0f weeks left", how, hours / 24 / 7);
    else swprintf_s(text, L"%s %.0f months left", how, hours / 24 / 30);
    return text;
}
// ---- taskbar progress: the battery level, green / yellow / red like the battery bar

static ITaskbarList3* g_taskbar;
static UINT g_taskbarButtonCreated;

static void UpdateTaskbarProgress() {
    if (!g_taskbar) return;
    int level = g_battery;
    if (level < 0) {
        g_taskbar->SetProgressState(g_window, TBPF_NOPROGRESS);
        return;
    }
    g_taskbar->SetProgressState(g_window, level < kBatteryCriticalLevel ? TBPF_ERROR
                                         : level < kBatteryLowLevel ? TBPF_PAUSED : TBPF_NORMAL);
    g_taskbar->SetProgressValue(g_window, (ULONGLONG)max(level, 1), 100);
}

// ---- icon: a white pill like the touch view, a glow below in the battery colour, and a
// soft dot in the middle while a button is pressed or a tap clicks

static HICON MakeIcon(int size, gp::Color glow, bool dot) {
    gp::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        gp::Graphics g(&bmp);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.Clear(gp::Color(0, 0, 0, 0));
        float s = (float)size;
        FillGlow(g, s * 0.5f, s * 0.5f, s * 0.5f, WithAlpha(glow, 255));       // behind the pill, all round
        gp::RectF pill(s * 0.32f, s * 0.10f, s * 0.36f, s * 0.80f);
        FillRound(g, pill, pill.Width / 2, kWhite);
        if (dot) FillGlow(g, s * 0.5f, s * 0.5f, s * 0.17f, WithAlpha(kAccent, 255));
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// ---- whole window

static void PaintWindow(HWND hwnd, HDC target) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        const Palette& p = Pal();
        g_elems.clear();

        for (int slot = 0; slot <= kSlotEnabled; slot++) {
            float goal = ToggleValue(slot) ? 1.0f : 0.0f;
            g_knob[slot] += (goal - g_knob[slot]) * 0.35f;
            if (fabsf(goal - g_knob[slot]) < 0.01f) g_knob[slot] = goal;
        }

        // Geometry: switches either side of the touch view, centred vertically.
        float w = (float)rc.right, h = (float)rc.bottom, m = S(kMargin);
        float top = S(kHeaderH), bottom = h - S(kFooterH) - S(12);
        float sideW = S(kSideW);
        float lx = m, rx = w - m - sideW;
        float cxl = lx + sideW + m, cxr = rx - m;
        TouchLayout layout = LayoutTouchView(gp::RectF(cxl, top, cxr - cxl, bottom - top));

        View v;
        AcquireSRWLockShared(&g_viewLock);
        v = g_view;
        ReleaseSRWLockShared(&g_viewLock);
        bool fresh = GetTickCount64() - v.frameTick < 1000;
        UpdateGlows(v, fresh);
        UpdateHyperspeed(v);
        int buttons = fresh ? v.frame.buttons : 0;
        EaseClick(&g_clickL, (buttons & 1) != 0);
        EaseClick(&g_clickR, (buttons & 2) != 0);
        EaseLift();
        float zoneTarget = (float)Engine::ZoneX(g_set.leftHanded);
        g_zoneX += (zoneTarget - g_zoneX) * 0.18f;          // slide the zone line to its side
        if (fabsf(zoneTarget - g_zoneX) < 1) g_zoneX = zoneTarget;
        static gp::Color hyperColor = kAccent;      // the swiping finger's colour, kept after it lifts
        if (!g_glows.empty()) hyperColor = GlowColor(g_glows.front());

        // Hyperspeed and the ambient light go under everything; the cards are slightly see-through.
        g.Clear(p.bg);
        DrawHyperspeed(g, w, h, hyperColor);
        DrawAmbient(g, layout, w, h, g_clickL, g_clickR, FlickPulse(v), v.flickDir, hyperColor);

        // Header: title and status; battery in the middle; always-on-top and theme at the right.
        DrawDeviceTitle(g, m, S(14));
        wchar_t status[128];
        int state = g_mouseState;
        const wchar_t* what = !g_connected ? L"No Magic Mouse 2009 with the driver found"
                            : state == MOUSE_ASLEEP ? L"Asleep \x2014 click the mouse to wake it"
                            : state == MOUSE_SEARCHING ? L"Looking for the mouse"
                            : g_set.enabled ? L"Connected" : L"Paused";
        swprintf_s(status, L"%s%s", what, g_elevated ? L"   \x00B7   Administrator" : L"");
        Label(g, status, g_fonts.caption, p.subtext, gp::RectF(m, S(42), S(330), S(20)));
        DrawBattery(g, w / 2, S(22));
        gp::RectF themeBtn(w - m - S(124), S(20), S(124), S(36));
        gp::RectF pinBtn(themeBtn.X - S(10) - S(36), S(20), S(36), S(36));
        DrawIconButton(g, pinBtn, E_PIN, g_onTop ? kGlyphPinned : kGlyphPin, nullptr, g_onTop);
        gp::RectF master(pinBtn.X - S(14) - S(132), S(20), S(132), S(36));     // all gestures on/off
        if (Hovered(AddElem(E_TOGGLE, master, kSlotEnabled))) FillRound(g, master, S(10), p.hover);
        Label(g, kEnabledSetting.label, g_fonts.body, p.text, gp::RectF(master.X + S(12), master.Y, S(72), master.Height));
        DrawSwitch(g, gp::RectF(master.X + master.Width - S(10) - S(kSwitchW), master.Y + (master.Height - S(kSwitchH)) / 2,
                                S(kSwitchW), S(kSwitchH)), g_knob[kSlotEnabled]);
        const wchar_t* themeGlyph = g_themeMode == THEME_LIGHT ? kGlyphSun : g_themeMode == THEME_DARK ? kGlyphMoon
                                    : (g_dark ? kGlyphMoon : kGlyphSun);
        DrawIconButton(g, themeBtn, E_THEME, themeGlyph, g_themeMode == THEME_AUTO ? L"Auto" : kThemeNames[g_themeMode], g_themeOpen);

        static const wchar_t* const speeds[] = { L"Slow", L"Normal", L"Fast" };
        static const wchar_t* const glides[] = { L"Short", L"Normal", L"Long" };
        float columnH = S(24) + S(kRowH) * kGestureCount + S(kSliderRowH) + S(20) + S(24) + S(34);   // the taller (left) column
        float y0 = top + max(0.0f, (bottom - top - columnH) / 2);

        float y = DrawSectionLabel(g, lx, y0, sideW, L"GESTURES");
        y = DrawToggleCard(g, lx, y, sideW, kGestureSettings, kGestureCount, 0, 1);   // slider under two-finger tap
        y = DrawSectionLabel(g, lx, y + S(20), sideW, L"SCROLL SPEED");
        DrawSegmented(g, gp::RectF(lx, y, sideW, S(34)), SEG_SPEED, speeds, g_set.speed);

        y = DrawSectionLabel(g, rx, y0, sideW, L"SCROLLING");
        y = DrawToggleCard(g, rx, y, sideW, kScrollSettings, kScrollCount, kGestureCount);
        y = DrawSectionLabel(g, rx, y + S(20), sideW, L"GLIDE LENGTH");
        DrawSegmented(g, gp::RectF(rx, y, sideW, S(34)), SEG_GLIDE, glides, g_set.glide);

        DrawTouchViewLayered(g, layout, v, fresh, g_clickL, g_clickR);

        // Footer card: start with Windows, then the app buttons on the right.
        gp::RectF footer(m, h - S(kFooterH), w - 2 * m, S(kFooterH) - S(16));
        FillRound(g, footer, S(12), p.card);
        DrawToggleRow(g, gp::RectF(footer.X, footer.Y, S(300), footer.Height), kSlotStartup, L"Start with Windows",
                      g_elevated ? L"As administrator, no prompt" : L"Signs in with you");
        float bx = footer.X + footer.Width - S(12), by = footer.Y + (footer.Height - S(32)) / 2;
        if (!g_elevated) {
            bx -= S(176);
            DrawButton(g, gp::RectF(bx, by, S(176), S(32)), BTN_ELEVATE, L"Run as administrator", true);
            bx -= S(10);
        }
        bx -= S(170);
        DrawButton(g, gp::RectF(bx, by, S(170), S(32)), BTN_REENABLE, L"Re-enable multi-touch", false);
        bx -= S(10) + S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_STATUS, L"Driver status", false);

        if (g_themeOpen) DrawThemeMenu(g, themeBtn);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---- behaviour

static bool SystemUsesDarkApps() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static void ApplyTheme() {
    g_dark = g_themeMode == THEME_DARK || (g_themeMode == THEME_AUTO && SystemUsesDarkApps());
    BOOL dark = g_dark;
    if (FAILED(DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark))))      // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(g_window, 19, &dark, sizeof(dark));              // its number before 20H1
    SetWindowPos(g_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(g_window, nullptr, FALSE);
}

static void ApplyOnTop() {
    SetWindowPos(g_window, g_onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static HANDLE g_single;     // single-instance mutex

// Starts an elevated copy (UAC prompt) and exits this one.
static void RestartElevated(HWND hwnd) {
    CloseHandle(g_single);  // let the new copy take the single-instance mutex
    g_single = nullptr;
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.hwnd = hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        DestroyWindow(g_tray);  // quits the message loop
    } else {
        g_single = CreateMutexW(nullptr, TRUE, L"Local\\MagicMouseNative.Tray");  // cancelled: keep running
    }
}

static void Activate(HWND hwnd, const Elem& e) {
    switch (e.kind) {
    case E_TOGGLE:
        if (e.id == kSlotStartup) {
            if (!SetStartsWithWindows(!g_startup)) {
                MessageBoxW(hwnd, g_startup ? L"The elevated logon task can only be removed while running as administrator."
                                            : L"Couldn't create the logon task.", kAppName, MB_ICONWARNING);
            }
            g_startup = StartsWithWindows();
            return;
        }
        {
            std::atomic<bool>* value = e.id == kSlotEnabled ? &g_set.enabled : e.id < kGestureCount ? kGestureSettings[e.id].value
                                                            : kScrollSettings[e.id - kGestureCount].value;
            *value = !*value;
            if (value == &g_set.leftHanded) SendDriverOptions();
        }
        break;
    case E_CHECK: {
        const BoolSetting& s = e.id < kGestureCount ? kGestureSettings[e.id] : kScrollSettings[e.id - kGestureCount];
        if (s.invert) *s.invert = !*s.invert;
        break;
    }
    case E_SEGMENT:
        (e.id == SEG_SPEED ? g_set.speed : g_set.glide) = e.sub;
        break;
    case E_BUTTON:
        if (e.id == BTN_STATUS) ShowStatus(hwnd);
        else if (e.id == BTN_ELEVATE) RestartElevated(hwnd);
        else if (!DriverCall(IOCTL_MM_ENABLE_TOUCH, nullptr, 0))
            MessageBoxW(hwnd, L"The driver couldn't resend D7 01 (is the mouse connected?).", kAppName, MB_ICONWARNING);
        return;
    case E_PIN:
        g_onTop = !g_onTop;
        ApplyOnTop();
        break;
    case E_THEME:
        g_themeOpen = !g_themeOpen;
        return;
    case E_DEVICE:
        BeginRename();
        return;
    case E_THEME_ITEM:
        g_themeMode = e.sub;
        g_themeOpen = false;
        ApplyTheme();
        break;
    }
    SaveSettings();
    PostMessageW(g_tray, WM_CONNECTED, g_connected, 0);   // refresh the tray tooltip
}

static int HitTest(LPARAM lp) {
    gp::PointF pt((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
    for (int i = (int)g_elems.size(); i-- > 0;) {       // last drawn is on top
        if (g_elems[i].rc.Contains(pt)) return i;
    }
    return -1;
}

// Whether the next frame would differ from the last one: touches arriving, glows fading,
// switches sliding, a gesture label showing, or the low-battery ring pulsing.
static bool Animating() {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_viewLock);
    bool live = now - g_view.frameTick < 1200 || now - g_view.gestureTick < kGestureShowMs + 100 || g_view.gliding;
    ReleaseSRWLockShared(&g_viewLock);
    if (live || !g_glows.empty() || g_hyper > 0 || g_clickL > 0 || g_clickR > 0) return true;
    if (g_mouseState == MOUSE_SEARCHING) return true;     // the dots
    if (g_lift != (g_lifted ? 1.0f : 0.0f) || g_zoneX != (float)Engine::ZoneX(g_set.leftHanded)) return true;
    AcquireSRWLockShared(&g_viewLock);
    bool flash = g_view.flickTick && now - g_view.flickTick < kFlickFlashMs;
    ReleaseSRWLockShared(&g_viewLock);
    if (flash) return true;
    for (int slot = 0; slot <= kSlotEnabled; slot++) {
        if (g_knob[slot] != (ToggleValue(slot) ? 1.0f : 0.0f)) return true;
    }
    int level = g_battery;
    return level >= 0 && level < kBatteryLowLevel;
}

static void ShowWindowFront() {
    g_startup = StartsWithWindows();
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    SetTimer(g_window, 1, 16, nullptr);     // repaint ~60 fps while shown (touches, animations)
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintWindow(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (Animating()) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        if (g_dragSlider) {
            SetTwoTapFromX((float)GET_X_LPARAM(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        int hit = HitTest(lp);
        if (hit != g_hover) {
            g_hover = hit;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            bool text = g_hover >= 0 && g_hover < (int)g_elems.size() && g_elems[g_hover].kind == E_DEVICE;
            SetCursor(LoadCursorW(nullptr, g_hover < 0 ? IDC_ARROW : text ? IDC_IBEAM : IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_LBUTTONDOWN:
        if (g_renameEdit) SetFocus(hwnd);
        g_pressed = HitTest(lp);
        SetCapture(hwnd);
        if (g_pressed >= 0 && g_elems[g_pressed].kind == E_SLIDER && g_set.twoFingerTap && !g_themeOpen) {
            g_dragSlider = true;            // jump to the press, then follow the drag
            SetTwoTapFromX((float)GET_X_LPARAM(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (g_dragSlider) {
            g_dragSlider = false;
            g_pressed = -1;
            SaveSettings();
            return 0;
        }
        int hit = HitTest(lp);
        bool same = hit >= 0 && hit == g_pressed;
        g_pressed = -1;
        // An open theme menu takes the next click: an item, its button, or anywhere to close.
        if (g_themeOpen && (!same || (g_elems[hit].kind != E_THEME_ITEM && g_elems[hit].kind != E_THEME))) {
            g_themeOpen = false;
            return 0;
        }
        if (same) Activate(hwnd, g_elems[hit]);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_themeOpen) g_themeOpen = false;
            else PostMessageW(hwnd, WM_CLOSE, 0, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT: {                // the rename box in the window's colours
        const Palette& p = Pal();
        SetTextColor((HDC)wp, RGB(p.text.GetR(), p.text.GetG(), p.text.GetB()));
        SetBkColor((HDC)wp, RGB(p.hover.GetR(), p.hover.GetG(), p.hover.GetB()));
        if (g_renameBrush) DeleteObject(g_renameBrush);
        g_renameBrush = CreateSolidBrush(RGB(p.hover.GetR(), p.hover.GetG(), p.hover.GetB()));
        return (LRESULT)g_renameBrush;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") && g_themeMode == THEME_AUTO) ApplyTheme();
        return 0;
    case WM_CAPTURECHANGED:
        if (g_dragSlider && (HWND)lp != hwnd) {     // capture taken away mid-drag
            g_dragSlider = false;
            SaveSettings();
        }
        return 0;
    case WM_CLOSE:          // hide; the app keeps running in the tray
        KillTimer(hwnd, 1);
        g_themeOpen = false;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        if (msg == g_taskbarButtonCreated) {
            if (!g_taskbar && SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar))))
                g_taskbar->HrInit();
            UpdateTaskbarProgress();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

enum TrayMenuId { ID_MENU_OPEN = 400, ID_MENU_ENABLED, ID_MENU_EXIT };

// ---------------------------------------------------------------------------------------
// Tray icon

// Icons: tray (with the press dot) and window, regenerated when the battery colour, the
// connection or the dot changes.
static bool  g_trayDot;                 // a tap just clicked
static bool  g_buttonHeld;              // a physical button is down
static HICON g_trayIcon, g_bigIcon, g_smallIcon;
static int   g_iconTier = -99;
static bool  g_iconDot;

static void RefreshIcons() {
    int level = g_battery;
    int tier = !g_connected ? -2 : level < 0 ? -1 : level < kBatteryCriticalLevel ? 0 : level < kBatteryLowLevel ? 1 : 2;
    bool dot = g_trayDot || g_buttonHeld;
    if (tier == g_iconTier && dot == g_iconDot && g_trayIcon) return;
    gp::Color glow = !g_connected ? gp::Color(255, 142, 142, 147) : level >= 0 ? BatteryColor(level) : kBatteryGreen;
    if (tier != g_iconTier || !g_bigIcon) {
        if (g_bigIcon) DestroyIcon(g_bigIcon);
        if (g_smallIcon) DestroyIcon(g_smallIcon);
        g_bigIcon = MakeIcon(GetSystemMetrics(SM_CXICON), glow, false);
        g_smallIcon = MakeIcon(GetSystemMetrics(SM_CXSMICON), glow, false);
        SendMessageW(g_window, WM_SETICON, ICON_BIG, (LPARAM)g_bigIcon);
        SendMessageW(g_window, WM_SETICON, ICON_SMALL, (LPARAM)g_smallIcon);
    }
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = MakeIcon(GetSystemMetrics(SM_CXSMICON), glow, dot);
    g_iconTier = tier;
    g_iconDot = dot;
}

static void UpdateTray(HWND hwnd, DWORD action) {
    RefreshIcons();
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_trayIcon;
    wchar_t battery[32] = L"";
    if (g_battery >= 0) swprintf_s(battery, L", battery %d%%", g_battery.load());
    int state = g_mouseState;
    swprintf_s(nid.szTip, L"%s - %s%s", g_deviceName.c_str(),
               state == MOUSE_SEARCHING ? L"searching" : state == MOUSE_ASLEEP ? L"asleep" : g_set.enabled ? L"on" : L"paused",
               battery);
    Shell_NotifyIconW(action, &nid);
}

static void OnBatteryChanged(HWND tray) {
    static bool warned;
    int level = g_battery;
    RecordBattery(level);
    UpdateTaskbarProgress();
    InvalidateRect(g_window, nullptr, FALSE);
    UpdateTray(tray, NIM_MODIFY);

    if (level >= 0 && level <= kBatteryLow && !warned) {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = tray;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_WARNING;
        wcscpy_s(nid.szInfoTitle, L"Magic Mouse battery low");
        swprintf_s(nid.szInfo, L"%d%% left - replace or recharge the batteries soon.", level);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    warned = level >= 0 && level <= kBatteryLow;
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_MENU_OPEN, L"Open MagicMouseNative");
    AppendMenuW(m, MF_STRING | (g_set.enabled ? MF_CHECKED : 0), ID_MENU_ENABLED, L"Enabled");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_MENU_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);      // so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) ShowWindowFront();
        return 0;
    case WM_CONNECTED:
        if (wp && !g_connected) RefreshDeviceName();
        g_connected = wp != 0;
        UpdateTray(hwnd, NIM_MODIFY);
        return 0;
    case WM_RESTORECURSOR:                 // a pinch ended
        SetCursorPos((int)wp, (int)lp);
        return 0;
    case WM_MOUSESTATE:                    // asleep / awake / searching
        if (g_mouseState == MOUSE_AWAKE) RefreshDeviceName();
        if (g_mouseState != MOUSE_AWAKE) EndRename(false);
        InvalidateRect(g_window, nullptr, FALSE);
        UpdateTray(hwnd, NIM_MODIFY);
        return 0;
    case WM_MIDDLEBUTTON:                  // two-finger click, decided in the mouse hook
        SendMouse(wp ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
        if (wp) NoteGesture(L"Two-finger click: middle button");
        return 0;
    case WM_TRAYDOT:                       // the tray icon's dot: a button held, or a tap
        if (wp == 2) {
            g_trayDot = true;
            SetTimer(hwnd, 1, 250, nullptr);
        } else {
            g_buttonHeld = wp != 0;
        }
        UpdateTray(hwnd, NIM_MODIFY);
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, 1);
        g_trayDot = false;
        UpdateTray(hwnd, NIM_MODIFY);
        return 0;
    case WM_BATTERY:
        OnBatteryChanged(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MENU_OPEN:    ShowWindowFront(); break;
        case ID_MENU_ENABLED:
            g_set.enabled = !g_set.enabled;
            SaveSettings();
            InvalidateRect(g_window, nullptr, FALSE);
            UpdateTray(hwnd, NIM_MODIFY);
            break;
        case ID_MENU_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        UpdateTray(hwnd, NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated) { UpdateTray(hwnd, NIM_ADD); return 0; }  // Explorer restarted
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    // Already running (an elevated copy's mutex is "access denied" to us): show its window.
    g_single = CreateMutexW(nullptr, TRUE, L"Local\\MagicMouseNative.Tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED) {
        HWND tray = FindWindowW(L"MagicMouseNativeTray", nullptr);
        if (tray) PostMessageW(tray, WM_COMMAND, ID_MENU_OPEN, 0);
        return 0;
    }
    g_elevated = ProcessElevated(GetCurrentProcess());

    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    g_pinchDpi = g_dpi;
    ReleaseDC(nullptr, screen);
    gp::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken;
    gp::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    CreateFonts();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);        // taskbar progress
    FindPortableIni();
    LoadSettings();
    LoadBatteryHistory();
    LoadBatteryRate();
    RefreshDeviceName();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_taskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = TrayProc;
    wc.lpszClassName = L"MagicMouseNativeTray";
    RegisterClassW(&wc);
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"MagicMouseNativeWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT size = { 0, 0, (LONG)S(kClientW), (LONG)S(kClientH) };
    AdjustWindowRect(&size, style, FALSE);
    g_window = CreateWindowW(L"MagicMouseNativeWindow", kAppName, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             size.right - size.left, size.bottom - size.top, nullptr, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(g_window, g_taskbarButtonCreated, MSGFLT_ALLOW, nullptr);  // when elevated
    ApplyTheme();
    ApplyOnTop();

    g_tray = CreateWindowW(L"MagicMouseNativeTray", kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(g_tray, WM_COMMAND, MSGFLT_ALLOW, nullptr);   // "open" from a non-elevated copy
    UpdateTray(g_tray, NIM_ADD);            // also sets the window's icon
    if (!wcsstr(cmdLine, L"/tray")) ShowWindowFront();     // "Start with Windows" passes /tray

    DWORD hookThreadId = 0;
    HANDLE hook = CreateThread(nullptr, 0, HookThread, nullptr, 0, &hookThreadId);
    SetThreadPriority(hook, THREAD_PRIORITY_TIME_CRITICAL);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE engine = CreateThread(nullptr, 0, EngineThread, nullptr, 0, nullptr);
    SetThreadPriority(engine, THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE battery = CreateThread(nullptr, 0, StatusThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hook, 2000);
    CloseHandle(hook);
    if (g_middleFor) SendMouse(MOUSEEVENTF_MIDDLEUP);
    SetEvent(g_stop);
    HANDLE threads[2] = { engine, battery };
    WaitForMultipleObjects(2, threads, TRUE, 5000);
    CloseHandle(engine);
    CloseHandle(battery);
    CloseHandle(g_stop);
    if (g_single) CloseHandle(g_single);
    if (g_taskbar) g_taskbar->Release();
    DeleteFonts();
    gp::GdiplusShutdown(gdiplusToken);
    return 0;
}
