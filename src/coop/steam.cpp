#include "steam.hpp"
#include "steam_abi.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>

namespace coop {
namespace {

using namespace steam;

using GetHSteamUserFn = int32_t (*)();
using FindInterfaceFn = void* (*)(int32_t, const char*);
using RegisterCallbackFn = void (*)(void*, int);
using RunCallbacksFn = void (*)();

struct Api {
    RegisterCallbackFn registerCallback = nullptr;
    RunCallbacksFn runCallbacks = nullptr;
    void* matchmaking = nullptr;
    void* friends = nullptr;
    void* user = nullptr;
    void* utils = nullptr;
    void* messages = nullptr;
    void* networking = nullptr;
} api;

template <typename R, typename... A>
R VCall(void* self, int index, A... args) {
    using Fn = R (*)(void*, A...);
    return (*static_cast<Fn**>(self))[index](self, args...);
}

// MSVC returns class types from member functions through a hidden pointer passed after `this`.
template <typename... A>
uint64_t VCallSteamId(void* self, int index, A... args) {
    using Fn = uint64_t* (*)(void*, uint64_t*, A...);
    uint64_t out = 0;
    (*static_cast<Fn**>(self))[index](self, &out, args...);
    return out;
}

struct Event {
    int id = 0;
    uint64_t a = 0, b = 0;
    uint32_t c = 0;
};
std::mutex eventMutex;
std::vector<Event> events;

// Steam may dispatch on whichever thread the game pumps callbacks from; only queue here.
void OnCallback(int id, const void* param) {
    Event e{id};
    switch (id) {
    case kGameLobbyJoinRequested: {
        GameLobbyJoinRequested v;
        std::memcpy(&v, param, sizeof v);
        e.a = v.lobby;
        e.b = v.friendId;
        break;
    }
    case kLobbyEnter: {
        LobbyEnter v;
        std::memcpy(&v, param, sizeof v);
        e.a = v.lobby;
        e.c = v.response;
        break;
    }
    case kLobbyCreated: {
        LobbyCreated v;
        std::memcpy(&v, param, sizeof v);
        e.a = v.lobby;
        e.c = v.result;
        break;
    }
    case kP2PSessionRequest: {
        P2PSessionRequest v;
        std::memcpy(&v, param, sizeof v);
        e.a = v.remote;
        break;
    }
    case kMessagesSessionRequest: {
        NetworkingIdentity v;
        std::memcpy(&v, param, sizeof v);
        if (v.type != kIdentityTypeSteamID) return;
        e.a = v.steamId;
        break;
    }
    default:
        return;
    }
    std::lock_guard lock(eventMutex);
    if (events.size() < 64) events.push_back(e);
}

void CallbackRun(CallbackObject* self, void* param) {
    if (param) self->handler(self->id, param);
}
void CallbackRunResult(CallbackObject* self, void* param, bool ioFailure, uint64_t) {
    if (!ioFailure) CallbackRun(self, param);
}
int CallbackSize(CallbackObject* self) {
    return self->bytes;
}
constexpr CallbackVTable kCallbackVTable{CallbackRunResult, CallbackRun, CallbackSize};

CallbackObject callbacks[] = {
    {&kCallbackVTable, 0, kGameLobbyJoinRequested, sizeof(GameLobbyJoinRequested), OnCallback},
    {&kCallbackVTable, 0, kLobbyEnter, sizeof(LobbyEnter), OnCallback},
    {&kCallbackVTable, 0, kLobbyCreated, sizeof(LobbyCreated), OnCallback},
    {&kCallbackVTable, 0, kP2PSessionRequest, sizeof(P2PSessionRequest), OnCallback},
    {&kCallbackVTable, 0, kMessagesSessionRequest, sizeof(NetworkingIdentity), OnCallback},
};

NetworkingIdentity Identity(uint64_t user) {
    NetworkingIdentity id{};
    id.type = kIdentityTypeSteamID;
    id.size = sizeof(uint64_t);
    id.steamId = user;
    return id;
}

std::string LobbyVersion() {
    return std::to_string(kProtocolVersion);
}

const char* EnterResponseText(uint32_t response) {
    switch (response) {
    case 2: return "the lobby no longer exists";
    case 3: return "not allowed";
    case 4: return "the lobby is full";
    case 5: return "unexpected error";
    case 6: return "banned";
    case 7: return "limited account";
    case 9: return "blocked by a member";
    case 10: return "you blocked a member";
    default: return "failed";
    }
}

} // namespace

bool SteamPeer::Open(const std::string& playerName, bool pumpCallbacks, std::string& diag) {
    name_ = playerName;
    pump_ = pumpCallbacks;
    connectLobby_ = ParseConnectLobby(GetCommandLineA());
    if (connectLobby_) Log("steam: launched from an invite for lobby %llu", static_cast<unsigned long long>(connectLobby_));
    std::string reason;
    if (Resolve(reason)) {
        diag = reason;
    } else {
        diag = "Steam transport waiting: " + reason + " (retrying)";
    }
    return true;
}

bool SteamPeer::Resolve(std::string& diag) {
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) {
        diag = "steam_api64.dll is not loaded";
        return false;
    }
    auto getUser = std::bit_cast<GetHSteamUserFn>(GetProcAddress(dll, "SteamAPI_GetHSteamUser"));
    auto find = std::bit_cast<FindInterfaceFn>(GetProcAddress(dll, "SteamInternal_FindOrCreateUserInterface"));
    api.registerCallback = std::bit_cast<RegisterCallbackFn>(GetProcAddress(dll, "SteamAPI_RegisterCallback"));
    api.runCallbacks = std::bit_cast<RunCallbacksFn>(GetProcAddress(dll, "SteamAPI_RunCallbacks"));
    if (!getUser || !find || !api.registerCallback || !api.runCallbacks) {
        diag = "steam_api64.dll lacks a required export";
        return false;
    }
    int32_t user = getUser();
    if (!user) {
        diag = "Steam is not initialized (no Steam user); is the game running through Steam?";
        return false;
    }
    api.matchmaking = find(user, kMatchmakingVersion);
    api.friends = find(user, kFriendsVersion);
    api.user = find(user, kUserVersion);
    // Like the SDK's SteamUtils() accessor, ISteamUtils is requested without a user handle.
    api.utils = find(0, kUtilsVersion);
    api.messages = find(user, kMessagesVersion);
    api.networking = find(user, kNetworkingVersion);
    Log("steam: interfaces matchmaking=%p friends=%p user=%p utils=%p messages=%p networking=%p", api.matchmaking,
        api.friends, api.user, api.utils, api.messages, api.networking);
    if (!api.matchmaking || !api.friends || !api.user || (!api.messages && !api.networking)) {
        diag = "the Steam client did not provide the required interfaces";
        return false;
    }
    messages_ = api.messages != nullptr;
    self_ = VCallSteamId(api.user, slot::kGetSteamID);
    for (auto& callback : callbacks) api.registerCallback(&callback, callback.id);
    ready_ = true;
    diag = "Steam ready as " + std::to_string(self_) + " using " +
           (messages_ ? "SteamNetworkingMessages" : "legacy SteamNetworking P2P") +
           (api.utils ? "" : " (no SteamUtils; lobby results rely on callbacks)") +
           (pump_ ? "; pumping Steam callbacks" : "");
    return true;
}

