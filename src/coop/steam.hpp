#pragma once
#include "net.hpp"
#include "session.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace coop {

struct SteamFriendLobby {
    uint64_t user = 0;
    uint64_t lobby = 0;
    std::string name;
};

// Two-player Steam lobby (invite only, friends only or public) for discovery and Steam Networking for traffic.
// Uses the game's own steam_api64.dll; nothing from the Steamworks SDK is bundled.
class SteamPeer final : public Transport {
public:
    bool Open(const std::string& playerName, bool pumpCallbacks, std::string& diag);
    void SendRaw(const void* data, int size) override;
    bool Receive(StatePacket& out, const std::function<void(const char*, int)>& other = {}) override;
    bool HasPeer() const override { return partner_ != 0; }
    std::string PeerText() const override;
    void Poll(double now) override;

    void Host(session::Visibility visibility);
    void Join(uint64_t lobby);
    void Leave();
    void Invite();
    void RefreshFriends(double now);
    // Applies to the current lobby when this player owns it, and to the next hosted lobby.
    void SetVisibility(session::Visibility visibility);
    session::Visibility Visibility() const { return visibility_; }
    // Searches public Theys Gone lobbies of this protocol version.
    void RequestPublic();
    const std::vector<SteamFriendLobby>& PublicLobbies() const { return public_; }
    bool Searching() const { return listCall_ != 0; }
    // Player-facing session events ("X is connecting", ...) since the last call.
    std::vector<std::string> TakeEvents();

    bool Available() const { return ready_; }
    bool InLobby() const { return lobby_ != 0; }
    bool Joining() const { return pending_ == Pending::Join; }
    std::string PersonaName() const;
    std::string PartnerName() const { return partner_ ? FriendName(partner_) : std::string(); }
    // The lobby owner is the world/enemy host; outside a lobby the player is their own host.
    bool IsOwner() const { return !lobby_ || !owner_ || owner_ == self_; }
    bool Busy() const { return pendingCall_ != 0; }
    std::string Status() const;
    const std::vector<SteamFriendLobby>& Friends() const { return friends_; }

private:
    enum class Pending { None, Create, Join };
    bool ready_ = false, pump_ = false, messages_ = false;
    std::string name_;
    double nextResolve_ = 0, nextMembers_ = 0, nextFriends_ = 0, pendingSince_ = 0;
    uint64_t self_ = 0, lobby_ = 0, owner_ = 0, partner_ = 0, connectLobby_ = 0;
    uint64_t pendingCall_ = 0, pendingLobby_ = 0;
    Pending pending_ = Pending::None;
    struct StaleCall {
        uint64_t call;
        Pending kind;
    };
    std::vector<StaleCall> staleCalls_;
    std::vector<uint64_t> members_;
    std::vector<SteamFriendLobby> friends_, public_;
    std::vector<std::string> events_;
    session::Visibility visibility_ = session::Visibility::Friends;
    uint64_t listCall_ = 0;
    double listSince_ = 0;
    bool announced_ = false;
    std::deque<std::vector<char>> inbox_;
    uint16_t lastForeignVersion_ = 0;
    uint32_t lastSendResult_ = 1;
    uint64_t lastRejected_ = 0;

    bool Resolve(std::string& diag);
    void Entered(uint64_t lobby, bool created);
    void PollCall();
    void PollStaleCalls();
    void LeaveStale(uint64_t lobby);
    void ClosePartnerSession();
    void HandleEvents();
    void RefreshMembers();
    void ReceiveAll();
    void PollList(double now);
    void Announce(std::string text);
    bool IsMember(uint64_t user) const;
    std::string FriendName(uint64_t user) const;
};

} // namespace coop
