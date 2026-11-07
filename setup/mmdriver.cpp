// mmdriver.cpp - installs and removes the MagicMouseNative filter driver on the Magic Mouse.
// Used by the installer and the portable version; needs no WDK tools (devcon) and doesn't
// read pnputil's output (it's translated on non-English Windows).
//
//   mmdriver status                       what's installed and whether the driver can load
//   mmdriver install [/testsigning] [/quiet]
//       Trusts the driver's test certificate, puts mmfilter.inf (next to this exe) on the
//       mouse and, with /testsigning or when asked, turns on test signing (BitLocker is
//       suspended for that one restart).
//   mmdriver uninstall [/quiet]           removes the driver from the mouse, the driver store
//                                         and the certificate stores
//   /quiet: no questions and no "press Enter" at the end (for the installer).
// install and uninstall ask for administrator rights (UAC) when not already elevated.
//
// Exit codes: 0 done, 1 failed, 2 Secure Boot is on, 3 no Magic Mouse paired, 10 done but a
// restart is needed.
//
// Build: cl /nologo /EHsc /O2 /W4 /std:c++17 /DUNICODE /D_UNICODE mmdriver.cpp
//        /link setupapi.lib newdev.lib crypt32.lib advapi32.lib shell32.lib

#define WIN32_LEAN_AND_MEAN
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000003    // DiUninstallDriverW: Windows 10 1703
#endif
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <string>

static const wchar_t kInf[]      = L"mmfilter.inf";
static const wchar_t kCatalog[]  = L"mmfilter.cat";
static const wchar_t kCert[]     = L"mmfilter_test.cer";
static const wchar_t kCertName[] = L"MagicMouseNative Test";
static const wchar_t kApp[]      = L"MagicMouseNative.exe";
static const wchar_t* const kHardwareIds[] = {
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&030d",
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&0310",
};
enum { EXIT_DONE = 0, EXIT_FAILED = 1, EXIT_SECURE_BOOT = 2, EXIT_NO_MOUSE = 3, EXIT_RESTART = 10 };

static bool g_quiet;

static std::wstring HereDir() {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L'\\') + 1);
}

static bool IsElevated() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

static bool SecureBootOn() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                        L"UEFISecureBootEnabled", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v;
}

// Whether this boot has test signing on (a change takes effect at the next restart).
static bool TestSigningOn() {
    wchar_t opts[1024] = L"";
    DWORD size = sizeof(opts);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions",
                 RRF_RT_REG_SZ, nullptr, opts, &size);
    _wcsupr_s(opts);
    return wcsstr(opts, L"TESTSIGNING") != nullptr;
}

static bool DriverRunning() {
    HANDLE h = CreateFileW(L"\\\\.\\MagicMouseNative", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static DWORD Run(const std::wstring& cmdLine) {
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static std::wstring System32(const wchar_t* exe) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(L"\"") + dir + L"\\" + exe + L"\"";
}

// Test signing changes the boot configuration, which BitLocker would answer with a recovery
// key prompt: its protectors are suspended for one restart first (harmless without BitLocker).
static bool EnableTestSigning() {
    wchar_t drive[8] = L"C:";
    GetEnvironmentVariableW(L"SystemDrive", drive, ARRAYSIZE(drive));
    Run(System32(L"manage-bde.exe") + L" -protectors -disable " + drive + L" -RebootCount 1");
    return Run(System32(L"bcdedit.exe") + L" /set testsigning on") == 0;
}

// The tray app keeps the driver's control device open, which would keep the old driver
// loaded: it's closed first (the installer / portable script start it again).
static void StopApp() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, kApp) != 0) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        TerminateProcess(p, 0);
        WaitForSingleObject(p, 10000);
        CloseHandle(p);
    }
    CloseHandle(snap);
}

static bool TrustCertificate(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BYTE data[8192];
    DWORD got = 0;
    BOOL read = ReadFile(f, data, sizeof(data), &got, nullptr);
    CloseHandle(f);
    if (!read || !got) return false;
    bool ok = true;
    for (const wchar_t* name : { L"Root", L"TrustedPublisher" }) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
        ok = store && CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, got,
                                                       CERT_STORE_ADD_REPLACE_EXISTING, nullptr) && ok;
        if (store) CertCloseStore(store, 0);
    }
    return ok;
}

static void UntrustCertificate() {
    for (const wchar_t* name : { L"Root", L"TrustedPublisher" }) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
        if (!store) continue;
        PCCERT_CONTEXT c;
        while ((c = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR, kCertName, nullptr)) != nullptr)
            CertDeleteCertificateFromStore(c);     // frees c
        CertCloseStore(store, 0);
    }
}