void SteamPeer::Poll(double now) {
    if (!ready_) {
        if (now < nextResolve_) return;
        nextResolve_ = now + 2;
        static std::string lastReason;
        std::string reason;
        if (!Resolve(reason)) {
            if (reason != lastReason) Log("steam: %s", reason.c_str());
            lastReason = reason;
            return;
        }
        Log("steam: %s", reason.c_str());
    }
    if (pump_) api.runCallbacks();
    HandleEvents();
    PollCall();
    if (connectLobby_ && !lobby_ && !pendingCall_) {
        uint64_t lobby = connectLobby_;
        connectLobby_ = 0;
        Join(lobby);
    }
    if (pending_ != Pending::None && !pendingSince_) pendingSince_ = now;
    if (pending_ != Pending::None && now - pendingSince_ > 20) {
        Log("steam: %s timed out", pending_ == Pending::Create ? "creating the lobby" : "joining the lobby");
        Announce(pending_ == Pending::Create ? "Creating the lobby timed out" : "Joining the lobby timed out");
        // Steam can still finish the call later; keep watching it so a late lobby gets left.
        if (pendingCall_ && staleCalls_.size() < 8) staleCalls_.push_back({pendingCall_, pending_});
        pending_ = Pending::None;
        pendingCall_ = 0;
    }
    PollStaleCalls();
    PollList(now);
    if (lobby_ && now >= nextMembers_) {
        nextMembers_ = now + 1;
        RefreshMembers();
    }
    ReceiveAll();
}

