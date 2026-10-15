// WDVR as an .asi plugin, loaded by Ultimate ASI Loader (dinput8.dll).
// DllMain only writes the log; real startup happens in InitializeASI (called by
// the loader once it's safe) or, as a fallback, on a short-delay thread.
#include <windows.h>
#include "log.h"

namespace wdvr { void start(HMODULE self); }

static HMODULE g_self;
static volatile LONG g_started = 0;

static void start_once(const char* why) {
    if (InterlockedExchange(&g_started, 1) == 0) {
        // Only one copy of the mod may run (the ASI loader loads EVERY .asi in plugins\ - backups included).
        CreateMutexA(nullptr, FALSE, "WDVR_single_instance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return;
        wdvr::log_open(g_self);
        wdvr::log("WDVR.asi loaded into process %lu, starting (%s)", GetCurrentProcessId(), why);
        wdvr::start(g_self);
    }
}

extern "C" __declspec(dllexport) void InitializeASI() { start_once("InitializeASI"); }

static DWORD WINAPI fallback_thread(LPVOID) {
    Sleep(3000);
    start_once("fallback timer");
    return 0;
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
        if (HANDLE t = CreateThread(nullptr, 0, fallback_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
