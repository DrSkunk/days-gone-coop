#include "save_transfer.hpp"

#include "log.hpp"
#include "net.hpp"
#include "save_guard.hpp"
#include "savesync.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace coop {

namespace {

using namespace savesync;

constexpr double kSnapshotReuseSeconds = 30;
constexpr double kSnapshotDropSeconds = 300;
constexpr double kRequestInterval = 1.0;
constexpr double kStallSeconds = 10;
constexpr size_t kPullPerTick = 24; // keeps one tick's burst inside a default 64 KB UDP receive buffer
constexpr int kMaxVerifyFailures = 3;

bool g_share = false, g_join = false;

struct Host {
    std::vector<uint8_t> bundle;
    uint8_t sha[32]{};
    uint32_t transfer = 0;
    double builtAt = -1e9, lastServed = -1e9;
    bool askedWithoutShare = false;
    std::string folder;
} g_host;

enum class GuestState { idle, requesting, receiving, adopted, failed };

struct Guest {
    GuestState state = GuestState::idle;
    Receiver receiver;
    double nextRequest = 0, lastProgress = 0, firstRequest = -1;
    int verifyFailures = 0;
    std::string error;
} g_guest;

std::string Narrow(const std::wstring& value) {
    std::string out;
    for (wchar_t c : value) out.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

uint32_t RandomId() {
    uint32_t id = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&id), sizeof id, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return id ? id : 1;
}

uint64_t WriteTime(const WIN32_FILE_ATTRIBUTE_DATA& data) {
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// The save folder the game used most recently: %LOCALAPPDATA%\BendGame\Saved\<account>\SaveGames with
// the newest profile.sav.
std::optional<std::wstring> FindSaveFolder() {
    wchar_t local[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!length || length >= MAX_PATH) return std::nullopt;
    std::wstring saved = std::wstring(local) + L"\\BendGame\\Saved";
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((saved + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return std::nullopt;
    std::optional<std::wstring> best;
    uint64_t bestTime = 0;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == L'.') continue;
        std::wstring folder = saved + L"\\" + entry.cFileName + L"\\SaveGames";
        WIN32_FILE_ATTRIBUTE_DATA profile;
        if (!GetFileAttributesExW((folder + L"\\profile.sav").c_str(), GetFileExInfoStandard, &profile)) continue;
        if (!best || WriteTime(profile) > bestTime) {
            best = folder;
            bestTime = WriteTime(profile);
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return best;
}

bool ReadStable(const std::wstring& path, std::vector<uint8_t>& out) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        WIN32_FILE_ATTRIBUTE_DATA before, after;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &before) || before.nFileSizeHigh) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        out.assign(before.nFileSizeLow, 0);
        DWORD read = 0;
        BOOL ok = out.empty() || ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != out.size()) continue;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &after) && WriteTime(after) == WriteTime(before) &&
            after.nFileSizeLow == before.nFileSizeLow && !after.nFileSizeHigh)
            return true;
        Sleep(50); // the game is writing this file; try again
    }
    return false;
}

bool BuildSnapshot(double now) {
    auto folder = FindSaveFolder();
    if (!folder) {
        Log("save transfer: no SaveGames folder with a profile.sav under %%LOCALAPPDATA%%\\BendGame\\Saved");
        return false;
    }
    std::vector<File> files;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW((*folder + L"\\*.sav").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = Narrow(entry.cFileName);
        if (!ValidName(name)) {
            Log("save transfer: skipping save file with an unsupported name");
            continue;
        }
        File file{name, {}};
        if (!ReadStable(*folder + L"\\" + entry.cFileName, file.data)) {
            Log("save transfer: could not read %s consistently", name.c_str());
            ok = false;
            break;
        }
        files.push_back(std::move(file));
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    if (!ok) return false;
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.name < b.name; });
    auto bundle = BuildBundle(files);
    if (bundle.empty()) {
        Log("save transfer: cannot bundle %u save files (limit %u files, %u bytes)", static_cast<unsigned>(files.size()),
            static_cast<unsigned>(kMaxFiles), kMaxBundleBytes);
        return false;
    }
    g_host.bundle = std::move(bundle);
    auto digest = Sha256::Of(g_host.bundle.data(), g_host.bundle.size());
    std::memcpy(g_host.sha, digest.data(), digest.size());
    g_host.transfer = RandomId();
    g_host.builtAt = now;
    g_host.folder = Narrow(*folder);
    Log("save transfer: snapshot %08x of %u files (%u bytes) from %s", g_host.transfer, static_cast<unsigned>(files.size()),
        static_cast<unsigned>(g_host.bundle.size()), g_host.folder.c_str());
    return true;
}