void SteamPeer::HandleEvents() {
    std::vector<Event> batch;
    {
        std::lock_guard lock(eventMutex);
        batch.swap(events);
    }
    for (const auto& e : batch) {
        switch (e.id) {
        case kGameLobbyJoinRequested:
            Log("steam: joining %s's lobby from the Steam overlay", FriendName(e.b).c_str());
            Announce("Joining " + FriendName(e.b) + "'s game");
            Join(e.a);
            break;
        case kLobbyCreated:
            if (pending_ != Pending::Create) {
                if (e.c == kResultOK) LeaveStale(e.a);
                break;
            }
            pending_ = Pending::None;
            pendingCall_ = 0;
            if (e.c == kResultOK && e.a) Entered(e.a, true);
            else {
                Log("steam: creating the lobby failed (result %u)", e.c);
                Announce("Could not create a Steam lobby");
            }
            break;
        case kLobbyEnter:
            if (pending_ != Pending::Join || e.a != pendingLobby_) {
                if (e.c == kChatRoomEnterSuccess) LeaveStale(e.a);
                break;
            }
            pending_ = Pending::None;
            pendingCall_ = 0;
            if (e.c == kChatRoomEnterSuccess) Entered(e.a, false);
            else {
                Log("steam: joining the lobby failed: %s (%u)", EnterResponseText(e.c), e.c);
                Announce(std::string("Could not join: ") + EnterResponseText(e.c));
            }
            break;
        case kMessagesSessionRequest:
            if (IsMember(e.a) && api.messages) {
                auto id = Identity(e.a);
                VCall<bool>(api.messages, slot::kAcceptSessionWithUser, &id);
            } else if (e.a != lastRejected_) {
                Log("steam: ignored a session request from %llu (not in our lobby)", static_cast<unsigned long long>(e.a));
                lastRejected_ = e.a;
            }
            break;
        case kP2PSessionRequest:
            if (IsMember(e.a) && api.networking) VCall<bool>(api.networking, slot::kAcceptP2PSessionWithUser, e.a);
            else if (e.a != lastRejected_) {
                Log("steam: ignored a P2P request from %llu (not in our lobby)", static_cast<unsigned long long>(e.a));
                lastRejected_ = e.a;
            }
            break;
        }
    }
}

void SteamPeer::PollCall() {
    if (!pendingCall_ || !api.utils) return;
    bool failed = false;
    if (!VCall<bool>(api.utils, slot::kIsAPICallCompleted, pendingCall_, &failed)) return;
    Pending kind = pending_;
    uint64_t call = pendingCall_;
    pending_ = Pending::None;
    pendingCall_ = 0;
    if (kind == Pending::Create) {
        LobbyCreated result{};
        bool ok = VCall<bool>(api.utils, slot::kGetAPICallResult, call, static_cast<void*>(&result),
                              static_cast<int>(sizeof result), kLobbyCreated, &failed);
        if (ok && !failed && result.result == kResultOK && result.lobby) Entered(result.lobby, true);
        else {
            Log("steam: creating the lobby failed (result %u, io failure %d)", result.result, failed);
            Announce("Could not create a Steam lobby");
        }
    } else if (kind == Pending::Join) {
        LobbyEnter result{};
        bool ok = VCall<bool>(api.utils, slot::kGetAPICallResult, call, static_cast<void*>(&result),
                              static_cast<int>(sizeof result), kLobbyEnter, &failed);
        if (ok && !failed && result.response == kChatRoomEnterSuccess && result.lobby) Entered(result.lobby, false);
        else {
            Log("steam: joining the lobby failed: %s (%u, io failure %d)", EnterResponseText(result.response),
                result.response, failed);
            Announce(std::string("Could not join: ") + EnterResponseText(result.response));
        }
    }
}

