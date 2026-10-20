#pragma once

#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Host-to-guest save transfer: wire format, bundle format and the guest's pull-based receiver.
// Pure code (no Win32) so it can be unit-tested.
namespace coop::savesync {

constexpr uint32_t kMagic = 0x56534744; // "DGSV"
constexpr uint16_t kVersion = 1;
constexpr size_t kChunkBytes = 1024;
constexpr size_t kMaxFiles = 32;
constexpr size_t kNameBytes = 32;
constexpr uint32_t kMaxBundleBytes = 64u << 20;
constexpr size_t kMaxPull = 128;
constexpr double kPullRetrySeconds = 0.5;

enum Type : uint8_t { kRequest = 1, kOffer = 2, kPull = 3, kChunk = 4 };

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint8_t type;
    uint8_t reserved;
    uint32_t session;  // sender's session id
    uint32_t transfer; // 0 in a request
};
struct OfferPacket {
    Header header;
    uint32_t size;
    uint8_t sha[32];
};
struct PullPacket {
    Header header;
    uint16_t count;
    uint32_t indices[kMaxPull];
};
struct ChunkPacket {
    Header header;
    uint32_t index;
    uint16_t size;
    uint8_t data[kChunkBytes];
};
struct BundleEntry {
    char name[kNameBytes];
    uint32_t size;
    uint8_t sha[32];
};
#pragma pack(pop)
static_assert(sizeof(Header) == 16 && sizeof(OfferPacket) == 52 && sizeof(BundleEntry) == 68, "wire format");
static_assert(sizeof(ChunkPacket) <= 1100, "chunk datagrams stay below the safe internet payload");

struct File {
    std::string name;
    std::vector<uint8_t> data;
};

inline Header MakeHeader(Type type, uint32_t session, uint32_t transfer) {
    return Header{kMagic, kVersion, type, 0, session, transfer};
}

inline uint32_t ChunkCount(uint32_t size) { return static_cast<uint32_t>((size + kChunkBytes - 1) / kChunkBytes); }

// Save names are plain "*.sav" file names; anything that could be a path is refused.
inline bool ValidName(const std::string& name) {
    if (name.size() < 5 || name.size() >= kNameBytes || name.front() == '.') return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return false;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
    if (!lower.ends_with(".sav") || lower.find("..") != std::string::npos) return false;
    // Windows (and Wine) treat reserved device names as devices even with an extension.
    std::string stem = lower.substr(0, lower.find('.'));
    for (const char* device : {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"})
        if (stem == device) return false;
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')
        return false;
    return true;
}

// Bundle: uint32 file count, one BundleEntry per file, then the file contents in order.
inline std::vector<uint8_t> BuildBundle(const std::vector<File>& files) {
    if (files.empty() || files.size() > kMaxFiles) return {};
    uint64_t total = 4 + files.size() * sizeof(BundleEntry);
    for (const auto& file : files) {
        if (!ValidName(file.name)) return {};
        total += file.data.size();
    }
    if (total > kMaxBundleBytes) return {};
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(total));
    uint32_t count = static_cast<uint32_t>(files.size());
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&count), reinterpret_cast<uint8_t*>(&count) + 4);
    for (const auto& file : files) {
        BundleEntry entry{};
        std::memcpy(entry.name, file.name.data(), file.name.size());
        entry.size = static_cast<uint32_t>(file.data.size());
        auto digest = Sha256::Of(file.data.data(), file.data.size());
        std::memcpy(entry.sha, digest.data(), digest.size());
        auto bytes = reinterpret_cast<const uint8_t*>(&entry);
        out.insert(out.end(), bytes, bytes + sizeof entry);
    }
    for (const auto& file : files) out.insert(out.end(), file.data.begin(), file.data.end());
    return out;
}

