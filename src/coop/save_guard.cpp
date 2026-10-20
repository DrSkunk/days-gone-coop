#include "save_guard.hpp"

#include "log.hpp"
#include "save_policy.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace coop {

namespace {

using namespace savepolicy;

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileWFn = BOOL(WINAPI*)(LPCWSTR);
using MoveFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using SetFileAttributesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using RemoveDirectoryWFn = BOOL(WINAPI*)(LPCWSTR);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using SetFileInformationByHandleFn = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);

struct Real {
    CreateFileWFn createFileW = CreateFileW;
    CreateFileAFn createFileA = CreateFileA;
    DeleteFileWFn deleteFileW = DeleteFileW;
    MoveFileWFn moveFileW = MoveFileW;
    GetFileAttributesWFn getFileAttributesW = GetFileAttributesW;
    GetFileAttributesExWFn getFileAttributesExW = GetFileAttributesExW;
    SetFileAttributesWFn setFileAttributesW = SetFileAttributesW;
    CreateDirectoryWFn createDirectoryW = CreateDirectoryW;
    RemoveDirectoryWFn removeDirectoryW = RemoveDirectoryW;
    FindFirstFileWFn findFirstFileW = FindFirstFileW;
    SetFileInformationByHandleFn setFileInformationByHandle = SetFileInformationByHandle;
} real;

std::wstring g_root, g_discard;
SRWLOCK g_lock = SRWLOCK_INIT;
std::set<std::wstring> g_deleted; // lower-case relative paths deleted during this session
std::set<std::wstring> g_pinned;  // lower-case adopted names protected until the next load completes
std::atomic<bool> g_active{false}, g_exclusive{false};
std::atomic<int> g_notes{0};

struct Mapped {
    bool guarded = false;
    bool refused = false;
    std::wstring path;
    std::wstring relative;
};

bool Exists(const std::wstring& path) { return real.getFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool Deleted(const std::wstring& relative) {
    AcquireSRWLockShared(&g_lock);
    bool deleted = g_deleted.contains(LowerCopy(relative));
    ReleaseSRWLockShared(&g_lock);
    return deleted;
}

bool Pinned(const std::wstring& relative) {
    AcquireSRWLockShared(&g_lock);
    bool pinned = g_pinned.contains(LowerCopy(relative));
    ReleaseSRWLockShared(&g_lock);
    return pinned;
}

void SetDeleted(const std::wstring& relative, bool deleted) {
    AcquireSRWLockExclusive(&g_lock);
    if (deleted)
        g_deleted.insert(LowerCopy(relative));
    else
        g_deleted.erase(LowerCopy(relative));
    ReleaseSRWLockExclusive(&g_lock);
}

std::string Narrow(const std::wstring& value) {
    std::string out;
    for (wchar_t c : value) out.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

void Note(const char* operation, const std::wstring& relative, const char* detail = "") {
    int count = ++g_notes;
    if (count <= 256)
        Log("save guard: %s SaveGames\\%s -> sandbox%s", operation, Narrow(relative).c_str(), detail);
    else if (count == 257)
        Log("save guard: further redirect messages suppressed");
}

// Maps a game path. Writes always land in the sandbox; reads use the sandbox copy if present, a
// missing path if the file was deleted this session, otherwise the original. In exclusive mode (after
// adopting the host's saves) reads never fall back to the original, and writes to pinned adopted files
// go to a discard folder.
Mapped Map(const wchar_t* path, bool write, bool copyOnWrite) {
    Mapped mapped;
    mapped.path = path;
    auto relative = SaveGamesRelative(path);
    if (!relative) return mapped;
    mapped.guarded = true;
    mapped.relative = *relative;
    if (!SafeRelative(*relative)) {
        mapped.refused = true;
        return mapped;
    }
    std::wstring sandbox = SandboxPath(g_root, *relative);
    bool exclusive = g_exclusive.load();
    if (!write) {
        if (exclusive || Exists(sandbox) || Deleted(*relative)) mapped.path = sandbox;
        return mapped;
    }
    if (exclusive && !relative->empty() && Pinned(*relative)) {
        mapped.path = SandboxPath(g_discard, *relative);
        return mapped;
    }
    if (exclusive) copyOnWrite = false;
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(sandbox).parent_path(), ignored);
    if (copyOnWrite && !relative->empty() && !Exists(sandbox) && !Deleted(*relative) && Exists(path) &&
        !CopyFileW(path, sandbox.c_str(), TRUE))
        Log("save guard: copy-on-write failed for SaveGames\\%s (error %lu)", Narrow(*relative).c_str(), GetLastError());
    SetDeleted(*relative, false);
    mapped.path = sandbox;
    return mapped;
}

HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (!name) return real.createFileW(name, access, share, security, disposition, flags, templateFile);
    bool write = IsWriteOpen(access, disposition);
    auto mapped = Map(name, write, NeedsCopyOnWrite(disposition));
    if (mapped.refused) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (mapped.guarded && write)
        Note("write", mapped.relative, mapped.path.starts_with(g_discard) ? " (discarded, host save pinned)" : "");
    return real.createFileW(mapped.path.c_str(), access, share, security, disposition, flags, templateFile);
}

HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (name) {
        int length = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        if (length > 0) {
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), length);
            if (SaveGamesRelative(wide.c_str()))
                return HookCreateFileW(wide.c_str(), access, share, security, disposition, flags, templateFile);
        }
    }
    return real.createFileA(name, access, share, security, disposition, flags, templateFile);
}

BOOL WINAPI HookDeleteFileW(LPCWSTR name) {
    auto relative = name ? SaveGamesRelative(name) : std::nullopt;
    if (!relative) return real.deleteFileW(name);
    if (!SafeRelative(*relative)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    std::wstring sandbox = SandboxPath(g_root, *relative);
    if (g_exclusive && Pinned(*relative)) {
        Note("delete", *relative, " (ignored, host save pinned)");
        return TRUE;
    }
    bool existed = Exists(sandbox) || (!g_exclusive && !Deleted(*relative) && Exists(name));
    if (Exists(sandbox)) real.deleteFileW(sandbox.c_str());
    SetDeleted(*relative, true);
    Note("delete", *relative, existed ? "" : " (missing)");
    if (!existed) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI HookMoveFileW(LPCWSTR from, LPCWSTR to) {
    auto fromRelative = from ? SaveGamesRelative(from) : std::nullopt;
    auto toRelative = to ? SaveGamesRelative(to) : std::nullopt;
    if (!fromRelative && !toRelative) return real.moveFileW(from, to);
    if ((fromRelative && !SafeRelative(*fromRelative)) || (toRelative && !SafeRelative(*toRelative))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    auto source = Map(from, false, false);
    auto target = toRelative ? Map(to, true, false) : Mapped{false, false, to, {}};
    BOOL ok;
    // Never move a real save or a pinned host save; copy it instead and hide the original for the
    // rest of the session.
    bool pinnedSource = fromRelative && g_exclusive && Pinned(*fromRelative);
    if ((fromRelative && source.path == from) || pinnedSource)
        ok = CopyFileW(source.path.c_str(), target.path.c_str(), TRUE);
    else
        ok = real.moveFileW(source.path.c_str(), target.path.c_str());
    if (ok && fromRelative && !pinnedSource) SetDeleted(*fromRelative, true);
    Note("move", toRelative ? *toRelative : *fromRelative, ok ? "" : " (failed)");
    return ok;
}

DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
    if (!name) return real.getFileAttributesW(name);
    return real.getFileAttributesW(Map(name, false, false).path.c_str());
}

BOOL WINAPI HookGetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (!name) return real.getFileAttributesExW(name, level, info);
    return real.getFileAttributesExW(Map(name, false, false).path.c_str(), level, info);
}

BOOL WINAPI HookSetFileAttributesW(LPCWSTR name, DWORD attributes) {
    if (!name) return real.setFileAttributesW(name, attributes);
    auto mapped = Map(name, true, true);
    if (mapped.refused) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return real.setFileAttributesW(mapped.path.c_str(), attributes);
}

BOOL WINAPI HookCreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES security) {
    if (!name) return real.createDirectoryW(name, security);
    auto mapped = Map(name, true, false);
    if (mapped.refused) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return real.createDirectoryW(mapped.path.c_str(), security);
}