void SteamPeer::PollStaleCalls() {
    if (!api.utils) {
        staleCalls_.clear();
        return;
    }
    std::erase_if(staleCalls_, [this](const StaleCall& stale) {
        bool failed = false;
        if (!VCall<bool>(api.utils, slot::kIsAPICallCompleted, stale.call, &failed)) return false;
        if (stale.kind == Pending::Create) {
            LobbyCreated result{};
            if (VCall<bool>(api.utils, slot::kGetAPICallResult, stale.call, static_cast<void*>(&result),
                            static_cast<int>(sizeof result), kLobbyCreated, &failed) &&
                !failed && result.result == kResultOK)
                LeaveStale(result.lobby);
        } else {
            LobbyEnter result{};
            if (VCall<bool>(api.utils, slot::kGetAPICallResult, stale.call, static_cast<void*>(&result),
                            static_cast<int>(sizeof result), kLobbyEnter, &failed) &&
                !failed && result.response == kChatRoomEnterSuccess)
                LeaveStale(result.lobby);
        }
        return true;
    });
}

void SteamPeer::LeaveStale(uint64_t lobby) {
    // Lobbies entered by a call we gave up on (or never made) are left so no hidden memberships pile up.
    // While creating, the creator's own LobbyEnter can arrive before the lobby ID is known.
    if (!lobby || lobby == lobby_ || pending_ == Pending::Create ||
        (pending_ == Pending::Join && lobby == pendingLobby_))
        return;
    VCall<void>(api.matchmaking, slot::kLeaveLobby, lobby);
    Log("steam: left lobby %llu that finished entering after its request was abandoned",
        static_cast<unsigned long long>(lobby));
}

void SteamPeer::ClosePartnerSession() {
    if (!partner_) return;
    if (messages_) {
        auto id = Identity(partner_);
        VCall<bool>(api.messages, slot::kCloseSessionWithUser, &id);
    } else {
        VCall<bool>(api.networking, slot::kCloseP2PSessionWithUser, partner_);
    }
    partner_ = 0;
    inbox_.clear();
}

void SteamPeer::Host(session::Visibility visibility) {
    if (!ready_ || pendingCall_) return;
    if (lobby_) Leave();
    visibility_ = visibility;
    uint64_t call = VCall<uint64_t>(api.matchmaking, slot::kCreateLobby, session::SteamLobbyType(visibility), 2);
    if (!call) {
        Log("steam: Steam refused to create a lobby");
        Announce("Steam refused to create a lobby");
        return;
    }
    pendingCall_ = call;
    pending_ = Pending::Create;
    pendingSince_ = 0;
    Log("steam: creating a %s lobby (call %llu)", session::VisibilityName(visibility),
        static_cast<unsigned long long>(pendingCall_));
}

void SteamPeer::SetVisibility(session::Visibility visibility) {
    if (!ready_ || !lobby_) {
        visibility_ = visibility;
        return;
    }
    if (owner_ != self_) return;
    if (VCall<bool>(api.matchmaking, slot::kSetLobbyType, lobby_, session::SteamLobbyType(visibility))) {
        visibility_ = visibility;
        if (!VCall<bool>(api.matchmaking, slot::kSetLobbyData, lobby_, "visibility", session::VisibilityKey(visibility)))
            Announce("Visibility changed, but Steam could not update the lobby description");
        Log("steam: lobby visibility is now %s", session::VisibilityName(visibility));
        Announce(std::string("Lobby is now ") + session::VisibilityName(visibility));
    } else {
        Log("steam: Steam refused to change the lobby visibility");
        Announce("Could not change lobby visibility");
    }
}

void SteamPeer::RequestPublic() {
    if (!ready_ || listCall_ || !api.utils) return;
    std::string version = LobbyVersion();
    VCall<void>(api.matchmaking, slot::kAddRequestLobbyListStringFilter, "dgt", version.c_str(), kLobbyComparisonEqual);
    VCall<void>(api.matchmaking, slot::kAddRequestLobbyListFilterSlotsAvailable, 1);
    VCall<void>(api.matchmaking, slot::kAddRequestLobbyListDistanceFilter, kLobbyDistanceWorldwide);
    VCall<void>(api.matchmaking, slot::kAddRequestLobbyListResultCountFilter, 20);
    listCall_ = VCall<uint64_t>(api.matchmaking, slot::kRequestLobbyList);
    listSince_ = 0;
    if (!listCall_) Announce("Steam refused the public lobby search");
    Log("steam: searching public lobbies (call %llu)", static_cast<unsigned long long>(listCall_));
}

