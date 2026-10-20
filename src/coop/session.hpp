#pragma once
// Platform-independent co-op session model: the event log, Steam lobby visibility, the native CO-OP menu
// pages and the LAN discovery beacon. The game-facing code in coop.cpp and native_ui.cpp renders this.
#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace coop::session {

// ---- Event log -------------------------------------------------------------------------------------

struct LogEntry {
    uint64_t serial = 0;
    std::string text;
};

// The visible session log ("X is connecting", "X disconnected", ...). Serials let each view (menus,
// HUD notifications) show only entries it has not displayed yet.
class EventLog {
public:
    static constexpr size_t kCapacity = 32;

    void Add(std::string text) {
        if (text.empty()) return;
        entries_.push_back({++serial_, std::move(text)});
        while (entries_.size() > kCapacity) entries_.pop_front();
    }
    uint64_t Serial() const { return serial_; }
    std::vector<std::string> Last(size_t count) const {
        std::vector<std::string> out;
        size_t start = entries_.size() > count ? entries_.size() - count : 0;
        for (size_t i = start; i < entries_.size(); ++i) out.push_back(entries_[i].text);
        return out;
    }
    std::vector<LogEntry> Since(uint64_t serial) const {
        std::vector<LogEntry> out;
        for (const auto& entry : entries_)
            if (entry.serial > serial) out.push_back(entry);
        return out;
    }

private:
    std::deque<LogEntry> entries_;
    uint64_t serial_ = 0;
};

// ---- Steam lobby visibility -------------------------------------------------------------------------

enum class Visibility : uint8_t { Private, Friends, Public };

inline const char* VisibilityName(Visibility v) {
    switch (v) {
    case Visibility::Private: return "INVITE ONLY";
    case Visibility::Public: return "PUBLIC";
    default: return "FRIENDS ONLY";
    }
}
inline Visibility NextVisibility(Visibility v) {
    return v == Visibility::Private ? Visibility::Friends :
           v == Visibility::Friends ? Visibility::Public : Visibility::Private;
}
// ELobbyType: 0 private (invite only), 1 friends only, 2 public.
inline int SteamLobbyType(Visibility v) {
    return v == Visibility::Private ? 0 : v == Visibility::Public ? 2 : 1;
}
inline const char* VisibilityKey(Visibility v) {
    return v == Visibility::Private ? "private" : v == Visibility::Public ? "public" : "friends";
}
inline bool ParseVisibility(std::string value, Visibility& out) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "private" || value == "invite") out = Visibility::Private;
    else if (value == "friends") out = Visibility::Friends;
    else if (value == "public") out = Visibility::Public;
    else return false;
    return true;
}

// ---- Native CO-OP menu ------------------------------------------------------------------------------

enum class Kind : uint8_t { None, Steam, LanHost, LanJoin };

enum class Page : uint8_t { Root, Friends, Public, Lan };

enum class Action : uint8_t {
    HostSteam,
    HostLan,
    BrowseFriends,
    BrowsePublic,
    BrowseLan,
    JoinFriend,
    JoinPublic,
    JoinLan,
    Start,
    Invite,
    CycleVisibility,
    Leave,
    Refresh,
    Back,
};

struct Row {
    std::string label;
    Action action = Action::Back;
    size_t index = 0; // entry for Join* actions
};

struct View {
    bool inGame = false;        // pause menu (gameplay) rather than the title menu
    bool steamAvailable = false;
    Kind kind = Kind::None;
    bool host = true;           // this player owns the session
    bool busy = false;          // a Steam lobby request is in flight
    Visibility visibility = Visibility::Friends;
    bool connected = false;
    std::string partner;
    std::vector<std::string> friends; // friends hosting a Days Gone Together lobby
    std::vector<std::string> publicLobbies;
    std::vector<std::string> lanHosts;
};

// Main menu rows 0-8 have native mouse targets; the pause list has ten entries.
constexpr size_t kMaxRows = 9;
constexpr size_t kMaxListEntries = kMaxRows - 2; // leaves room for REFRESH and BACK

// Cuts UTF-8 text to at most maxBytes without splitting a character.
inline std::string TruncateUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

inline std::string Upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return ch < 0x80 ? static_cast<char>(std::toupper(ch)) : static_cast<char>(ch); });
    return text;
}

