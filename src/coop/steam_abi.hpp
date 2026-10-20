#pragma once
// Minimal, hand-written Steamworks ABI used by the experimental Steam transport.
// Layouts, callback IDs and vtable slots were checked against the public Steamworks SDK headers
// (isteammatchmaking.h, isteamfriends.h, isteamuser.h, isteamutils.h, isteamnetworking.h,
// isteamnetworkingmessages.h, steamnetworkingtypes.h). No SDK code is included.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace coop::steam {

constexpr uint32_t kDaysGoneAppId = 1259420;

// Interface versions requested from the running Steam client.
constexpr const char* kMatchmakingVersion = "SteamMatchMaking009";
constexpr const char* kFriendsVersion = "SteamFriends018";
constexpr const char* kUserVersion = "SteamUser020";
constexpr const char* kUtilsVersion = "SteamUtils011";
constexpr const char* kMessagesVersion = "SteamNetworkingMessages002";
constexpr const char* kNetworkingVersion = "SteamNetworking006";

// Callback IDs: k_iSteamFriendsCallbacks 300, k_iSteamMatchmakingCallbacks 500,
// k_iSteamNetworkingCallbacks 1200, k_iSteamNetworkingMessagesCallbacks 1250.
constexpr int kGameLobbyJoinRequested = 300 + 33;
constexpr int kLobbyEnter = 500 + 4;
constexpr int kLobbyCreated = 500 + 13;
constexpr int kLobbyMatchList = 500 + 10;
constexpr int kP2PSessionRequest = 1200 + 2;
constexpr int kMessagesSessionRequest = 1250 + 1;

// Vtable slots (MSVC order; each interface is a single-inheritance pure virtual class).
namespace slot {
constexpr int kRequestLobbyList = 4, kAddRequestLobbyListStringFilter = 5, kAddRequestLobbyListFilterSlotsAvailable = 8,
              kAddRequestLobbyListDistanceFilter = 9, kAddRequestLobbyListResultCountFilter = 10,
              kGetLobbyByIndex = 12, kSetLobbyType = 33;
constexpr int kCreateLobby = 13, kJoinLobby = 14, kLeaveLobby = 15, kGetNumLobbyMembers = 17,
              kGetLobbyMemberByIndex = 18, kGetLobbyData = 19, kSetLobbyData = 20, kSetLobbyJoinable = 34,
              kGetLobbyOwner = 35;
constexpr int kGetPersonaName = 0;
constexpr int kGetFriendCount = 2, kGetFriendByIndex = 3, kGetFriendPersonaName = 6, kGetFriendGamePlayed = 7,
              kActivateGameOverlayInviteDialog = 32;
constexpr int kGetSteamID = 2;
constexpr int kGetAppID = 8, kIsAPICallCompleted = 10, kGetAPICallResult = 12;
constexpr int kSendMessageToUser = 0, kReceiveMessagesOnChannel = 1, kAcceptSessionWithUser = 2,
              kCloseSessionWithUser = 3;
constexpr int kSendP2PPacket = 0, kIsP2PPacketAvailable = 1, kReadP2PPacket = 2, kAcceptP2PSessionWithUser = 3,
              kCloseP2PSessionWithUser = 4;
} // namespace slot

constexpr int kLobbyComparisonEqual = 0;
constexpr int kLobbyDistanceWorldwide = 3;
constexpr uint32_t kResultOK = 1;
constexpr uint32_t kChatRoomEnterSuccess = 1;
constexpr int kFriendFlagImmediate = 0x04;
constexpr int kSendUnreliableNoNagle = 1;
constexpr int kSendAutoRestartBrokenSession = 32;
constexpr int kP2PSendUnreliable = 0;
constexpr int kIdentityTypeSteamID = 16;