void SendOffer(const SaveTransferContext& context) {
    OfferPacket offer{MakeHeader(kOffer, context.session, g_host.transfer), static_cast<uint32_t>(g_host.bundle.size()), {}};
    std::memcpy(offer.sha, g_host.sha, sizeof offer.sha);
    context.net->SendRaw(&offer, sizeof offer);
}

void HostHandle(const Header& header, const char* data, int size, const SaveTransferContext& context) {
    if (header.type == kRequest) {
        if (!g_share) {
            if (!g_host.askedWithoutShare) Log("save transfer: partner asked for your save; HostSave=0 so it is not shared");
            g_host.askedWithoutShare = true;
            return;
        }
        if (context.now - g_host.builtAt > kSnapshotReuseSeconds && !BuildSnapshot(context.now)) return;
        SendOffer(context);
        return;
    }
    if (header.type != kPull || !g_share || g_host.bundle.empty()) return;
    if (header.transfer != g_host.transfer) {
        SendOffer(context); // the guest is pulling an older snapshot
        return;
    }
    if (size < static_cast<int>(sizeof(Header) + 2)) return;
    uint16_t count;
    std::memcpy(&count, data + sizeof(Header), 2);
    if (count > kMaxPull || size < static_cast<int>(sizeof(Header) + 2 + count * 4u)) return;
    uint32_t chunks = ChunkCount(static_cast<uint32_t>(g_host.bundle.size()));
    ChunkPacket chunk{};
    chunk.header = MakeHeader(kChunk, context.session, g_host.transfer);
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t index;
        std::memcpy(&index, data + sizeof(Header) + 2 + i * 4u, 4);
        if (index >= chunks) continue;
        size_t offset = static_cast<size_t>(index) * kChunkBytes;
        chunk.index = index;
        chunk.size = static_cast<uint16_t>(std::min(kChunkBytes, g_host.bundle.size() - offset));
        std::memcpy(chunk.data, g_host.bundle.data() + offset, chunk.size);
        context.net->SendRaw(&chunk, static_cast<int>(offsetof(ChunkPacket, data) + chunk.size));
    }
    g_host.lastServed = context.now;
}

void GuestHandle(const Header& header, const char* data, int size, const SaveTransferContext& context) {
    if (g_guest.state != GuestState::requesting && g_guest.state != GuestState::receiving) return;
    if (header.type == kOffer && size >= static_cast<int>(sizeof(OfferPacket))) {
        OfferPacket offer;
        std::memcpy(&offer, data, sizeof offer);
        bool fresh = header.transfer != g_guest.receiver.Transfer();
        if (!g_guest.receiver.Offer(header.transfer, offer.size, offer.sha)) {
            Log("save transfer: ignoring invalid offer (%u bytes)", offer.size);
            return;
        }
        if (fresh) Log("save transfer: host offered snapshot %08x (%u bytes)", header.transfer, offer.size);
        g_guest.state = GuestState::receiving;
        g_guest.lastProgress = context.now;
        return;
    }
    if (header.type == kChunk && size >= static_cast<int>(offsetof(ChunkPacket, data))) {
        uint32_t index;
        uint16_t length;
        std::memcpy(&index, data + offsetof(ChunkPacket, index), 4);
        std::memcpy(&length, data + offsetof(ChunkPacket, size), 2);
        if (length > kChunkBytes || size != static_cast<int>(offsetof(ChunkPacket, data) + length)) return;
        if (g_guest.receiver.Chunk(header.transfer, index,
                                   reinterpret_cast<const uint8_t*>(data + offsetof(ChunkPacket, data)), length))
            g_guest.lastProgress = context.now;
    }
}

void Fail(const std::string& reason) {
    g_guest.state = GuestState::failed;
    g_guest.error = reason;
    g_guest.receiver.Reset();
    Log("save transfer: %s", reason.c_str());
}

void Adopt() {
    std::vector<File> files;
    if (!ParseBundle(g_guest.receiver.Bundle(), files)) {
        Fail("host save bundle is malformed; not loading it");
        return;
    }
    std::vector<AdoptedFile> adopted;
    for (auto& file : files) adopted.push_back({std::wstring(file.name.begin(), file.name.end()), std::move(file.data)});
    std::string error;
    if (!SaveGuardAdopt(adopted, error)) {
        Fail("cannot install the host save: " + error);
        return;
    }
    g_guest.state = GuestState::adopted;
    g_guest.receiver.Reset();
    Log("save transfer: host save installed; Continue from the main menu to load it");
}

} // namespace

