#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace coop {

enum class DebugCommand {
    unknown, inspect, begin, end, status, godOn, godOff, spawnSingle, spawnPack,
    cleanup, probePlayer, probeEnemy, timeShift, weatherNext
};

inline DebugCommand ParseDebugCommand(std::string_view text) {
    if (text == "inspect") return DebugCommand::inspect;
    if (text == "begin") return DebugCommand::begin;
    if (text == "end") return DebugCommand::end;
    if (text == "status") return DebugCommand::status;
    if (text == "god-on") return DebugCommand::godOn;
    if (text == "god-off") return DebugCommand::godOff;
    if (text == "spawn-single") return DebugCommand::spawnSingle;
    if (text == "spawn-pack") return DebugCommand::spawnPack;
    if (text == "cleanup") return DebugCommand::cleanup;
    if (text == "probe-player") return DebugCommand::probePlayer;
    if (text == "probe-enemy") return DebugCommand::probeEnemy;
    if (text == "time-shift") return DebugCommand::timeShift;
    if (text == "weather-next") return DebugCommand::weatherNext;
    return DebugCommand::unknown;
}

class DebugRequestGate {
public:
    void Initialize(uint32_t request) { last_ = request; }
    bool Accept(uint32_t request) {
        if (!request || request == last_) return false;
        last_ = request;
        return true;
    }
private:
    uint32_t last_ = 0;
};

constexpr size_t kDebugEnemyLimit = 16;
constexpr size_t kDebugPackSize = 8;

inline uint8_t DebugDamageMask(uint8_t before, uint8_t after) {
    uint8_t mask = before ^ after;
    return mask && !(mask & (mask - 1)) ? mask : 0;
}

inline uint8_t DebugMaskedFlag(uint8_t byte, uint8_t mask, bool value) {
    return static_cast<uint8_t>((byte & ~mask) | (value ? mask : 0));
}

} // namespace coop
