#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop {

constexpr uint32_t kMagic = 0x4F434744; // "DGCO"
constexpr uint16_t kProtocolVersion = 1;

enum StateFlags : uint16_t {
    kHasPawn = 1 << 0,
    kInVehicle = 1 << 1,
};

#pragma pack(push, 1)
// Positions are absolute world coordinates (world-origin rebasing removed).
struct StatePacket {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t seq;
    uint32_t timeMs;
    float pos[3];
    float yaw;
    float pitch;
    float vel[3];
    char name[16];
};
#pragma pack(pop)
static_assert(sizeof(StatePacket) == 64, "wire format");

inline bool DecodeState(const char* data, int len, StatePacket& out) {
    if (len != static_cast<int>(sizeof(StatePacket))) return false;
    std::memcpy(&out, data, sizeof out);
    if (out.magic != kMagic || out.version != kProtocolVersion ||
        (out.flags & ~(kHasPawn | kInVehicle))) return false;
    const float values[] = {out.pos[0], out.pos[1], out.pos[2], out.yaw, out.pitch,
                            out.vel[0], out.vel[1], out.vel[2]};
    for (float f : values)
        if (!std::isfinite(f) || std::fabs(f) > 1e8f) return false;
    out.name[sizeof out.name - 1] = 0;
    return true;
}

inline bool NewerSequence(uint32_t incoming, uint32_t previous) {
    uint32_t delta = incoming - previous;
    return delta != 0 && delta < 0x80000000u;
}

} // namespace coop