void SaveTransferConfigure(bool share, bool join) {
    g_share = share;
    g_join = join;
}

void SaveTransferInvalidate() { g_host.builtAt = -1e9; }

bool SaveTransferResync() {
    if (g_guest.state == GuestState::requesting || g_guest.state == GuestState::receiving) return false;
    g_guest = Guest{};
    return true;
}

bool SaveTransferHandle(const char* data, int size, const SaveTransferContext& context) {
    Header header;
    if (size < 4) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != savesync::kMagic) return false;
    if (!DecodeHeader(data, size, header) || !context.peerSession || header.session != context.peerSession) return true;
    if (context.host)
        HostHandle(header, data, size, context);
    else if (g_join)
        GuestHandle(header, data, size, context);
    return true;
}

void SaveTransferTick(const SaveTransferContext& context) {
    if (g_host.transfer && context.now - std::max(g_host.builtAt, g_host.lastServed) > kSnapshotDropSeconds) {
        g_host.bundle.clear();
        g_host.bundle.shrink_to_fit();
        g_host.transfer = 0;
        g_host.builtAt = -1e9;
    }
    if (!g_join || context.host || !SaveGuardActive()) return;
    if (g_guest.state == GuestState::idle && context.connected && context.peerSession) {
        g_guest.state = GuestState::requesting;
        g_guest.firstRequest = context.now;
        Log("save transfer: requesting the host's save");
    }
    if (!context.connected || !context.peerSession) return;
    if (g_guest.state == GuestState::requesting) {
        if (context.now >= g_guest.nextRequest) {
            g_guest.nextRequest = context.now + kRequestInterval;
            Header request = MakeHeader(kRequest, context.session, 0);
            context.net->SendRaw(&request, sizeof request);
        }
        return;
    }
    if (g_guest.state != GuestState::receiving) return;
    if (g_guest.receiver.Complete()) {
        if (context.loading) return;
        if (g_guest.receiver.Verified()) {
            Adopt();
            return;
        }
        if (++g_guest.verifyFailures >= kMaxVerifyFailures) {
            Fail("host save failed verification repeatedly");
            return;
        }
        Log("save transfer: snapshot digest mismatch; requesting it again");
        g_guest.receiver.Reset();
        g_guest.state = GuestState::requesting;
        return;
    }
    if (context.now - g_guest.lastProgress > kStallSeconds) {
        Log("save transfer: stalled; asking the host again");
        g_guest.receiver.Reset();
        g_guest.state = GuestState::requesting;
        return;
    }
    auto indices = g_guest.receiver.NextPull(context.now, kPullPerTick);
    if (indices.empty()) return;
    PullPacket pull{};
    pull.header = MakeHeader(kPull, context.session, g_guest.receiver.Transfer());
    pull.count = static_cast<uint16_t>(indices.size());
    std::copy(indices.begin(), indices.end(), pull.indices);
    context.net->SendRaw(&pull, static_cast<int>(offsetof(PullPacket, indices) + indices.size() * 4));
}

std::string SaveTransferStatus(const SaveTransferContext& context) {
    if (context.host) {
        if (g_host.askedWithoutShare && !g_share) return "Partner wants your save (HostSave=0)";
        if (g_share && context.now - g_host.lastServed < 3) return "Sending your save to partner";
        return {};
    }
    if (!g_join) return {};
    if (!SaveGuardActive()) return "Save sandbox failed; host save disabled";
    char buffer[96];
    switch (g_guest.state) {
    case GuestState::idle:
        return {};
    case GuestState::requesting:
        return context.now - g_guest.firstRequest > 10 ? "Waiting for host save (host has HostSave=0?)" :
                                                         "Requesting host save...";
    case GuestState::receiving:
        std::snprintf(buffer, sizeof buffer, "Receiving host save %d%%",
                      static_cast<int>(g_guest.receiver.Progress() * 100));
        return buffer;
    case GuestState::adopted:
        if (!SaveGuardPinned()) return "Playing on the host's save";
        return context.inGameplay ? "Loading the host save..." : "Host save ready: press Continue";
    case GuestState::failed:
        return g_guest.error;
    }
    return {};
}

} // namespace coop