BOOL WINAPI HookRemoveDirectoryW(LPCWSTR name) {
    auto relative = name ? SaveGamesRelative(name) : std::nullopt;
    if (!relative) return real.removeDirectoryW(name);
    if (!SafeRelative(*relative)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    Note("remove directory", *relative);
    return real.removeDirectoryW(SandboxPath(g_root, *relative).c_str());
}

HANDLE WINAPI HookFindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    if (!pattern) return real.findFirstFileW(pattern, data);
    std::wstring value = pattern;
    // Exact lookups follow the sandbox; wildcard listings enumerate the real folder unless the host's
    // saves were adopted, in which case the sandbox is the only visible SaveGames folder.
    if (value.find_first_of(L"*?") == std::wstring::npos || g_exclusive)
        return real.findFirstFileW(Map(pattern, false, false).path.c_str(), data);
    return real.findFirstFileW(pattern, data);
}

BOOL WINAPI HookSetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS type, LPVOID info, DWORD size) {
    if ((type != FileRenameInfo && type != static_cast<FILE_INFO_BY_HANDLE_CLASS>(22)) || !info ||
        size < offsetof(FILE_RENAME_INFO, FileName))
        return real.setFileInformationByHandle(file, type, info, size);
    auto rename = static_cast<const FILE_RENAME_INFO*>(info);
    size_t chars = rename->FileNameLength / sizeof(wchar_t);
    if (offsetof(FILE_RENAME_INFO, FileName) + rename->FileNameLength > size)
        return real.setFileInformationByHandle(file, type, info, size);
    std::wstring target(rename->FileName, chars);
    auto relative = SaveGamesRelative(target);
    if (!relative) return real.setFileInformationByHandle(file, type, info, size);
    auto mapped = Map(target.c_str(), true, false);
    if (mapped.refused) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    std::vector<uint8_t> buffer(offsetof(FILE_RENAME_INFO, FileName) + (mapped.path.size() + 1) * sizeof(wchar_t));
    std::memcpy(buffer.data(), info, offsetof(FILE_RENAME_INFO, FileName));
    auto replacement = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    replacement->RootDirectory = nullptr;
    replacement->FileNameLength = static_cast<DWORD>(mapped.path.size() * sizeof(wchar_t));
    std::memcpy(replacement->FileName, mapped.path.c_str(), (mapped.path.size() + 1) * sizeof(wchar_t));
    Note("rename", *relative);
    return real.setFileInformationByHandle(file, type, buffer.data(), static_cast<DWORD>(buffer.size()));
}

// Replaces one KERNEL32 import of the game executable and returns the previous target.
bool PatchImport(HMODULE module, const char* function, void* hook, void** original) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    for (auto import = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name; ++import) {
        if (_stricmp(reinterpret_cast<const char*>(base + import->Name), "KERNEL32.dll") != 0) continue;
        if (!import->OriginalFirstThunk) return false;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD protection;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protection)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), protection, &protection);
            return true;
        }
    }
    return false;
}

template <typename Fn>
bool Patch(HMODULE module, const char* function, Fn hook, Fn& original) {
    void* previous = nullptr;
    if (!PatchImport(module, function, reinterpret_cast<void*>(hook), &previous)) {
        Log("save guard: import %s not found; leaving the system function in place", function);
        return false;
    }
    original = reinterpret_cast<Fn>(previous);
    return true;
}

} // namespace

