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
constexpr size_t kEnemyMaxBones = 159; // at most three packets per enemy, including @root
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

// Guest-to-host hit on a replicated enemy. The host applies the damage to its real pawn.
constexpr uint32_t kEnemyHitMagic = 0x48454D44; // "DMEH"
constexpr uint16_t kEnemyHitVersion = 1;
constexpr float kEnemyHitMaxDamage = 1000.f;
constexpr float kEnemyMeleeRange = 400.f;  // generous: guest and host positions differ by a few frames
constexpr float kEnemyShotRange = 15000.f;

#pragma pack(push, 1)
struct EnemyHitPacket {
    uint32_t magic;
    uint16_t version;
    uint8_t kind, reserved; // kind 1 melee, 2 gunshot
    uint32_t session, entity, sequence;
    float damage;
};
#pragma pack(pop)

inline bool DecodeEnemyHit(const char* data, int len, EnemyHitPacket& out) {
    if (len != static_cast<int>(sizeof(EnemyHitPacket))) return false;
    std::memcpy(&out, data, sizeof out);
    return out.magic == kEnemyHitMagic && out.version == kEnemyHitVersion && (out.kind == 1 || out.kind == 2) &&
        !out.reserved && out.session && out.entity && out.sequence && out.damage > 0 &&
        out.damage <= kEnemyHitMaxDamage;
}

inline EnemyHitPacket EncodeEnemyHit(uint32_t session, uint32_t entity, uint32_t sequence, bool melee, float damage) {
    EnemyHitPacket p{};
    p.magic = kEnemyHitMagic;
    p.version = kEnemyHitVersion;
    p.kind = melee ? 1 : 2;
    p.session = session;
    p.entity = entity;
    p.sequence = sequence;
    p.damage = damage;
    return p;
}

// Host-to-guest damage: host enemies attacked the guest's stand-in pawn in the host world.
constexpr uint32_t kGuestStrikeMagic = 0x58504D44; // "DMPX"
constexpr uint16_t kGuestStrikeVersion = 1;

#pragma pack(push, 1)
struct GuestStrikePacket {
    uint32_t magic;
    uint16_t version, reserved;
    uint32_t session, sequence;
    float damage;
};
#pragma pack(pop)

inline bool DecodeGuestStrike(const char* data, int len, GuestStrikePacket& out) {
    if (len != static_cast<int>(sizeof(GuestStrikePacket))) return false;
    std::memcpy(&out, data, sizeof out);
    return out.magic == kGuestStrikeMagic && out.version == kGuestStrikeVersion && !out.reserved && out.session &&
        out.sequence && out.damage > 0 && out.damage <= kEnemyHitMaxDamage;
}

inline GuestStrikePacket EncodeGuestStrike(uint32_t session, uint32_t sequence, float damage) {
    GuestStrikePacket p{};
    p.magic = kGuestStrikeMagic;
    p.version = kGuestStrikeVersion;
    p.session = session;
    p.sequence = sequence;
    p.damage = damage;
    return p;
}

// Hits travel twice over UDP; the receiver applies each sequence number once.
class EnemyHitDedupe {
public:
    bool Accept(uint32_t session, uint32_t sequence) {
        if (session != session_) {
            session_ = session;
            count_ = 0;
        }
        for (size_t i = 0; i < count_ && i < kWindow; ++i)
            if (seen_[i] == sequence) return false;
        seen_[count_++ % kWindow] = sequence;
        return true;
    }
private:
    static constexpr size_t kWindow = 64;
    uint32_t session_ = 0, seen_[kWindow]{};
    size_t count_ = 0;
};

// Distance along the aim ray to a standing body whose feet are at `feet`, or -1 when the ray misses its
// 50 cm radius, 190 cm tall cylinder. Direction must be normalized.
inline float AimDistanceToBody(const float start[3], const float direction[3], const float feet[3]) {
    float dx = feet[0] - start[0], dy = feet[1] - start[1];
    float horizontal = direction[0] * direction[0] + direction[1] * direction[1];
    if (horizontal < 1e-6f) return -1;
    // Closest approach of the ray to the body's vertical axis, measured horizontally.
    float t = (dx * direction[0] + dy * direction[1]) / horizontal;
    if (t < 0 || t > kEnemyShotRange) return -1;
    float ax = start[0] + direction[0] * t - feet[0], ay = start[1] + direction[1] * t - feet[1];
    float az = start[2] + direction[2] * t - feet[2];
    if (ax * ax + ay * ay > 50.f * 50.f || az < -10.f || az > 190.f) return -1;
    return t;
}

} // namespace coop
