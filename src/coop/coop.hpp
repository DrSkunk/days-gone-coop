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
};

Config LoadConfig(const std::wstring& iniPath);
bool Install(const std::wstring& basePath);

} // namespace coop
