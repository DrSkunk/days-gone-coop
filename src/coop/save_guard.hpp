#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop {

// Experimental guest save sandbox. Redirects every write the game makes below its SaveGames folder
// into `sandboxRoot` (wiped at install) so the player's real saves are never modified. Reads prefer
// sandbox copies and fall back to the real files. Returns false if the hooks could not be installed.
bool InstallSaveGuard(const std::wstring& sandboxRoot);

bool SaveGuardActive();

struct AdoptedFile {
    std::wstring name; // plain file name, validated by the caller
    std::vector<uint8_t> data;
};

// Replaces the sandbox contents with `files` (the host's saves) and switches to exclusive mode: the
// real SaveGames folder is no longer visible to the game at all. The adopted files are pinned, so game
// writes to them are discarded until SaveGuardLoaded() reports that a save load finished.
bool SaveGuardAdopt(const std::vector<AdoptedFile>& files, std::string& error);
bool SaveGuardAdopted();
bool SaveGuardPinned();
void SaveGuardLoaded();

} // namespace coop
