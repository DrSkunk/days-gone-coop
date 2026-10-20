#pragma once

#include <string>

namespace coop {

// Experimental guest save sandbox. Redirects every write the game makes below its SaveGames folder
// into `sandboxRoot` (wiped at install) so the player's real saves are never modified. Reads prefer
// sandbox copies and fall back to the real files. Returns false if the hooks could not be installed.
bool InstallSaveGuard(const std::wstring& sandboxRoot);

} // namespace coop