// The driver store's copies of mmfilter.inf (%windir%\INF\oemNN.inf), found by catalog name.
template <class F> static void ForEachInstalledInf(F f) {
    wchar_t dir[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring inf = std::wstring(dir) + L"\\INF\\";
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((inf + L"oem*.inf").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring path = inf + fd.cFileName;
        wchar_t cat[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"Version", L"CatalogFile", L"", cat, MAX_PATH, path.c_str());
        if (_wcsicmp(cat, kCatalog) == 0) f(path);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static bool Ask(const wchar_t* question) {
    if (g_quiet) return false;
    wprintf(L"%s [y/n] ", question);
    wchar_t line[16] = L"";
    if (!fgetws(line, ARRAYSIZE(line), stdin)) return false;
    return line[0] == L'y' || line[0] == L'Y';
}

static int Status() {
    bool secure = SecureBootOn(), test = TestSigningOn();
    int installed = 0;
    ForEachInstalledInf([&](const std::wstring&) { installed++; });
    wprintf(L"Secure Boot:    %s\n", secure ? L"on (test-signed drivers can't load)" : L"off");
    wprintf(L"Test signing:   %s\n", test ? L"on" : L"off");
    wprintf(L"Driver package: %s\n", installed ? L"installed" : L"not installed");
    wprintf(L"Driver running: %s\n", DriverRunning() ? L"yes" : L"no (is the mouse connected?)");
    return EXIT_DONE;
}

static int Install(bool testSigning) {
    std::wstring dir = HereDir(), inf = dir + kInf;
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"%s isn't next to mmdriver.exe.\n", kInf);
        return EXIT_FAILED;
    }
    if (SecureBootOn()) {
        wprintf(L"Secure Boot is on, so Windows won't load this test-signed driver (and would\n"
                L"leave the mouse without a driver). Turn Secure Boot off in your PC's UEFI/BIOS\n"
                L"setup, then run this again.\n");
        return EXIT_SECURE_BOOT;
    }
    bool restart = false;
    if (!TestSigningOn()) {
        wprintf(L"Test signing is off. Windows only loads this driver with it on; turning it on\n"
                L"needs a restart (BitLocker, if on, is suspended for that one restart).\n");
        if (testSigning || Ask(L"Turn test signing on now?")) {
            if (!EnableTestSigning()) {
                wprintf(L"Couldn't turn test signing on (bcdedit failed).\n");
                return EXIT_FAILED;
            }
            wprintf(L"Test signing will be on after the restart.\n");
            restart = true;
        } else {
            wprintf(L"Not installing: without test signing the mouse would stop working.\n");
            return EXIT_FAILED;
        }
    }

    StopApp();
    if (!TrustCertificate(dir + kCert)) {
        wprintf(L"Couldn't add the driver's certificate (%s).\n", kCert);
        return EXIT_FAILED;
    }
    // Forced onto the mouse: Windows' own Bluetooth HID driver would otherwise win on rank.
    bool found = false, done = false;
    for (const wchar_t* id : kHardwareIds) {
        BOOL reboot = FALSE;
        if (UpdateDriverForPlugAndPlayDevicesW(nullptr, id, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
            found = done = true;
            restart = restart || reboot;
        } else if (GetLastError() != ERROR_NO_SUCH_DEVINST) {
            found = true;
            wprintf(L"Installing on the mouse failed (error %lu).\n", GetLastError());
        }
    }
    if (!found) {
        wprintf(L"No Magic Mouse (2009) is paired. Pair it in Bluetooth settings, then run this again.\n");
        return EXIT_NO_MOUSE;
    }
    if (!done) return EXIT_FAILED;
    wprintf(L"The driver is installed on the Magic Mouse.%s\n",
            !TestSigningOn() ? L" It loads after a restart: until then the mouse doesn't work."
            : restart ? L" Restart Windows to finish." : L"");
    return restart ? EXIT_RESTART : EXIT_DONE;
}

static int Uninstall() {
    StopApp();
    int removed = 0;
    bool restart = false, failed = false;
    ForEachInstalledInf([&](const std::wstring& path) {
        BOOL reboot = FALSE;
        // Takes it off the mouse (which goes back to Windows' own driver) and out of the store.
        if (DiUninstallDriverW(nullptr, path.c_str(), 0, &reboot)) {
            removed++;
            restart = restart || reboot;
        } else {
            failed = true;
            wprintf(L"Removing %s failed (error %lu).\n", path.c_str(), GetLastError());
        }
    });
    UntrustCertificate();
    if (failed) return EXIT_FAILED;
    wprintf(removed ? L"The driver is removed; the mouse uses Windows' own driver again.%s\n"
                    : L"The driver wasn't installed.%s\n", restart ? L" Restart Windows to finish." : L"");
    return restart ? EXIT_RESTART : EXIT_DONE;
}

// Runs this exe again as administrator (a UAC prompt) with the same arguments, and waits.
static int RunElevated(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring args;
    for (int i = 1; i < argc; i++) args += std::wstring(i > 1 ? L" " : L"") + argv[i];
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        wprintf(L"Administrator rights are needed (%s).\n", GetLastError() == ERROR_CANCELLED ? L"cancelled" : L"failed");
        return EXIT_FAILED;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = EXIT_FAILED;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

int wmain(int argc, wchar_t** argv) {
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    bool testSigning = false;
    for (int i = 2; i < argc; i++) {
        if (_wcsicmp(argv[i], L"/quiet") == 0) g_quiet = true;
        if (_wcsicmp(argv[i], L"/testsigning") == 0) testSigning = true;
    }
    if ((cmd == L"install" || cmd == L"uninstall") && !IsElevated()) return RunElevated(argc, argv);
    int code;
    if (cmd == L"status") {
        code = Status();
    } else if (cmd == L"install" || cmd == L"uninstall") {
        code = cmd == L"install" ? Install(testSigning) : Uninstall();
    } else {
        wprintf(L"Usage: mmdriver status | install [/testsigning] [/quiet] | uninstall [/quiet]\n");
        code = EXIT_FAILED;
    }
    // Started in its own console window (e.g. elevated from a script): keep it readable.
    DWORD procs[2];
    if (!g_quiet && GetConsoleProcessList(procs, 2) == 1) {
        wprintf(L"\nPress Enter to close.");
        getwchar();
    }
    return code;
}
