// mmprobe.cpp - Stage 1 feasibility probe for Apple Magic Mouse 2009 (A1296) on Windows.
//
// Answers one question: can a plain user-mode app receive the mouse's multi-touch
// reports (report ID 0x29), or does Windows drop them so we need a small HID lower
// filter driver to fix up the report descriptor?
//
// What it does:
//   1. Lists every HID top-level collection Windows exposes for VID 05AC / PID 030D|0310.
//   2. Prints each collection's usage page, report sizes and declared report IDs.
//   3. Tries to enable multi-touch mode (feature report D7 01, as the Linux driver does).
//   4. Listens on every readable collection for 15 seconds and logs each report.
//   5. Prints a verdict.
//
// Build (x64 Native Tools / Developer Command Prompt for VS):
//   cl /nologo /EHsc /O2 /W4 mmprobe.cpp
// Run with the mouse connected, then slide a finger on it while it listens.
// Output is also written to mmprobe_log.txt next to the exe.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static const USHORT kAppleVid = 0x05AC;
static const USHORT kMm1Pids[] = { 0x030D, 0x0310 };
static const BYTE   kTouchReportId = 0x29;          // MM1 multi-touch input report
static const BYTE   kEnableTouch[] = { 0xD7, 0x01 }; // MM1 "multi-touch on" feature report

static FILE* g_log = nullptr;
static std::mutex g_logLock;
static std::atomic<long> g_touchReports{0};
static std::atomic<long> g_otherReports{0};

static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logLock);
    va_list a;
    va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_log) { va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fflush(g_log); }
}

static std::string ErrText(DWORD e) {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof(buf), nullptr);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return std::to_string(e) + " (" + s + ")";
}

static bool IsMm1(const HIDD_ATTRIBUTES& at) {
    if (at.VendorID != kAppleVid) return false;
    for (USHORT p : kMm1Pids) if (at.ProductID == p) return true;
    return false;
}

struct Collection {
    std::wstring path;
    HANDLE readHandle = INVALID_HANDLE_VALUE;
    USHORT inputLen = 0;
};

// Decode one MM1 touch record (8 bytes), same bit layout as Linux hid-magicmouse.
static void DecodeTouch(const BYTE* t, int idx) {
    int id    = ((t[6] << 2) | (t[5] >> 6)) & 0xF;
    int x     = (int32_t)((uint32_t)t[1] << 28 | (uint32_t)t[0] << 20) >> 20;
    int y     = -((int32_t)((uint32_t)t[2] << 24 | (uint32_t)t[1] << 16) >> 20);
    int major = t[3], minor = t[4];
    int state = t[7] & 0xF0;
    const char* st = state == 0x30 ? "start" : state == 0x40 ? "drag" : state == 0x00 ? "none" : "other";
    Log("      touch[%d] id=%d x=%5d y=%5d major=%3d minor=%3d state=%s\n", idx, id, x, y, major, minor, st);
}

static DWORD WINAPI Reader(LPVOID p) {
    Collection* c = (Collection*)p;
    std::vector<BYTE> buf(c->inputLen ? c->inputLen : 64);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(c->readHandle, buf.data(), (DWORD)buf.size(), &got, nullptr)) return 0;
        if (got == 0) continue;
        BYTE id = buf[0];
        if (id == kTouchReportId) {
            long n = ++g_touchReports;
            if (n <= 40) { // keep the log readable
                Log("  [TOUCH 0x29] %lu bytes:", got);
                for (DWORD i = 0; i < got; i++) Log(" %02X", buf[i]);
                Log("\n");
                if (got >= 6 && (got - 6) % 8 == 0)
                    for (DWORD i = 0; i < (got - 6) / 8; i++) DecodeTouch(&buf[6 + i * 8], (int)i);
            }
        } else {
            long n = ++g_otherReports;
            if (n <= 20) {
                Log("  [report 0x%02X] %lu bytes:", id, got);
                for (DWORD i = 0; i < got && i < 32; i++) Log(" %02X", buf[i]);
                Log("\n");
            }
        }
    }
}