#pragma pack(push, 8)
struct LobbyCreated {
    uint32_t result;
    uint64_t lobby;
};
struct LobbyMatchList {
    uint32_t count;
};
struct LobbyEnter {
    uint64_t lobby;
    uint32_t permissions;
    bool locked;
    uint32_t response;
};
struct GameLobbyJoinRequested {
    uint64_t lobby;
    uint64_t friendId;
};
struct P2PSessionRequest {
    uint64_t remote;
};
struct FriendGameInfo {
    uint64_t gameId; // CGameID; the low 24 bits are the app ID
    uint32_t ip;
    uint16_t port;
    uint16_t queryPort;
    uint64_t lobby;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct NetworkingIdentity {
    int32_t type;
    int32_t size;
    union {
        uint64_t steamId;
        char raw[128];
    };
};
#pragma pack(pop)

struct NetworkingMessage {
    void* data;
    int32_t size;
    uint32_t connection;
    NetworkingIdentity peer;
    int64_t connectionUserData;
    int64_t receivedMicroseconds;
    int64_t messageNumber;
    void (*freeData)(NetworkingMessage*);
    void (*release)(NetworkingMessage*);
    int32_t channel;
    int32_t flags;
    int64_t userData;
    uint16_t lane;
    uint16_t padding;
};

// CCallbackBase as laid out by MSVC: vptr, uint8 m_nCallbackFlags, int m_iCallback.
struct CallbackObject;
struct CallbackVTable {
    // MSVC places overloaded virtuals in reverse declaration order.
    void (*runResult)(CallbackObject* self, void* param, bool ioFailure, uint64_t call);
    void (*run)(CallbackObject* self, void* param);
    int (*size)(CallbackObject* self);
};
struct CallbackObject {
    const CallbackVTable* vtable;
    uint8_t flags;
    int32_t id;
    int32_t bytes;
    void (*handler)(int id, const void* param);
};

static_assert(sizeof(LobbyCreated) == 16 && offsetof(LobbyCreated, lobby) == 8);
static_assert(sizeof(LobbyEnter) == 24 && offsetof(LobbyEnter, locked) == 12 && offsetof(LobbyEnter, response) == 16);
static_assert(sizeof(GameLobbyJoinRequested) == 16);
static_assert(sizeof(FriendGameInfo) == 24 && offsetof(FriendGameInfo, lobby) == 16);
static_assert(sizeof(NetworkingIdentity) == 136 && offsetof(NetworkingIdentity, steamId) == 8);
static_assert(offsetof(NetworkingMessage, peer) == 16 && offsetof(NetworkingMessage, connectionUserData) == 152 &&
              offsetof(NetworkingMessage, release) == 184 && offsetof(NetworkingMessage, channel) == 192);
static_assert(offsetof(CallbackObject, flags) == 8 && offsetof(CallbackObject, id) == 12);

inline uint32_t GameAppId(uint64_t gameId) {
    return static_cast<uint32_t>(gameId & 0xFFFFFF);
}

// Steam launches a game with "+connect_lobby <64-bit lobby id>" when a friend accepts an invite
// while the game is not running. Returns 0 when absent or malformed.
inline uint64_t ParseConnectLobby(std::string_view commandLine) {
    constexpr std::string_view key = "+connect_lobby";
    size_t at = commandLine.find(key);
    if (at == std::string_view::npos) return 0;
    size_t i = at + key.size();
    if (i >= commandLine.size() || (commandLine[i] != ' ' && commandLine[i] != '\t')) return 0;
    while (i < commandLine.size() && (commandLine[i] == ' ' || commandLine[i] == '\t')) ++i;
    uint64_t value = 0;
    size_t digits = 0;
    for (; i < commandLine.size() && commandLine[i] >= '0' && commandLine[i] <= '9'; ++i, ++digits) {
        uint64_t digit = static_cast<uint64_t>(commandLine[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if (!digits || (i < commandLine.size() && commandLine[i] != ' ' && commandLine[i] != '\t' && commandLine[i] != '"'))
        return 0;
    return value;
}

} // namespace coop::steam
