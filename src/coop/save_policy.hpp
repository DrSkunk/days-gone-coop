#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Pure path policy for the guest save sandbox (no Win32 dependencies so it can be unit-tested).
namespace coop::savepolicy {

// Win32 access/disposition values, duplicated so the policy compiles on any host.
inline constexpr uint32_t kGenericAll = 0x10000000, kGenericWrite = 0x40000000;
inline constexpr uint32_t kWriteData = 0x2, kAppendData = 0x4, kWriteEa = 0x10, kWriteAttributes = 0x100;
inline constexpr uint32_t kDelete = 0x10000, kWriteDac = 0x40000, kWriteOwner = 0x80000;
inline constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kOpenAlways = 4,
                          kTruncateExisting = 5;

inline wchar_t Lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

inline std::wstring Normalize(const std::wstring& path) {
    std::wstring out = path;
    for (auto& c : out)
        if (c == L'/') c = L'\\';
    return out;
}

inline std::wstring LowerCopy(const std::wstring& value) {
    std::wstring out = value;
    for (auto& c : out) c = Lower(c);
    return out;
}

// Returns the path below "...\Saved\...\SaveGames\" (may be empty for the folder itself), or nothing
// when the path is not inside a game SaveGames folder.
inline std::optional<std::wstring> SaveGamesRelative(const std::wstring& path) {
    std::wstring normal = Normalize(path), lower = LowerCopy(normal);
    size_t saved = lower.find(L"\\saved\\");
    if (saved == std::wstring::npos) return std::nullopt;
    size_t games = lower.find(L"\\savegames", saved);
    if (games == std::wstring::npos) return std::nullopt;
    size_t end = games + 10;
    if (end < lower.size() && lower[end] != L'\\') return std::nullopt;
    std::wstring rest = end < normal.size() ? normal.substr(end + 1) : std::wstring();
    while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
    return rest;
}

// Relative components that could escape the sandbox are refused.
inline bool SafeRelative(const std::wstring& relative) {
    if (!relative.empty() && (relative.front() == L'\\' || relative.find(L':') != std::wstring::npos)) return false;
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find(L'\\', start);
        if (end == std::wstring::npos) end = relative.size();
        std::wstring part = relative.substr(start, end - start);
        if (part == L"..") return false;
        start = end + 1;
    }
    return true;
}

inline std::wstring SandboxPath(const std::wstring& root, const std::wstring& relative) {
    return relative.empty() ? root : root + L"\\" + relative;
}

inline bool IsWriteOpen(uint32_t access, uint32_t disposition) {
    constexpr uint32_t writeMask = kGenericAll | kGenericWrite | kWriteData | kAppendData | kWriteEa |
                                   kWriteAttributes | kDelete | kWriteDac | kWriteOwner;
    // OPEN_ALWAYS creates a missing file even with read-only access.
    return (access & writeMask) != 0 || disposition == kCreateNew || disposition == kCreateAlways ||
           disposition == kOpenAlways || disposition == kTruncateExisting;
}

// Opening an existing file for modification must start from the original contents.
inline bool NeedsCopyOnWrite(uint32_t disposition) {
    return disposition == kOpenExisting || disposition == kOpenAlways;
}

} // namespace coop::savepolicy
