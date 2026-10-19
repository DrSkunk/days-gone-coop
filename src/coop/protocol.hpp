#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop {

constexpr uint32_t kMagic = 0x4F434744; // "DGCO"
constexpr uint16_t kProtocolVersion = 4;

enum StateFlags : uint16_t {
    kHasPawn = 1 << 0,
    kInVehicle = 1 << 1,
    kHasBike = 1 << 2,
    kPassenger = 1 << 3,
    kFriendlyFire = 1 << 4,
    kAcceptPassengers = 1 << 5,
    kAlive = 1 << 6,
    kSyncBikes = 1 << 7,
    kWorldState = 1 << 8,
};

enum ActionFlags : uint8_t {
    kAiming = 1 << 0,
    kCrouched = 1 << 1,
    kReloading = 1 << 2,
};

enum WeaponType : uint8_t {
    kWeaponNone = 0,
    kWeaponGun = 1,
    kWeaponCrossbow = 2,
    kWeaponMelee = 3,
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
    uint32_t session;
    uint32_t targetSession;
    uint32_t consent;
    uint32_t targetConsent;
    uint32_t attackId;
    uint32_t attackAck;
    float damage;
    float bikePos[3];
    float bikeRotation[3]; // pitch, yaw, roll
    uint8_t jacket, trousers, capColor, cap;
    uint32_t attackKind; // 0 = none, 1 = melee, 2 = gunshot
    float health;
    uint8_t actions;
    uint8_t weaponType;
    uint32_t shotSequence;
    float worldTimeOfDay;
    float worldRelativeTime;
    uint8_t worldWeather;
    uint8_t worldTransitionRate;
};
#pragma pack(pop)
static_assert(sizeof(StatePacket) == 144, "wire format");

inline bool DecodeState(const char* data, int len, StatePacket& out) {
    if (len != static_cast<int>(sizeof(StatePacket))) return false;
    std::memcpy(&out, data, sizeof out);
    if (out.magic != kMagic || out.version != kProtocolVersion ||
        (out.flags & ~(kHasPawn | kInVehicle | kHasBike | kPassenger | kFriendlyFire |
                       kAcceptPassengers | kAlive | kSyncBikes | kWorldState))) return false;
    const float values[] = {out.pos[0], out.pos[1], out.pos[2], out.yaw, out.pitch,
                            out.vel[0], out.vel[1], out.vel[2], out.bikePos[0], out.bikePos[1], out.bikePos[2],
                            out.bikeRotation[0], out.bikeRotation[1], out.bikeRotation[2], out.damage, out.health,
                            out.worldTimeOfDay, out.worldRelativeTime};
    for (float f : values)
        if (!std::isfinite(f) || std::fabs(f) > 1e8f) return false;
    if (out.jacket > 6 || out.trousers > 6 || out.capColor > 6 || out.cap > 1 ||
        out.damage < 0 || out.damage > 1000 || out.health < 0 || out.health > 100000 || out.attackKind > 2 ||
        (out.actions & ~(kAiming | kCrouched | kReloading)) || out.weaponType > kWeaponMelee) return false;
    if ((out.flags & kWorldState) &&
        (out.worldTimeOfDay < 0 || out.worldTimeOfDay > 24 ||
         out.worldRelativeTime < 0 || out.worldRelativeTime > 1)) return false;
    if ((out.flags & kPassenger) && (!(out.flags & kInVehicle) || !(out.flags & kHasPawn))) return false;
    if ((out.flags & kInVehicle) && !(out.flags & kHasPawn)) return false;
    if (out.attackKind == 0) {
        if (out.attackId || out.damage != 0) return false;
    } else if (!out.attackId || !out.session || !out.targetSession || !out.consent || !out.targetConsent ||
               !(out.flags & kFriendlyFire) || out.damage <= 0) return false;
    out.name[sizeof out.name - 1] = 0;
    return true;
}

inline bool NewerSequence(uint32_t incoming, uint32_t previous) {
    uint32_t delta = incoming - previous;
    return delta != 0 && delta < 0x80000000u;
}

} // namespace coop
