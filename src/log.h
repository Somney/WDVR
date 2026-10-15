// Tiny logger: writes to wdvr.log next to the mod's dxgi.dll.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace wdvr {

inline FILE*& log_file() { static FILE* f = nullptr; return f; }

inline void log_open(HMODULE self) {
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "wdvr.log");
    log_file() = fopen(path, "w");
}

inline void log(const char* fmt, ...) {
    FILE* f = log_file();
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f);
    fflush(f);
}

} // namespace wdvr