static void ListReportIds(PHIDP_PREPARSED_DATA pp, HIDP_REPORT_TYPE type, USHORT nButtons, USHORT nValues, const char* label) {
    std::set<int> ids;
    if (nButtons) {
        std::vector<HIDP_BUTTON_CAPS> bc(nButtons);
        if (HidP_GetButtonCaps(type, bc.data(), &nButtons, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < nButtons; i++) ids.insert(bc[i].ReportID);
    }
    if (nValues) {
        std::vector<HIDP_VALUE_CAPS> vc(nValues);
        if (HidP_GetValueCaps(type, vc.data(), &nValues, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < nValues; i++) ids.insert(vc[i].ReportID);
    }
    Log("    %s report IDs:", label);
    if (ids.empty()) Log(" (none)");
    for (int id : ids) Log(" 0x%02X", id);
    Log("\n");
}

static void TryEnableTouch(HANDLE h, USHORT featureLen) {
    std::vector<USHORT> sizes = { (USHORT)sizeof(kEnableTouch) };
    if (featureLen > sizeof(kEnableTouch)) sizes.push_back(featureLen);
    for (USHORT sz : sizes) {
        std::vector<BYTE> b(sz, 0);
        memcpy(b.data(), kEnableTouch, sizeof(kEnableTouch));
        if (HidD_SetFeature(h, b.data(), sz)) { Log("    SetFeature D7 01 (len %u): OK\n", sz); return; }
        Log("    SetFeature D7 01 (len %u): failed, error %s\n", sz, ErrText(GetLastError()).c_str());
    }
    std::vector<BYTE> b(sizeof(kEnableTouch));
    memcpy(b.data(), kEnableTouch, sizeof(kEnableTouch));
    if (HidD_SetOutputReport(h, b.data(), (ULONG)b.size())) Log("    SetOutputReport D7 01: OK\n");
    else Log("    SetOutputReport D7 01: failed, error %s\n", ErrText(GetLastError()).c_str());
}

int main() {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string logPath(exe); logPath = logPath.substr(0, logPath.find_last_of("\\/") + 1) + "mmprobe_log.txt";
    fopen_s(&g_log, logPath.c_str(), "w");

    Log("Magic Mouse 2009 probe\n======================\n");
    GUID hidGuid; HidD_GetHidGuid(&hidGuid);
    HDEVINFO devs = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) { Log("SetupDiGetClassDevs failed: %s\n", ErrText(GetLastError()).c_str()); return 1; }

    std::vector<Collection*> readable;
    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> raw(need);
        auto det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)raw.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &ifd, det, need, nullptr, nullptr)) continue;

        // Zero-access handle: allowed even for collections Windows owns exclusively (mouse).
        HANDLE q = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{ sizeof(at) };
        if (!HidD_GetAttributes(q, &at) || !IsMm1(at)) { CloseHandle(q); continue; }

        found++;
        Log("\nCollection %d\n  path: %ls\n  VID %04X PID %04X ver %04X\n", found, det->DevicePath, at.VendorID, at.ProductID, at.VersionNumber);

        PHIDP_PREPARSED_DATA pp = nullptr; HIDP_CAPS caps{};
        if (HidD_GetPreparsedData(q, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            Log("  usage page 0x%04X usage 0x%04X  (input %u, output %u, feature %u bytes)\n",
                caps.UsagePage, caps.Usage, caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength);
            ListReportIds(pp, HidP_Input,   caps.NumberInputButtonCaps,   caps.NumberInputValueCaps,   "input  ");
            ListReportIds(pp, HidP_Output,  caps.NumberOutputButtonCaps,  caps.NumberOutputValueCaps,  "output ");
            ListReportIds(pp, HidP_Feature, caps.NumberFeatureButtonCaps, caps.NumberFeatureValueCaps, "feature");
        }

        // Try enabling multi-touch on the zero-access handle first, then a read/write one.
        TryEnableTouch(q, caps.FeatureReportByteLength);
        HANDLE rw = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (rw == INVALID_HANDLE_VALUE)
            rw = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (rw == INVALID_HANDLE_VALUE) {
            Log("  open for reading: denied, error %s (normal for the mouse collection Windows owns)\n", ErrText(GetLastError()).c_str());
        } else {
            Log("  open for reading: OK, will listen\n");
            TryEnableTouch(rw, caps.FeatureReportByteLength);
            auto c = new Collection{ det->DevicePath, rw, caps.InputReportByteLength };
            readable.push_back(c);
        }
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(devs);

    if (!found) {
        Log("\nNo Magic Mouse 2009 (05AC:030D / 05AC:0310) found. Is it paired and connected?\n");
        Log("Note: if another Magic Mouse driver is installed it may hide or reshape these collections.\n");
        return 2;
    }

    if (!readable.empty()) {
        Log("\nListening for 15 seconds - slide one or two fingers on the mouse now...\n");
        for (auto c : readable) CreateThread(nullptr, 0, Reader, c, 0, nullptr);
        Sleep(15000);
        for (auto c : readable) CancelIoEx(c->readHandle, nullptr);
    }

    Log("\n==== Verdict ====\n");
    Log("Collections found: %d, readable: %zu, touch reports (0x29): %ld, other reports: %ld\n",
        found, readable.size(), g_touchReports.load(), g_otherReports.load());
    if (g_touchReports > 0)
        Log("User mode CAN see touch data. Stage 1 can be a pure user-mode app, no driver needed.\n");
    else
        Log("User mode CANNOT see touch data (Windows drops report 0x29 because the mouse's\n"
            "descriptor doesn't declare it). Stage 1 needs a small KMDF lower filter that adds\n"
            "a vendor collection for 0x29; the scrolling logic still runs in the user-mode app.\n");
    Log("\nLog saved to %s\nPress Enter to exit.\n", logPath.c_str());
    getchar();
    return 0;
}
