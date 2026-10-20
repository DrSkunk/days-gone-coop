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
    std::string lobbyVisibility = "friends"; // initial Steam lobby visibility: private, friends or public
    std::string playerName;        // empty: the Steam persona name (or "Deacon" without Steam)
    bool discovery = false;
    bool missionDiscovery = false; // log runtime mission/save UFunction signatures; read-only
    bool menuDiscovery = false;    // F6 dumps native menu classes and live widget references
    bool guestSaveGuard = false;   // experimental: redirect SaveGames writes to a session sandbox
    bool shareSave = false;        // experimental: host serves its saves to a GuestSaveGuard partner
    bool debugTools = false;
    std::string animIdle = "dcn_idle_normal";
    std::string animWalk = "dcn_walk";
    std::string animRun = "/Game/Animation/Deacon/run/run.Run";
    std::string animSprint = "/Game/Animation/Deacon/run/sprint.Sprint";
    int teleportKey = 0x76; // VK_F7
    int poseRate = 20;      // player and bike pose updates per second (10-30)
    bool poseFace = false;  // also stream the face rig (about 280 extra bones)
    bool autoLan = false;   // Transport=udp: start a LAN session at launch (host, or join PeerHost)
    bool steamPumpCallbacks = false; // also run SteamAPI_RunCallbacks from the mod (only if invites never arrive)
};

Config LoadConfig(const std::wstring& iniPath);
bool Install(const std::wstring& basePath);

} // namespace coop