inline std::vector<Row> BuildRows(Page page, const View& v) {
    std::vector<Row> rows;
    if (v.kind != Kind::None) {
        if (!v.inGame) rows.push_back({v.host ? "START GAME" : "START GAME (JOIN HOST)", Action::Start});
        if (v.kind == Kind::Steam && !v.busy) {
            rows.push_back({"INVITE FRIENDS", Action::Invite});
            if (v.host)
                rows.push_back({std::string("VISIBILITY: ") + VisibilityName(v.visibility), Action::CycleVisibility});
        }
        rows.push_back({v.host ? "CLOSE SESSION" : "LEAVE SESSION", Action::Leave});
        rows.push_back({"BACK", Action::Back});
        return rows;
    }
    auto list = [&](const std::vector<std::string>& names, Action join) {
        for (size_t i = 0; i < names.size() && i < kMaxListEntries; ++i) rows.push_back({Upper(names[i]), join, i});
        rows.push_back({"REFRESH", Action::Refresh});
        rows.push_back({"BACK", Action::Back});
    };
    switch (page) {
    case Page::Friends: list(v.friends, Action::JoinFriend); break;
    case Page::Public: list(v.publicLobbies, Action::JoinPublic); break;
    case Page::Lan: list(v.lanHosts, Action::JoinLan); break;
    default:
        if (v.steamAvailable) {
            rows.push_back({"HOST ONLINE GAME", Action::HostSteam});
            rows.push_back({"JOIN A FRIEND", Action::BrowseFriends});
            rows.push_back({"FIND PUBLIC GAMES", Action::BrowsePublic});
        }
        rows.push_back({"HOST LAN GAME", Action::HostLan});
        rows.push_back({"JOIN LAN GAME", Action::BrowseLan});
        rows.push_back({"BACK", Action::Back});
        break;
    }
    return rows;
}

inline const char* PageTitle(Page page, const View& v) {
    if (v.kind == Kind::Steam) return "CO-OP LOBBY";
    if (v.kind != Kind::None) return "LAN LOBBY";
    switch (page) {
    case Page::Friends: return "JOIN A FRIEND";
    case Page::Public: return "PUBLIC GAMES";
    case Page::Lan: return "LAN GAMES";
    default: return "CO-OP";
    }
}

// One status line followed by the newest log entries, for the menu status text.
inline std::string StatusText(Page page, const View& v, const std::vector<std::string>& log) {
    std::string status;
    switch (v.kind) {
    case Kind::Steam:
        status = v.busy ? "Steam lobby: working..." :
                 std::string("Steam lobby (") + VisibilityName(v.visibility) + ") - " +
                     (v.connected ? "playing with " + v.partner : v.host ? "waiting for a player" : "waiting for the host");
        break;
    case Kind::LanHost:
        status = v.connected ? "LAN host - playing with " + v.partner : "LAN host - waiting for a player";
        break;
    case Kind::LanJoin:
        status = v.connected ? "LAN - playing with " + v.partner : "LAN - connecting to the host...";
        break;
    default:
        if (page == Page::Friends) status = v.friends.empty() ? "No friends are hosting a co-op lobby" : "Friends hosting a co-op lobby";
        else if (page == Page::Public) status = v.busy ? "Searching..." : v.publicLobbies.empty() ? "No public games found" : "Public co-op games";
        else if (page == Page::Lan) status = v.lanHosts.empty() ? "Looking for LAN games..." : "LAN games";
        else status = v.steamAvailable ? "Play together over Steam or the local network" : "Steam unavailable - LAN only";
        break;
    }
    for (const auto& line : log) status += "\n" + line;
    return status;
}

// ---- LAN discovery ----------------------------------------------------------------------------------

constexpr uint32_t kBeaconMagic = 0x42544744; // "DGTB"

#pragma pack(push, 1)
struct Beacon {
    uint32_t magic = kBeaconMagic;
    uint16_t version = kProtocolVersion;
    uint16_t port = 0;     // the host's game port
    uint32_t session = 0;  // random per process; filters our own broadcasts
    uint8_t open = 1;      // 0 once a partner is connected
    char name[32] = {};
};
#pragma pack(pop)
static_assert(sizeof(Beacon) == 45);

inline bool DecodeBeacon(const char* data, int size, Beacon& out) {
    if (size != static_cast<int>(sizeof(Beacon))) return false;
    std::memcpy(&out, data, sizeof out);
    if (out.magic != kBeaconMagic || out.version != kProtocolVersion || !out.port) return false;
    out.name[sizeof out.name - 1] = '\0';
    return true;
}

struct LanHost {
    uint32_t address = 0; // IPv4, network byte order
    uint16_t port = 0;    // host byte order
    uint32_t session = 0;
    std::string name;
    double seen = 0;
};

// Hosts heard recently, oldest first; a host disappears after it stops beaconing or fills up.
class LanDirectory {
public:
    static constexpr double kExpiry = 4.0;
    static constexpr size_t kLimit = 16;

    void Heard(const Beacon& beacon, uint32_t address, double now) {
        auto found = std::find_if(hosts_.begin(), hosts_.end(),
                                  [&](const LanHost& h) { return h.session == beacon.session; });
        if (!beacon.open) {
            if (found != hosts_.end()) hosts_.erase(found);
            return;
        }
        if (found == hosts_.end()) {
            if (hosts_.size() >= kLimit) return;
            hosts_.push_back({});
            found = hosts_.end() - 1;
        }
        found->address = address;
        found->port = beacon.port;
        found->session = beacon.session;
        found->name = beacon.name[0] ? beacon.name : "Days Gone Together";
        found->seen = now;
    }
    void Prune(double now) {
        std::erase_if(hosts_, [now](const LanHost& h) { return now - h.seen > kExpiry; });
    }
    void Clear() { hosts_.clear(); }
    const std::vector<LanHost>& Hosts() const { return hosts_; }

private:
    std::vector<LanHost> hosts_;
};

} // namespace coop::session
