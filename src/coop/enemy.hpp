#pragma once

#include "pose.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace coop {

// Host-to-client enemy replication: poses travel as kPoseEnemy frames, appearance as EnemyInfoPacket.
constexpr uint32_t kEnemyInfoMagic = 0x4E454D44; // "DMEN"
constexpr uint16_t kEnemyInfoVersion = 1;
constexpr size_t kEnemyMeshPaths = 4;      // body plus up to three modular parts
constexpr size_t kEnemyPathLength = 120;
constexpr size_t kEnemyMaxBones = 2 * kPoseBonesPerPacket - 1; // at most two packets per enemy, including @root
constexpr size_t kEnemyMaxStreamed = 16;
constexpr float kEnemyRadius = 8000.f;     // 80 m around either player
constexpr double kEnemySendInterval = 1.0 / 15;
constexpr double kEnemyInfoInterval = 1.0;
constexpr double kEnemyTimeout = 1.5;

#pragma pack(push, 1)
struct EnemyInfoPacket {
    uint32_t magic;
    uint16_t version;
    uint8_t paths, reserved;
    uint32_t session, entity;
    char path[kEnemyMeshPaths][kEnemyPathLength]; // "Package.Asset" object paths of SkeletalMesh assets
};
#pragma pack(pop)

inline bool DecodeEnemyInfo(const char* data, int len, EnemyInfoPacket& out) {
    if (len != static_cast<int>(sizeof(EnemyInfoPacket))) return false;
    std::memcpy(&out, data, sizeof out);
    if (out.magic != kEnemyInfoMagic || out.version != kEnemyInfoVersion || !out.entity || !out.paths ||
        out.paths > kEnemyMeshPaths || out.reserved)
        return false;
    for (size_t i = 0; i < kEnemyMeshPaths; ++i) {
        const char* p = out.path[i];
        size_t n = strnlen(p, kEnemyPathLength);
        if (n == kEnemyPathLength) return false;
        if (i < out.paths && (n < 3 || p[0] != '/' || !std::memchr(p, '.', n))) return false;
        if (i >= out.paths && n) return false;
        for (size_t c = 0; c < n; ++c)
            if (p[c] < 0x21 || p[c] > 0x7e) return false;
    }
    return true;
}

inline bool EncodeEnemyInfo(uint32_t session, uint32_t entity, const std::vector<std::string>& paths, EnemyInfoPacket& out) {
    out = EnemyInfoPacket{};
    out.magic = kEnemyInfoMagic;
    out.version = kEnemyInfoVersion;
    out.session = session;
    out.entity = entity;
    for (const auto& p : paths) {
        if (out.paths == kEnemyMeshPaths || p.size() >= kEnemyPathLength) continue;
        std::memcpy(out.path[out.paths++], p.data(), p.size());
    }
    EnemyInfoPacket check;
    return DecodeEnemyInfo(reinterpret_cast<const char*>(&out), sizeof out, check);
}

// Face rig, tongue, eyes, fingers and camera/reference helpers do not change an enemy's silhouette.
inline bool EnemyBoneSkipped(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* term : {"face", "tongue", "palate", "eye", "thumb", "index", "ring", "pinky", "middle", "cam0", "ref0", "lip", "brow", "cheek"})
        if (lower.find(term) != std::string::npos) return true;
    return false;
}

// Bones to stream: skipped bones drop their whole subtree so every sent bone's parent is sent too.
inline std::vector<int> EnemyBoneSubset(const std::vector<std::string>& names, const std::vector<int>& parents) {
    std::vector<int> subset;
    std::vector<bool> kept(names.size(), false);
    for (size_t i = 0; i < names.size() && subset.size() < kEnemyMaxBones; ++i) {
        int parent = i < parents.size() ? parents[i] : -1;
        bool parentKept = parent < 0 || (static_cast<size_t>(parent) < i && kept[parent]);
        if (!parentKept || EnemyBoneSkipped(names[i])) continue;
        kept[i] = true;
        subset.push_back(static_cast<int>(i));
    }
    return subset;
}

} // namespace coop