void SteamPeer::PollList(double now) {
    if (!listCall_) return;
    if (!listSince_) listSince_ = now;
    bool failed = false;
    if (!VCall<bool>(api.utils, slot::kIsAPICallCompleted, listCall_, &failed)) {
        if (now - listSince_ > 15) {
            Log("steam: the public lobby search timed out");
            Announce("Public lobby search timed out");
            listCall_ = 0;
        }
        return;
    }
    LobbyMatchList result{};
    bool ok = VCall<bool>(api.utils, slot::kGetAPICallResult, listCall_, static_cast<void*>(&result),
                          static_cast<int>(sizeof result), kLobbyMatchList, &failed);
    listCall_ = 0;
    public_.clear();
    if (!ok || failed) {
        Log("steam: the public lobby search failed");
        Announce("Public lobby search failed");
        return;
    }
    for (uint32_t i = 0; i < result.count && i < 50; ++i) {
        uint64_t lobby = VCallSteamId(api.matchmaking, slot::kGetLobbyByIndex, static_cast<int>(i));
        if (!lobby || lobby == lobby_) continue;
        const char* name = VCall<const char*>(api.matchmaking, slot::kGetLobbyData, lobby, "name");
        public_.push_back({0, lobby, name && *name ? name : "Days Gone Together"});
    }
    Log("steam: %u public lobbies found", static_cast<unsigned>(public_.size()));
}

std::string SteamPeer::PersonaName() const {
    if (!ready_) return {};
    const char* name = VCall<const char*>(api.friends, slot::kGetPersonaName);
    return name ? name : "";
}

void SteamPeer::Announce(std::string text) {
    if (events_.size() < 32) events_.push_back(std::move(text));
}

std::vector<std::string> SteamPeer::TakeEvents() {
    std::vector<std::string> out;
    out.swap(events_);
    return out;
}

void SteamPeer::Join(uint64_t lobby) {
    if (!ready_ || !lobby || lobby == lobby_ || pendingCall_) return;
    if (lobby_) Leave();
    uint64_t call = VCall<uint64_t>(api.matchmaking, slot::kJoinLobby, lobby);
    if (!call) {
        Log("steam: Steam refused to join lobby %llu", static_cast<unsigned long long>(lobby));
        Announce("Steam refused to join the lobby");
        return;
    }
    pendingCall_ = call;
    pendingLobby_ = lobby;
    pending_ = Pending::Join;
    pendingSince_ = 0;
    Log("steam: joining lobby %llu (call %llu)", static_cast<unsigned long long>(lobby),
        static_cast<unsigned long long>(pendingCall_));
}

void SteamPeer::Leave() {
    if (!ready_ || !lobby_) return;
    ClosePartnerSession();
    VCall<void>(api.matchmaking, slot::kLeaveLobby, lobby_);
    Log("steam: left lobby %llu", static_cast<unsigned long long>(lobby_));
    Announce("Left the Steam lobby");
    lobby_ = owner_ = 0;
    members_.clear();
    inbox_.clear();
}

void SteamPeer::Invite() {
    if (!ready_ || !lobby_) return;
    VCall<void>(api.friends, slot::kActivateGameOverlayInviteDialog, lobby_);
    Log("steam: opened the Steam overlay invite dialog (needs the Steam overlay enabled)");
}