inline bool ParseBundle(const std::vector<uint8_t>& bundle, std::vector<File>& result) {
    result.clear();
    std::vector<File> out;
    if (bundle.size() < 4 || bundle.size() > kMaxBundleBytes) return false;
    uint32_t count;
    std::memcpy(&count, bundle.data(), 4);
    if (!count || count > kMaxFiles) return false;
    size_t offset = 4 + static_cast<size_t>(count) * sizeof(BundleEntry);
    if (offset > bundle.size()) return false;
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < count; ++i) {
        BundleEntry entry;
        std::memcpy(&entry, bundle.data() + 4 + i * sizeof(BundleEntry), sizeof entry);
        size_t length = strnlen(entry.name, kNameBytes);
        if (length == kNameBytes) return false;
        std::string name(entry.name, length), lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
        if (!ValidName(name) || std::find(seen.begin(), seen.end(), lower) != seen.end()) return false;
        seen.push_back(lower);
        if (entry.size > bundle.size() - offset) return false;
        File file{name, std::vector<uint8_t>(bundle.begin() + static_cast<std::ptrdiff_t>(offset),
                                             bundle.begin() + static_cast<std::ptrdiff_t>(offset + entry.size))};
        auto digest = Sha256::Of(file.data.data(), file.data.size());
        if (std::memcmp(digest.data(), entry.sha, digest.size()) != 0) return false;
        offset += entry.size;
        out.push_back(std::move(file));
    }
    if (offset != bundle.size()) return false;
    result = std::move(out);
    return true;
}

inline bool DecodeHeader(const char* data, int size, Header& out) {
    if (size < static_cast<int>(sizeof(Header))) return false;
    std::memcpy(&out, data, sizeof out);
    return out.magic == kMagic && out.version == kVersion && out.type >= kRequest && out.type <= kChunk &&
           out.session != 0 && (out.type == kRequest || out.transfer != 0);
}

// Guest side: tracks one offered bundle and decides which chunks to pull next.
class Receiver {
public:
    void Reset() { *this = Receiver{}; }

    // Starts (or keeps) the offered transfer. Returns false for an invalid offer.
    bool Offer(uint32_t transfer, uint32_t size, const uint8_t* sha) {
        if (!transfer || !size || size > kMaxBundleBytes) return false;
        if (transfer == transfer_ && size == bundle_.size()) return true;
        Reset();
        transfer_ = transfer;
        bundle_.assign(size, 0);
        std::memcpy(sha_, sha, 32);
        uint32_t chunks = ChunkCount(size);
        have_.assign(chunks, false);
        requested_.assign(chunks, -1e9);
        return true;
    }

    // Stores one chunk; returns true if it was new and valid.
    bool Chunk(uint32_t transfer, uint32_t index, const uint8_t* data, size_t size) {
        if (!transfer_ || transfer != transfer_ || index >= have_.size() || have_[index]) return false;
        size_t offset = static_cast<size_t>(index) * kChunkBytes;
        size_t expected = std::min(kChunkBytes, bundle_.size() - offset);
        if (size != expected) return false;
        std::memcpy(bundle_.data() + offset, data, size);
        have_[index] = true;
        ++received_;
        return true;
    }

    // Missing chunks that were not requested within the retry window, in order, starting at a rolling cursor.
    std::vector<uint32_t> NextPull(double now, size_t max) {
        std::vector<uint32_t> out;
        size_t chunks = have_.size();
        for (size_t step = 0; step < chunks && out.size() < max; ++step) {
            size_t index = (cursor_ + step) % chunks;
            if (have_[index] || now - requested_[index] < kPullRetrySeconds) continue;
            requested_[index] = now;
            out.push_back(static_cast<uint32_t>(index));
        }
        if (!out.empty()) cursor_ = (out.back() + 1) % chunks;
        return out;
    }

    bool Active() const { return transfer_ != 0; }
    bool Complete() const { return transfer_ && received_ == have_.size(); }
    bool Verified() const {
        if (!Complete()) return false;
        auto digest = Sha256::Of(bundle_.data(), bundle_.size());
        return std::memcmp(digest.data(), sha_, 32) == 0;
    }
    uint32_t Transfer() const { return transfer_; }
    float Progress() const { return have_.empty() ? 0.f : static_cast<float>(received_) / have_.size(); }
    const std::vector<uint8_t>& Bundle() const { return bundle_; }

private:
    uint32_t transfer_ = 0;
    std::vector<uint8_t> bundle_;
    uint8_t sha_[32]{};
    std::vector<bool> have_;
    std::vector<double> requested_;
    size_t received_ = 0, cursor_ = 0;
};

} // namespace coop::savesync