bool InstallSaveGuard(const std::wstring& sandboxRoot) {
    g_root = Normalize(sandboxRoot);
    g_discard = g_root + L"-discard";
    std::error_code error;
    std::filesystem::remove_all(g_discard, error);
    std::filesystem::remove_all(g_root, error);
    std::filesystem::create_directories(g_discard, error);
    if (!error) std::filesystem::create_directories(g_root, error);
    if (error) {
        Log("save guard: cannot prepare sandbox %s (%s); guard disabled", Narrow(g_root).c_str(), error.message().c_str());
        return false;
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    // Write paths are mandatory; without them the real saves would not be protected.
    bool ok = Patch(exe, "CreateFileW", HookCreateFileW, real.createFileW) &&
              Patch(exe, "DeleteFileW", HookDeleteFileW, real.deleteFileW) &&
              Patch(exe, "MoveFileW", HookMoveFileW, real.moveFileW);
    if (!ok) {
        Log("save guard: FAILED to install write hooks; real saves are NOT protected");
        return false;
    }
    Patch(exe, "CreateFileA", HookCreateFileA, real.createFileA);
    Patch(exe, "GetFileAttributesW", HookGetFileAttributesW, real.getFileAttributesW);
    Patch(exe, "GetFileAttributesExW", HookGetFileAttributesExW, real.getFileAttributesExW);
    Patch(exe, "SetFileAttributesW", HookSetFileAttributesW, real.setFileAttributesW);
    Patch(exe, "CreateDirectoryW", HookCreateDirectoryW, real.createDirectoryW);
    Patch(exe, "RemoveDirectoryW", HookRemoveDirectoryW, real.removeDirectoryW);
    Patch(exe, "FindFirstFileW", HookFindFirstFileW, real.findFirstFileW);
    Patch(exe, "SetFileInformationByHandle", HookSetFileInformationByHandle, real.setFileInformationByHandle);
    g_active = true;
    Log("save guard: EXPERIMENTAL save sandbox active; SaveGames writes go to %s", Narrow(g_root).c_str());
    return true;
}

bool SaveGuardActive() { return g_active; }
bool SaveGuardAdopted() { return g_exclusive; }

bool SaveGuardPinned() {
    AcquireSRWLockShared(&g_lock);
    bool pinned = !g_pinned.empty();
    ReleaseSRWLockShared(&g_lock);
    return pinned;
}

bool SaveGuardAdopt(const std::vector<AdoptedFile>& files, std::string& error) {
    if (!g_active) {
        error = "save guard is not active";
        return false;
    }
    for (const auto& file : files)
        if (file.name.empty() || file.name.find_first_of(L"\\/:") != std::wstring::npos || !SafeRelative(file.name)) {
            error = "invalid file name";
            return false;
        }
    // Publish the new contents under the lock so no game read sees a half-written bundle.
    AcquireSRWLockExclusive(&g_lock);
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(g_root, code)) {
        std::error_code ignored;
        std::filesystem::remove_all(entry.path(), ignored);
    }
    bool ok = !code;
    for (const auto& file : files) {
        if (!ok) break;
        std::ofstream out(std::filesystem::path(SandboxPath(g_root, file.name)), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
        ok = static_cast<bool>(out);
        if (!ok) error = "cannot write " + Narrow(file.name);
    }
    g_deleted.clear();
    g_pinned.clear();
    if (ok)
        for (const auto& file : files) g_pinned.insert(LowerCopy(file.name));
    g_exclusive = true; // even on failure, never fall back to the guest's own saves
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok && error.empty()) error = "cannot clear sandbox";
    Log("save guard: %s %u host save files; real SaveGames folder hidden%s", ok ? "adopted" : "FAILED to adopt",
        static_cast<unsigned>(files.size()), ok ? ", pinned until the next load" : "");
    return ok;
}

void SaveGuardLoaded() {
    AcquireSRWLockExclusive(&g_lock);
    bool had = !g_pinned.empty();
    g_pinned.clear();
    ReleaseSRWLockExclusive(&g_lock);
    if (had) Log("save guard: host save loaded; guest progress now saves into the sandbox");
}

} // namespace coop