void SteamPeer::Entered(uint64_t lobby, bool created) {
    lobby_ = lobby;
    partner_ = 0;
    members_.clear();
    inbox_.clear();
    announced_ = false;
    std::string version = LobbyVersion();
    if (created) {
        std::string name = name_.empty() ? PersonaName() : name_;
        VCall<bool>(api.matchmaking, slot::kSetLobbyData, lobby, "dgt", version.c_str());
        VCall<bool>(api.matchmaking, slot::kSetLobbyData, lobby, "name", name.c_str());
        if (!VCall<bool>(api.matchmaking, slot::kSetLobbyData, lobby, "visibility", session::VisibilityKey(visibility_)))
            Announce("Steam could not publish the lobby visibility description");
        VCall<bool>(api.matchmaking, slot::kSetLobbyJoinable, lobby, true);
        Log("steam: hosting %s lobby %llu; invite a friend from the co-op menu or the Steam overlay",
            session::VisibilityName(visibility_), static_cast<unsigned long long>(lobby));
        Announce(std::string("Hosting a Steam lobby (") + session::VisibilityName(visibility_) + ")");
    } else {
        const char* value = VCall<const char*>(api.matchmaking, slot::kGetLobbyData, lobby, "dgt");
        if (!value || version != value) {
            Log("steam: lobby %llu is not a Days Gone Together protocol v%s lobby (it reports '%s'); leaving",
                static_cast<unsigned long long>(lobby), version.c_str(), value ? value : "");
            Announce("That lobby runs a different Days Gone Together version");
            Leave();
            return;
        }
        Log("steam: joined lobby %llu", static_cast<unsigned long long>(lobby));
        Announce("Joined " + FriendName(VCallSteamId(api.matchmaking, slot::kGetLobbyOwner, lobby)) + "'s lobby");
    }
    nextMembers_ = 0;
    RefreshMembers();
}

void SteamPeer::RefreshMembers() {
    int count = VCall<int>(api.matchmaking, slot::kGetNumLobbyMembers, lobby_);
    if (count <= 0) {
        Log("steam: no longer in lobby %llu", static_cast<unsigned long long>(lobby_));
        Announce("The lobby closed");
        ClosePartnerSession();
        lobby_ = owner_ = 0;
        members_.clear();
        inbox_.clear();
        return;
    }
    uint64_t owner = VCallSteamId(api.matchmaking, slot::kGetLobbyOwner, lobby_);
    if (owner_ && owner != owner_) {
        Announce("The host left; the session has ended");
        Leave();
        return;
    }
    owner_ = owner;
    if (owner_ != self_) {
        const char* visibility = VCall<const char*>(api.matchmaking, slot::kGetLobbyData, lobby_, "visibility");
        session::Visibility parsed;
        if (visibility && session::ParseVisibility(visibility, parsed)) visibility_ = parsed;
    }
    std::vector<uint64_t> previous;
    previous.swap(members_);
    for (int i = 0; i < count && i < 16; ++i) members_.push_back(VCallSteamId(api.matchmaking, slot::kGetLobbyMemberByIndex, lobby_, i));
    if (announced_) {
        for (uint64_t member : members_)
            if (member && member != self_ && std::find(previous.begin(), previous.end(), member) == previous.end())
                Announce(FriendName(member) + " is connecting");
        for (uint64_t member : previous)
            if (member && member != self_ && !IsMember(member)) Announce(FriendName(member) + " left the lobby");
    }
    announced_ = true;
    uint64_t next = 0;
    for (uint64_t member : members_) {
        if (member && member != self_) {
            next = member;
            break;
        }
    }
    if (next == partner_) return;
    if (partner_) Log("steam: %s left the lobby", FriendName(partner_).c_str());
    ClosePartnerSession();
    partner_ = next;
    inbox_.clear();
    if (partner_)
        Log("steam: partner %s (%llu) is in the lobby; streaming over %s", FriendName(partner_).c_str(),
            static_cast<unsigned long long>(partner_), messages_ ? "SteamNetworkingMessages" : "SteamNetworking P2P");
}

bool SteamPeer::IsMember(uint64_t user) const {
    return user && std::find(members_.begin(), members_.end(), user) != members_.end();
}

void SteamPeer::SendRaw(const void* data, int size) {
    if (!ready_ || !partner_ || size <= 0) return;
    uint32_t result;
    if (messages_) {
        auto id = Identity(partner_);
        result = VCall<uint32_t>(api.messages, slot::kSendMessageToUser, &id, data, static_cast<uint32_t>(size),
                                 kSendUnreliableNoNagle | kSendAutoRestartBrokenSession, 0);
    } else {
        result = VCall<bool>(api.networking, slot::kSendP2PPacket, partner_, data, static_cast<uint32_t>(size),
                             kP2PSendUnreliable, 0) ? kResultOK : 2;
    }
    if (result != kResultOK && result != lastSendResult_) Log("steam: send failed (result %u)", result);
    lastSendResult_ = result;
}

