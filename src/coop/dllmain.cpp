#include "coop.hpp"

#include <windows.h>

namespace {
HMODULE g_self = nullptr;
volatile LONG g_started = 0;

void Start() {
    if (InterlockedExchange(&g_started, 1)) return;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring base(path, n);
    if (auto dot = base.find_last_of(L'.'); dot != std::wstring::npos) base.resize(dot);
    coop::Install(base);
}
} // namespace

// Ultimate ASI Loader calls this after loading the plugin when it is exported.
extern "C" __declspec(dllexport) void InitializeASI() { Start(); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        Start();
    }
    return TRUE;
}
