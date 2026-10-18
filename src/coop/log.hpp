#pragma once
#include <string>

namespace coop {
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