void SteamPeer::ReceiveAll() {
    constexpr int kMaxSize = 2048;
    constexpr size_t kInboxLimit = 512;
    if (messages_) {
        NetworkingMessage* batch[64];
        for (int round = 0; round < 8; ++round) {
            int n = VCall<int>(api.messages, slot::kReceiveMessagesOnChannel, 0, batch, 64);
            for (int i = 0; i < n; ++i) {
                auto message = batch[i];
                bool fromPartner = partner_ && message->peer.type == kIdentityTypeSteamID &&
                                   message->peer.steamId == partner_;
                if (fromPartner && message->size > 0 && message->size <= kMaxSize && inbox_.size() < kInboxLimit) {
                    auto bytes = static_cast<const char*>(message->data);
                    inbox_.emplace_back(bytes, bytes + message->size);
                }
                message->release(message);
            }
            if (n < 64) break;
        }
        return;
    }
    char buffer[kMaxSize];
    uint32_t available = 0;
    for (int budget = 0; budget < 512 && VCall<bool>(api.networking, slot::kIsP2PPacketAvailable, &available, 0); ++budget) {
        uint32_t size = 0;
        uint64_t remote = 0;
        if (!VCall<bool>(api.networking, slot::kReadP2PPacket, static_cast<void*>(buffer),
                         static_cast<uint32_t>(sizeof buffer), &size, &remote, 0)) break;
        if (partner_ && remote == partner_ && size > 0 && size <= sizeof buffer && inbox_.size() < kInboxLimit)
            inbox_.emplace_back(buffer, buffer + size);
    }
}

bool SteamPeer::Receive(StatePacket& out, const std::function<void(const char*, int)>& other) {
    while (!inbox_.empty()) {
        std::vector<char> message = std::move(inbox_.front());
        inbox_.pop_front();
        int size = static_cast<int>(message.size());
        if (DecodeIncoming(message.data(), size, out, lastForeignVersion_)) return true;
        if (other) other(message.data(), size);
    }
    return false;
}

void SteamPeer::RefreshFriends(double now) {
    if (!ready_ || now < nextFriends_) return;
    nextFriends_ = now + 3;
    friends_.clear();
    uint32_t app = api.utils ? VCall<uint32_t>(api.utils, slot::kGetAppID) : kDaysGoneAppId;
    int count = std::min(VCall<int>(api.friends, slot::kGetFriendCount, kFriendFlagImmediate), 1000);
    for (int i = 0; i < count; ++i) {
        uint64_t user = VCallSteamId(api.friends, slot::kGetFriendByIndex, i, kFriendFlagImmediate);
        FriendGameInfo info{};
        if (!user || !VCall<bool>(api.friends, slot::kGetFriendGamePlayed, user, &info)) continue;
        if (GameAppId(info.gameId) != app || !info.lobby || info.lobby == lobby_) continue;
        friends_.push_back({user, info.lobby, FriendName(user)});
    }
}

std::string SteamPeer::FriendName(uint64_t user) const {
    if (!ready_ || !user) return "?";
    const char* name = VCall<const char*>(api.friends, slot::kGetFriendPersonaName, user);
    return name && *name ? name : std::to_string(user);
}

std::string SteamPeer::PeerText() const {
    return partner_ ? FriendName(partner_) + " via Steam" : "Steam";
}

std::string SteamPeer::Status() const {
    if (!ready_) return "Steam unavailable - see DaysGoneTogether.log";
    if (pending_ == Pending::Create) return "Steam: creating a lobby...";
    if (pending_ == Pending::Join) return "Steam: joining a lobby...";
    if (!lobby_) return "Steam: host a lobby or join a friend";
    if (partner_) return "Steam lobby with " + FriendName(partner_);
    return owner_ == self_ ? std::string("Steam: hosting (") + session::VisibilityName(visibility_) + ") - invite a friend" :
                             "Steam: in lobby, waiting for the host";
}

} // namespace coop
