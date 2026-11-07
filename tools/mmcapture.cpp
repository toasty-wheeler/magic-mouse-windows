// mmcapture.cpp - logs decoded Magic Mouse 2009 touch frames from mmfilter.sys.
//
// Reads \\.\MagicMouseNative for N seconds (default 20) and writes one line per frame to
// mmcapture_log.txt next to the exe: time, mouse motion, buttons, and every touch's id,
// state, position and contact size. Close the tray app first: both would share the frames.
//
// Build: cl /nologo /EHsc /O2 /W4 mmcapture.cpp

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include "..\shared\mmioctl.h"

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 20;
    if (seconds <= 0) seconds = 20;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath(exe);
    logPath = logPath.substr(0, logPath.find_last_of(L"\\/") + 1) + L"mmcapture_log.txt";
    FILE* log = nullptr;
    if (_wfopen_s(&log, logPath.c_str(), L"w") || !log) { wprintf(L"can't write %ls\n", logPath.c_str()); return 1; }

    HANDLE h = CreateFileW(MM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { wprintf(L"can't open %ls: error %lu\n", MM_DEVICE_PATH, GetLastError()); return 1; }

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    ULONGLONG deadline = GetTickCount64() + seconds * 1000ULL;
    std::vector<MM_FRAME> frames(64);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    long total = 0;

    wprintf(L"Capturing for %d s - swipe, flick and tap now...\n", seconds);
    fprintf(log, "# t_ms  motion(dx,dy) buttons | touches: id state x y major minor\n");
    while (GetTickCount64() < deadline) {
        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_MM_READ_FRAMES, nullptr, 0, frames.data(),
                             (DWORD)(frames.size() * sizeof(MM_FRAME)), &got, &ov) && GetLastError() != ERROR_IO_PENDING) {
            wprintf(L"READ_FRAMES failed: %lu\n", GetLastError());
            break;
        }
        DWORD left = (DWORD)(deadline - min(deadline, GetTickCount64()));
        if (WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &got, TRUE);
            break;
        }
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) break;

        for (DWORD i = 0; i < got / sizeof(MM_FRAME); i++) {
            const MM_FRAME& f = frames[i];
            const UCHAR* r = f.Report;
            if (r[0] != MM_REPORT_TOUCH) {          // another report (e.g. 0x61): log it raw
                fprintf(log, "%9.1f  REPORT 0x%02X len %u:", (f.Qpc - start.QuadPart) * 1000.0 / freq.QuadPart, r[0], f.Length);
                for (int b = 0; b < f.Length; b++) fprintf(log, " %02X", r[b]);
                fprintf(log, "\n");
                total++;
                continue;
            }
            int dx = (int32_t)(((uint32_t)(r[3] & 0x0C) << 28) | ((uint32_t)r[1] << 22)) >> 22;
            int dy = (int32_t)(((uint32_t)(r[3] & 0x30) << 26) | ((uint32_t)r[2] << 22)) >> 22;
            fprintf(log, "%9.1f  mv(%4d,%4d) b%d |", (f.Qpc - start.QuadPart) * 1000.0 / freq.QuadPart, dx, dy, r[3] & 3);
            int n = f.Length >= 6 ? (f.Length - 6) / 8 : 0;
            for (int t = 0; t < n; t++) {
                const UCHAR* d = r + 6 + t * 8;
                int id = ((d[6] << 2) | (d[5] >> 6)) & 0xF;
                int x = (int32_t)((uint32_t)d[1] << 28 | (uint32_t)d[0] << 20) >> 20;
                int y = -((int32_t)((uint32_t)d[2] << 24 | (uint32_t)d[1] << 16) >> 20);
                fprintf(log, " [%d %02X %5d %5d %3d %3d]", id, d[7] & 0xF0, x, y, d[3], d[4]);
            }
            fprintf(log, "\n");
            total++;
        }
    }
    fclose(log);
    CloseHandle(h);
    wprintf(L"%ld frames -> %ls\n", total, logPath.c_str());
    return 0;
}
