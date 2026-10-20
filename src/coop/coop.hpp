#pragma once
#include <cstdint>
#include <string>

namespace coop {

// Supported executable: Steam build 19221447.
constexpr uint32_t kExpectedTimestamp = 0x68754d46;
constexpr uint32_t kExpectedImageSize = 0x093f0000;
constexpr uintptr_t kProcessEventRva = 0x1AC2A20;
constexpr uintptr_t kObjectArrayRva = 0x4C12290;

struct Config {
    uint32_t port = 27777;
    std::string peerHost;
    uint32_t peerPort = 27777;
    std::string playerName = "Deacon";
    bool discovery = false;
    bool debugTools = false;
    std::string animIdle = "dcn_idle_normal";
    std::string animWalk = "dcn_walk";
    std::string animRun = "/Game/Animation/Deacon/run/run.Run";
    std::string animSprint = "/Game/Animation/Deacon/run/sprint.Sprint";
    int teleportKey = 0x76; // VK_F7
    int poseRate = 20;      // player and bike pose updates per second (10-30)
    bool poseFace = false;  // also stream the face rig (about 280 extra bones)
    bool steam = false;     // Transport=steam: experimental Steam lobby + Steam Networking instead of LAN UDP
    bool steamPumpCallbacks = false; // also run SteamAPI_RunCallbacks from the mod (only if invites never arrive)
};

Config LoadConfig(const std::wstring& iniPath);
bool Install(const std::wstring& basePath);

} // namespace coop
