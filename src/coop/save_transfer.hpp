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

// Experimental host-to-guest save transfer. The host (ShareSave=1) serves a snapshot of its newest
// SaveGames folder; the guest (GuestSaveGuard=1) pulls it in 1 KB chunks, verifies the SHA-256 digests
// and adopts it into the save sandbox, so pressing Continue loads the host's progress.
void SaveTransferConfigure(bool share, bool join);
// Returns true when the datagram belonged to the save transfer.
bool SaveTransferHandle(const char* data, int size, const SaveTransferContext& context);
void SaveTransferTick(const SaveTransferContext& context);
// Short status for the co-op menu; empty when there is nothing to report.
std::string SaveTransferStatus(const SaveTransferContext& context);

} // namespace coop
