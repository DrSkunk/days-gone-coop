#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop {

// Held-item mirroring: which mesh a player holds, the body socket it hangs from and its offset from that socket.
constexpr uint32_t kGearMagic = 0x52474D44; // "DMGR"
constexpr uint16_t kGearVersion = 1;
constexpr size_t kGearSocketLength = 64;
constexpr size_t kGearPathLength = 120;
constexpr double kGearInterval = 0.5;      // resent while unchanged so a lost datagram heals quickly
constexpr double kGearRetry = 2.0;         // spawn retry when the asset is not loaded yet

enum GearKind : uint8_t { kGearNone = 0, kGearSkeletal = 1, kGearStatic = 2 };

#pragma pack(push, 1)
struct GearPacket {
    uint32_t magic;
    uint16_t version;
    uint8_t kind, reserved;
    uint32_t session, socketNumber;
    float q[4];      // offset rotation from the socket, x y z w
    float t[3];      // offset translation from the socket, cm
    float scale[3];  // item scale relative to the body mesh
    char socket[kGearSocketLength];
    char path[kGearPathLength]; // "Package.Asset" of a SkeletalMesh or StaticMesh
};
#pragma pack(pop)

namespace detail {
inline bool GearText(const char* p, size_t cap, size_t& n) {
    n = strnlen(p, cap);
    if (n == cap) return false;
    for (size_t c = 0; c < n; ++c)
        if (p[c] < 0x21 || p[c] > 0x7e) return false;
    return true;
}
} // namespace detail

inline bool DecodeGear(const char* data, int len, GearPacket& out) {
    if (len != static_cast<int>(sizeof(GearPacket))) return false;
    std::memcpy(&out, data, sizeof out);
    if (out.magic != kGearMagic || out.version != kGearVersion || out.kind > kGearStatic || out.reserved) return false;
    size_t socket = 0, path = 0;
    if (!detail::GearText(out.socket, kGearSocketLength, socket) || !detail::GearText(out.path, kGearPathLength, path))
        return false;
    if (out.kind == kGearNone) return !socket && !path && !out.socketNumber;
    if (!socket || path < 3 || out.path[0] != '/' || !std::memchr(out.path, '.', path)) return false;
    float norm = 0;
    for (float f : out.q) {
        if (!std::isfinite(f)) return false;
        norm += f * f;
    }
    if (std::fabs(norm - 1) > 0.01f) return false;
    for (float f : out.t)
        if (!std::isfinite(f) || std::fabs(f) > 500) return false;
    for (float f : out.scale)
        if (!std::isfinite(f) || f < 0.01f || f > 100) return false;
    return true;
}

inline bool EncodeGear(uint32_t session, uint8_t kind, const std::string& socket, uint32_t socketNumber,
                       const std::string& path, const float q[4], const float t[3], const float scale[3], GearPacket& out) {
    out = GearPacket{};
    out.magic = kGearMagic;
    out.version = kGearVersion;
    out.session = session;
    out.kind = kind;
    if (kind != kGearNone) {
        if (socket.size() >= kGearSocketLength || path.size() >= kGearPathLength) return false;
        std::memcpy(out.socket, socket.data(), socket.size());
        std::memcpy(out.path, path.data(), path.size());
        out.socketNumber = socketNumber;
        std::memcpy(out.q, q, sizeof out.q);
        std::memcpy(out.t, t, sizeof out.t);
        std::memcpy(out.scale, scale, sizeof out.scale);
    }
    GearPacket check;
    return DecodeGear(reinterpret_cast<const char*>(&out), sizeof out, check);
}

// Same item at the same place: avoids rebuilding the remote mesh for sub-millimetre jitter.
inline bool SameGear(const GearPacket& a, const GearPacket& b) {
    if (a.kind != b.kind || a.socketNumber != b.socketNumber || std::strncmp(a.socket, b.socket, kGearSocketLength) ||
        std::strncmp(a.path, b.path, kGearPathLength))
        return false;
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += a.q[i] * b.q[i];
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a.t[i] - b.t[i]) > 0.1f || std::fabs(a.scale[i] - b.scale[i]) > 0.001f) return false;
    return std::fabs(dot) > 0.99999f;
}

} // namespace coop
