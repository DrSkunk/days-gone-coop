#pragma once

#include <cstdint>
#include <string>

namespace coop {

class Transport;

struct SaveTransferContext {
    Transport* net = nullptr;
    double now = 0;
    uint32_t session = 0;     // ours
    uint32_t peerSession = 0; // partner's, 0 when unknown
    bool host = false;
    bool connected = false;
    bool inGameplay = false;
    bool loading = false; // a save load is in progress; adopting now could race it
};

// Host-first save transfer (HostSave=1). The host serves a snapshot of its newest SaveGames folder;
// the guest pulls it in 1 KB chunks, verifies the SHA-256 digests and adopts it into the save sandbox,
// then loads it so both players play the host's progress.
void SaveTransferConfigure(bool share, bool join);
// Host: the game saved, so the next request gets a fresh snapshot.
void SaveTransferInvalidate();
// Guest: download the host's save again (its progress moved on); false while a transfer is running.
bool SaveTransferResync();
// Returns true when the datagram belonged to the save transfer.
bool SaveTransferHandle(const char* data, int size, const SaveTransferContext& context);
void SaveTransferTick(const SaveTransferContext& context);
// Short status for the co-op menu; empty when there is nothing to report.
std::string SaveTransferStatus(const SaveTransferContext& context);

} // namespace coop
