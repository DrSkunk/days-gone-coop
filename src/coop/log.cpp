#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace coop {

namespace {
FILE* g_file = nullptr;
CRITICAL_SECTION g_lock;
}

void LogOpen(const std::wstring& path) {
    InitializeCriticalSection(&g_lock);
    g_file = _wfopen(path.c_str(), L"w");
}

void Log(const char* fmt, ...) {
    if (!g_file) return;
    EnterCriticalSection(&g_lock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_file, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_file);
    std::fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

} // namespace coop
