// Read-only reverse-engineering aids behind DebugTools=1: reflection dumps, live object
// census and a ProcessEvent call trace. Nothing here changes game state.
#pragma once
#include "ue.hpp"

#include <string>

namespace coop::research {

bool DumpReflection(const std::wstring& path, std::string& summary);
bool DumpObjects(const std::wstring& path, std::string& summary);
void TraceStart();
bool TraceStop(const std::wstring& path, std::string& summary);
bool Tracing();
// Called from the ProcessEvent hook on the game thread.
void TraceEvent(ue::UObject* obj, ue::UObject* fn, void* parms);

} // namespace coop::research
