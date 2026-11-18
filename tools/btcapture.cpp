// btcapture.cpp - checks Boot Camp mode's touch source on a real mouse: reads Windows'
// Bluetooth packet log (ETW, BTHPORT event 402) for a while and decodes touch frames with
// app\hciraw.h, as the app does. Works with mmfilter or Apple's driver on the mouse (either
// one puts it in multi-touch mode). Run as administrator, touching the mouse:
//   btcapture [seconds]        (default 15; the output is also saved to btcapture_log.txt)
// Build: cl /nologo /EHsc /W4 /std:c++17 btcapture.cpp

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "..\app\hciraw.h"

#pragma comment(lib, "advapi32.lib")

// Everything printed also goes to btcapture_log.txt next to the exe.
static FILE* g_log;
static void Out(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    if (g_log) {
        va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fflush(g_log);
    }
}
#define printf Out

static const wchar_t kSession[] = L"MagicMouseNative btcapture";
static const GUID kBthPort = { 0x8a1f9517, 0x3a8c, 0x4a9e, { 0xa0, 0x18, 0x4f, 0x17, 0xa2, 0x00, 0xf2, 0x77 } };

static FILE* g_frameLog;            // btcapture_frames_log.txt: mmcapture's format, for mmreplay
static LONGLONG g_startQpc;
static double g_qpcMs;

static void LogFrame(const uint8_t* r, uint32_t len, LONGLONG qpc) {
    if (!g_frameLog) return;
    double ms = (qpc - g_startQpc) * g_qpcMs;
    if (r[0] != kMm1Touch) {
        fprintf(g_frameLog, "%9.1f  REPORT 0x%02X len %u:", ms, r[0], len);
        for (uint32_t b = 0; b < len; b++) fprintf(g_frameLog, " %02X", r[b]);
        fprintf(g_frameLog, "\n");
        return;
    }
    int dx = (int32_t)(((uint32_t)(r[3] & 0x0C) << 28) | ((uint32_t)r[1] << 22)) >> 22;
    int dy = (int32_t)(((uint32_t)(r[3] & 0x30) << 26) | ((uint32_t)r[2] << 22)) >> 22;
    fprintf(g_frameLog, "%9.1f  mv(%4d,%4d) b%d |", ms, dx, dy, r[3] & 3);
    for (uint32_t t = 0; t < (len - 6) / 8; t++) {
        const uint8_t* d = r + 6 + t * 8;
        int id = ((d[6] << 2) | (d[5] >> 6)) & 0xF;
        int x = (int32_t)((uint32_t)d[1] << 28 | (uint32_t)d[0] << 20) >> 20;
        int y = -((int32_t)((uint32_t)d[2] << 24 | (uint32_t)d[1] << 16) >> 20);
        fprintf(g_frameLog, " [%d %02X %5d %5d %3d %3d]", id, d[7] & 0xF0, x, y, d[3], d[4]);
    }
    fprintf(g_frameLog, "\n");
}

static HciRawDecoder g_hci;
static ULONG g_events, g_acl, g_payloads, g_frames, g_scrubbed, g_printed;
static LONGLONG g_firstQpc, g_lastQpc;

static VOID WINAPI OnEvent(PEVENT_RECORD e) {
    if (e->EventHeader.EventDescriptor.Id != 402 || !e->UserData) return;
    g_events++;
    const uint8_t* p = (const uint8_t*)e->UserData;
    if (e->UserDataLength >= 4 && p[3] == kHciAclIn) g_acl++;
    static int dumped;
    if (e->UserDataLength >= 4 && p[3] == kHciAclIn && dumped < 6) {      // raw bytes of the first few
        dumped++;
        printf("  raw ACL event, %u bytes:", e->UserDataLength);
        for (int i = 0; i < e->UserDataLength && i < 48; i++) printf(" %02X", p[i]);
        printf("\n");
    }
    HciRawDecoder::Result r = g_hci.Feed(p, e->UserDataLength);
    if (r == HciRawDecoder::Scrubbed) g_scrubbed++;
    if (r == HciRawDecoder::Disconnected) printf("  link 0x%03X disconnected\n", g_hci.Handle());
    if (r != HciRawDecoder::Payload) return;
    g_payloads++;
    uint8_t out[2][kMaxFrame];
    uint32_t len[2];
    int n = MouseFrames(g_hci.Data(), g_hci.Length(), out, len);
    for (int i = 0; i < n; i++) {
        if (!g_startQpc) g_startQpc = e->EventHeader.TimeStamp.QuadPart;
        LogFrame(out[i], len[i], e->EventHeader.TimeStamp.QuadPart);
        if (out[i][0] != kMm1Touch) continue;
        g_frames++;
        if (!g_firstQpc) g_firstQpc = e->EventHeader.TimeStamp.QuadPart;
        g_lastQpc = e->EventHeader.TimeStamp.QuadPart;
        if (g_printed < 12) {
            g_printed++;
            const uint8_t* t = out[i] + 6;
            int touches = (len[i] - 6) / 8;
            printf("  link 0x%03X report %02X -> frame, %d touch(es)", g_hci.Handle(), g_hci.Data()[1], touches);
            if (touches) {
                int x = (int32_t)((uint32_t)t[1] << 28 | (uint32_t)t[0] << 20) >> 20;
                int y = -((int32_t)((uint32_t)t[2] << 24 | (uint32_t)t[1] << 16) >> 20);
                printf(", first at (%d, %d) state %02X", x, y, t[7] & 0xF0);
            }
            printf("\n");
        }
    }
}

static std::vector<BYTE> Props() {
    std::vector<BYTE> buf(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSession) + 16, 0);
    auto p = (EVENT_TRACE_PROPERTIES*)buf.data();
    p->Wnode.BufferSize = (ULONG)buf.size();
    p->Wnode.ClientContext = 1;
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->BufferSize = 16;
    p->MinimumBuffers = 4;
    p->MaximumBuffers = 32;
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    p->FlushTimer = 1;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buf;
}

static DWORD WINAPI Process(LPVOID trace) {
    TRACEHANDLE h = (TRACEHANDLE)trace;
    ProcessTrace(&h, 1, nullptr, nullptr);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 15;
    wchar_t logPath[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    while (n && logPath[n - 1] != L'\\') n--;
    wcscpy_s(logPath + n, MAX_PATH - n, L"btcapture_log.txt");
    _wfopen_s(&g_log, logPath, L"w");
    wchar_t framePath[MAX_PATH];
    wcscpy_s(framePath, logPath);
    wcscpy_s(framePath + n, MAX_PATH - n, L"btcapture_frames_log.txt");
    _wfopen_s(&g_frameLog, framePath, L"w");
    if (g_frameLog) fprintf(g_frameLog, "# t_ms  motion(dx,dy) buttons | touches: id state x y major minor\n");
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    g_qpcMs = 1000.0 / qf.QuadPart;
    std::vector<BYTE> props = Props();
    ControlTraceW(0, kSession, (EVENT_TRACE_PROPERTIES*)props.data(), EVENT_TRACE_CONTROL_STOP);
    props = Props();
    TRACEHANDLE session = 0;
    ULONG rc = StartTraceW(&session, kSession, (EVENT_TRACE_PROPERTIES*)props.data());
    if (rc != ERROR_SUCCESS) {
        printf("Starting the trace failed (error %lu%s).\n", rc, rc == ERROR_ACCESS_DENIED ? ": run as administrator" : "");
        return 1;
    }
    rc = EnableTraceEx2(session, &kBthPort, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, ~0ULL, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) printf("Enabling the Bluetooth provider failed (error %lu).\n", rc);
    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = (LPWSTR)kSession;
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = OnEvent;
    TRACEHANDLE trace = OpenTraceW(&log);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        printf("Opening the trace failed (error %lu).\n", GetLastError());
        props = Props();
        ControlTraceW(session, nullptr, (EVENT_TRACE_PROPERTIES*)props.data(), EVENT_TRACE_CONTROL_STOP);
        return 1;
    }
    HANDLE thread = CreateThread(nullptr, 0, Process, (LPVOID)trace, 0, nullptr);
    printf("Reading Bluetooth packets for %d s: touch and move the Magic Mouse.\n", seconds);
    ULONGLONG end = GetTickCount64() + seconds * 1000ULL;
    while (GetTickCount64() < end) {
        Sleep(10);
        props = Props();
        ControlTraceW(session, nullptr, (EVENT_TRACE_PROPERTIES*)props.data(), EVENT_TRACE_CONTROL_FLUSH);
    }
    props = Props();
    ControlTraceW(session, nullptr, (EVENT_TRACE_PROPERTIES*)props.data(), EVENT_TRACE_CONTROL_STOP);
    const EVENT_TRACE_PROPERTIES* st = (const EVENT_TRACE_PROPERTIES*)props.data();     // filled in by the stop
    ULONG lost = st->EventsLost, buffersLost = st->RealTimeBuffersLost;
    WaitForSingleObject(thread, 3000);
    CloseTrace(trace);
    if (g_frameLog) fclose(g_frameLog);
    printf("Events Windows dropped from the trace: %lu (real-time buffers lost: %lu)\n", lost, buffersLost);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double span = g_frames > 1 ? (double)(g_lastQpc - g_firstQpc) / freq.QuadPart : 0;
    printf("\nBluetooth events: %lu, incoming ACL packets: %lu, payloads: %lu, blanked: %lu\n",
           g_events, g_acl, g_payloads, g_scrubbed);
    if (g_lastQpc) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double behind = (double)(now.QuadPart - g_lastQpc) / freq.QuadPart;
        printf("Timestamps: %s (last frame %.1f s before now on the QPC clock)\n",
               behind >= 0 && behind < 60 ? "QPC, the app's clock - good" : "NOT on the QPC clock", behind);
    }
    printf("Touch frames: %lu%s", g_frames, span > 0 ? "" : "\n");
    if (span > 0) printf(" over %.1f s (%.0f per second)\n", span, (g_frames - 1) / span);
    if (!g_events) printf("No Bluetooth events at all: the provider didn't log anything.\n");
    else if (g_scrubbed && !g_frames) printf("Windows blanks the packets: EtwLogSensitiveData=1 is needed (mmdriver bootcamp).\n");
    else if (!g_frames) printf("No touch frames: was the mouse touched, and is it in multi-touch mode?\n");
    printf("\n(saved to %ls)\n", logPath);
    if (g_log) fclose(g_log);
    if (GetConsoleProcessList(&n, 1) == 1) {     // its own window (e.g. Run as administrator): keep it open
        printf("Press Enter to close.");
        getchar();
    }
    return g_frames ? 0 : 2;
}
