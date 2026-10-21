#include "coop.hpp"

#include "log.hpp"
#include "net.hpp"
#include "steam.hpp"
#include "save_guard.hpp"
#include "save_transfer.hpp"
#include "story.hpp"
#include "menu.hpp"
#include "native_ui.hpp"
#include "session.hpp"
#include <ws2tcpip.h>
#include "motion.hpp"
#include "frameclock.hpp"
#include "appearance.hpp"
#include "overlay.hpp"
#include "combat.hpp"
#include "debug_commands.hpp"
#include "research.hpp"
#include "ride_prompt.hpp"

#ifndef COOP_VERSION
#define COOP_VERSION "dev"
#endif
#include "enemy.hpp"
#include "gear.hpp"
#include "pose.hpp"
#include "skeleton.hpp"
#include "ue.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ue;

namespace coop {

namespace {

constexpr uint8_t kPrologue[19] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                   0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};
constexpr double kTickInterval = 1.0 / 60.0;
constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kPeerTimeout = 5.0;
// Who was reported as connected without a matching disconnect yet (empty: nobody).
std::string g_announcedPartner;

std::wstring g_basePath;
Config g_cfg;
MenuSettings g_settings;
SettingsMenu g_menu;
std::atomic<DWORD> g_gameThread{0};
bool g_inTick = false;
bool g_failed = false;
bool g_ready = false;
double g_lastTick = 0, g_nextSend = 0, g_lastScan = -10;
UdpPeer g_udp;
SteamPeer g_steam;
Transport* g_net = &g_udp;
size_t g_joinChoice = 0;
bool g_udpOpen = false;
session::Kind g_kind = session::Kind::None;
session::EventLog g_events;
session::LanDirectory g_lan;
uint64_t g_notified = 0;
double g_notifyRetryAt = 0;
double g_tickNow = 0;

// Steam: the lobby owner hosts. LAN: the player who chose HOST LAN GAME (or has no PeerHost) hosts.
bool IsHost() {
    switch (g_kind) {
    case session::Kind::Steam: return g_steam.IsOwner();
    case session::Kind::LanJoin: return false;
    default: return true;
    }
}

// Playing in someone else's session: their save is authoritative and ours must stay untouched.
bool IsGuest() {
    return g_kind == session::Kind::LanJoin ||
           (g_kind == session::Kind::Steam && g_steam.InLobby() && !g_steam.IsOwner());
}

// The name shown to the partner: PlayerName from the INI, else the Steam persona name.
std::string LocalName() {
    static std::string cached;
    static double next = -1;
    if (!g_cfg.playerName.empty()) return g_cfg.playerName;
    if (g_tickNow >= next || cached.empty()) {
        next = g_tickNow + 5;
        std::string persona = g_steam.PersonaName();
        cached = persona.empty() ? "Deacon" : persona;
    }
    return cached;
}
uint32_t g_seq = 0;
uint32_t g_localShotSequence = 0;
uint32_t g_session = 0, g_consent = 1;
StatePacket g_peer{};
DamageChannel g_damage;
Appearance g_localAppearance, g_remoteAppearance;
UObject* g_localBike = nullptr;
struct BikePart {
    UObject* actor = nullptr;
    UObject* component = nullptr;
    bool mastered = false;
    FName socket{};
    FVector location{}, scale{1, 1, 1};
    FRotator rotation{};
};
struct BikeAvatar {
    UObject* actor = nullptr;
    UObject* component = nullptr;
    UObject* source = nullptr;
    std::vector<UObject*> parts;
    std::vector<BikePart> attachments;
    UObject* pose = nullptr;
    PoseWriter writer;
    bool posing = false;
} g_bike;
struct PoseSource {
    UObject* mesh = nullptr;
    UObject* asset = nullptr;
    BoneTable table;
    PoseReader reader;
    PoseTranslationTracker tracker;
    std::vector<int> subset; // streamed bone indices; empty streams every bone
    std::vector<uint32_t> names;
    std::vector<BonePose> all, bones;
};
PoseSource g_bodySource, g_bikeSource;
PoseBuffer g_bodyPoses, g_bikePoses;
double g_lastBodyPose = -10, g_lastBikePose = -10, g_nextPoseSend = 0;
constexpr double kPoseFresh = 0.5;
struct PoseStats {
    uint64_t sent = 0, bytes = 0, received = 0, rejected = 0, reads = 0;
    double readTime = 0;
} g_poseStats;
double g_lastBikeSpawn = -10;
std::string g_settingsError;
struct Avatar {
    UObject* actor = nullptr;
    UObject* component = nullptr;
    std::vector<UObject*> parts;
    UObject* currentAnim = nullptr;
    uint32_t lastShotSequence = 0;
    double shotUntil = 0;
    UObject* seatBike = nullptr;
    UObject* seatMesh = nullptr;
    FName seatSocket{}, pelvis{};
    bool hidden = false;
    // Streamed-pose body: a runtime PoseableMeshComponent; `component` keeps the clip-driven fallback.
    UObject* pose = nullptr;
    PoseWriter writer;
    bool posing = false;
};
Avatar g_passengerAvatar;
Appearance g_passengerAppearance;
struct Passenger {
    UObject* pawn = nullptr;
    UObject* movement = nullptr;
    UObject* mesh = nullptr;
    UObject* controller = nullptr;
    UObject* gameMode = nullptr;
    UObject* seatMesh = nullptr;
    FName socket{};
    FName pelvis{};
    FVector boardingPosition{};
    std::vector<std::pair<UObject*, bool>> visibility;
    uint8_t movementMode = 1, customMode = 0;
    bool collision = true, savingBlocked = false;
    bool movementChanged = false, collisionChanged = false, moveBlocked = false, saveChanged = false, positioned = false;
} g_passenger;

struct Classes {
    UObject *playerController, *character, *skelMeshActor, *skelMeshComponent, *animSequence;
    UObject *statics, *worldManager; // GameplayStatics default object and BendWorldManager class
} C{};

struct WorldSnapshot {
    bool valid = false;
    float timeOfDay = 0;
    float relativeTime = 0;
    uint8_t weather = 0;
    uint8_t transitionRate = 0;
};
struct WorldRestore {
    UObject* manager = nullptr;
    WorldSnapshot state{};
};
WorldSnapshot g_worldState;
WorldRestore g_worldRestore;
UObject* g_worldManager = nullptr;
UObject* g_worldPawn = nullptr;
double g_worldScanAt = -10, g_worldUpdateAt = -10;

struct DebugEnemy {
    std::shared_ptr<UObject> pawn;
    std::shared_ptr<UObject> controller;
    FVector start{};
    bool destroyRequested = false;
};
struct DebugSession {
    UObject *pawn = nullptr, *controller = nullptr, *gameMode = nullptr, *cheats = nullptr;
    std::shared_ptr<UObject> pawnHold, gameModeHold;
    bool originalSavingBlocked = false;
    uint8_t damageMask = 0;
    bool originalDamageable = true;
    std::vector<DebugEnemy> enemies;
    double expires = 0, reportAt = 0;
} g_debug;
DebugRequestGate g_debugRequests;
struct DebugPending {
    uint32_t request = 0;
    std::string command;
    double deadline = 0;
    bool ending = false;
} g_debugPending;

struct OwnedMeshCache {
    UObject* owner = nullptr;
    double refreshed = -10;
    std::vector<UObject*> meshes;
} g_characterMeshes, g_nativeBikeMeshes;

UObject* g_controller = nullptr;
UObject* g_localPlayer = nullptr;
UObject* g_pawn = nullptr;
UObject* g_discoveredPawn = nullptr;
UObject* g_debugDiscoveredPawn = nullptr;

using Snapshot = MotionSnapshot;
MotionBuffer g_motion;
FrameClock g_frameClock;
std::string g_remoteName;
double g_lastRecv = -100;
double g_lastStatus = -100;
double g_lastSpawnAttempt = -100;
uint32_t g_lastRemoteSeq = 0;
uint64_t g_receivedStates = 0;
double g_ridingLookupTime = 0;
uint64_t g_ridingLookups = 0;

Avatar g_avatar;
UObject* g_localMesh = nullptr;
FVector g_localPos{};
double g_localSettledAt = 0;

void DestroyAvatar();
void DestroyBike();
void DestroyEnemyReplicas();
void ReceiveEnemyPose(const PosePacket& packet, double now);
void ReceiveEnemyInfo(const char* data, int len, double now);
void ReceiveGear(const char* data, int len, double now);
void ReceiveStory(const char* data, int len);
void SendGear(UObject* pawn, double now);
void DestroyGear();
void ReceiveDamage(const StatePacket& packet);
void ReceiveEnemyHit(const char* data, int len, double now);
void ReceiveGuestStrike(const char* data, int len);
bool IsGuestProxy(UObject* pawn);
void QueueEnemyHit(bool melee, UObject* weapon);
bool HasEnemyReplicas();
bool DebugHitEnemy();
void UpdateDamageScope(double now);
bool EndPassenger(bool forced);
bool SpawnAvatar(UObject* localPawn, const FVector& at, Avatar& avatar);
void AttachPoseable(UObject* actor, UObject* fallback, UObject* source, UObject*& pose, PoseWriter& writer, const char* label);
bool SetPosing(bool on, bool& posing, UObject* pose, UObject* fallback, const std::vector<UObject*>& masteredParts);
UObject* g_anims[4] = {};
bool g_teleportHeld = false;
bool OwnWindowFocused();
std::string Narrow(const std::wstring& w);
void SaveSettings() {
    std::wstring ini = g_basePath + L".ini";
    const std::pair<const wchar_t*, int> values[] = {{L"JacketColor", g_settings.jacket}, {L"TrousersColor", g_settings.trousers},
        {L"CapColor", g_settings.capColor}, {L"ShowCap", g_settings.cap}, {L"SyncMotorcycles", g_settings.motorcycles}, {L"SyncEnemies", g_settings.enemies},
        {L"AllowPassenger", g_settings.passengers}};
    for (auto [key, value] : values) {
        auto text = std::to_wstring(value);
        if (!WritePrivateProfileStringW(L"Coop", key, text.c_str(), ini.c_str()))
        {
            g_settingsError = "Settings could not be saved; check the plugin log";
            Log("settings: could not save %s (error %lu)", Narrow(std::wstring(key)).c_str(), GetLastError());
        }
    }
}

double Now() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string IniString(const std::wstring& ini, const wchar_t* key, const char* def) {
    wchar_t buf[256];
    std::wstring wdef(def, def + std::strlen(def));
    GetPrivateProfileStringW(L"Coop", key, wdef.c_str(), buf, 256, ini.c_str());
    return Narrow(buf);
}

UObject* Prop(UObject* obj, const char* name) {
    UObject* v = nullptr;
    return obj && GetProp(obj, name, v) ? v : nullptr;
}

UObject* WeakProp(UObject* obj, const char* name) {
    uint64_t v = 0;
    return obj && GetProp(obj, name, v) ? ResolveWeak(v) : nullptr;
}

// Modular character parts: Bend "look part" components, or anything slaved to the body mesh.
bool IsCharacterPart(UObject* comp, UObject* mainMesh) {
    return Name(comp->cls).find("LookPart") != std::string::npos || WeakProp(comp, "MasterPoseComponent") == mainMesh;
}

const std::vector<UObject*>& OwnedMeshes(OwnedMeshCache& cache, UObject* owner, double now) {
    if (cache.owner != owner || !IsLive(owner)) {
        cache.owner = owner;
        cache.refreshed = -10;
        cache.meshes.clear();
    }
    if (!IsLive(owner) || now - cache.refreshed < 1) return cache.meshes;
    cache.refreshed = now;
    cache.meshes.clear();
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto component = ObjectAt(i);
        // The object array owns these pointers; reject unrelated objects before expensive validation.
        if (component && component->outer == owner && IsLive(component) && IsA(component, C.skelMeshComponent))
            cache.meshes.push_back(component);
    }
    return cache.meshes;
}

FVector WorldOrigin(UObject* context) {
    Call c(C.statics, "GetWorldOriginLocation");
    if (!c.ok()) return {0, 0, 0};
    c.set("WorldContextObject", context);
    if (!c.invoke()) return {0, 0, 0};
    auto o = c.get<FIntVector>("ReturnValue");
    return {static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z)};
}

bool ResolveEngine() {
    std::string diag;
    if (!InitNames(diag)) {
        Log("FATAL: %s", diag.c_str());
        return false;
    }
    Log("%s", diag.c_str());
    if (!DetectPropertyOffsetField(diag)) {
        Log("FATAL: %s", diag.c_str());
        return false;
    }
    Log("%s", diag.c_str());
    C.playerController = FindClass("PlayerController");
    C.character = FindClass("Character");
    C.skelMeshActor = FindClass("SkeletalMeshActor");
    C.skelMeshComponent = FindClass("SkeletalMeshComponent");
    C.animSequence = FindClass("AnimSequence");
    C.statics = FindDefaultObject("GameplayStatics");
    C.worldManager = FindClass("BendWorldManager");
    Log("classes: PlayerController=%p Character=%p SkeletalMeshActor=%p SkeletalMeshComponent=%p "
        "AnimSequence=%p GameplayStatics CDO=%p BendWorldManager=%p",
        (void*)C.playerController, (void*)C.character, (void*)C.skelMeshActor, (void*)C.skelMeshComponent,
        (void*)C.animSequence, (void*)C.statics, (void*)C.worldManager);
    if (!C.playerController || !C.character || !C.skelMeshActor || !C.skelMeshComponent || !C.statics) {
        Log("FATAL: required engine classes are missing");
        return false;
    }
    const std::string* names[4] = {&g_cfg.animIdle, &g_cfg.animWalk, &g_cfg.animRun, &g_cfg.animSprint};
    for (int i = 0; i < 4; ++i) {
        if (names[i]->empty()) continue;
        g_anims[i] = FindObject(names[i]->c_str(), C.animSequence);
        Log("animation slot %d '%s' -> %s", i, names[i]->c_str(), g_anims[i] ? "found" : "not loaded yet");
    }
    std::string net;
    g_steam.Open(g_cfg.playerName, g_cfg.steamPumpCallbacks, net);
    Log("network: %s", net.c_str());
    g_udpOpen = g_udp.Open(g_cfg.port, net);
    Log("network: %s%s", net.c_str(), g_udpOpen ? "" : "; LAN play is unavailable");
    g_udp.SetBeaconHandler([](const char* data, int size, uint32_t address) {
        session::Beacon beacon;
        if (session::DecodeBeacon(data, size, beacon) && beacon.session != g_session) g_lan.Heard(beacon, address, g_tickNow);
    });
    g_net = &g_udp;
    if (g_cfg.autoLan && g_udpOpen) {
        if (g_cfg.peerHost.empty()) {
            g_udp.Listen();
            g_kind = session::Kind::LanHost;
            Log("network: LAN host on UDP %u (Transport=udp)", g_cfg.port);
        } else if (g_udp.Connect(g_cfg.peerHost, static_cast<uint16_t>(g_cfg.peerPort), net)) {
            g_kind = session::Kind::LanJoin;
            Log("network: LAN %s (Transport=udp)", net.c_str());
        } else {
            Log("network: %s", net.c_str());
        }
    }
    return true;
}

UObject* FindWorldManager() {
    if (!C.worldManager) C.worldManager = FindClass("BendWorldManager");
    if (!C.worldManager) return nullptr;
    // A full object-array walk costs tens of milliseconds; reuse the manager while it lives.
    static UObject* cached = nullptr;
    if (IsLive(cached) && IsA(cached, C.worldManager)) return cached;
    cached = nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto object = ObjectAt(i);
        if (IsLive(object) && !IsDefaultOrArchetype(object) && IsA(object, C.worldManager)) return cached = object;
    }
    return nullptr;
}

bool ReadWorldSnapshot(UObject* manager, WorldSnapshot& state) {
    if (!IsLive(manager)) return false;
    Call mission(manager, "GetIsMissionActive"), cinematic(manager, "GetIsCinematicActive");
    bool missionAvailable = mission.invoke();
    bool cinematicAvailable = cinematic.invoke();
    bool missionActive = missionAvailable && mission.get<bool>("ReturnValue");
    bool cinematicActive = cinematicAvailable && cinematic.get<bool>("ReturnValue");
    static ULONGLONG lastDiagnostic = 0;
    ULONGLONG now = GetTickCount64();
    if (!missionAvailable || !cinematicAvailable || missionActive || cinematicActive) {
        if (now - lastDiagnostic >= 10000) {
            Log("world sync: snapshot paused mission-api=%d cinematic-api=%d mission=%d cinematic=%d",
                missionAvailable, cinematicAvailable, missionActive, cinematicActive);
            lastDiagnostic = now;
        }
        return false;
    }
    Call time(manager, "GetWorldTimeOfDay"), relative(manager, "GetWorldTimeFractional");
    uint8_t transitionRate = 0, weather = 0;
    bool timeAvailable = time.invoke();
    bool relativeAvailable = relative.invoke();
    // GetWorldWeather() always returns 0 in this build; the property tracks the selected weather.
    bool weatherAvailable = GetProp(manager, "WorldWeather", weather);
    bool transitionAvailable = GetProp(manager, "WeatherTransitionType", transitionRate);
    if (!timeAvailable || !relativeAvailable || !weatherAvailable || !transitionAvailable) {
        if (now - lastDiagnostic >= 10000) {
            Log("world sync: getters time=%d relative=%d weather=%d transition=%d",
                timeAvailable, relativeAvailable, weatherAvailable, transitionAvailable);
            lastDiagnostic = now;
        }
        return false;
    }
    state.timeOfDay = time.get<float>("ReturnValue");
    state.relativeTime = relative.get<float>("ReturnValue");
    state.weather = weather;
    state.transitionRate = transitionRate;
    state.valid = std::isfinite(state.timeOfDay) && std::fabs(state.timeOfDay) < 1e6f &&
                  std::isfinite(state.relativeTime) && std::fabs(state.relativeTime) < 1e6f;
    return state.valid;
}

bool WorldSyncAllowed(UObject* manager) {
    if (!IsLive(manager)) return false;
    Call mission(manager, "GetIsMissionActive"), cinematic(manager, "GetIsCinematicActive");
    return mission.invoke() && cinematic.invoke() &&
           !mission.get<bool>("ReturnValue") && !cinematic.get<bool>("ReturnValue");
}

void ApplyWorldSnapshot(UObject* manager, const WorldSnapshot& state) {
    if (!state.valid || !WorldSyncAllowed(manager)) return;
    WorldSnapshot current{};
    if (!ReadWorldSnapshot(manager, current)) return;
    // Small corrections keep lighting from visibly stepping while the client clock drifts.
    if (std::fabs(current.timeOfDay - state.timeOfDay) > 0.02f) {
        if (Call(manager, "SetWorldTimeOfDay").set("TimeOfDay", state.timeOfDay)
                .setBool("CanAdvanceDay", false).invoke())
            if (std::fabs(current.timeOfDay - state.timeOfDay) > 0.25f)
                Log("world sync: time %.3f -> %.3f", current.timeOfDay, state.timeOfDay);
    }
    bool transitioning = true;
    if (current.weather != state.weather && GetProp(manager, "IsWeatherTransitioning", transitioning) &&
        !transitioning) {
        if (Call(manager, "SelectWorldWeather")
                .set("NewWeather", state.weather)
                .set("NewWeatherTransitionRate", state.transitionRate)
                .set("RelativeTime", state.relativeTime).invoke())
            Log("world sync: weather %u -> %u", current.weather, state.weather);
    }
}

void RestoreWorldSnapshot() {
    auto manager = g_worldRestore.manager;
    auto state = g_worldRestore.state;
    if (!state.valid || !IsLive(manager) || !WorldSyncAllowed(manager)) return;
    WorldSnapshot current{};
    if (!ReadWorldSnapshot(manager, current)) return;
    if (std::fabs(current.timeOfDay - state.timeOfDay) > 0.1f &&
        !Call(manager, "SetWorldTimeOfDay").set("TimeOfDay", state.timeOfDay)
            .setBool("CanAdvanceDay", false).invoke()) return;
    bool transitioning = true;
    if (current.weather != state.weather) {
        if (!GetProp(manager, "IsWeatherTransitioning", transitioning) || transitioning) return;
        if (!Call(manager, "SelectWorldWeather")
                .set("NewWeather", state.weather)
                .set("NewWeatherTransitionRate", state.transitionRate)
                .set("RelativeTime", state.relativeTime).invoke()) return;
    }
    Log("world sync: restored local session time %.3f -> %.3f weather %u -> %u",
        current.timeOfDay, state.timeOfDay, current.weather, state.weather);
    g_worldRestore = {};
}

void UpdateWorldSync(UObject* pawn, double now) {
    if (pawn != g_worldPawn) {
        if (g_worldRestore.state.valid && g_worldRestore.manager == g_worldManager)
            RestoreWorldSnapshot();
        g_worldPawn = pawn;
        g_worldManager = nullptr;
        g_worldRestore = {};
        g_worldState = {};
        g_worldScanAt = -10;
        g_worldUpdateAt = -10;
    }
    if (!pawn) {
        g_worldState = {};
        return;
    }
    if (!IsLive(g_worldManager) && now >= g_worldScanAt) {
        g_worldScanAt = now + 2.0;
        g_worldManager = FindWorldManager();
        if (IsLive(g_worldManager)) Log("world sync: manager %s", FullName(g_worldManager).c_str());
    }
    if (!IsLive(g_worldManager)) {
        g_worldState = {};
        return;
    }
    if (now < g_worldUpdateAt) return;
    g_worldUpdateAt = now + 0.5;
    if (IsHost()) {
        // A former client (e.g. after leaving a Steam lobby) first returns to its own time and weather.
        if (g_worldRestore.state.valid) {
            if (g_worldRestore.manager == g_worldManager) RestoreWorldSnapshot();
            else g_worldRestore = {};
            if (g_worldRestore.state.valid) {
                g_worldState = {};
                return;
            }
        }
        WorldSnapshot state{};
        bool valid = ReadWorldSnapshot(g_worldManager, state);
        if (valid && (!g_worldState.valid || std::fabs(g_worldState.timeOfDay - state.timeOfDay) > 0.1f ||
                      g_worldState.weather != state.weather))
            Log("world sync: host time=%.3f relative=%.3f weather=%u transition=%u",
                state.timeOfDay, state.relativeTime, state.weather, state.transitionRate);
        g_worldState = valid ? state : WorldSnapshot{};
        return;
    }
    bool peerConnected = now - g_lastRecv <= kPeerTimeout;
    if (!WorldSyncAllowed(g_worldManager)) return;
    if (!peerConnected || !(g_peer.flags & kWorldState)) {
        RestoreWorldSnapshot();
        return;
    }
    if (!g_worldRestore.state.valid || g_worldRestore.manager != g_worldManager) {
        WorldSnapshot original{};
        if (!ReadWorldSnapshot(g_worldManager, original)) return;
        g_worldRestore = {g_worldManager, original};
        Log("world sync: captured local session time/weather");
    }
    WorldSnapshot remote{true, g_peer.worldTimeOfDay, g_peer.worldRelativeTime,
                         g_peer.worldWeather, g_peer.worldTransitionRate};
    ApplyWorldSnapshot(g_worldManager, remote);
}

// Stale controllers from an unloaded world can linger until GC, so prefer the newest one whose pawn
// points back at it.
UObject* FindLocalController() {
    if (IsLive(g_localPlayer)) {
        auto current = Prop(g_localPlayer, "PlayerController");
        if (IsLive(current) && Prop(current, "Player") == g_localPlayer) return current;
    }
    UObject* best = nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || !IsA(o, C.playerController) || IsDefaultOrArchetype(o) || !IsLive(o)) continue;
        auto pawn = Prop(o, "Pawn");
        if (!IsLive(pawn) || Prop(pawn, "Controller") != o || !IsLive(Prop(o, "Player"))) continue;
        if (!best || o->internalIndex > best->internalIndex) best = o;
    }
    if (best) g_localPlayer = Prop(best, "Player");
    return best;
}

void DumpClass(FILE* file, UObject* cls, bool includeParents = true) {
    for (auto type = cls; type; type = includeParents ? Super(type) : nullptr) {
        std::fprintf(file, "\nCLASS %s\n", FullName(type).c_str());
        auto field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(type) + off::StructChildren);
        for (int budget = 0; field && budget < 4096; ++budget) {
            bool function = Name(field->cls) == "Function";
            std::fprintf(file, "  %s %s offset=%d\n", Name(field->cls).c_str(), Name(field).c_str(),
                         function ? -1 : PropertyOffset(field));
            if (function) {
                auto parameter = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(field) + off::StructChildren);
                for (int p = 0; parameter && p < 128; ++p) {
                    std::fprintf(file, "    %s %s offset=%d size=%d\n", Name(parameter->cls).c_str(),
                                 Name(parameter).c_str(), PropertyOffset(parameter),
                                 *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(parameter) + off::PropertyElementSize));
                    parameter = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(parameter) + off::FieldNext);
                }
            }
            field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(field) + off::FieldNext);
        }
    }
}

void DumpObjectReferences(FILE* file, UObject* object) {
    std::fprintf(file, "REFS %s\n", FullName(object).c_str());
    for (auto type = IsLive(object) ? object->cls : nullptr; type; type = Super(type)) {
        auto field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(type) + off::StructChildren);
        for (int budget = 0; field && budget < 4096; ++budget) {
            auto kind = Name(field->cls);
            if (kind == "ObjectProperty" || kind == "ClassProperty") {
                UObject* value = nullptr;
                std::memcpy(&value, reinterpret_cast<uint8_t*>(object) + PropertyOffset(field), sizeof value);
                if (value) std::fprintf(file, "  %s.%s = %s\n", Name(type).c_str(), Name(field).c_str(), FullName(value).c_str());
            }
            field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(field) + off::FieldNext);
        }
    }
}

void DiscoverMenus() {
    FILE* file = _wfopen((g_basePath + L"-menus.txt").c_str(), L"w");
    if (!file) {
        Log("menu discovery: cannot write native menu report");
        return;
    }
    std::unordered_set<UObject*> classes;
    auto classType = FindClass("Class");
    auto widgetType = FindClass("Widget");
    auto menuType = FindClass("BaseMenu");
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        auto object = ObjectAt(i);
        if (!IsLive(object) || !IsLive(object->cls)) continue;
        std::string name = Name(object);
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (classType && IsA(object, classType)) {
            if (lower.find("menu") != std::string::npos || lower.find("widget") != std::string::npos ||
                lower.find("button") != std::string::npos || lower.find("textblock") != std::string::npos ||
                lower.find("panel") != std::string::npos || lower.find("uimanager") != std::string::npos)
                classes.insert(object);
            continue;
        }
        if (IsDefaultOrArchetype(object)) continue;
        std::string typeName = Name(object->cls);
        std::string lowerType = typeName;
        std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if ((widgetType && IsA(object, widgetType)) || lowerType.find("menu") != std::string::npos ||
            lowerType.find("uimanager") != std::string::npos || (menuType && IsA(object, menuType))) {
            std::fprintf(file, "\nOBJECT %s\n", FullName(object).c_str());
            DumpObjectReferences(file, object);
            if (Name(object->cls) == "UI_MainMenuWidget_C") {
                for (auto key : {"CursorPosition", "MaxMenuOptions", "ActiveMenuPageIndex"}) {
                    int value = 0;
                    if (GetProp(object, key, value)) std::fprintf(file, "  VALUE %s=%d\n", key, value);
                }
                struct Array { void* data; int32_t count, capacity; };
                for (auto key : {"MenuOptionNames_Text", "MainTextFields", "ActiveTextFields", "ActiveOptionsNames_Text"}) {
                    Array value{};
                    if (GetProp(object, key, value))
                        std::fprintf(file, "  ARRAY %s count=%d capacity=%d\n", key, value.count, value.capacity);
                }
            }
            classes.insert(object->cls);
        }
    }
    for (auto name : {"KismetTextLibrary", "KismetSystemLibrary", "GameplayStatics"})
        if (auto cls = FindClass(name)) classes.insert(cls);
    if (auto library = FindDefaultObject("UIManager")) {
        Call get(library, "GetUIManager");
        if (get.invoke()) {
            auto manager = get.get<UObject*>("ReturnValue");
            if (IsLive(manager)) {
                std::fprintf(file, "\nUI MANAGER %s\n", FullName(manager).c_str());
                DumpObjectReferences(file, manager);
                struct Array { uint8_t* data; int32_t count, capacity; };
                for (auto key : {"MenuWidgetClass", "Menus", "HudMenus"}) {
                    Array entries{};
                    if (!GetProp(manager, key, entries) || entries.count < 0 ||
                        entries.count > 128 || entries.count > entries.capacity) continue;
                    std::fprintf(file, "  ARRAY %s count=%d capacity=%d\n", key, entries.count, entries.capacity);
                }
            }
        }
    }
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        auto object = ObjectAt(i);
        if (!IsLive(object) || !IsLive(object->cls) || Name(object->cls) != "ScriptStruct") continue;
        std::string name = Name(object);
        if (name.find("Notification") == std::string::npos && name.find("MenuProfile") == std::string::npos &&
            name.find("Category") == std::string::npos && name.find("MenuOption") == std::string::npos)
            continue;
        std::fprintf(file, "\nSTRUCT %s\n", FullName(object).c_str());
        auto field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(object) + off::StructChildren);
        for (int budget = 0; field && budget < 256; ++budget) {
            std::fprintf(file, "  %s %s offset=%d size=%d\n", Name(field->cls).c_str(), Name(field).c_str(),
                         PropertyOffset(field),
                         *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(field) + off::PropertyElementSize));
            field = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(field) + off::FieldNext);
        }
    }
    std::vector<UObject*> sortedClasses(classes.begin(), classes.end());
    std::sort(sortedClasses.begin(), sortedClasses.end(),
              [](UObject* a, UObject* b) { return FullName(a) < FullName(b); });
    for (auto cls : sortedClasses) DumpClass(file, cls, false);
    std::fclose(file);
    Log("menu discovery: native widget report written (%u classes)", static_cast<unsigned>(classes.size()));
}

void Discover(UObject* pawn) {
    Log("discovery: controller %s", FullName(g_controller).c_str());
    std::string chain;
    for (auto c = pawn->cls; c; c = Super(c)) chain += Name(c) + (Super(c) ? " < " : "");
    Log("discovery: pawn %s (%s)", FullName(pawn).c_str(), chain.c_str());
    auto mainMesh = Prop(pawn, "Mesh");
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->outer != pawn) continue;
        std::string extra;
        if (IsA(o, C.skelMeshComponent)) {
            extra = " mesh=" + FullName(Prop(o, "SkeletalMesh")) + " anim=" + FullName(Prop(o, "AnimBlueprintGeneratedClass")) +
                    " master=" + Name(WeakProp(o, "MasterPoseComponent")) + (o == mainMesh ? " [Mesh]" : "");
        }
        Log("discovery: component %s %s%s", Name(o->cls).c_str(), Name(o).c_str(), extra.c_str());
    }
    if (!C.animSequence) return;
    std::wstring path = g_basePath + L"-anims.txt";
    FILE* f = _wfopen(path.c_str(), L"w");
    int count = 0;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || !IsA(o, C.animSequence) || IsDefaultOrArchetype(o)) continue;
        if (f) std::fprintf(f, "%s\n", FullName(o).c_str());
        ++count;
    }
    if (f) std::fclose(f);
    Log("discovery: %d loaded AnimSequence objects listed in %s", count, Narrow(path).c_str());
    path = g_basePath + L"-reflection.txt";
    f = _wfopen(path.c_str(), L"w");
    if (!f) {
        Log("discovery: cannot write reflection report");
        return;
    }
    const char* classes[] = {"VehiclePawn", "BendVehicleSeatsComponent", "BendRideVehicleComponent",
                             "BendPlayerPawn", "BendWeapon", "HUD", "BendHUD", "Canvas", "MaterialInstanceDynamic",
                             "BendDamageComponent", "BendHealthComponent"};
    for (const char* name : classes)
        if (auto cls = FindClass(name)) DumpClass(f, cls);
    auto vehicles = FindClass("VehiclePawn");
    auto materials = FindClass("MaterialInstance");
    struct Array { uint8_t* data; int32_t count, capacity; };
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto obj = ObjectAt(i);
        if (!IsLive(obj) || IsDefaultOrArchetype(obj)) continue;
        if (vehicles && IsA(obj, vehicles)) {
            std::fprintf(f, "\nVEHICLE %s\n", FullName(obj).c_str());
            for (int32_t j = 0; j < n; ++j) {
                auto component = ObjectAt(j);
                if (IsLive(component) && component->outer == obj)
                    std::fprintf(f, "  COMPONENT %s mesh=%s\n", FullName(component).c_str(),
                                 FullName(Prop(component, "SkeletalMesh")).c_str());
            }
        }
        if (materials && IsA(obj, materials)) {
            Array values{};
            if (!GetProp(obj, "VectorParameterValues", values) || !values.data ||
                values.count < 1 || values.count > 128 || values.count > values.capacity) continue;
            std::fprintf(f, "\nMATERIAL %s parent=%s\n", FullName(obj).c_str(), FullName(Prop(obj, "Parent")).c_str());
            for (int j = 0; j < values.count; ++j) {
                FName name{};
                std::memcpy(&name, values.data + j * 0x28, sizeof name);
                std::fprintf(f, "  VECTOR %s\n", NameString(name.index).c_str());
            }
            if (FullName(obj).find("/Characters/Deacon/") != std::string::npos) {
                for (auto field : {"ScalarParameterValues", "TextureParameterValues"}) {
                    Array entries{};
                    if (!GetProp(obj, field, entries) || !entries.data || entries.count < 0 ||
                        entries.count > 128 || entries.count > entries.capacity) continue;
                    for (int j = 0; j < entries.count; ++j) {
                        FName name{};
                        std::memcpy(&name, entries.data + j * 0x20, sizeof name);
                        if (std::strcmp(field, "TextureParameterValues") == 0) {
                            UObject* texture = nullptr;
                            std::memcpy(&texture, entries.data + j * 0x20 + 8, sizeof texture);
                            std::fprintf(f, "  TEXTURE %s = %s\n", NameString(name.index).c_str(), FullName(texture).c_str());
                        } else {
                            float value = 0;
                            std::memcpy(&value, entries.data + j * 0x20 + 8, sizeof value);
                            std::fprintf(f, "  SCALAR %s = %f\n", NameString(name.index).c_str(), value);
                        }
                    }
                }
            }
        }
    }
    const char* terms[] = {"time", "weather", "climate", "daynight", "worldmanager", "freaker",
                           "freakercontroller", "aipawn", "enemy", "infected", "aicontroller", "spawner"};
    auto matches = [&](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto* term : terms)
            if (text.find(term) != std::string::npos) return true;
        return false;
    };
    auto classClass = FindClass("Class");
    auto actorClass = FindClass("Actor");
    auto componentClass = FindClass("ActorComponent");
    std::vector<UObject*> emittedClasses;
    int reportedObjects = 0;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto obj = ObjectAt(i);
        if (!IsLive(obj) || IsDefaultOrArchetype(obj) || !IsLive(obj->cls)) continue;
        bool classObject = classClass && IsA(obj, classClass);
        bool actorObject = actorClass && IsA(obj, actorClass);
        bool componentObject = componentClass && IsA(obj, componentClass);
        if (!classObject && !actorObject && !componentObject) continue;
        const std::string className = Name(classObject ? obj : obj->cls);
        const std::string objectName = Name(obj);
        if (!matches(className) && !matches(objectName)) continue;
        if (reportedObjects < 512) {
            std::fprintf(f, "\nDISCOVERY_MATCH %s class=%s\n", FullName(obj).c_str(), FullName(obj->cls).c_str());
            ++reportedObjects;
        }
        if (actorObject)
            std::fprintf(f, "  ACTOR %s\n", FullName(obj).c_str());
        UObject* candidateClass = classObject ? obj : obj->cls;
        if (emittedClasses.size() < 64 &&
            std::find(emittedClasses.begin(), emittedClasses.end(), candidateClass) == emittedClasses.end()) {
            emittedClasses.push_back(candidateClass);
            DumpClass(f, candidateClass);
        }
    }
    Log("discovery: wrote %d time/weather/enemy candidate objects and %d candidate classes",
        reportedObjects, static_cast<int>(emittedClasses.size()));
    std::fclose(f);
    Log("discovery: reflection report written to %s", Narrow(path).c_str());
}

UObject* SpawnMeshActor(UObject* context, const FVector& at);

bool DiscoverDebug(UObject* pawn) {
    auto path = g_basePath + L"-debug.txt";
    FILE* file = _wfopen(path.c_str(), L"w");
    if (!file) {
        g_settingsError = "Cannot write the native debug API report";
        Log("debug: cannot write discovery report");
        return false;
    }
    std::fprintf(file, "CONTEXT pawn=%s controller=%s cheat-manager=%s cheat-class=%s\n",
        FullName(pawn).c_str(), FullName(g_controller).c_str(),
        FullName(Prop(g_controller, "CheatManager")).c_str(), FullName(Prop(g_controller, "CheatClass")).c_str());
    const char* required[] = {"PlayerController", "BendPlayerController", "BendPlayerPawn", "BendPawn",
        "BendGameMode", "GameMode", "GameplayStatics", "AIBlueprintHelperLibrary", "AIController",
        "BendAIController", "BendAIPawn", "BendFreakerPawn", "CheatManager",
        "BrainComponent", "BehaviorTreeComponent", "NavigationSystem"};
    std::vector<UObject*> emitted;
    for (const auto* name : required) {
        if (auto cls = FindClass(name)) {
            emitted.push_back(cls);
            DumpClass(file, cls, false);
        }
    }
    auto classClass = FindClass("Class");
    auto aiPawnClass = FindClass("BendAIPawn");
    auto matches = [](std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto* term : {"cheat", "debug", "spawner", "spawnmanager", "population", "horde", "swarmer"})
            if (name.find(term) != std::string::npos) return true;
        return false;
    };
    int aiCount = 0;
    UObject* nearestAI = nullptr;
    float nearestDistance = 1e12f;
    Call playerAt(pawn, "K2_GetActorLocation");
    FVector player = playerAt.invoke() ? playerAt.get<FVector>("ReturnValue") : FVector{};
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        auto object = ObjectAt(i);
        if (!IsLive(object) || IsDefaultOrArchetype(object) || !IsLive(object->cls)) continue;
        if (classClass && IsA(object, classClass) && matches(Name(object)) &&
            std::find(emitted.begin(), emitted.end(), object) == emitted.end()) {
            emitted.push_back(object);
            DumpClass(file, object, false);
        }
        if (aiPawnClass && IsA(object, aiPawnClass)) {
            ++aiCount;
            Call location(object, "K2_GetActorLocation");
            float health = 0;
            if (!location.invoke() || !GetProp(object, "Health", health)) {
                std::fprintf(file, "AI unreadable=%s\n", FullName(object).c_str());
                continue;
            }
            auto at = location.get<FVector>("ReturnValue");
            std::fprintf(file, "AI %s health=%.3f at=(%.1f %.1f %.1f) controller=%s mesh=%s\n",
                FullName(object).c_str(), health, at.x, at.y, at.z,
                FullName(Prop(object, "Controller")).c_str(),
                FullName(Prop(Prop(object, "Mesh"), "SkeletalMesh")).c_str());
            float d = (at.x - player.x) * (at.x - player.x) + (at.y - player.y) * (at.y - player.y);
            if (Prop(Prop(object, "Mesh"), "SkeletalMesh") && d < nearestDistance) {
                nearestDistance = d;
                nearestAI = object;
            }
        } else if (matches(Name(object->cls))) {
            std::fprintf(file, "INSTANCE %s\n", FullName(object).c_str());
        }
    }
    if (auto manager = FindWorldManager()) DumpClass(file, manager->cls, true);
    for (const auto* name : {"SkinnedMeshComponent", "PoseableMeshComponent", "SkeletalMeshComponent",
                             "Actor", "BlueprintGeneratedClass", "SkeletalMeshActor", "StaticMeshComponent"})
        if (auto cls = FindClass(name)) DumpClass(file, cls, false);
    auto dumpBones = [&](const char* label, UObject* mesh) {
        BoneTable table;
        if (!IsLive(mesh) || !ReadBoneTable(mesh, table)) {
            std::fprintf(file, "\nBONES %s unavailable\n", label);
            return;
        }
        std::fprintf(file, "\nBONES %s %s mesh=%s count=%zu\n", label, FullName(mesh).c_str(),
                     FullName(table.asset).c_str(), table.names.size());
        for (size_t b = 0; b < table.names.size(); ++b)
            std::fprintf(file, "  %zu %s parent=%d\n", b, NameString(table.names[b].index).c_str(), table.parents[b]);
        PoseReader reader;
        std::vector<BonePose> pose;
        double start = Now();
        bool read = reader.Read(mesh, table, table.names.size(), pose);
        std::fprintf(file, "POSEREAD %s ok=%d bones=%zu ms=%.3f\n", label, read, pose.size(), (Now() - start) * 1000);
        for (size_t b = 0; read && b < pose.size() && b < 4; ++b)
            std::fprintf(file, "  %zu t=(%.2f %.2f %.2f) q=(%.3f %.3f %.3f %.3f)\n", b, pose[b].t.x, pose[b].t.y,
                         pose[b].t.z, pose[b].q[0], pose[b].q[1], pose[b].q[2], pose[b].q[3]);
    };
    dumpBones("player", Prop(pawn, "Mesh"));
    if (IsLive(g_localBike)) {
        dumpBones("bike", Prop(g_localBike, "VehicleMesh"));
        auto sceneClass = FindClass("SceneComponent");
        for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
            auto o = ObjectAt(i);
            if (!o || o->outer != g_localBike || !IsLive(o) || !IsA(o, sceneClass)) continue;
            Call visible(o, "IsVisible");
            int vis = visible.invoke() ? visible.get<bool>("ReturnValue") : -1;
            FName socket{};
            GetProp(o, "AttachSocketName", socket);
            std::fprintf(file, "BIKECOMP %s class=%s skel=%s static=%s parent=%s socket=%s master=%s visible=%d\n",
                         Name(o).c_str(), Name(o->cls).c_str(), FullName(Prop(o, "SkeletalMesh")).c_str(),
                         FullName(Prop(o, "StaticMesh")).c_str(), Name(Prop(o, "AttachParent")).c_str(),
                         NameString(socket.index).c_str(), Name(WeakProp(o, "MasterPoseComponent")).c_str(), vis);
        }
        for (auto part : OwnedMeshes(g_nativeBikeMeshes, g_localBike, Now()))
            std::fprintf(file, "BIKEPART %s mesh=%s master=%s anim=%s\n", FullName(part).c_str(),
                         FullName(Prop(part, "SkeletalMesh")).c_str(), Name(WeakProp(part, "MasterPoseComponent")).c_str(),
                         FullName(Prop(part, "AnimBlueprintGeneratedClass")).c_str());
    }
    auto dumpEnemy = [&](const char* label, UObject* enemyPawn) {
            auto enemyMesh = Prop(enemyPawn, "Mesh");
            dumpBones(label, enemyMesh);
            std::fprintf(file, "ENEMYLOOK pawn=%s lookComponent=%s meshLook=%s\n", FullName(enemyPawn).c_str(),
                         FullName(Prop(enemyPawn, "LookComponent")).c_str(), FullName(Prop(enemyMesh, "Look")).c_str());
            if (auto look = Prop(enemyPawn, "LookComponent"); IsLive(look)) {
                DumpClass(file, look->cls, false);
                DumpObjectReferences(file, look);
            }
            DumpObjectReferences(file, enemyPawn);
            for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
                auto o = ObjectAt(i);
                if (!o || !IsLive(o) || !IsA(o, C.skelMeshComponent)) continue;
                if (o->outer != enemyPawn && Prop(o, "AttachParent") != enemyMesh) continue;
                Call visible(o, "IsVisible");
                std::fprintf(file, "ENEMYPART %s class=%s outer=%s mesh=%s master=%s look=%s visible=%d\n", FullName(o).c_str(),
                             Name(o->cls).c_str(), Name(o->outer).c_str(), FullName(Prop(o, "SkeletalMesh")).c_str(),
                             Name(WeakProp(o, "MasterPoseComponent")).c_str(), FullName(Prop(o, "Look")).c_str(),
                             visible.invoke() ? visible.get<bool>("ReturnValue") : -1);
            }
    };
    for (const auto& enemy : g_debug.enemies)
        if (IsLive(enemy.pawn.get())) {
            dumpEnemy("enemy", enemy.pawn.get());
            break;
        }
    if (IsLive(nearestAI)) {
        std::fprintf(file, "\nNEARESTAI %s distance=%.0f\n", FullName(nearestAI).c_str(), std::sqrt(nearestDistance));
        dumpEnemy("nearest-ai", nearestAI);
    }
    if (auto held = Call(pawn, "GetEquippedItemCurrentlyInHand"); held.invoke()) {
        auto weapon = held.get<UObject*>("ReturnValue");
        std::fprintf(file, "\nWEAPON %s class=%s\n", FullName(weapon).c_str(), IsLive(weapon) ? FullName(weapon->cls).c_str() : "-");
        for (int32_t i = 0, n = IsLive(weapon) ? ObjectCount() : 0; i < n; ++i) {
            auto o = ObjectAt(i);
            if (o && o->outer == weapon && IsLive(o))
                std::fprintf(file, "  WEAPONPART %s class=%s mesh=%s static=%s attach=%s\n", FullName(o).c_str(),
                             Name(o->cls).c_str(), FullName(Prop(o, "SkeletalMesh")).c_str(),
                             FullName(Prop(o, "StaticMesh")).c_str(), Name(Prop(o, "AttachParent")).c_str());
        }
    }
    Call pawnAt(pawn, "K2_GetActorLocation");
    FVector below = pawnAt.invoke() ? pawnAt.get<FVector>("ReturnValue") : FVector{};
    below.z -= 500;
    if (auto test = SpawnMeshActor(pawn, below)) {
        auto playerMesh = Prop(pawn, "Mesh");
        auto created = CreatePoseableComponent(test);
        bool meshSet = created && Call(created, "SetSkeletalMesh").set("NewMesh", Prop(playerMesh, "SkeletalMesh")).invoke();
        PoseWriter writer;
        bool attached = meshSet && writer.Attach(created);
        bool applied = false, matched = false;
        if (attached) {
            BoneTable table;
            PoseReader reader;
            std::vector<BonePose> pose;
            if (ReadBoneTable(playerMesh, table) && reader.Read(playerMesh, table, table.names.size(), pose)) {
                PoseFrame frame;
                frame.names = table.hashes;
                frame.bones = pose;
                applied = writer.Apply(frame);
                Call get(created, "GetBoneTransformByName");
                size_t probe = std::min<size_t>(1, pose.size() - 1);
                get.set("BoneName", table.names[probe]).set<uint8_t>("BoneSpace", 1);
                Call ref(playerMesh, "GetSocketTransform");
                ref.set("InSocketName", table.names[probe]).set<uint8_t>("TransformSpace", 2);
                if (applied && get.invoke() && ref.invoke()) {
                    auto a = get.get<FTransform>("ReturnValue").translation, b = ref.get<FTransform>("ReturnValue").translation;
                    matched = std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) < 0.5f;
                    std::fprintf(file, "POSEWRITE probe=%zu poseable=(%.2f %.2f %.2f) source=(%.2f %.2f %.2f)\n", probe,
                                 a.x, a.y, a.z, b.x, b.y, b.z);
                }
            }
        }
        std::fprintf(file, "POSEABLE_SELFTEST created=%s mesh=%d attach=%d offset=%d applied=%d matched=%d\n",
                     FullName(created).c_str(), meshSet, attached, attached ? writer.Offset() : -1, applied, matched);
        Log("debug: poseable self-test created=%d mesh=%d attach=%d applied=%d matched=%d", created != nullptr, meshSet,
            attached, applied, matched);
        Call(test, "K2_DestroyActor").invoke();
    }
    auto poseable = FindClass("PoseableMeshComponent");
    auto bpgc = FindClass("BlueprintGeneratedClass");
    for (int32_t i = 0, count = ObjectCount(); i < count; ++i) {
        auto object = ObjectAt(i);
        if (!object || !IsLive(object->cls)) continue;
        if (poseable && IsA(object, poseable))
            std::fprintf(file, "POSEABLE %s outer=%s\n", FullName(object).c_str(), FullName(object->outer).c_str());
        if (bpgc && IsA(object, bpgc) && IsLive(object)) {
            struct Array { UObject** data; int32_t count, capacity; } templates{};
            if (GetProp(object, "ComponentTemplates", templates) && templates.data && templates.count > 0 &&
                templates.count <= templates.capacity && templates.count < 256)
                for (int t = 0; t < templates.count; ++t)
                    if (poseable && templates.data[t] && IsA(templates.data[t], poseable))
                        std::fprintf(file, "POSEABLE_TEMPLATE %s in %s\n", FullName(templates.data[t]).c_str(),
                                     FullName(object).c_str());
        }
    }
    std::fprintf(file, "\nSUMMARY classes=%llu ai-pawns=%d\n",
        static_cast<unsigned long long>(emitted.size()), aiCount);
    bool written = std::ferror(file) == 0;
    written = std::fclose(file) == 0 && written;
    if (!written) {
        g_settingsError = "Writing the native debug API report failed";
        Log("debug: discovery report write failed");
        return false;
    }
    Log("debug: report %s classes=%llu ai-pawns=%d", Narrow(path).c_str(),
        static_cast<unsigned long long>(emitted.size()), aiCount);
    return true;
}

bool ReadPoseSource(PoseSource& source, UObject* mesh, bool body) {
    if (!IsLive(mesh)) return false;
    auto asset = Prop(mesh, "SkeletalMesh");
    if (mesh != source.mesh || asset != source.asset || !source.table.Valid()) {
        source = PoseSource{};
        if (!ReadBoneTable(mesh, source.table)) return false;
        source.mesh = mesh;
        source.asset = asset;
        if (body) {
            std::vector<std::string> names;
            for (auto name : source.table.names) names.push_back(NameString(name.index));
            source.subset = BodyBoneSubset(names, source.table.parents, g_cfg.poseFace);
            for (int i : source.subset) source.names.push_back(source.table.hashes[i]);
        } else {
            source.names = source.table.hashes;
        }
        Log("pose: streaming %d of %d bones of %s", static_cast<int>(source.names.size()),
            static_cast<int>(source.table.hashes.size()), Name(asset).c_str());
    }
    if (source.names.empty()) return false;
    double start = Now();
    bool read;
    if (source.subset.empty()) {
        read = source.reader.Read(mesh, source.table, source.names.size(), source.bones);
    } else {
        read = source.reader.Read(mesh, source.table, static_cast<size_t>(source.subset.back()) + 1, source.all);
        source.bones.clear();
        if (read)
            for (int i : source.subset) source.bones.push_back(source.all[i]);
    }
    g_poseStats.readTime += Now() - start;
    ++g_poseStats.reads;
    return read;
}

// Streams the evaluated local pose of `mesh`; the receiver drives a poseable copy with it.
void SendPose(PoseKind kind, PoseSource& source, UObject* mesh, uint32_t timeMs, float health, bool withRoot) {
    if (!ReadPoseSource(source, mesh, kind == kPoseBody)) return;
    PoseHeader header{};
    header.kind = kind;
    header.session = g_session;
    header.timeMs = timeMs;
    header.health = std::clamp(health, 0.f, 100000.f);
    size_t real = source.names.size();
    if (withRoot) {
        BonePose root;
        Call rotation(mesh, "K2_GetComponentRotation");
        if (rotation.invoke()) {
            auto r = rotation.get<FRotator>("ReturnValue");
            RotatorToQuat(r.pitch, r.yaw, r.roll, root.q);
            header.yaw = r.yaw;
            source.names.push_back(kRootBoneHash);
            source.bones.push_back(root);
        }
    }
    EncodePose(header, source.names, source.bones, [](const char* packet, size_t size) {
        g_net->SendRaw(packet, static_cast<int>(size));
        ++g_poseStats.sent;
        g_poseStats.bytes += size;
    }, &source.tracker);
    source.names.resize(real);
    source.bones.resize(real);
}

void SendLocalState(UObject* pawn, double now) {
    StatePacket p{};
    p.magic = kMagic;
    p.version = kProtocolVersion;
    p.seq = ++g_seq;
    p.timeMs = static_cast<uint32_t>(static_cast<uint64_t>(now * 1000.0));
    p.session = g_session;
    p.targetSession = g_peer.session;
    p.consent = g_consent;
    p.jacket = static_cast<uint8_t>(g_settings.jacket);
    p.trousers = static_cast<uint8_t>(g_settings.trousers);
    p.capColor = static_cast<uint8_t>(g_settings.capColor);
    p.cap = g_settings.cap;
    if (IsHost() && g_worldState.valid) {
        p.flags |= kWorldState;
        p.worldTimeOfDay = g_worldState.timeOfDay;
        p.worldRelativeTime = g_worldState.relativeTime;
        p.worldWeather = g_worldState.weather;
        p.worldTransitionRate = g_worldState.transitionRate;
    }
    if (g_settings.friendlyFire) p.flags |= kFriendlyFire;
    if (g_settings.motorcycles) p.flags |= kSyncBikes;
    if (g_settings.enemies) p.flags |= kSyncEnemies;
    if (g_settings.passengers) p.flags |= kAcceptPassengers;
    std::snprintf(p.name, sizeof p.name, "%s", session::TruncateUtf8(LocalName(), sizeof p.name - 1).c_str());
    g_damage.Fill(p);
    if (pawn) {
        uint8_t crouched = 0;
        if (GetProp(pawn, "bIsCrouched", crouched) && crouched) p.actions |= kCrouched;
        Call aiming(pawn, "IsAiming");
        if (aiming.invoke() && aiming.get<bool>("ReturnValue")) p.actions |= kAiming;
        Call reloading(pawn, "IsReloading");
        if (reloading.invoke() && reloading.get<bool>("ReturnValue")) p.actions |= kReloading;
        Call held(pawn, "GetEquippedItemCurrentlyInHand");
        UObject* weapon = held.invoke() ? held.get<UObject*>("ReturnValue") : nullptr;
        if (IsLive(weapon)) {
            Call crossbow(weapon, "IsCrossbow"), gun(weapon, "IsGun"), melee(weapon, "IsMelee");
            if (crossbow.invoke() && crossbow.get<bool>("ReturnValue")) p.weaponType = kWeaponCrossbow;
            else if (gun.invoke() && gun.get<bool>("ReturnValue")) p.weaponType = kWeaponGun;
            else if (melee.invoke() && melee.get<bool>("ReturnValue")) p.weaponType = kWeaponMelee;
        }
        p.shotSequence = g_localShotSequence;
        static uint8_t lastActions = 0xff, lastWeaponType = 0xff;
        if (g_cfg.discovery && (p.actions != lastActions || p.weaponType != lastWeaponType)) {
            Log("presentation: local actions=0x%x weapon=%u", p.actions, p.weaponType);
            lastActions = p.actions;
            lastWeaponType = p.weaponType;
        }
        UObject* mesh = g_passenger.pawn == pawn && IsLive(g_passengerAvatar.component) ?
            g_passengerAvatar.component : IsA(pawn, C.character) ? Prop(pawn, "Mesh") : nullptr;
        FVector pos{};
        FRotator rot{};
        if (mesh) {
            Call loc(mesh, "K2_GetComponentLocation"), r(mesh, "K2_GetComponentRotation");
            loc.invoke();
            r.invoke();
            pos = loc.get<FVector>("ReturnValue");
            rot = r.get<FRotator>("ReturnValue");
        } else {
            Call loc(pawn, "K2_GetActorLocation"), r(pawn, "K2_GetActorRotation");
            loc.invoke();
            r.invoke();
            pos = loc.get<FVector>("ReturnValue");
            rot = r.get<FRotator>("ReturnValue");
            p.flags |= kInVehicle;
        }
        Call vel(pawn, "GetVelocity");
        vel.invoke();
        FVector v = vel.get<FVector>("ReturnValue");
        FVector origin = WorldOrigin(pawn);
        p.flags |= kHasPawn;
        GetProp(pawn, "Health", p.health);
        // Native lethal damage can leave negative health; the wire uses zero for death.
        if (p.health < 0) p.health = 0;
        Call alive(pawn, "IsAlive");
        if (alive.invoke() && alive.get<bool>("ReturnValue")) p.flags |= kAlive;
        Call mounted(pawn, "IsInVehicle");
        if (mounted.invoke() && mounted.get<bool>("ReturnValue")) p.flags |= kInVehicle;
        if (g_passenger.pawn == pawn) p.flags |= kPassenger | kInVehicle;
        p.pos[0] = pos.x + origin.x;
        p.pos[1] = pos.y + origin.y;
        p.pos[2] = pos.z + origin.z;
        p.yaw = rot.yaw;
        p.pitch = rot.pitch;
        p.vel[0] = v.x;
        p.vel[1] = v.y;
        p.vel[2] = v.z;
        if (g_settings.motorcycles && IsLive(g_localBike)) {
            auto mesh = Prop(g_localBike, "VehicleMesh");
            Call location(mesh, "K2_GetComponentLocation"), rotation(mesh, "K2_GetComponentRotation");
            if (location.invoke() && rotation.invoke()) {
                auto at = location.get<FVector>("ReturnValue");
                auto r = rotation.get<FRotator>("ReturnValue");
                p.flags |= kHasBike;
                p.bikePos[0] = at.x + origin.x;
                p.bikePos[1] = at.y + origin.y;
                p.bikePos[2] = at.z + origin.z;
                p.bikeRotation[0] = r.pitch;
                p.bikeRotation[1] = r.yaw;
                p.bikeRotation[2] = r.roll;
            }
        }
    }
    g_net->Send(p);
    if (!(p.flags & kHasPawn) || now - g_lastRecv >= kPeerTimeout) return;
    SendGear(pawn, now);
    if (now < g_nextPoseSend) return;
    double poseInterval = 1.0 / g_cfg.poseRate;
    g_nextPoseSend += poseInterval;
    if (g_nextPoseSend <= now) g_nextPoseSend = now + poseInterval;
    UObject* mesh = g_passenger.pawn == pawn && IsLive(g_passengerAvatar.component) ? g_passengerAvatar.component :
        IsA(pawn, C.character) ? Prop(pawn, "Mesh") : nullptr;
    if (mesh) SendPose(kPoseBody, g_bodySource, mesh, p.timeMs, p.health, true);
    if (p.flags & kHasBike) SendPose(kPoseBike, g_bikeSource, Prop(g_localBike, "VehicleMesh"), p.timeMs, 0, false);
}

bool g_saveLoading = false, g_loadAfterAdopt = false;
double g_loadStartedAt = 0;

SaveTransferContext TransferContext(double now) {
    SaveTransferContext context;
    context.net = g_net;
    context.now = now;
    context.session = g_session;
    context.connected = now - g_lastRecv < kPeerTimeout;
    context.peerSession = context.connected ? g_peer.session : 0;
    context.host = IsHost();
    context.inGameplay = g_pawn != nullptr;
    context.loading = g_saveLoading && Now() - g_loadStartedAt < 60;
    return context;
}

void ReceivePose(const char* data, int len, double now) {
    if (SaveTransferHandle(data, len, TransferContext(now))) return;
    uint32_t magic = 0;
    if (len >= 4) std::memcpy(&magic, data, 4);
    if (magic == kEnemyInfoMagic) {
        ReceiveEnemyInfo(data, len, now);
        return;
    }
    if (magic == kGearMagic) {
        ReceiveGear(data, len, now);
        return;
    }
    if (magic == kStoryMagic) {
        ReceiveStory(data, len);
        return;
    }
    if (magic == kEnemyHitMagic) {
        ReceiveEnemyHit(data, len, now);
        return;
    }
    if (magic == kGuestStrikeMagic) {
        ReceiveGuestStrike(data, len);
        return;
    }
    PosePacket packet;
    if (!DecodePose(data, len, packet) || packet.header.session != g_peer.session || !g_peer.session) {
        ++g_poseStats.rejected;
        return;
    }
    ++g_poseStats.received;
    if (packet.header.kind == kPoseBody && g_bodyPoses.Push(packet, now)) g_lastBodyPose = now;
    else if (packet.header.kind == kPoseBike && g_bikePoses.Push(packet, now)) g_lastBikePose = now;
    else if (packet.header.kind == kPoseEnemy) ReceiveEnemyPose(packet, now);
}

void ReceiveRemote(double now) {
    StatePacket p;
    auto other = [now](const char* data, int len) { ReceivePose(data, len, now); };
    for (int budget = 0; budget < 64 && g_net->Receive(p, other); ++budget) {
        bool reconnect = now - g_lastRecv > kPeerTimeout || p.session != g_peer.session;
        if (!reconnect && !NewerSequence(p.seq, g_lastRemoteSeq)) continue;
        if (reconnect) {
            ++g_consent;
            g_motion.Clear();
            g_bodyPoses.Clear();
            g_bikePoses.Clear();
            Log("peer %s (%s) connected", p.name, g_net->PeerText().c_str());
            std::string name = g_kind == session::Kind::Steam ? g_steam.PartnerName() : std::string(p.name);
            if (name.empty()) name = "Your partner";
            g_events.Add(name + " connected");
            g_announcedPartner = name;
            DestroyAvatar();  // Rebuild from the player's current look; also a manual "respawn" lever.
            DestroyBike();
            DestroyEnemyReplicas();
        }
        g_lastRecv = now;
        ++g_receivedStates;
        g_lastRemoteSeq = p.seq;
        g_remoteName = g_kind == session::Kind::Steam ? g_steam.PartnerName() : std::string(p.name);
        g_peer = p;
        UpdateDamageScope(now);
        g_damage.Acknowledge(p);
        ReceiveDamage(p);
        if (!g_motion.Push(p, now))
            Log("network: rejected backward motion timestamp session=%u seq=%u time=%u", p.session, p.seq, p.timeMs);
    }
}

bool SampleRemote(double now, Snapshot& out) {
    return g_motion.Sample(now, out);
}

bool WorldTransform(UObject* component, FName socket, FTransform& out) {
    Call world(component, "GetSocketTransform");
    if (!world.set("InSocketName", socket).set("TransformSpace", static_cast<uint8_t>(0)).invoke()) return false;
    out = world.get<FTransform>("ReturnValue");
    for (float f : {out.rotation.x, out.rotation.y, out.rotation.z, out.rotation.w, out.translation.x, out.translation.y,
                    out.translation.z, out.scale.x, out.scale.y, out.scale.z})
        if (!std::isfinite(f)) return false;
    return true;
}

BonePose AsPose(const FTransform& x) {
    BonePose pose;
    pose.q[0] = x.rotation.x;
    pose.q[1] = x.rotation.y;
    pose.q[2] = x.rotation.z;
    pose.q[3] = x.rotation.w;
    NormalizeQuat(pose.q);
    pose.t = x.translation;
    return pose;
}

// PoseWriter fills the poseable's bone array directly, which bypasses the engine's child-transform update, so
// actors mounted on a streamed socket are re-placed from the live socket every tick.
bool FollowSocket(UObject* actor, UObject* parent, FName socket, const BonePose& offset) {
    FTransform at{};
    if (!IsLive(actor) || !IsLive(parent) || !WorldTransform(parent, socket, at)) return false;
    BonePose world = ComposePose(offset, AsPose(at));
    float pitch = 0, yaw = 0, roll = 0;
    QuatToRotator(world.q, pitch, yaw, roll);
    return Call(actor, "K2_SetActorLocationAndRotation").set("NewLocation", world.t)
        .set("NewRotation", FRotator{pitch, yaw, roll}).setBool("bSweep", false).setBool("bTeleport", true).invoke();
}

UObject* SpawnMeshActor(UObject* context, const FVector& at) {
    FTransform xf{};
    xf.rotation = {0, 0, 0, 1};
    xf.translation = at;
    xf.scale = {1, 1, 1};
    Call begin(C.statics, "BeginSpawningActorFromClass");
    begin.set("WorldContextObject", context).set("ActorClass", C.skelMeshActor).set("SpawnTransform", xf);
    begin.setBool("bNoCollisionFail", true);
    if (!begin.invoke()) return nullptr;
    auto actor = begin.get<UObject*>("ReturnValue");
    if (!actor) return nullptr;
    Call finish(C.statics, "FinishSpawningActor");
    finish.set("Actor", actor).set("SpawnTransform", xf);
    finish.invoke();
    Call(actor, "SetActorEnableCollision").setBool("bNewActorEnableCollision", false).invoke();
    return actor;
}

void DestroyMeshAvatar(Avatar& avatar) {
    for (auto a : avatar.parts)
        if (IsLive(a)) Call(a, "K2_DestroyActor").invoke();
    if (IsLive(avatar.actor)) Call(avatar.actor, "K2_DestroyActor").invoke();
    avatar = {};
}

void DestroyAvatar() {
    DestroyGear();
    DestroyMeshAvatar(g_avatar);
    g_remoteAppearance.Clear();
}

bool CopyMeshAppearance(UObject* source, UObject* destination) {
    Call mesh(destination, "SetSkeletalMesh");
    mesh.set("NewMesh", Prop(source, "SkeletalMesh"));
    if (!mesh.invoke()) {
        Log("avatar: SetSkeletalMesh failed");
        return false;
    }
    Call count(source, "GetNumMaterials");
    if (!count.invoke()) {
        Log("avatar: GetNumMaterials failed");
        return false;
    }
    int materials = count.get<int32_t>("ReturnValue");
    if (materials < 0 || materials > 64) {
        Log("avatar: invalid material count %d", materials);
        return false;
    }
    for (int i = 0; i < materials; ++i) {
        Call get(source, "GetMaterial");
        get.set("ElementIndex", i);
        if (!get.invoke()) {
            Log("avatar: GetMaterial failed for slot %d", i);
            return false;
        }
        auto material = get.get<UObject*>("ReturnValue");
        if (!material) continue;
        material = g_localAppearance.BaseMaterial(source, i, material);
        if (!IsLive(material)) {
            Log("avatar: original material unavailable for slot %d", i);
            return false;
        }
        if (!Call(destination, "SetMaterial").set("ElementIndex", i).set("Material", material).invoke()) {
            Log("avatar: SetMaterial failed for slot %d", i);
            return false;
        }
    }
    return true;
}

void DestroyBike() {
    EndPassenger(true);
    for (auto part : g_bike.parts)
        if (IsLive(part)) Call(part, "K2_DestroyActor").invoke();
    if (IsLive(g_bike.actor)) Call(g_bike.actor, "K2_DestroyActor").invoke();
    g_bike = {};
}

void FindLocalBike(UObject* pawn) {
    UObject* bike = nullptr;
    Call riding(pawn, "GetVehicle");
    if (riding.invoke()) bike = riding.get<UObject*>("ReturnValue");
    if (!IsLive(bike)) {
        Call mode(C.statics, "GetGameMode");
        if (mode.set("WorldContextObject", pawn).invoke()) {
            auto gameMode = mode.get<UObject*>("ReturnValue");
            if (auto vehicleClass = Prop(gameMode, "CurrentPlayerVehicle")) {
                Call vehicle(gameMode, "GetVehicle");
                if (vehicle.set("VehicleClass", vehicleClass).invoke())
                    bike = vehicle.get<UObject*>("ReturnValue");
            }
        }
    }
    bike = IsLive(bike) ? bike : nullptr;
    if (g_localBike != bike) {
        Log("bike: local bike %s", FullName(bike).c_str());
        g_localBike = bike;
        DestroyBike();
    }
}

// Socket-mounted parts hang off whichever chassis body is visible so they follow its bones.
void AttachBikeParts() {
    UObject* chassis = g_bike.posing && IsLive(g_bike.pose) ? g_bike.pose : g_bike.component;
    for (const auto& part : g_bike.attachments) {
        if (part.mastered) continue;
        bool attached = Call(part.actor, "K2_AttachRootComponentTo").set("InParent", chassis).set("InSocketName", part.socket)
            .set("AttachLocationType", static_cast<uint8_t>(2)).invoke() &&
            Call(part.actor, "K2_SetActorRelativeLocation").set("NewRelativeLocation", part.location).invoke() &&
            Call(part.actor, "K2_SetActorRelativeRotation").set("NewRelativeRotation", part.rotation).invoke() &&
            Call(part.actor, "SetActorRelativeScale3D").set("NewRelativeScale", part.scale).invoke();
        if (!attached) Log("bike: could not mount %s at %s", Name(Prop(part.component, "SkeletalMesh")).c_str(),
                           NameString(part.socket.index).c_str());
    }
}

std::vector<UObject*> MasteredBikeParts() {
    std::vector<UObject*> parts;
    for (const auto& part : g_bike.attachments)
        if (part.mastered) parts.push_back(part.component);
    return parts;
}

bool SpawnBike(UObject* pawn, const FVector& at) {
    auto source = Prop(g_localBike, "VehicleMesh");
    if (!IsLive(source)) return false;
    g_bike.actor = SpawnMeshActor(pawn, at);
    g_bike.component = Prop(g_bike.actor, "SkeletalMeshComponent");
    if (!IsLive(g_bike.component) || !CopyMeshAppearance(source, g_bike.component)) {
        Log("bike: failed to clone chassis");
        DestroyBike();
        return false;
    }
    g_bike.source = Prop(source, "SkeletalMesh");
    AttachPoseable(g_bike.actor, g_bike.component, source, g_bike.pose, g_bike.writer, "bike");
    int mastered = 0, mounted = 0;
    for (auto component : OwnedMeshes(g_nativeBikeMeshes, g_localBike, Now())) {
        if (!IsLive(component) || component == source) continue;
        auto mesh = Prop(component, "SkeletalMesh");
        Call visible(component, "IsVisible");
        if (!IsLive(mesh) || !visible.invoke() || !visible.get<bool>("ReturnValue")) continue;
        auto actor = SpawnMeshActor(pawn, at);
        BikePart part;
        part.actor = actor;
        part.component = Prop(actor, "SkeletalMeshComponent");
        if (!IsLive(part.component) || !CopyMeshAppearance(component, part.component)) {
            if (IsLive(actor)) Call(actor, "K2_DestroyActor").invoke();
            Log("bike: could not clone part %s", Name(mesh).c_str());
            DestroyBike();
            return false;
        }
        g_bike.parts.push_back(actor);
        // Parts either share the chassis skeleton (master pose) or are rigid pieces mounted on a chassis socket.
        part.mastered = WeakProp(component, "MasterPoseComponent") == source;
        if (part.mastered) {
            if (!Call(actor, "K2_AttachRootComponentToActor").set("InParentActor", g_bike.actor)
                    .set("AttachLocationType", static_cast<uint8_t>(2)).invoke() ||
                !Call(part.component, "SetMasterPoseComponent").set("NewMasterBoneComponent", g_bike.component).invoke()) {
                Log("bike: could not slave part %s", Name(mesh).c_str());
                DestroyBike();
                return false;
            }
            ++mastered;
        } else {
            GetProp(component, "AttachSocketName", part.socket);
            GetProp(component, "RelativeLocation", part.location);
            GetProp(component, "RelativeRotation", part.rotation);
            GetProp(component, "RelativeScale3D", part.scale);
            if (Prop(component, "AttachParent") != source)
                Log("bike: part %s is mounted on %s; approximating on the chassis", Name(mesh).c_str(),
                    Name(Prop(component, "AttachParent")).c_str());
            ++mounted;
        }
        g_bike.attachments.push_back(part);
    }
    AttachBikeParts();
    auto animation = FindObject("/Game/Vehicles/BikeHero/bike_basic_anim.bike_basic_anim", C.animSequence);
    if (!animation || !Call(g_bike.component, "PlayAnimation").set("NewAnimToPlay", animation)
            .setBool("bLooping", true).invoke() || !Call(g_bike.component, "Stop").invoke()) {
        Log("bike: neutral chassis pose unavailable");
        DestroyBike();
        return false;
    }
    Log("bike: cloned chassis, %d skeleton-shared parts and %d socket-mounted parts; streamed pose %s", mastered, mounted,
        g_bike.pose ? "ready" : "unavailable");
    return true;
}

void UpdateBike(UObject* pawn, double now) {
    Snapshot sample{};
    bool have = g_settings.motorcycles && pawn && now - g_lastRecv < kPeerTimeout &&
                SampleRemote(now, sample) && (sample.flags & kHasBike) && now >= g_localSettledAt;
    if (!have || !IsLive(g_localBike)) {
        if (IsLive(g_bike.actor)) DestroyBike();
        return;
    }
    if (IsLive(g_bike.actor) && g_bike.source != Prop(Prop(g_localBike, "VehicleMesh"), "SkeletalMesh")) DestroyBike();
    FVector origin = WorldOrigin(pawn);
    FVector position{sample.bikePos.x - origin.x, sample.bikePos.y - origin.y, sample.bikePos.z - origin.z};
    if (!IsLive(g_bike.actor)) {
        if (now - g_lastBikeSpawn < 2) return;
        g_lastBikeSpawn = now;
        if (!SpawnBike(pawn, position)) return;
    }
    PoseFrame frame;
    bool streamed = g_bike.pose && now - g_lastBikePose < kPoseFresh &&
        g_bikePoses.Sample(g_motion.SenderMs(), frame) && g_bike.writer.Apply(frame);
    bool wasPosing = g_bike.posing;
    SetPosing(streamed, g_bike.posing, g_bike.pose, g_bike.component, MasteredBikeParts());
    if (wasPosing != g_bike.posing) {
        AttachBikeParts();
        Log("bike: %s", g_bike.posing ? "following the partner's streamed chassis" : "chassis stream stale; neutral pose");
    }
    Call(g_bike.actor, "K2_SetActorLocationAndRotation").set("NewLocation", position).set("NewRotation", sample.bikeRotation)
        .setBool("bSweep", false).setBool("bTeleport", true).invoke();
    if (!g_bike.posing) return;
    for (const auto& part : g_bike.attachments) {
        if (part.mastered) continue;
        BonePose offset;
        RotatorToQuat(part.rotation.pitch, part.rotation.yaw, part.rotation.roll, offset.q);
        offset.t = part.location;
        FollowSocket(part.actor, g_bike.pose, part.socket, offset);
    }
}

bool Alive(UObject* pawn) {
    Call alive(pawn, "IsAlive");
    return alive.invoke() && alive.get<bool>("ReturnValue");
}

FName PassengerSocket() {
    struct Array { uint8_t* data; int32_t count, capacity; } seats{};
    auto component = Prop(g_localBike, "SeatsComponent");
    FName socket{};
    if (component && GetProp(component, "SeatSetups", seats) && seats.data &&
        seats.count > 0 && seats.count <= 8 && seats.count <= seats.capacity) {
        for (int i = 0; i < seats.count; ++i) {
            uint8_t driver = 1;
            if (!ReadStructFields(seats.data + i * 0x20, 0x20, {{"BendSeatSetup", "bIsDriverSeat"}}, &driver, sizeof driver)) break;
            if (!(driver & 1) &&
                ReadStructFields(seats.data + i * 0x20, 0x20, {{"BendSeatSetup", "SeatSocketName"}}, &socket, sizeof socket)) break;
        }
    }
    return socket;
}

FName HipBone(UObject* mesh, std::string& names) {
    for (int i = 0; i < 24; ++i) {
        Call bone(mesh, "GetBoneName");
        if (!bone.set("BoneIndex", static_cast<int32_t>(i)).invoke()) break;
        auto name = bone.get<FName>("ReturnValue");
        std::string text = NameString(name.index);
        names += text + " ";
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        if (text.find("pelvis") != std::string::npos || text.find("hips") != std::string::npos || text == "hip") return name;
    }
    return {};
}

void UpdateDamageScope(double now) {
    bool eligible = g_settings.friendlyFire && g_pawn && Alive(g_pawn) && now >= g_localSettledAt &&
        now - g_lastRecv < kPeerTimeout && (g_peer.flags & kFriendlyFire) && (g_peer.flags & kAlive);
    g_damage.Configure(g_session, g_consent, g_peer, eligible);
    if (g_damage.Expire(now)) {
        g_settingsError = "A friendly-fire event timed out; stopped retrying that attack";
        Log("combat: outgoing damage event expired without acknowledgement");
    }
}

int TraceWorld(const FVector& start, const FVector& end, FVector* impact = nullptr, bool capsule = false) {
    auto library = FindDefaultObject("KismetSystemLibrary");
    UObject* ignored[] = {g_pawn};
    struct Array { UObject** data; int32_t count, capacity; } actors{ignored, 1, 1};
    Call trace(library, capsule ? "CapsuleTraceSingle_NEW" : "LineTraceSingle_NEW");
    if (capsule) trace.set("Radius", 42.f).set("HalfHeight", 90.f);
    if (!trace.set("WorldContextObject", g_pawn).set("Start", start).set("End", end)
            .set("TraceChannel", static_cast<uint8_t>(0)).setBool("bTraceComplex", !capsule)
            .set("ActorsToIgnore", actors).set("DrawDebugType", static_cast<uint8_t>(0))
            .setBool("bIgnoreSelf", true).invoke()) {
        g_settingsError = "World trace unavailable; unsafe actions are blocked";
        Log("gameplay: world trace invocation failed");
        return -1;
    }
    if (!trace.get<bool>("ReturnValue")) return 0;
    if (impact && !trace.member("OutHit", {{"HitResult", "ImpactPoint"}}, *impact)) return -1;
    return 1;
}

bool DebugError(const char* message) {
    g_settingsError = message;
    Log("debug: ERROR %s", message);
    return false;
}

bool DebugSetDamageable(bool damageable) {
    if (!IsLive(g_debug.pawn) || !g_debug.damageMask) return DebugError("Debug protection state is unavailable");
    auto property = FindChild(g_debug.pawn->cls, "bCanBeDamaged");
    int offset = property ? PropertyOffset(property) : -1;
    uint8_t current = 0, checked = 0;
    if (offset < 0 || !GetProp(g_debug.pawn, "bCanBeDamaged", current))
        return DebugError("Cannot read the native damage flag");
    // The mask was measured from God(), not guessed; preserve every adjacent bitfield.
    uint8_t updated = DebugMaskedFlag(current, g_debug.damageMask, damageable);
    std::memcpy(reinterpret_cast<uint8_t*>(g_debug.pawn) + offset, &updated, sizeof updated);
    if (!GetProp(g_debug.pawn, "bCanBeDamaged", checked) || checked != updated)
        return DebugError("Could not verify the native damage flag");
    Log("debug: native damageable=%d mask=0x%02x flags=0x%02x", damageable, g_debug.damageMask, checked);
    return true;
}

bool DebugGod(bool enabled) {
    if (!g_debug.damageMask) {
        uint8_t before = 0, after = 0;
        if (!IsLive(g_debug.pawn) || Prop(g_debug.controller, "Pawn") != g_debug.pawn ||
            !GetProp(g_debug.pawn, "bCanBeDamaged", before) ||
            !Call(g_debug.cheats, "God").invoke() || !GetProp(g_debug.pawn, "bCanBeDamaged", after))
            return DebugError("Native God could not be measured; restart before continuing tests");
        g_debug.damageMask = DebugDamageMask(before, after);
        if (!g_debug.damageMask) {
            if (before != after && !Call(g_debug.cheats, "God").invoke())
                Log("debug: ERROR unexpected God flag change could not be reverted");
            return DebugError("Native God did not change exactly one damage flag; protection is unverified");
        }
        g_debug.originalDamageable = (before & g_debug.damageMask) != 0;
    }
    return DebugSetDamageable(!enabled);
}

bool DebugCleanup() {
    for (auto it = g_debug.enemies.begin(); it != g_debug.enemies.end();) {
        auto pawn = it->pawn.get(), controller = it->controller.get();
        if (!it->destroyRequested) {
            if (IsLive(controller)) {
                auto possessed = Prop(controller, "Pawn");
                if (possessed && possessed != pawn)
                    return DebugError("A test controller changed ownership; cleanup stopped without touching its new pawn");
                if (!Call(controller, "K2_DestroyActor").invoke())
                    return DebugError("A test controller could not be destroyed; saving remains blocked");
            }
            if (IsLive(pawn) && !Call(pawn, "K2_DestroyActor").invoke())
                return DebugError("A test pawn could not be destroyed; saving remains blocked");
            it->destroyRequested = true;
        }
        if (!IsLive(pawn) && !IsLive(controller)) it = g_debug.enemies.erase(it);
        else ++it;
    }
    // Actor destruction can complete on a later engine frame; acknowledge only once observed.
    if (!g_debug.enemies.empty()) return false;
    Log("debug: owned test actors cleaned up");
    return true;
}

bool DebugEnd() {
    if (!g_debug.pawn) return true;
    if (!DebugCleanup()) return false;
    if (g_debug.damageMask && IsLive(g_debug.pawn) && !DebugSetDamageable(g_debug.originalDamageable)) return false;
    if (IsLive(g_debug.gameMode)) {
        Call blocked(g_debug.gameMode, "IsSavingBlockedByBlueprint");
        if (!Call(g_debug.gameMode, "SetSavingBlockedByBlueprint")
                .setBool("SavingBlocked", g_debug.originalSavingBlocked).invoke() ||
            !blocked.invoke() || blocked.get<bool>("ReturnValue") != g_debug.originalSavingBlocked)
            return DebugError("Could not restore saving; restart the game before continuing");
    }
    Log("debug: session ended; original damage and saving state restored");
    g_debug = {};
    return true;
}

bool DebugBegin(double now) {
    if (g_debug.pawn) return DebugError("A debug session is already active; end it first");
    Call riding(g_pawn, "IsInVehicle"), mode(C.statics, "GetGameMode");
    if (!IsLive(g_pawn) || !Alive(g_pawn) || g_passenger.pawn || now < g_localSettledAt ||
        !riding.invoke() || riding.get<bool>("ReturnValue") || !mode.set("WorldContextObject", g_pawn).invoke())
        return DebugError("Debug sessions require a living, settled gameplay pawn on foot, not a passenger");
    auto manager = FindWorldManager();
    if (!WorldSyncAllowed(manager))
        return DebugError("Debug sessions are blocked during missions, cinematics, or unavailable world state");
    auto gameMode = mode.get<UObject*>("ReturnValue");
    Call blocked(gameMode, "IsSavingBlockedByBlueprint");
    if (!blocked.invoke()) return DebugError("Cannot capture the original saving state");
    g_debug.pawn = g_pawn;
    g_debug.controller = g_controller;
    g_debug.cheats = Prop(g_controller, "CheatManager");
    g_debug.gameMode = gameMode;
    g_debug.pawnHold = RetainObject(g_pawn);
    g_debug.gameModeHold = RetainObject(gameMode);
    g_debug.originalSavingBlocked = blocked.get<bool>("ReturnValue");
    g_debug.expires = now + 300;
    if (!Call(gameMode, "SetSavingBlockedByBlueprint").setBool("SavingBlocked", true).invoke() ||
        !blocked.invoke() || !blocked.get<bool>("ReturnValue"))
        return DebugError("Saving could not be blocked; no test mutations were permitted, send end to restore");
    Log("debug: session started; saving blocked, previous=%d; expires after 300 seconds without commands",
        g_debug.originalSavingBlocked);
    return true;
}

void DebugStatus() {
    float health = 0;
    if (IsLive(g_pawn) && !GetProp(g_pawn, "Health", health)) {
        DebugError("The player's native health could not be inspected");
        return;
    }
    Log("debug: status active=%d playerHealth=%.3f owned=%llu", g_debug.pawn != nullptr, health,
        static_cast<unsigned long long>(g_debug.enemies.size()));
    auto origin = IsLive(g_pawn) ? WorldOrigin(g_pawn) : FVector{};
    for (const auto& enemy : g_debug.enemies) {
        auto pawn = enemy.pawn.get(), controller = enemy.controller.get();
        if (!IsLive(pawn)) { Log("debug: owned pawn is destroyed"); continue; }
        Call where(pawn, "K2_GetActorLocation");
        if (!where.invoke() || !GetProp(pawn, "Health", health)) {
            DebugError("An owned enemy's location or health could not be inspected");
            continue;
        }
        auto at = where.get<FVector>("ReturnValue");
        float dx = at.x + origin.x - enemy.start.x, dy = at.y + origin.y - enemy.start.y;
        auto brain = IsLive(controller) ? Prop(controller, "BrainComponent") : nullptr;
        int running = -1;
        if (IsLive(brain) && FindFunction(brain->cls, "IsRunning")) {
            Call check(brain, "IsRunning");
            if (check.invoke()) running = check.get<bool>("ReturnValue");
        }
        Log("debug: owned=%s health=%.3f at=(%.1f %.1f %.1f) moved=%.1f controller=%s brain=%s running=%d",
            FullName(pawn).c_str(), health, at.x, at.y, at.z, std::sqrt(dx * dx + dy * dy),
            IsLive(controller) ? FullName(controller).c_str() : "none", brain ? FullName(brain).c_str() : "none", running);
    }
}

bool DebugSpawn(size_t count, double now) {
    if (g_debug.enemies.size() + count > kDebugEnemyLimit)
        return DebugError("Test spawn limit is 16 owned enemies; clean up before spawning more");
    auto pawnClass = FindClass("SwarmerPawn_C"), library = FindDefaultObject("AIBlueprintHelperLibrary");
    Call where(g_pawn, "K2_GetActorLocation"), facing(g_pawn, "K2_GetActorRotation");
    if (!pawnClass || !library || !where.invoke() || !facing.invoke())
        return DebugError("Loaded swarmer class, spawn library, or player transform is unavailable");
    auto at = where.get<FVector>("ReturnValue");
    auto rotation = facing.get<FRotator>("ReturnValue");
    float yaw = rotation.yaw * 3.14159265f / 180;
    auto origin = WorldOrigin(g_pawn);
    for (size_t i = 0; i < count; ++i) {
        float forward = 600 + (i / 4) * 180.f, side = (static_cast<int>(i % 4) - 1.5f) * 180.f;
        FVector start{at.x + std::cos(yaw) * forward - std::sin(yaw) * side,
                      at.y + std::sin(yaw) * forward + std::cos(yaw) * side, at.z + 300};
        FVector ground{};
        if (TraceWorld(start, {start.x, start.y, at.z - 1500}, &ground) != 1)
            return DebugError("No verified ground at the test spawn point; any earlier spawns remain owned for cleanup");
        FVector position{ground.x, ground.y, ground.z + 100};
        if (TraceWorld(position, {position.x, position.y, position.z + 1}, nullptr, true) != 0)
            return DebugError("Test spawn clearance is blocked; any earlier spawns remain owned for cleanup");
        Call spawn(library, "SpawnAIFromClass");
        if (!spawn.set("WorldContextObject", g_pawn).set("PawnClass", pawnClass)
                .set("BehaviorTree", static_cast<UObject*>(nullptr)).set("Location", position)
                .set("Rotation", rotation).setBool("bNoCollisionFail", false).invoke())
            return DebugError("Native AI spawn failed; any earlier spawns remain owned for cleanup");
        auto pawn = spawn.get<UObject*>("ReturnValue");
        if (!IsLive(pawn)) return DebugError("Native AI spawn returned no live pawn; no ambient actors were adopted");
        auto controller = Prop(pawn, "Controller");
        // The population system normally assigns a look; bare spawns render nothing without one.
        auto body = Prop(pawn, "Mesh");
        if (IsLive(body) && !Prop(body, "SkeletalMesh")) {
            static UObject* meshClass = FindClass("SkeletalMesh");
            auto look = FindObject("/Game/Characters/Freaker/Swarmer/parts_combo/swarmer_horde_c.swarmer_horde_c", meshClass);
            bool dressed = look && Call(body, "SetSkeletalMesh").set("NewMesh", look).invoke();
            Log("debug: test pawn look %s", dressed ? "set to the pooled horde swarmer mesh" : "unavailable; it stays invisible");
        }
        g_debug.enemies.push_back({RetainObject(pawn), RetainObject(controller),
            {position.x + origin.x, position.y + origin.y, position.z + origin.z}});
        Log("debug: spawned owned pawn=%s controller=%s", FullName(pawn).c_str(),
            IsLive(controller) ? FullName(controller).c_str() : "none");
        if (!IsLive(controller)) return DebugError("Test pawn has no AI controller; send cleanup before retrying");
    }
    g_debug.reportAt = now + 3;
    DebugStatus();
    return true;
}

bool DebugProbe(bool player) {
    UObject* target = player ? g_pawn : nullptr;
    bool protectedPlayer = false;
    if (player) {
        uint8_t flags = 0;
        if (!g_debug.damageMask || !GetProp(g_pawn, "bCanBeDamaged", flags))
            return DebugError("Measure native debug protection with god-on or god-off before probing player damage");
        protectedPlayer = !(flags & g_debug.damageMask);
    } else {
        for (const auto& enemy : g_debug.enemies) {
            if (IsLive(enemy.pawn.get()) && Alive(enemy.pawn.get())) { target = enemy.pawn.get(); break; }
        }
    }
    float before = 0, after = 0;
    if (!IsLive(target) || !GetProp(target, "Health", before) || !std::isfinite(before) || before <= 25)
        return DebugError("Damage probe requires a live target with more than 25 health");
    Call damage(C.statics, "ApplyDamage");
    if (!damage.set("DamagedActor", target).set("BaseDamage", 5.f).set("EventInstigator", g_controller)
            .set("DamageCauser", g_pawn).set("DamageTypeClass", FindClass("DamageType")).invoke() ||
        !GetProp(target, "Health", after) || !std::isfinite(after))
        return DebugError("Native damage probe failed");
    Log("debug: damage probe target=%s before=%.3f after=%.3f returned=%.3f",
        protectedPlayer ? "protected player" : player ? "damageable player" : "owned enemy",
        before, after, damage.get<float>("ReturnValue"));
    if (protectedPlayer ? after != before : after >= before)
        return DebugError("Damage probe did not produce the required health outcome; inspect the runtime log");
    return true;
}

// Host-only world tests: the client must follow through the regular world-state stream.
bool DebugWorld(bool weather) {
    if (!IsHost()) return DebugError("World tests run on the host; the client follows the host");
    UObject* manager = FindWorldManager();
    WorldSnapshot current{};
    if (!WorldSyncAllowed(manager) || !ReadWorldSnapshot(manager, current))
        return DebugError("World state is unavailable during missions or cinematics");
    if (!weather) {
        float target = std::fmod(current.timeOfDay + 6.0f, 24.0f);
        if (!Call(manager, "SetWorldTimeOfDay").set("TimeOfDay", target).setBool("CanAdvanceDay", false).invoke())
            return DebugError("SetWorldTimeOfDay failed");
        Log("debug: host time %.3f -> %.3f", current.timeOfDay, target);
        return true;
    }
    bool transitioning = true;
    if (!GetProp(manager, "IsWeatherTransitioning", transitioning) || transitioning)
        return DebugError("Weather is transitioning; retry in a moment");
    uint8_t target = static_cast<uint8_t>((current.weather + 1) % 4);
    if (!Call(manager, "SelectWorldWeather").set("NewWeather", target)
            .set("NewWeatherTransitionRate", current.transitionRate).set("RelativeTime", current.relativeTime).invoke())
        return DebugError("SelectWorldWeather failed");
    WorldSnapshot after{};
    ReadWorldSnapshot(manager, after);
    Log("debug: host weather %u -> %u (now %u)", current.weather, target, after.weather);
    return true;
}

void DebugReply(uint32_t request, const std::string& command, bool success) {
    success = success && g_settingsError.empty();
    Log("debug: request=%u command=%s result=%s", request, command.c_str(), success ? "ok" : "error");
    auto result = g_basePath + L"-debug-result.ini";
    auto id = std::to_wstring(request);
    auto message = success ? std::wstring(L"See TheysGone.log for measured details") :
        std::wstring(g_settingsError.begin(), g_settingsError.end());
    bool written = WritePrivateProfileStringW(L"Result", L"State", success ? L"ok" : L"error", result.c_str()) &&
        WritePrivateProfileStringW(L"Result", L"Message", message.c_str(), result.c_str()) &&
        WritePrivateProfileStringW(L"Result", L"Request", id.c_str(), result.c_str());
    if (!written) DebugError("Could not write debug command acknowledgement; inspect the runtime log");
}

bool g_rideTest = false;
void ProbeStory();
bool DivergeStory();

UObject* FindLive(const char* className) {
    static std::unordered_map<std::string, std::pair<UObject*, UObject*>> cache; // class name -> (class, instance)
    auto& [cls, instance] = cache[className];
    if (!cls) cls = FindClass(className);
    if (!cls) return nullptr;
    if (IsLive(instance) && IsA(instance, cls)) return instance;
    instance = nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto object = ObjectAt(i);
        if (IsLive(object) && !IsDefaultOrArchetype(object) && IsA(object, cls)) return instance = object;
    }
    return nullptr;
}

// Reloads the newest save in place, the way the game does after a mission failure. For a guest that
// has adopted the host's save, that newest save is the host's.
bool ReloadNewestSave() {
    Call reload(FindLive("BendGameMission"), "ReloadBecauseOfJobFailure");
    if (!reload.ok()) return false;
    reload.set<uint8_t>("Subject", 0);
    reload.set<uint8_t>("Reason", 0);
    reload.setBool("DisplayFailReason", false);
    bool invoked = reload.invoke();
    Log("save reload: ReloadBecauseOfJobFailure invoked=%d returned=%d", invoked, invoked && reload.get<bool>("ReturnValue"));
    return invoked;
}
std::string PartnerName();
void TogglePassenger(double now);

void UpdateDebugTools(double now) {
    if (!g_cfg.debugTools) return;
    static double pollAt = -10;
    if (now < pollAt) return;
    pollAt = now + 0.5;
    if (g_debugPending.request) {
        g_settingsError.clear();
        bool complete = g_debugPending.ending ? DebugEnd() : DebugCleanup();
        if (!complete && g_settingsError.empty() && now < g_debugPending.deadline) return;
        if (!complete && g_settingsError.empty())
            DebugError("Test actor cleanup timed out; saving remains blocked, restart before continuing");
        DebugReply(g_debugPending.request, g_debugPending.command, complete);
        g_debugPending = {};
        return;
    }
    if (g_debug.pawn) {
        Call riding(g_debug.pawn, "IsInVehicle");
        if (g_debug.pawn != g_pawn || !IsLive(g_debug.pawn) || !Alive(g_debug.pawn) ||
            !WorldSyncAllowed(FindWorldManager()) || !riding.invoke() || riding.get<bool>("ReturnValue") ||
            now >= g_debug.expires) {
            if (!DebugEnd()) return;
        }
    }
    if (g_debug.reportAt && now >= g_debug.reportAt) {
        g_debug.reportAt = 0;
        DebugStatus();
    }
    auto path = g_basePath + L"-debug.ini";
    uint32_t request = GetPrivateProfileIntW(L"Debug", L"Request", 0, path.c_str());
    if (!g_debugRequests.Accept(request)) return;
    wchar_t raw[64]{};
    GetPrivateProfileStringW(L"Debug", L"Command", L"", raw, 64, path.c_str());
    auto text = Narrow(std::wstring(raw));
    auto command = ParseDebugCommand(text);
    g_settingsError.clear();
    bool success = true;
    if (command == DebugCommand::unknown) success = DebugError("Unknown debug command; only documented commands are allowed");
    else if (command == DebugCommand::inspect) {
        if (IsLive(g_pawn)) success = DiscoverDebug(g_pawn);
        else success = DebugError("Debug inspection requires a gameplay pawn");
    } else if (command == DebugCommand::status) DebugStatus();
    else if (command == DebugCommand::storyProbe) ProbeStory();
    else if (command == DebugCommand::storyDiverge) {
        if (!DivergeStory()) success = DebugError("story-diverge needs a guest with the save sandbox active");
    }
    else if (command == DebugCommand::hitEnemy) {
        if (!DebugHitEnemy()) success = DebugError("hit-enemy needs a guest with a live enemy replica");
    }
    else if (command == DebugCommand::reloadSave) {
        if (!ReloadNewestSave()) success = DebugError("No mission manager to reload with");
    } else if (command == DebugCommand::rideTest) {
        g_rideTest = !g_rideTest;
        Log("ride prompt: test prompt %s", g_rideTest ? "on" : "off");
    } else if (command == DebugCommand::dumpReflection || command == DebugCommand::dumpObjects ||
             command == DebugCommand::traceOn || command == DebugCommand::traceOff) {
        std::string summary = "trace running";
        if (command == DebugCommand::dumpReflection)
            success = research::DumpReflection(g_basePath + L"-reflection.txt", summary);
        else if (command == DebugCommand::dumpObjects)
            success = research::DumpObjects(g_basePath + L"-objects.txt", summary);
        else if (command == DebugCommand::traceOn) research::TraceStart();
        else success = research::TraceStop(g_basePath + L"-trace.txt", summary);
        Log("research: %s: %s", text.c_str(), summary.c_str());
        if (!success) DebugError(summary.c_str());
    } else if (command == DebugCommand::begin) success = DebugBegin(now);
    else if (command == DebugCommand::end || command == DebugCommand::cleanup) {
        if (command == DebugCommand::cleanup && !g_debug.pawn)
            success = DebugError("Send begin before using test tools");
        else {
            g_debugPending = {request, text, now + 10, command == DebugCommand::end};
            return;
        }
    }
    else if (!g_debug.pawn || !IsLive(g_pawn)) success = DebugError("Send begin before using test tools");
    else {
        Call blocked(g_debug.gameMode, "IsSavingBlockedByBlueprint");
        if (!blocked.invoke() || !blocked.get<bool>("ReturnValue"))
            success = DebugError("Saving is no longer blocked; test mutations stopped, send end");
        else switch (command) {
            case DebugCommand::godOn: success = DebugGod(true); break;
            case DebugCommand::godOff: success = DebugGod(false); break;
            case DebugCommand::spawnSingle: success = DebugSpawn(1, now); break;
            case DebugCommand::spawnPack: success = DebugSpawn(kDebugPackSize, now); break;
            case DebugCommand::probePlayer: success = DebugProbe(true); break;
            case DebugCommand::probeEnemy: success = DebugProbe(false); break;
            case DebugCommand::timeShift: success = DebugWorld(false); break;
            case DebugCommand::weatherNext: success = DebugWorld(true); break;
            default: success = DebugError("Debug command is unavailable in the current session"); break;
        }
    }
    if (g_debug.pawn) g_debug.expires = now + 300;
    DebugReply(request, text, success);
}

bool PassengerExitPosition(FVector& destination) {
    if (!IsLive(g_passenger.pawn)) return false;
    Call where(g_passenger.pawn, "K2_GetActorLocation");
    if (!where.invoke()) return false;
    FVector at = where.get<FVector>("ReturnValue");
    for (auto offset : {FVector{200, 0, 0}, FVector{-200, 0, 0}, FVector{0, 200, 0}, FVector{0, -200, 0}}) {
        FVector start{at.x + offset.x, at.y + offset.y, at.z + 300};
        FVector end{start.x, start.y, at.z - 1500};
        FVector ground{};
        if (TraceWorld(start, end, &ground) != 1) continue;
        destination = {ground.x, ground.y, ground.z + 100};
        if (TraceWorld(destination, {destination.x, destination.y, destination.z + 1}, nullptr, true) == 0) return true;
    }
    return false;
}

bool EndPassenger(bool forced) {
    if (!g_passenger.pawn) return true;
    FVector destination{};
    bool samePawn = IsLive(g_passenger.pawn) && g_passenger.pawn == g_pawn;
    Call riding(g_passenger.pawn, "IsInVehicle");
    bool nativeVehicle = samePawn && riding.invoke() && riding.get<bool>("ReturnValue");
    bool safe = !g_passenger.positioned || nativeVehicle || (samePawn && PassengerExitPosition(destination));
    if (!safe && !forced) {
        g_settingsError = "No safe place to dismount; stop the motorcycle on level ground";
        Log("passenger: dismount blocked because no safe ground was found");
        return false;
    }
    auto state = std::move(g_passenger);
    g_passenger = {};
    DestroyMeshAvatar(g_passengerAvatar);
    g_passengerAppearance.Clear();
    bool restored = true;
    for (auto [component, visible] : state.visibility)
        if (IsLive(component) && !Call(component, "SetVisibility").setBool("bNewVisibility", visible)
                .setBool("bPropagateToChildren", false).invoke()) {
            Log("passenger: could not restore native mesh visibility");
            restored = false;
        }
    if (samePawn && state.positioned && !nativeVehicle) {
        if (!safe) {
            FVector origin = WorldOrigin(state.pawn);
            destination = {state.boardingPosition.x - origin.x, state.boardingPosition.y - origin.y,
                           state.boardingPosition.z - origin.z};
            Log("passenger: returning to boarding position because no safe exit was found");
        }
        restored = Call(state.pawn, "K2_SetActorLocation").set("NewLocation", destination)
            .setBool("bSweep", false).setBool("bTeleport", true).invoke() && restored;
    }
    if (state.collisionChanged && !nativeVehicle && IsLive(state.pawn) && Alive(state.pawn))
        restored = Call(state.pawn, "SetActorEnableCollision").setBool("bNewActorEnableCollision", state.collision).invoke() && restored;
    if (state.movementChanged && !nativeVehicle && IsLive(state.movement) && IsLive(state.pawn) && Alive(state.pawn))
        restored = Call(state.movement, "SetMovementMode").set("NewMovementMode", state.movementMode)
            .set("NewCustomMode", state.customMode).invoke() && restored;
    if (state.moveBlocked && IsLive(state.controller))
        restored = Call(state.controller, "SetIgnoreMoveInput").setBool("bNewMoveInput", false).invoke() && restored;
    if (state.saveChanged && IsLive(state.gameMode))
        restored = Call(state.gameMode, "SetSavingBlockedByBlueprint").setBool("SavingBlocked", state.savingBlocked).invoke() && restored;
    if (!restored) {
        g_settingsError = "Passenger state restoration failed; restart the game before continuing";
        Log("passenger: dismounted, but native state restoration was incomplete");
    } else Log("passenger: dismounted; native body, movement, collision, and saving restored");
    return restored;
}


// Offers the native "hold to interact" prompt on the partner's motorcycle.
void UpdateRidePrompt(double now) {
    bool visible = false;
    FVector at{};
    std::string label;
    bool riding = false;
    if (IsLive(g_pawn)) {
        Call inVehicle(g_pawn, "IsInVehicle");
        riding = inVehicle.invoke() && inVehicle.get<bool>("ReturnValue");
    }
    if (g_rideTest && IsLive(g_pawn) && !riding) {
        Call where(g_pawn, "K2_GetActorLocation"), forward(g_pawn, "GetActorForwardVector");
        if (where.invoke() && forward.invoke()) {
            auto p = where.get<FVector>("ReturnValue");
            auto f = forward.get<FVector>("ReturnValue");
            static FVector anchor{};
            static bool anchored = false;
            if (!anchored) anchor = {p.x + f.x * 300, p.y + f.y * 300, p.z}, anchored = true;
            at = anchor;
            visible = true;
            label = "Ride with " + PartnerName();
        }
    } else if (g_passenger.pawn) {
        Call where(g_bike.component, "K2_GetComponentLocation");
        if (IsLive(g_bike.component) && where.invoke()) {
            at = where.get<FVector>("ReturnValue");
            visible = true;
            label = "Get off";
        }
    } else if (IsLive(g_pawn) && !riding && !g_debug.pawn && g_settings.motorcycles && IsLive(g_bike.component) &&
               now - g_lastRecv < kPeerTimeout && (g_peer.flags & kAlive) && (g_peer.flags & kAcceptPassengers) &&
               (g_peer.flags & kInVehicle) && !(g_peer.flags & kPassenger) && now >= g_localSettledAt &&
               g_motion.BikeSpeed() <= 200) {
        Call where(g_bike.component, "K2_GetComponentLocation");
        if (where.invoke()) {
            at = where.get<FVector>("ReturnValue");
            visible = true;
            label = "Ride with " + PartnerName();
        }
    }
    if (ride::Update(g_pawn, visible, at, label)) {
        if (g_rideTest) Log("ride prompt: test interaction completed");
        else TogglePassenger(now);
    }
}

void TogglePassenger(double now) {
    if (g_passenger.pawn) { EndPassenger(false); return; }
    if (g_debug.pawn) {
        g_settingsError = "End the debug test session before boarding a passenger seat";
        Log("passenger: boarding blocked by the active debug session");
        return;
    }
    if (!g_pawn || !Alive(g_pawn) || !g_settings.motorcycles || !IsLive(g_bike.component) ||
        now - g_lastRecv >= kPeerTimeout || !(g_peer.flags & kAlive) || !(g_peer.flags & kAcceptPassengers) ||
        !(g_peer.flags & kInVehicle) || (g_peer.flags & kPassenger) || now < g_localSettledAt) {
        g_settingsError = "Partner must be riding nearby, with motorcycles and passengers enabled";
        Log("passenger: boarding prerequisites not satisfied");
        return;
    }
    Call riding(g_pawn, "IsInVehicle"), where(g_pawn, "K2_GetActorLocation");
    if (!riding.invoke() || riding.get<bool>("ReturnValue") || !where.invoke()) {
        g_settingsError = "Dismount your own motorcycle before boarding your partner's";
        return;
    }
    auto position = where.get<FVector>("ReturnValue");
    FVector origin = WorldOrigin(g_pawn);
    float dx = position.x + origin.x - g_peer.bikePos[0], dy = position.y + origin.y - g_peer.bikePos[1];
    float dz = position.z + origin.z - g_peer.bikePos[2];
    float speed = g_motion.BikeSpeed();
    if (dx * dx + dy * dy + dz * dz > 450 * 450 || speed > 200) {
        g_settingsError = "Get within 4.5 m and ask your partner to stop before boarding";
        return;
    }
    FName socket = PassengerSocket();
    auto animation = FindObject("/Game/Animation/Deacon/bike/bikeBuddy/bike_basic_control_buddy.bike_basic_control_buddy", C.animSequence);
    Passenger state;
    state.pawn = g_pawn;
    state.controller = g_controller;
    state.mesh = Prop(g_pawn, "Mesh");
    std::string boneNames;
    state.pelvis = HipBone(state.mesh, boneNames);
    state.movement = Prop(g_pawn, "CharacterMovement");
    state.socket = socket;
    std::vector<UObject*> seatMeshes{g_bike.component};
    for (auto part : g_bike.parts) seatMeshes.push_back(Prop(part, "SkeletalMeshComponent"));
    for (auto mesh : seatMeshes) {
        Call hasSocket(mesh, "DoesSocketExist");
        if (hasSocket.set("InSocketName", socket).invoke() && hasSocket.get<bool>("ReturnValue")) {
            state.seatMesh = mesh;
            break;
        }
    }
    state.boardingPosition = {position.x + origin.x, position.y + origin.y, position.z + origin.z};
    Call mode(C.statics, "GetGameMode");
    if (mode.set("WorldContextObject", g_pawn).invoke()) state.gameMode = mode.get<UObject*>("ReturnValue");
    Call blocked(state.gameMode, "IsSavingBlockedByBlueprint"), collision(g_pawn, "GetActorEnableCollision");
    bool movementRead = IsLive(state.movement) && GetProp(state.movement, "MovementMode", state.movementMode) &&
        GetProp(state.movement, "CustomMovementMode", state.customMode);
    bool blockedRead = blocked.invoke(), collisionRead = collision.invoke();
    if (!socket.index || !state.pelvis.index || !animation || !state.seatMesh || !movementRead || state.movementMode != 1 ||
        !blockedRead || !collisionRead) {
        g_settingsError = "Passenger seat or animation unavailable; boarding was not attempted";
        Log("passenger: prerequisites socket=%s mesh=%p animation=%p movement=%d/%u saving=%d collision=%d",
            NameString(socket.index).c_str(), static_cast<void*>(state.seatMesh), static_cast<void*>(animation),
            movementRead, state.movementMode, blockedRead, collisionRead);
        if (!state.pelvis.index) Log("passenger: pelvis bone unavailable; first bones: %s", boneNames.c_str());
        return;
    }
    state.savingBlocked = blocked.get<bool>("ReturnValue");
    state.collision = collision.get<bool>("ReturnValue");
    g_passenger = state;
    bool changed = SpawnAvatar(g_pawn, position, g_passengerAvatar) &&
        Call(g_passengerAvatar.component, "PlayAnimation").set("NewAnimToPlay", animation).setBool("bLooping", true).invoke() &&
        Call(g_passengerAvatar.component, "Stop").invoke() &&
        (g_passenger.saveChanged = Call(state.gameMode, "SetSavingBlockedByBlueprint").setBool("SavingBlocked", true).invoke()) &&
        (g_passenger.moveBlocked = Call(state.controller, "SetIgnoreMoveInput").setBool("bNewMoveInput", true).invoke()) &&
        (g_passenger.movementChanged = Call(state.movement, "SetMovementMode").set("NewMovementMode", static_cast<uint8_t>(0))
            .set("NewCustomMode", static_cast<uint8_t>(0)).invoke()) &&
        (g_passenger.collisionChanged = Call(state.pawn, "SetActorEnableCollision").setBool("bNewActorEnableCollision", false).invoke());
    if (!changed) {
        g_settingsError = "Boarding failed; your original state has been restored";
        EndPassenger(true);
        return;
    }
    g_settingsError.clear();
    Log("passenger: boarded using socket %s and pelvis bone %s", NameString(socket.index).c_str(), NameString(state.pelvis.index).c_str());
}

void UpdatePassenger(double now) {
    if (!g_passenger.pawn) return;
    if (g_passenger.pawn != g_pawn || !IsLive(g_bike.component) || !IsLive(g_passengerAvatar.component) ||
        !Alive(g_pawn) || !g_settings.motorcycles ||
        now - g_lastRecv >= kPeerTimeout || !(g_peer.flags & kAcceptPassengers) || !(g_peer.flags & kAlive) ||
        !(g_peer.flags & kInVehicle) || (g_peer.flags & kPassenger)) {
        EndPassenger(true);
        return;
    }
    Call nativeVehicle(g_pawn, "IsInVehicle");
    bool vehicleKnown = nativeVehicle.invoke();
    if (!vehicleKnown || nativeVehicle.get<bool>("ReturnValue")) {
        g_settingsError = vehicleKnown ? "Native vehicle control took over; passenger attachment ended" :
            "Native vehicle state unavailable; passenger attachment ended";
        EndPassenger(true);
        return;
    }
    Call seat(g_passenger.seatMesh, "GetSocketLocation");
    if (!seat.set("InSocketName", g_passenger.socket).invoke()) { EndPassenger(true); return; }
    auto location = seat.get<FVector>("ReturnValue");
    Snapshot sample{};
    Call hip(g_passengerAvatar.component, "GetSocketTransform");
    if (!SampleRemote(now, sample) || !hip.set("InSocketName", g_passenger.pelvis)
            .set("TransformSpace", static_cast<uint8_t>(2)).invoke()) {
        g_settingsError = "Passenger pose unavailable; dismounting safely";
        EndPassenger(true);
        return;
    }
    // The avatar mesh is its actor root: component space already is actor space.
    auto offset = hip.get<FTransform>("ReturnValue").translation;
    FVector capsule = location;
    location = AlignBoneToSocket(location, offset, sample.yaw);
    static double lastSeatLog = 0;
    if (now - lastSeatLog >= 10) {
        lastSeatLog = now;
        Log("passenger: seat z=%.1f relative hips z=%.1f visual root z=%.1f",
            seat.get<FVector>("ReturnValue").z, offset.z, location.z);
    }
    bool movedPawn = Call(g_pawn, "K2_SetActorLocationAndRotation").set("NewLocation", capsule)
            .set("NewRotation", FRotator{0, sample.yaw + 90, 0})
            .setBool("bSweep", false).setBool("bTeleport", true).invoke();
    if (movedPawn) g_passenger.positioned = true;
    if (!movedPawn || !Call(g_passengerAvatar.actor, "K2_SetActorLocationAndRotation").set("NewLocation", location)
            .set("NewRotation", FRotator{0, sample.yaw, 0})
            .setBool("bSweep", false).setBool("bTeleport", true).invoke()) {
        g_settingsError = "Passenger positioning failed; dismounting safely";
        EndPassenger(true);
        return;
    }
    static double lastAppearance = -10;
    if (now - lastAppearance >= 0.25) {
        lastAppearance = now;
        g_passengerAppearance.Apply(g_passengerAvatar.component, g_settings);
        for (auto part : g_passengerAvatar.parts)
            g_passengerAppearance.Apply(Prop(part, "SkeletalMeshComponent"), g_settings);
        for (auto component : OwnedMeshes(g_characterMeshes, g_pawn, now)) {
            if (!IsLive(component)) continue;
            auto found = std::find_if(g_passenger.visibility.begin(), g_passenger.visibility.end(),
                [component](const auto& entry) { return entry.first == component; });
            if (found == g_passenger.visibility.end()) {
                Call visible(component, "IsVisible");
                if (!visible.invoke()) {
                    g_settingsError = "Native body visibility unavailable; dismounting safely";
                    EndPassenger(true);
                    return;
                }
                g_passenger.visibility.emplace_back(component, visible.get<bool>("ReturnValue"));
            }
            if (!Call(component, "SetVisibility").setBool("bNewVisibility", false)
                    .setBool("bPropagateToChildren", false).invoke()) {
                g_settingsError = "Native body could not be hidden; dismounting safely";
                EndPassenger(true);
                return;
            }
        }
    }
}

void ReceiveDamage(const StatePacket& packet) {
    if (!g_damage.Accept(packet)) return;
    Call location(Prop(g_pawn, "Mesh"), "K2_GetComponentLocation");
    if (!location.invoke() || !Alive(g_pawn) || !(packet.flags & kAlive)) return;
    FVector origin = WorldOrigin(g_pawn);
    FVector position = location.get<FVector>("ReturnValue");
    float dx = position.x + origin.x - packet.pos[0], dy = position.y + origin.y - packet.pos[1];
    float dz = position.z + origin.z - packet.pos[2];
    float range = packet.attackKind == 1 ? 350.f : 50000.f;
    if (dx * dx + dy * dy + dz * dz > range * range) {
        Log("combat: rejected out-of-range attack %u", packet.attackId);
        return;
    }
    float before = 0, after = 0;
    GetProp(g_pawn, "Health", before);
    Call damage(C.statics, "ApplyDamage");
    if (!damage.set("DamagedActor", g_pawn).set("BaseDamage", packet.damage)
            .set("EventInstigator", static_cast<UObject*>(nullptr)).set("DamageCauser", g_avatar.actor)
            .set("DamageTypeClass", FindClass("DamageType")).set("DamagedBodyIndex", static_cast<int32_t>(-1)).invoke()) {
        g_settings.friendlyFire = false;
        ++g_consent;
        g_settingsError = "Native damage failed; friendly fire has been switched off";
        Log("combat: ApplyDamage failed; friendly fire disabled");
        return;
    }
    GetProp(g_pawn, "Health", after);
    Log("combat: received attack=%u kind=%u requested=%.1f applied=%.1f health %.1f -> %.1f",
        packet.attackId, packet.attackKind, packet.damage, damage.get<float>("ReturnValue"), before, after);
}

// Midpoint of the weapon's native hit damage range.
bool WeaponDamage(bool melee, UObject* weapon, float& amount) {
    int32_t minimum = 0, maximum = 0;
    bool read = melee ?
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "MeleeData"}, {"WeaponMeleeData", "MeleeHitDamage"}}, minimum) &&
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "MeleeData"}, {"WeaponMeleeData", "MeleeHitDamageMax"}}, maximum) :
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "CoreData"}, {"WeaponCoreData", "MinHitDamage"}}, minimum) &&
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "CoreData"}, {"WeaponCoreData", "MaxHitDamage"}}, maximum);
    if (!read || minimum <= 0 || maximum < minimum || maximum > 1000) {
        g_settingsError = "Weapon damage data unavailable; attack was not sent";
        Log("combat: invalid native damage range %d..%d for %s", minimum, maximum, FullName(weapon).c_str());
        return false;
    }
    amount = (minimum + maximum) * 0.5f;
    return true;
}

void QueueAttack(bool melee, UObject* weapon) {
    if (!g_pawn || g_passenger.pawn || !g_settings.friendlyFire || g_menu.IsOpen() ||
        Now() < g_localSettledAt || Now() - g_lastRecv >= kPeerTimeout ||
        !(g_peer.flags & kFriendlyFire) || !(g_peer.flags & kAlive) || !Alive(g_pawn)) return;
    if (!IsLive(weapon)) return;
    if (!IsLive(g_avatar.actor) || g_avatar.hidden) return;
    Call rendered(g_avatar.actor, "K2_GetActorLocation");
    if (!rendered.invoke()) return;
    FVector target = rendered.get<FVector>("ReturnValue");
    target.z += 90;
    FVector start{};
    float distance = 0;
    if (melee) {
        Call at(Prop(g_pawn, "Mesh"), "K2_GetComponentLocation"), facing(Prop(g_pawn, "Mesh"), "K2_GetComponentRotation");
        if (!at.invoke() || !facing.invoke()) return;
        start = at.get<FVector>("ReturnValue");
        start.z += 90;
        float dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
        distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float yaw = (facing.get<FRotator>("ReturnValue").yaw + 90) * 3.14159265f / 180.f;
        if (distance > 250 || std::fabs(dz) > 140 ||
            dx * std::cos(yaw) + dy * std::sin(yaw) < distance * 0.35f) {
            if (g_cfg.discovery) Log("combat: melee missed partner range=%.0f facing=%.0f", distance,
                dx * std::cos(yaw) + dy * std::sin(yaw));
            return;
        }
    } else {
        auto camera = Prop(g_controller, "PlayerCameraManager");
        Call where(camera, "GetCameraLocation"), rotation(camera, "GetCameraRotation");
        if (!where.invoke() || !rotation.invoke()) return;
        start = where.get<FVector>("ReturnValue");
        auto r = rotation.get<FRotator>("ReturnValue");
        float yaw = r.yaw * 3.14159265f / 180.f, pitch = r.pitch * 3.14159265f / 180.f;
        FVector direction{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
        float dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
        distance = dx * direction.x + dy * direction.y + dz * direction.z;
        if (distance < 0 || distance > 50000) return;
        FVector aim{start.x + distance * direction.x, start.y + distance * direction.y, start.z + distance * direction.z};
        float missX = aim.x - target.x, missY = aim.y - target.y;
        if (missX * missX + missY * missY > 45 * 45 || std::fabs(aim.z - target.z) > 80) {
            if (g_cfg.discovery) Log("combat: shot missed partner horizontal=%.0f vertical=%.0f",
                std::sqrt(missX * missX + missY * missY), aim.z - target.z);
            return;
        }
        target = aim;
    }
    if (TraceWorld(start, target) != 0) {
        if (g_cfg.discovery) Log("combat: world geometry blocked the attack");
        return;
    }
    float amount = 0;
    if (!WeaponDamage(melee, weapon, amount)) return;
    UpdateDamageScope(Now());
    if (!g_damage.Queue(melee ? 1 : 2, amount, Now())) {
        g_settingsError = "Friendly-fire queue unavailable; attack was not sent";
        Log("combat: could not queue damage event");
    } else Log("combat: queued %s damage %.1f from %s", melee ? "melee" : "gunshot", amount, Name(weapon).c_str());
}

void ObserveAttack(UObject* object, UObject* function, void* parameters) {
    static int shotName = FindNameIndex("OnWeaponFired_Event");
    if (object != g_pawn || function->name.index != shotName || !parameters) return;
    if (g_cfg.discovery) Log("combat: real shot observed localConsent=%d peerFlags=0x%x", g_settings.friendlyFire, g_peer.flags);
    UObject* weapon = nullptr;
    if (!ReadEventParam(function, parameters, "Weapon", weapon)) {
        Log("combat: shot event did not expose a weapon");
        return;
    }
    ++g_localShotSequence;
    QueueAttack(false, weapon);
    QueueEnemyHit(false, weapon);
}

void PollMelee() {
    static UObject* lastPawn = nullptr;
    static bool previousWindow = false, previousAttack = false;
    if (lastPawn != g_pawn) {
        lastPawn = g_pawn;
        previousWindow = previousAttack = false;
    }
    if (!g_pawn || (!g_settings.friendlyFire && !HasEnemyReplicas())) { previousWindow = false; return; }
    uint8_t attacking = 0;
    if (!GetProp(g_pawn, "m_bIsInMeleeAttack", attacking) || attacking > 1) {
        g_settingsError = "Melee state unavailable; melee friendly fire is blocked";
        Log("combat: invalid native melee attack state");
        return;
    }
    UObject* weapon = nullptr;
    Call held(g_pawn, "GetEquippedItemCurrentlyInHand");
    if (held.invoke()) weapon = held.get<UObject*>("ReturnValue");
    Call isMelee(weapon, "IsMelee");
    if (!isMelee.invoke() || !isMelee.get<bool>("ReturnValue")) {
        Call knife(g_pawn, "GetBootKnifeWeapon");
        if (knife.invoke()) weapon = knife.get<UObject*>("ReturnValue");
    }
    Call collision(weapon, "IsWeaponCollisionEnabled");
    bool window = attacking && collision.invoke() && collision.get<bool>("ReturnValue");
    if (g_cfg.discovery && (previousAttack != (attacking != 0) || previousWindow != window))
        Log("combat: native melee attack=%u collisionWindow=%d weapon=%s", attacking, window, Name(weapon).c_str());
    if (window && !previousWindow) {
        if (g_settings.friendlyFire) QueueAttack(true, weapon);
        QueueEnemyHit(true, weapon);
    }
    previousWindow = window;
    previousAttack = attacking != 0;
}

bool SpawnAvatar(UObject* localPawn, const FVector& at, Avatar& avatar) {
    auto localMesh = Prop(localPawn, "Mesh");
    auto skeletal = Prop(localMesh, "SkeletalMesh");
    if (!skeletal) return false;
    avatar.actor = SpawnMeshActor(localPawn, at);
    avatar.component = Prop(avatar.actor, "SkeletalMeshComponent");
    if (!avatar.component) {
        Log("avatar: spawn failed");
        DestroyMeshAvatar(avatar);
        return false;
    }
    if (!CopyMeshAppearance(localMesh, avatar.component)) {
        DestroyMeshAvatar(avatar);
        return false;
    }
    // Days Gone characters are modular: clone extra skinned parts and slave them to the body pose.
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || o == localMesh || o->outer != localPawn || !IsA(o, C.skelMeshComponent) || !IsLive(o)) continue;
        auto partMesh = Prop(o, "SkeletalMesh");
        if (!partMesh || !IsCharacterPart(o, localMesh)) continue;
        auto part = SpawnMeshActor(localPawn, at);
        auto comp = Prop(part, "SkeletalMeshComponent");
        if (!comp) {
            Log("avatar: modular part spawn failed");
            if (IsLive(part)) Call(part, "K2_DestroyActor").invoke();
            continue;
        }
        if (!CopyMeshAppearance(o, comp)) {
            Call(part, "K2_DestroyActor").invoke();
            continue;
        }
        if (!Call(part, "K2_AttachRootComponentToActor")
            .set("InParentActor", avatar.actor)
            .set("AttachLocationType", static_cast<uint8_t>(2))
            .invoke() ||
            !Call(comp, "SetMasterPoseComponent").set("NewMasterBoneComponent", avatar.component).invoke()) {
            Log("avatar: modular attachment failed");
            Call(part, "K2_DestroyActor").invoke();
            DestroyMeshAvatar(avatar);
            return false;
        }
        avatar.parts.push_back(part);
        Log("avatar: part %s", Name(partMesh).c_str());
    }
    AttachPoseable(avatar.actor, avatar.component, localMesh, avatar.pose, avatar.writer, "avatar");
    Log("avatar: spawned %s with mesh %s, %d extra parts, streamed pose %s", Name(avatar.actor).c_str(),
        Name(skeletal).c_str(), static_cast<int>(avatar.parts.size()), avatar.pose ? "ready" : "unavailable");
    return true;
}

// Adds a hidden poseable copy of `source` under `actor`; on failure the clip-driven `fallback` stays in use.
void AttachPoseable(UObject* actor, UObject* fallback, UObject* source, UObject*& pose, PoseWriter& writer, const char* label) {
    pose = CreatePoseableComponent(actor);
    if (pose && CopyMeshAppearance(source, pose) && writer.Attach(pose) &&
        Call(pose, "SetVisibility").setBool("bNewVisibility", false).setBool("bPropagateToChildren", false).invoke()) {
        Log("%s: poseable body %s with %d bones (array +0x%x)", label, Name(pose).c_str(),
            static_cast<int>(writer.Table().names.size()),
            writer.Offset());
        return;
    }
    Log("%s: streamed pose unavailable; keeping animation clips", label);
    if (IsLive(pose)) Call(actor, "K2_DestroyComponent").set("Component", pose).invoke();
    if (IsLive(fallback)) Call(fallback, "SetVisibility").setBool("bNewVisibility", true).setBool("bPropagateToChildren", false).invoke();
    pose = nullptr;
    writer.Reset();
}

// Swaps which body renders; slaved parts follow whichever component is visible.
bool SetPosing(bool on, bool& posing, UObject* pose, UObject* fallback, const std::vector<UObject*>& masteredParts) {
    if (on == posing || (on && !IsLive(pose))) return posing;
    posing = on;
    if (IsLive(pose))
        Call(pose, "SetVisibility").setBool("bNewVisibility", on).setBool("bPropagateToChildren", false).invoke();
    Call(fallback, "SetVisibility").setBool("bNewVisibility", !on).setBool("bPropagateToChildren", false).invoke();
    for (auto part : masteredParts)
        Call(part, "SetMasterPoseComponent").set("NewMasterBoneComponent", on ? pose : fallback).invoke();
    return posing;
}

UObject* AvatarBody() {
    return g_avatar.posing && IsLive(g_avatar.pose) ? g_avatar.pose : g_avatar.component;
}

void SetAvatarHidden(bool hidden) {
    if (g_avatar.hidden == hidden) return;
    g_avatar.hidden = hidden;
    Call(g_avatar.actor, "SetActorHiddenInGame").setBool("bNewHidden", hidden).invoke();
    for (auto a : g_avatar.parts) Call(a, "SetActorHiddenInGame").setBool("bNewHidden", hidden).invoke();
}

void AnimateAvatar(const Snapshot& s, double now) {
    float speed = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y);
    int tier = speed < 30 ? 0 : speed < 250 ? 1 : speed < 520 ? 2 : 3;
    UObject* anim = nullptr;
    bool riding = (s.flags & kInVehicle) != 0;
    if (s.shotSequence && s.shotSequence != g_avatar.lastShotSequence) {
        g_avatar.lastShotSequence = s.shotSequence;
        if (!riding && (s.weaponType == kWeaponGun || s.weaponType == kWeaponCrossbow)) {
            const char* shotPath = s.weaponType == kWeaponCrossbow ?
                "/Game/Animation/Deacon/gun/gun_poses/dcn_gun_fire_crossbow.dcn_gun_fire_crossbow" :
                "/Game/Animation/Deacon/gun/gun_poses/dcn_gun_fire_ak47.dcn_gun_fire_ak47";
            anim = FindObject(shotPath, C.animSequence);
            if (anim) {
                g_avatar.currentAnim = anim;
                g_avatar.shotUntil = now + 0.3;
                Call(g_avatar.component, "PlayAnimation").set("NewAnimToPlay", anim).setBool("bLooping", false).invoke();
                return;
            }
        }
    }
    if (g_avatar.shotUntil > 0) {
        if (now < g_avatar.shotUntil) return;
        g_avatar.shotUntil = 0;
        g_avatar.currentAnim = nullptr;
    }
    if (s.flags & kInVehicle) {
        double started = Now();
        anim = FindObject(s.flags & kPassenger ?
            "/Game/Animation/Deacon/bike/bikeBuddy/bike_basic_control_buddy.bike_basic_control_buddy" :
            "/Game/Animation/Deacon/bike/bike_basic_control.bike_basic_control", C.animSequence);
        g_ridingLookupTime += Now() - started;
        ++g_ridingLookups;
    } else if (speed < 30 && (s.actions & kCrouched)) {
        anim = FindObject((s.actions & kAiming) && s.weaponType != kWeaponNone ?
            "/Game/Animation/Deacon/gun/aimed_locomotion/rifle_aimed_idle_crouch.rifle_aimed_idle_crouch" :
            "/Game/Animation/Deacon/crouch/dcn_idle_crouch.dcn_idle_crouch", C.animSequence);
    } else if (speed < 30 && (s.actions & kAiming) &&
               (s.weaponType == kWeaponGun || s.weaponType == kWeaponCrossbow)) {
        anim = FindObject(s.weaponType == kWeaponGun ?
            "/Game/Animation/Deacon/gun/aimed_locomotion/1lg2h_aimed_idle.1lg2h_aimed_idle" :
            "/Game/Animation/Deacon/gun/aimed_locomotion/pistol_aimed_idle.pistol_aimed_idle", C.animSequence);
    }
    for (int i = tier; i >= 0 && !anim; --i) {
        if (!IsLive(g_anims[i])) g_anims[i] = nullptr;
        anim = g_anims[i];
    }
    if (!anim) {
        const std::string* names[4] = {&g_cfg.animIdle, &g_cfg.animWalk, &g_cfg.animRun, &g_cfg.animSprint};
        for (int i = tier; i >= 0 && !anim; --i) {
            if (names[i]->empty()) continue;
            g_anims[i] = FindObject(names[i]->c_str(), C.animSequence);
            anim = g_anims[i];
        }
    }
    if (!anim || anim == g_avatar.currentAnim) return;
    g_avatar.currentAnim = anim;
    Call(g_avatar.component, "PlayAnimation").set("NewAnimToPlay", anim).setBool("bLooping", true).invoke();
    // These assets encode riding control poses rather than a timed idle loop.
    if ((s.flags & kInVehicle) && !Call(g_avatar.component, "Stop").invoke()) {
        Log("avatar: could not hold the riding control pose");
        g_avatar.currentAnim = nullptr;
    }
}

// Save loads and fast travel swap the player's meshes and play a dissolve-in; a copy taken then never renders.
void TrackLocalAppearance(UObject* localPawn, double now) {
    constexpr double kSettleSeconds = 5.0;
    bool changed = false;
    auto mesh = Prop(Prop(localPawn, "Mesh"), "SkeletalMesh");
    if (mesh != g_localMesh) {
        g_localMesh = mesh;
        changed = true;
    }
    Call where(localPawn, "K2_GetActorLocation");
    if (where.invoke()) {
        auto me = where.get<FVector>("ReturnValue");
        float dx = me.x - g_localPos.x, dy = me.y - g_localPos.y;
        changed |= dx * dx + dy * dy > 5000.f * 5000.f;
        g_localPos = me;
    }
    if (!changed) return;
    g_localSettledAt = now + kSettleSeconds;
    if (IsLive(g_avatar.actor)) {
        Log("avatar: local player reloaded or travelled, rebuilding avatar once settled");
        DestroyAvatar();
    }
}

bool AnchorRemotePassenger(FVector& location, float& yaw, double now) {
    if (!IsLive(g_localBike)) return false;
    if (g_avatar.seatBike != g_localBike || !IsLive(g_avatar.seatMesh)) {
        g_avatar.seatBike = g_localBike;
        g_avatar.seatMesh = nullptr;
        g_avatar.seatSocket = PassengerSocket();
        if (!g_avatar.seatSocket.index) return false;
        for (auto mesh : OwnedMeshes(g_nativeBikeMeshes, g_localBike, now)) {
            if (!IsLive(mesh)) continue;
            Call has(mesh, "DoesSocketExist");
            if (has.set("InSocketName", g_avatar.seatSocket).invoke() && has.get<bool>("ReturnValue")) {
                g_avatar.seatMesh = mesh;
                break;
            }
        }
    }
    if (!g_avatar.pelvis.index) {
        std::string names;
        g_avatar.pelvis = HipBone(AvatarBody(), names);
        if (!g_avatar.pelvis.index) return false;
    }
    Call seat(g_avatar.seatMesh, "GetSocketLocation"), hip(AvatarBody(), "GetSocketTransform");
    Call facing(Prop(g_pawn, "Mesh"), "K2_GetComponentRotation");
    if (!seat.set("InSocketName", g_avatar.seatSocket).invoke() ||
        !hip.set("InSocketName", g_avatar.pelvis).set("TransformSpace", static_cast<uint8_t>(2)).invoke() ||
        !facing.invoke()) return false;
    yaw = facing.get<FRotator>("ReturnValue").yaw;
    location = AlignBoneToSocket(seat.get<FVector>("ReturnValue"), hip.get<FTransform>("ReturnValue").translation, yaw);
    return true;
}

void UpdateAvatar(UObject* localPawn, double now) {
    bool peerAlive = now - g_lastRecv < kPeerTimeout;
    Snapshot s{};
    bool have = peerAlive && SampleRemote(now, s) && (s.flags & kHasPawn);
    if (localPawn) TrackLocalAppearance(localPawn, now);
    if (!localPawn || !have) {
        if (IsLive(g_avatar.actor)) SetAvatarHidden(true);
        return;
    }
    FVector origin = WorldOrigin(localPawn);
    FVector local{s.pos.x - origin.x, s.pos.y - origin.y, s.pos.z - origin.z};
    if (!IsLive(g_avatar.actor)) {
        // Copying the player's look mid-load would capture fade-in materials that never become visible.
        if (now < g_localSettledAt || now - g_lastSpawnAttempt < 2.0) return;
        g_lastSpawnAttempt = now;
        g_avatar = Avatar{};
        if (!SpawnAvatar(localPawn, local, g_avatar)) return;
        g_avatar.lastShotSequence = s.shotSequence;
    }
    PoseFrame frame;
    bool streamed = g_avatar.pose && now - g_lastBodyPose < kPoseFresh &&
        g_bodyPoses.Sample(g_motion.SenderMs(), frame) && g_avatar.writer.Apply(frame);
    std::vector<UObject*> mastered;
    for (auto part : g_avatar.parts) mastered.push_back(Prop(part, "SkeletalMeshComponent"));
    bool wasPosing = g_avatar.posing;
    SetPosing(streamed, g_avatar.posing, g_avatar.pose, g_avatar.component, mastered);
    if (wasPosing != g_avatar.posing) {
        Log("avatar: %s", g_avatar.posing ? "following the partner's streamed skeleton" : "pose stream stale; using animation clips");
        g_avatar.pelvis = {};
        g_avatar.currentAnim = nullptr;
    }
    if (!g_avatar.posing) AnimateAvatar(s, now);
    FRotator facing{0, s.yaw, 0};
    if (g_avatar.posing)
        if (auto root = FindBone(frame, kRootBoneHash)) QuatToRotator(root->q, facing.pitch, facing.yaw, facing.roll);
    if (s.flags & kPassenger) {
        if (!AnchorRemotePassenger(local, s.yaw, now)) {
            constexpr const char* error = "Partner passenger seat unavailable; waiting for the local bike";
            if (g_settingsError != error) Log("passenger: %s", error);
            g_settingsError = error;
            SetAvatarHidden(true);
            return;
        }
        if (g_settingsError == "Partner passenger seat unavailable; waiting for the local bike") g_settingsError.clear();
    }
    SetAvatarHidden(false);
    Call move(g_avatar.actor, "K2_SetActorLocationAndRotation");
    if (s.flags & kPassenger) facing = FRotator{0, s.yaw, 0};
    move.set("NewLocation", local).set("NewRotation", facing);
    bool moved = move.setBool("bSweep", false).setBool("bTeleport", true).invoke();
    MenuSettings remoteSettings;
    remoteSettings.jacket = g_peer.jacket;
    remoteSettings.trousers = g_peer.trousers;
    remoteSettings.capColor = g_peer.capColor;
    remoteSettings.cap = g_peer.cap != 0;
    static double lastAppearance = -10;
    if (now - lastAppearance >= 0.25) {
        lastAppearance = now;
        g_remoteAppearance.Apply(g_avatar.component, remoteSettings);
        if (IsLive(g_avatar.pose)) g_remoteAppearance.Apply(g_avatar.pose, remoteSettings);
        for (auto part : g_avatar.parts) g_remoteAppearance.Apply(Prop(part, "SkeletalMeshComponent"), remoteSettings);
    }
    if (now - g_lastStatus >= 10.0) {
        g_lastStatus = now;
        Call at(g_avatar.actor, "K2_GetActorLocation"), me(localPawn, "K2_GetActorLocation");
        at.invoke();
        me.invoke();
        auto a = at.get<FVector>("ReturnValue");
        auto m = me.get<FVector>("ReturnValue");
        // Where the avatar lands on screen tells "not rendered" apart from "outside the camera view".
        Call project(g_controller, "ProjectWorldLocationToScreen");
        project.set("WorldLocation", FVector{a.x, a.y, a.z + 100.f});
        bool onScreen = project.invoke() && project.get<bool>("ReturnValue");
        struct Vec2 { float x, y; } screen = onScreen ? project.get<Vec2>("ScreenLocation") : Vec2{-1, -1};
        Call visible(AvatarBody(), "IsVisible");
        int vis = visible.invoke() ? visible.get<bool>("ReturnValue") : -1;
        Log("status: partner %s at (%.0f %.0f %.0f) moved=%d, avatar at (%.0f %.0f %.0f), you at (%.0f %.0f %.0f) "
            "origin (%.0f %.0f %.0f) anim=%s actions=0x%x weapon=%u shot=%u screen=(%.0f %.0f) visible=%d",
            g_remoteName.c_str(), local.x, local.y, local.z, moved, a.x, a.y, a.z, m.x, m.y, m.z, origin.x, origin.y,
            origin.z, g_avatar.posing ? "streamed" : Name(g_avatar.currentAnim).c_str(), s.actions, s.weaponType, s.shotSequence, screen.x, screen.y, vis);
    }
}

// Host-to-client enemy replication. The host streams the evaluated skeletons of AI pawns near either
// player; the client renders poseable copies. Replicas are visual only: they have no AI or collision.
struct EnemySource {
    UObject* pawn = nullptr;
    uint32_t entity = 0;
    UObject* mesh = nullptr;
    UObject* asset = nullptr;
    BoneTable table;
    PoseReader reader;
    PoseTranslationTracker tracker;
    std::vector<int> subset;
    std::vector<uint32_t> names;
    std::vector<BonePose> all, bones;
    std::vector<std::string> paths;
    double lastInfo = -10;
};
struct EnemyReplica {
    UObject* actor = nullptr;
    UObject* pose = nullptr;
    std::vector<UObject*> parts;
    PoseWriter writer;
    PoseBuffer poses;
    std::vector<std::string> paths;
    double lastInfo = -10, lastSpawnAttempt = -10;
    bool missingLogged = false, alive = false;
    FVector at{}; // feet, in local engine coordinates
};
std::vector<EnemySource> g_enemySources;
std::unordered_map<uint32_t, EnemyReplica> g_enemyReplicas;
uint32_t g_nextEnemyEntity = 1;
double g_nextEnemySend = 0;
struct EnemyStats {
    uint64_t sent = 0, bytes = 0, infos = 0, reads = 0, applied = 0, rejected = 0;
    double readTime = 0;
} g_enemyStats;
constexpr size_t kEnemyMaxReplicas = 32;

bool EnemyReplicationActive(double now) {
    return g_settings.enemies && now - g_lastRecv < kPeerTimeout;
}

// "Package.Asset" path that FindObject resolves on the other machine.
std::string AssetPath(UObject* asset) {
    if (!IsLive(asset) || !asset->outer || asset->outer->outer) return {};
    return Name(asset->outer) + "." + Name(asset);
}

// ---- Held item mirroring --------------------------------------------------------------------------------------

struct RemoteGear {
    GearPacket desc{};
    bool have = false;
    double lastInfo = -10, lastAttempt = -10;
    UObject* actor = nullptr;
    UObject* component = nullptr;
    UObject* parent = nullptr;
    bool hidden = false, missingLogged = false;
} g_gear;
struct LocalGear {
    GearPacket packet{};
    bool valid = false;
    double lastSent = -10;
    UObject* loggedItem = nullptr;
    bool loggedOk = false;
    int32_t loggedSocket = 0;
} g_localGear;

void DestroyGear() {
    if (IsLive(g_gear.actor)) Call(g_gear.actor, "K2_DestroyActor").invoke();
    g_gear.actor = g_gear.component = g_gear.parent = nullptr;
    g_gear.hidden = false;
}


// Finds the socket on `body` that the item's attach chain hangs from.
bool GearSocket(UObject* item, UObject* body, FName& socket) {
    UObject* node = item;
    for (int depth = 0; depth < 8 && IsLive(node); ++depth) {
        UObject* parent = Prop(node, "AttachParent");
        if (!parent) {
            Call get(node, "GetAttachParent");
            if (get.invoke()) parent = get.get<UObject*>("ReturnValue");
        }
        if (parent == body) {
            if (GetProp(node, "AttachSocketName", socket)) return true;
            Call name(node, "GetAttachSocketName");
            if (!name.invoke()) return false;
            socket = name.get<FName>("ReturnValue");
            return true;
        }
        node = parent;
    }
    return false;
}

bool DescribeGear(UObject* pawn, GearPacket& out) {
    static UObject* staticComponent = FindClass("StaticMeshComponent");
    auto none = [&] { return EncodeGear(g_session, kGearNone, {}, 0, {}, nullptr, nullptr, nullptr, out); };
    UObject* body = IsA(pawn, C.character) ? Prop(pawn, "Mesh") : nullptr;
    Call held(pawn, "GetEquippedItemCurrentlyInHand");
    UObject* item = held.invoke() ? held.get<UObject*>("ReturnValue") : nullptr;
    if (!IsLive(item) || !IsLive(body)) {
        if (g_localGear.loggedItem) Log("gear: hands empty");
        g_localGear.loggedItem = nullptr;
        return none();
    }
    Call meshCall(item, "GetItemMesh");
    UObject* mesh = meshCall.invoke() ? meshCall.get<UObject*>("ReturnValue") : nullptr;
    if (!IsLive(mesh)) mesh = Prop(item, "Mesh");
    uint8_t kind = kGearNone;
    UObject* asset = nullptr;
    if (IsLive(mesh) && IsA(mesh, C.skelMeshComponent)) {
        kind = kGearSkeletal;
        asset = Prop(mesh, "SkeletalMesh");
    } else if (IsLive(mesh) && staticComponent && IsA(mesh, staticComponent)) {
        kind = kGearStatic;
        asset = Prop(mesh, "StaticMesh");
    }
    std::string path = AssetPath(asset);
    FName socket{};
    FTransform socketWorld{}, itemWorld{};
    bool ok = kind != kGearNone && !path.empty() && GearSocket(mesh, body, socket) && socket.index &&
              WorldTransform(body, socket, socketWorld) && WorldTransform(mesh, FName{0, 0}, itemWorld);
    if (item != g_localGear.loggedItem || ok != g_localGear.loggedOk || socket.index != g_localGear.loggedSocket) {
        g_localGear.loggedItem = item;
        g_localGear.loggedOk = ok;
        g_localGear.loggedSocket = socket.index;
        Log("gear: holding %s mesh=%s socket=%s%s", Name(item).c_str(), path.empty() ? "-" : path.c_str(),
            socket.index ? NameString(socket.index).c_str() : "-", ok ? "" : " (not mirrored)");
    }
    if (!ok) return none();
    BonePose offset = RelativePose(AsPose(itemWorld), AsPose(socketWorld));
    float t[3] = {offset.t.x, offset.t.y, offset.t.z};
    auto ratio = [](float a, float b) { return std::fabs(b) > 1e-4f ? std::fabs(a / b) : 1.f; };
    float scale[3] = {ratio(itemWorld.scale.x, socketWorld.scale.x), ratio(itemWorld.scale.y, socketWorld.scale.y),
                      ratio(itemWorld.scale.z, socketWorld.scale.z)};
    return EncodeGear(g_session, kind, NameString(socket.index), static_cast<uint32_t>(socket.number), path, offset.q, t,
                      scale, out);
}

void SendGear(UObject* pawn, double now) {
    GearPacket packet;
    if (!DescribeGear(pawn, packet)) return;
    bool changed = !g_localGear.valid || !SameGear(packet, g_localGear.packet);
    if (!changed && now - g_localGear.lastSent < kGearInterval) return;
    g_localGear.packet = packet;
    g_localGear.valid = true;
    g_localGear.lastSent = now;
    g_net->SendRaw(&packet, sizeof packet);
}

void ReceiveGear(const char* data, int len, double now) {
    GearPacket packet;
    if (!DecodeGear(data, len, packet) || packet.session != g_peer.session || !g_peer.session) return;
    if (!g_gear.have || !SameGear(packet, g_gear.desc)) {
        bool sameAsset = g_gear.have && g_gear.desc.kind == packet.kind && !std::strcmp(g_gear.desc.path, packet.path);
        if (!sameAsset) {
            DestroyGear();
            g_gear.missingLogged = false;
            g_gear.lastAttempt = -10;
            if (packet.kind != kGearNone) Log("gear: peer holds %s at %s", packet.path, packet.socket);
        }
        g_gear.parent = nullptr; // re-mount with the new offset
    }
    g_gear.desc = packet;
    g_gear.have = true;
    g_gear.lastInfo = now;
}

// Days Gone fades fresh meshes in through per-instance materials, so asset defaults can stay invisible. Borrow
// the materials of a live component that already shows the same asset (usually the local player's own copy).
// The scan walks the whole object array (~80 ms), so remember each asset's donor while it stays valid.
UObject* LiveMeshUser(UObject* asset, bool skeletal, UObject* exclude) {
    static std::unordered_map<UObject*, UObject*> donors;
    const char* property = skeletal ? "SkeletalMesh" : "StaticMesh";
    if (auto cached = donors.find(asset); cached != donors.end()) {
        UObject* o = cached->second;
        if (o != exclude && IsLive(o) && Prop(o, property) == asset) return o;
        donors.erase(cached);
    }
    static UObject* staticComponent = FindClass("StaticMeshComponent");
    UObject* cls = skeletal ? C.skelMeshComponent : staticComponent;
    UObject* found = nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n && !found; ++i) {
        auto o = ObjectAt(i);
        if (!o || o == exclude || !IsLive(o) || IsDefaultOrArchetype(o) || !IsA(o, cls) || Prop(o, property) != asset) continue;
        Call visible(o, "IsVisible");
        if (visible.invoke() && visible.get<bool>("ReturnValue")) found = o;
    }
    if (found) donors[asset] = found;
    return found;
}

int CopyMaterials(UObject* from, UObject* to) {
    Call count(from, "GetNumMaterials");
    int n = count.invoke() ? count.get<int32_t>("ReturnValue") : 0, copied = 0;
    for (int i = 0; i < n && i < 64; ++i) {
        Call get(from, "GetMaterial");
        if (!get.set("ElementIndex", i).invoke()) continue;
        auto material = get.get<UObject*>("ReturnValue");
        if (IsLive(material) && Call(to, "SetMaterial").set("ElementIndex", i).set("Material", material).invoke()) ++copied;
    }
    return copied;
}

bool SpawnGear(UObject* context, const FVector& at) {
    static UObject* skeletalClass = FindClass("SkeletalMesh");
    static UObject* staticClass = FindClass("StaticMesh");
    bool skeletal = g_gear.desc.kind == kGearSkeletal;
    UObject* asset = FindObject(g_gear.desc.path, skeletal ? skeletalClass : staticClass);
    if (!IsLive(asset)) {
        if (!g_gear.missingLogged) Log("gear: %s is not loaded here; not shown", g_gear.desc.path);
        g_gear.missingLogged = true;
        return false;
    }
    g_gear.actor = SpawnMeshActor(context, at);
    UObject* carrier = Prop(g_gear.actor, "SkeletalMeshComponent");
    if (skeletal) {
        if (Call(carrier, "SetSkeletalMesh").set("NewMesh", asset).invoke()) g_gear.component = carrier;
    } else {
        UObject* component = CreateRuntimeComponent(g_gear.actor, "StaticMeshComponent");
        if (IsLive(component) && Call(component, "SetStaticMesh").set("NewMesh", asset).invoke()) g_gear.component = component;
        Call(g_gear.actor, "SetActorEnableCollision").setBool("bNewActorEnableCollision", false).invoke();
    }
    if (!IsLive(g_gear.component) || Prop(g_gear.component, skeletal ? "SkeletalMesh" : "StaticMesh") != asset) {
        if (!g_gear.missingLogged) Log("gear: could not build %s", g_gear.desc.path);
        g_gear.missingLogged = true;
        DestroyGear();
        return false;
    }
    double started = Now();
    UObject* donor = LiveMeshUser(asset, skeletal, g_gear.component);
    int copied = donor ? CopyMaterials(donor, g_gear.component) : 0;
    Call first(g_gear.component, "GetMaterial");
    UObject* material = first.set("ElementIndex", 0).invoke() ? first.get<UObject*>("ReturnValue") : nullptr;
    Log("gear: showing %s; %d materials from %s (%.1f ms), slot 0 %s", g_gear.desc.path, copied,
        donor ? FullName(donor).c_str() : "asset defaults", (Now() - started) * 1000, FullName(material).c_str());
    return true;
}

void UpdateGear(UObject* localPawn, double now) {
    UObject* body = AvatarBody();
    bool want = g_gear.have && g_gear.desc.kind != kGearNone && now - g_gear.lastInfo < kPeerTimeout &&
                IsLive(g_avatar.actor) && IsLive(body) && IsLive(localPawn);
    if (!want) {
        if (g_gear.actor) DestroyGear();
        return;
    }
    if (!IsLive(g_gear.actor)) {
        g_gear.actor = nullptr;
        if (now - g_gear.lastAttempt < kGearRetry) return;
        g_gear.lastAttempt = now;
        Call location(g_avatar.actor, "K2_GetActorLocation");
        if (!location.invoke() || !SpawnGear(localPawn, location.get<FVector>("ReturnValue"))) return;
        g_gear.parent = nullptr;
    }
    if (g_gear.parent != body) {
        int32_t index = FindNameIndex(g_gear.desc.socket);
        FName socket{index, static_cast<int32_t>(g_gear.desc.socketNumber)};
        float pitch = 0, yaw = 0, roll = 0;
        QuatToRotator(g_gear.desc.q, pitch, yaw, roll);
        bool mounted = index >= 0 &&
            Call(g_gear.actor, "K2_AttachRootComponentTo").set("InParent", body).set("InSocketName", socket)
                .set("AttachLocationType", static_cast<uint8_t>(2)).invoke() &&
            Call(g_gear.actor, "K2_SetActorRelativeLocation")
                .set("NewRelativeLocation", FVector{g_gear.desc.t[0], g_gear.desc.t[1], g_gear.desc.t[2]}).invoke() &&
            Call(g_gear.actor, "K2_SetActorRelativeRotation").set("NewRelativeRotation", FRotator{pitch, yaw, roll}).invoke() &&
            Call(g_gear.actor, "SetActorRelativeScale3D")
                .set("NewRelativeScale", FVector{g_gear.desc.scale[0], g_gear.desc.scale[1], g_gear.desc.scale[2]}).invoke();
        if (!mounted) {
            Log("gear: could not mount %s at %s", g_gear.desc.path, g_gear.desc.socket);
            DestroyGear();
            g_gear.lastAttempt = now;
            return;
        }
        g_gear.parent = body;
    }
    BonePose offset;
    std::memcpy(offset.q, g_gear.desc.q, sizeof offset.q);
    offset.t = {g_gear.desc.t[0], g_gear.desc.t[1], g_gear.desc.t[2]};
    if (int32_t index = FindNameIndex(g_gear.desc.socket); index >= 0)
        FollowSocket(g_gear.actor, body, FName{index, static_cast<int32_t>(g_gear.desc.socketNumber)}, offset);
    if (g_gear.hidden != g_avatar.hidden) {
        g_gear.hidden = g_avatar.hidden;
        Call(g_gear.actor, "SetActorHiddenInGame").setBool("bNewHidden", g_gear.hidden).invoke();
    }
}

// Enemies the host owns for both players: freakers, animals, and anyone hostile to the local player.
// Camp NPCs and companions stay local on each machine so they are never doubled.
bool SharedEnemy(UObject* ai, UObject* player) {
    static UObject* freaker = FindClass("BendFreakerController");
    static UObject* animal = FindClass("BendAnimalController");
    auto controller = Prop(ai, "Controller");
    if (!IsLive(controller)) return false;
    if ((freaker && IsA(controller, freaker)) || (animal && IsA(controller, animal))) return true;
    Call enemy(controller, "IsEnemy");
    return enemy.set("BendPawn", player).invoke() && enemy.get<bool>("ReturnValue");
}

// Walks the object array in slices so finding enemies never stalls a frame.
struct EnemyScan {
    int32_t cursor = 0;
    std::vector<UObject*> pawns;
    std::unordered_map<UObject*, std::vector<UObject*>> components;
    std::unordered_map<UObject*, uint8_t> kinds; // class -> 0 other, 1 skeletal mesh component, 2 AI pawn
    double time = 0;
} g_enemyScan;

bool ClassIsA(UObject* cls, UObject* base) {
    for (auto c = cls; c && base; c = Super(c))
        if (c == base) return true;
    return false;
}

uint8_t EnemyClassKind(UObject* cls) {
    static UObject* aiClass = FindClass("BendAIPawn");
    auto [it, added] = g_enemyScan.kinds.try_emplace(cls, 0);
    if (added) it->second = ClassIsA(cls, C.skelMeshComponent) ? 1 : ClassIsA(cls, aiClass) ? 2 : 0;
    return it->second;
}

void FinishEnemyScan(UObject* pawn) {
    Call me(pawn, "K2_GetActorLocation");
    if (!me.invoke()) return;
    FVector origin = WorldOrigin(pawn), at = me.get<FVector>("ReturnValue");
    std::vector<FVector> players{{at.x + origin.x, at.y + origin.y, at.z + origin.z}};
    if (g_peer.flags & kHasPawn) players.push_back({g_peer.pos[0], g_peer.pos[1], g_peer.pos[2]});
    struct Candidate { UObject* pawn; float distance; };
    std::vector<Candidate> found;
    for (auto o : g_enemyScan.pawns) {
        if (!IsLive(o) || !IsLive(Prop(Prop(o, "Mesh"), "SkeletalMesh"))) continue;
        // Pooled pawns parked near the world origin are alive but have no controller.
        Call alive(o, "IsAlive");
        bool living = alive.invoke() && alive.get<bool>("ReturnValue");
        if (living && !IsLive(Prop(o, "Controller"))) continue;
        if (IsGuestProxy(o) || (living && !SharedEnemy(o, pawn))) continue;
        Call where(o, "K2_GetActorLocation");
        if (!where.invoke()) continue;
        auto p = where.get<FVector>("ReturnValue");
        FVector world{p.x + origin.x, p.y + origin.y, p.z + origin.z};
        float best = 1e30f;
        for (const auto& player : players) {
            float dx = world.x - player.x, dy = world.y - player.y, dz = world.z - player.z;
            best = std::min(best, dx * dx + dy * dy + dz * dz);
        }
        if (best <= kEnemyRadius * kEnemyRadius) found.push_back({o, best});
    }
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
    if (found.size() > kEnemyMaxStreamed) found.resize(kEnemyMaxStreamed);
    std::vector<EnemySource> next;
    for (const auto& candidate : found) {
        auto existing = std::find_if(g_enemySources.begin(), g_enemySources.end(),
            [&](const EnemySource& e) { return e.pawn == candidate.pawn; });
        EnemySource source;
        if (existing != g_enemySources.end()) source = std::move(*existing);
        else {
            source.pawn = candidate.pawn;
            source.entity = g_nextEnemyEntity++;
            if (!g_nextEnemyEntity) g_nextEnemyEntity = 1;
        }
        auto mesh = Prop(candidate.pawn, "Mesh");
        std::vector<std::string> paths{AssetPath(Prop(mesh, "SkeletalMesh"))};
        for (auto part : g_enemyScan.components[candidate.pawn]) {
            if (part == mesh || paths.size() == kEnemyMeshPaths || !IsLive(part) || !IsCharacterPart(part, mesh)) continue;
            auto path = AssetPath(Prop(part, "SkeletalMesh"));
            if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
        }
        if (paths[0].empty()) continue;
        if (paths != source.paths) source.lastInfo = -10;
        source.paths = std::move(paths);
        next.push_back(std::move(source));
    }
    if (next.size() != g_enemySources.size())
        Log("enemies: streaming %d of the host's AI pawns", static_cast<int>(next.size()));
    g_enemySources = std::move(next);
}

void StepEnemyScan(UObject* pawn) {
    double start = Now();
    int32_t count = ObjectCount();
    // A full pass takes about two seconds at 60 Hz; new enemies appear within that window.
    int32_t end = std::min(count, g_enemyScan.cursor + std::max<int32_t>(2048, count / 120));
    UObject* lastClass = nullptr;
    uint8_t lastKind = 0;
    for (int32_t i = g_enemyScan.cursor; i < end; ++i) {
        auto o = ObjectAt(i);
        if (!o || !o->cls || !o->outer || IsDefaultOrArchetype(o)) continue;
        if (o->cls != lastClass) {
            lastClass = o->cls;
            lastKind = EnemyClassKind(lastClass);
        }
        uint8_t kind = lastKind;
        if (kind == 1 && o->outer->cls && EnemyClassKind(o->outer->cls) == 2 && IsLive(o))
            g_enemyScan.components[o->outer].push_back(o);
        else if (kind == 2 && IsLive(o))
            g_enemyScan.pawns.push_back(o);
    }
    g_enemyScan.cursor = end;
    if (end >= count) {
        FinishEnemyScan(pawn);
        g_enemyScan.cursor = 0;
        g_enemyScan.pawns.clear();
        g_enemyScan.components.clear();
    }
    g_enemyScan.time += Now() - start;
}

void SendEnemy(EnemySource& e, const FVector& origin, uint32_t timeMs, double now) {
    auto mesh = Prop(e.pawn, "Mesh");
    auto asset = Prop(mesh, "SkeletalMesh");
    if (!IsLive(e.pawn) || !IsLive(mesh) || !IsLive(asset)) return;
    if (mesh != e.mesh || asset != e.asset || !e.table.Valid()) {
        e.table = BoneTable{};
        e.reader = PoseReader{};
        e.tracker.Reset();
        if (!ReadBoneTable(mesh, e.table)) return;
        e.mesh = mesh;
        e.asset = asset;
        std::vector<std::string> names;
        for (auto name : e.table.names) names.push_back(NameString(name.index));
        e.subset = EnemyBoneSubset(names, e.table.parents);
        e.names.clear();
        for (int i : e.subset) e.names.push_back(e.table.hashes[i]);
        e.lastInfo = -10;
        Log("enemies: %s streams %d of %d bones", Name(asset).c_str(), static_cast<int>(e.subset.size()),
            static_cast<int>(names.size()));
    }
    if (e.subset.empty()) return;
    if (now - e.lastInfo >= kEnemyInfoInterval) {
        EnemyInfoPacket info;
        if (EncodeEnemyInfo(g_session, e.entity, e.paths, info)) {
            g_net->SendRaw(&info, sizeof info);
            ++g_enemyStats.infos;
            g_enemyStats.bytes += sizeof info;
        }
        e.lastInfo = now;
    }
    double start = Now();
    bool read = e.reader.Read(mesh, e.table, static_cast<size_t>(e.subset.back()) + 1, e.all);
    g_enemyStats.readTime += Now() - start;
    ++g_enemyStats.reads;
    Call location(mesh, "K2_GetComponentLocation"), rotation(mesh, "K2_GetComponentRotation");
    if (!read || !location.invoke() || !rotation.invoke()) return;
    e.bones.clear();
    for (int i : e.subset) e.bones.push_back(e.all[i]);
    auto at = location.get<FVector>("ReturnValue");
    auto r = rotation.get<FRotator>("ReturnValue");
    BonePose root;
    RotatorToQuat(r.pitch, r.yaw, r.roll, root.q);
    e.names.push_back(kRootBoneHash);
    e.bones.push_back(root);
    PoseHeader header{};
    header.kind = kPoseEnemy;
    header.session = g_session;
    header.timeMs = timeMs;
    header.entity = e.entity;
    header.root[0] = at.x + origin.x;
    header.root[1] = at.y + origin.y;
    header.root[2] = at.z + origin.z;
    header.yaw = r.yaw;
    float health = 0;
    GetProp(e.pawn, "Health", health);
    header.health = std::isfinite(health) ? std::clamp(health, 0.f, 100000.f) : 0.f;
    Call alive(e.pawn, "IsAlive");
    if (alive.invoke() && alive.get<bool>("ReturnValue")) header.flags |= kPoseAlive;
    EncodePose(header, e.names, e.bones, [](const char* packet, size_t size) {
        g_net->SendRaw(packet, static_cast<int>(size));
        ++g_enemyStats.sent;
        g_enemyStats.bytes += size;
    }, &e.tracker);
    e.names.pop_back();
}

void SendEnemies(UObject* pawn, double now) {
    if (!IsHost() || !pawn || !EnemyReplicationActive(now) || !(g_peer.flags & kSyncEnemies)) {
        if (!g_enemySources.empty()) Log("enemies: stopped streaming");
        g_enemySources.clear();
        g_enemyScan.cursor = 0;
        g_enemyScan.pawns.clear();
        g_enemyScan.components.clear();
        if (!pawn) g_enemyScan.kinds.clear();
        return;
    }
    StepEnemyScan(pawn);
    if (now < g_nextEnemySend) return;
    g_nextEnemySend += kEnemySendInterval;
    if (g_nextEnemySend <= now) g_nextEnemySend = now + kEnemySendInterval;
    FVector origin = WorldOrigin(pawn);
    auto timeMs = static_cast<uint32_t>(static_cast<uint64_t>(now * 1000.0));
    for (auto& e : g_enemySources) SendEnemy(e, origin, timeMs, now);
}

void DestroyReplica(EnemyReplica& r) {
    if (IsLive(r.actor)) Call(r.actor, "K2_DestroyActor").invoke();
    r.actor = r.pose = nullptr;
    r.parts.clear();
    r.writer.Reset();
}

void DestroyEnemyReplicas() {
    for (auto& [entity, replica] : g_enemyReplicas) DestroyReplica(replica);
    if (!g_enemyReplicas.empty()) Log("enemies: removed %d replicas", static_cast<int>(g_enemyReplicas.size()));
    g_enemyReplicas.clear();
}

bool AcceptEnemies(double now) {
    return !IsHost() && g_pawn && EnemyReplicationActive(now);
}

EnemyReplica* ReplicaFor(uint32_t entity) {
    auto it = g_enemyReplicas.find(entity);
    if (it != g_enemyReplicas.end()) return &it->second;
    if (g_enemyReplicas.size() >= kEnemyMaxReplicas) return nullptr;
    return &g_enemyReplicas[entity];
}

void ReceiveEnemyInfo(const char* data, int len, double now) {
    EnemyInfoPacket info;
    if (!DecodeEnemyInfo(data, len, info) || info.session != g_peer.session || !g_peer.session) {
        ++g_enemyStats.rejected;
        return;
    }
    if (!AcceptEnemies(now)) return;
    auto replica = ReplicaFor(info.entity);
    if (!replica) return;
    std::vector<std::string> paths;
    for (int i = 0; i < info.paths; ++i) paths.emplace_back(info.path[i]);
    if (paths != replica->paths) {
        DestroyReplica(*replica);
        replica->paths = std::move(paths);
        replica->missingLogged = false;
        replica->lastSpawnAttempt = -10;
    }
    replica->lastInfo = now;
}

void ReceiveEnemyPose(const PosePacket& packet, double now) {
    if (!AcceptEnemies(now)) return;
    if (auto replica = ReplicaFor(packet.header.entity)) replica->poses.Push(packet, now);
}

bool SpawnReplica(UObject* context, EnemyReplica& r, uint32_t entity, const FVector& at) {
    static UObject* meshClass = FindClass("SkeletalMesh");
    std::vector<UObject*> assets;
    for (const auto& path : r.paths) {
        auto asset = FindObject(path.c_str(), meshClass);
        if (!asset) {
            if (!r.missingLogged) Log("enemies: replica %u skipped; %s is not loaded on this machine", entity, path.c_str());
            r.missingLogged = true;
            if (assets.empty()) return false;
            continue;
        }
        assets.push_back(asset);
    }
    r.actor = SpawnMeshActor(context, at);
    if (!r.actor) return false;
    r.pose = CreatePoseableComponent(r.actor);
    bool ok = r.pose && Call(r.pose, "SetSkeletalMesh").set("NewMesh", assets[0]).invoke() && r.writer.Attach(r.pose);
    if (!ok) {
        Log("enemies: replica %u could not create a poseable %s", entity, Name(assets[0]).c_str());
        DestroyReplica(r);
        return false;
    }
    for (size_t i = 1; i < assets.size(); ++i) {
        auto part = CreatePoseableComponent(r.actor);
        if (part && Call(part, "SetSkeletalMesh").set("NewMesh", assets[i]).invoke() &&
            Call(part, "SetMasterPoseComponent").set("NewMasterBoneComponent", r.pose).invoke())
            r.parts.push_back(part);
    }
    Log("enemies: replica %u %s with %d bones and %d parts", entity, Name(assets[0]).c_str(),
        static_cast<int>(r.writer.Table().names.size()), static_cast<int>(r.parts.size()));
    return true;
}

void UpdateEnemyReplicas(UObject* pawn, double now) {
    if (!AcceptEnemies(now) || !pawn) {
        DestroyEnemyReplicas();
        return;
    }
    FVector origin = WorldOrigin(pawn);
    double senderMs = g_motion.SenderMs();
    for (auto it = g_enemyReplicas.begin(); it != g_enemyReplicas.end();) {
        auto& r = it->second;
        if (now - std::max(r.poses.LastArrival(), r.lastInfo) > kEnemyTimeout) {
            if (IsLive(r.actor)) Log("enemies: replica %u expired", it->first);
            DestroyReplica(r);
            it = g_enemyReplicas.erase(it);
            continue;
        }
        PoseFrame frame;
        if (r.paths.empty() || !r.poses.Sample(senderMs, frame)) {
            ++it;
            continue;
        }
        FVector at{frame.root[0] - origin.x, frame.root[1] - origin.y, frame.root[2] - origin.z};
        if (!IsLive(r.actor) && now - r.lastSpawnAttempt >= 1.0) {
            r.lastSpawnAttempt = now;
            SpawnReplica(pawn, r, it->first, at);
        }
        if (IsLive(r.actor)) {
            FRotator facing{0, frame.yaw, 0};
            if (auto root = FindBone(frame, kRootBoneHash)) QuatToRotator(root->q, facing.pitch, facing.yaw, facing.roll);
            Call move(r.actor, "K2_SetActorLocationAndRotation");
            move.set("NewLocation", at).set("NewRotation", facing).setBool("bSweep", false).setBool("bTeleport", true).invoke();
            if (r.writer.Apply(frame)) ++g_enemyStats.applied;
            r.at = at;
            r.alive = (frame.flags & kPoseAlive) && frame.health > 0;
        }
        ++it;
    }
}

// Shared combat: the guest's hits on replicas become real damage on the host's pawns.
struct EnemyHitState {
    uint32_t sequence = 0;
    struct Resend { EnemyHitPacket packet; double at; };
    std::vector<Resend> resend;
    EnemyHitDedupe dedupe;
    uint64_t sent = 0, applied = 0, ignored = 0;
} g_enemyHits;

bool HasEnemyReplicas() { return !g_enemyReplicas.empty(); }

void QueueEnemyHit(bool melee, UObject* weapon) {
    double now = Now();
    if (IsHost() || !g_pawn || g_passenger.pawn || g_menu.IsOpen() || g_enemyReplicas.empty() ||
        !IsLive(weapon) || !AcceptEnemies(now) || !Alive(g_pawn)) return;
    uint32_t best = 0;
    float bestDistance = 1e30f;
    FVector start{}, aim{};
    if (melee) {
        Call at(Prop(g_pawn, "Mesh"), "K2_GetComponentLocation"), facing(Prop(g_pawn, "Mesh"), "K2_GetComponentRotation");
        if (!at.invoke() || !facing.invoke()) return;
        start = at.get<FVector>("ReturnValue");
        start.z += 90;
        float yaw = (facing.get<FRotator>("ReturnValue").yaw + 90) * 3.14159265f / 180.f;
        for (const auto& [entity, r] : g_enemyReplicas) {
            if (!r.alive || !IsLive(r.actor)) continue;
            FVector center{r.at.x, r.at.y, r.at.z + 90};
            float dx = center.x - start.x, dy = center.y - start.y, dz = center.z - start.z;
            float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > kEnemyMeleeRange || std::fabs(dz) > 160 ||
                dx * std::cos(yaw) + dy * std::sin(yaw) < distance * 0.35f || distance >= bestDistance) continue;
            best = entity;
            bestDistance = distance;
            aim = center;
        }
    } else {
        auto camera = Prop(g_controller, "PlayerCameraManager");
        Call where(camera, "GetCameraLocation"), rotation(camera, "GetCameraRotation");
        if (!where.invoke() || !rotation.invoke()) return;
        start = where.get<FVector>("ReturnValue");
        auto r = rotation.get<FRotator>("ReturnValue");
        float yaw = r.yaw * 3.14159265f / 180.f, pitch = r.pitch * 3.14159265f / 180.f;
        float s0[3]{start.x, start.y, start.z};
        float d[3]{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
        for (const auto& [entity, replica] : g_enemyReplicas) {
            if (!replica.alive || !IsLive(replica.actor)) continue;
            float feet[3]{replica.at.x, replica.at.y, replica.at.z};
            float t = AimDistanceToBody(s0, d, feet);
            if (t < 0 || t >= bestDistance) continue;
            best = entity;
            bestDistance = t;
            aim = {start.x + d[0] * t, start.y + d[1] * t, start.z + d[2] * t};
        }
    }
    if (!best) {
        if (g_cfg.discovery) Log("enemies: %s hit no replica", melee ? "melee" : "shot");
        return;
    }
    if (TraceWorld(start, aim) != 0) {
        if (g_cfg.discovery) Log("enemies: world geometry blocked the hit on %u", best);
        return;
    }
    float amount = 0;
    if (!WeaponDamage(melee, weapon, amount)) return;
    if (!++g_enemyHits.sequence) ++g_enemyHits.sequence;
    auto packet = EncodeEnemyHit(g_session, best, g_enemyHits.sequence, melee, amount);
    g_net->SendRaw(&packet, sizeof packet);
    g_enemyHits.resend.push_back({packet, now + 0.1});
    ++g_enemyHits.sent;
    Log("enemies: %s hit replica %u at %.0f cm for %.1f", melee ? "melee" : "shot", best, bestDistance, amount);
}

// Sends a 25-point gunshot hit on the nearest live replica, bypassing aim; tests the network and host path.
bool DebugHitEnemy() {
    if (IsHost() || !g_pawn || !AcceptEnemies(Now())) return false;
    uint32_t best = 0;
    float bestDistance = 1e30f;
    Call me(g_pawn, "K2_GetActorLocation");
    if (!me.invoke()) return false;
    auto at = me.get<FVector>("ReturnValue");
    for (const auto& [entity, r] : g_enemyReplicas) {
        if (!r.alive || !IsLive(r.actor)) continue;
        float dx = r.at.x - at.x, dy = r.at.y - at.y, d = dx * dx + dy * dy;
        if (d < bestDistance) { best = entity; bestDistance = d; }
    }
    if (!best) return false;
    if (!++g_enemyHits.sequence) ++g_enemyHits.sequence;
    auto packet = EncodeEnemyHit(g_session, best, g_enemyHits.sequence, false, 25.f);
    g_net->SendRaw(&packet, sizeof packet);
    g_enemyHits.resend.push_back({packet, Now() + 0.1});
    Log("debug: hit-enemy sent 25 to replica %u at %.0f cm", best, std::sqrt(bestDistance));
    return true;
}

void ResendEnemyHits(double now) {
    for (auto it = g_enemyHits.resend.begin(); it != g_enemyHits.resend.end();) {
        if (now < it->at) { ++it; continue; }
        g_net->SendRaw(&it->packet, sizeof it->packet);
        it = g_enemyHits.resend.erase(it);
    }
}

void ReceiveEnemyHit(const char* data, int len, double now) {
    EnemyHitPacket hit;
    if (!DecodeEnemyHit(data, len, hit) || hit.session != g_peer.session || !g_peer.session) {
        ++g_enemyStats.rejected;
        return;
    }
    if (!IsHost() || !g_pawn || !EnemyReplicationActive(now) || !g_enemyHits.dedupe.Accept(hit.session, hit.sequence)) return;
    auto source = std::find_if(g_enemySources.begin(), g_enemySources.end(),
        [&](const EnemySource& e) { return e.entity == hit.entity; });
    if (source == g_enemySources.end() || !IsLive(source->pawn) || !Alive(source->pawn)) {
        ++g_enemyHits.ignored;
        Log("enemies: partner hit on %u ignored; that enemy is gone", hit.entity);
        return;
    }
    Call where(source->pawn, "K2_GetActorLocation");
    if (!where.invoke()) return;
    FVector origin = WorldOrigin(g_pawn), at = where.get<FVector>("ReturnValue");
    float dx = at.x + origin.x - g_peer.pos[0], dy = at.y + origin.y - g_peer.pos[1], dz = at.z + origin.z - g_peer.pos[2];
    float range = hit.kind == 1 ? kEnemyMeleeRange + 400 : kEnemyShotRange;
    if (dx * dx + dy * dy + dz * dz > range * range) {
        ++g_enemyHits.ignored;
        Log("enemies: partner hit on %u ignored; out of range", hit.entity);
        return;
    }
    float before = 0, after = 0;
    GetProp(source->pawn, "Health", before);
    Call damage(C.statics, "ApplyDamage");
    if (!damage.set("DamagedActor", source->pawn).set("BaseDamage", hit.damage).set("EventInstigator", g_controller)
            .set("DamageCauser", g_pawn).set("DamageTypeClass", FindClass("DamageType")).invoke()) {
        Log("enemies: ApplyDamage failed for partner hit on %u", hit.entity);
        return;
    }
    GetProp(source->pawn, "Health", after);
    ++g_enemyHits.applied;
    Log("enemies: partner %s hit %s (%u) for %.1f: health %.1f -> %.1f", hit.kind == 1 ? "melee" : "shot",
        Name(source->pawn).c_str(), hit.entity, hit.damage, before, after);
}

// The host's AI only fights pawns. A hidden stand-in pawn with the player's faction follows the guest's
// avatar in the host world. Enemies nearer to it than to the host are made aware of it; their melee swings
// at it (OnEnemySwing) and any damage it takes are forwarded to the guest, and its health is refilled.
struct GuestProxy {
    UObject* pawn = nullptr;
    UObject* controller = nullptr;
    float health = 0;
    uint32_t sequence = 0;
    double nextSpawn = 0, nextReport = 0, settleAt = 0, nextAggro = 0;
    struct Resend { GuestStrikePacket packet; double at; };
    std::vector<Resend> resend;
    EnemyHitDedupe dedupe; // guest side
    bool failedLogged = false;
} g_proxy;
constexpr float kProxyMaxStrike = 400.f; // larger drops are engine resets, not hits

bool WriteFloatProp(UObject* o, const char* name, float value) {
    auto property = IsLive(o) ? FindChild(o->cls, name) : nullptr;
    if (!property) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(o) + PropertyOffset(property), &value, sizeof value);
    return true;
}

bool ProxyWanted(double now) {
    return IsHost() && g_pawn && EnemyReplicationActive(now) && (g_peer.flags & kSyncEnemies) &&
        (g_peer.flags & kHasPawn) && (g_peer.flags & kAlive) && IsLive(g_avatar.actor) && !g_avatar.hidden;
}

void DestroyProxy(const char* why) {
    if (IsLive(g_proxy.controller)) Call(g_proxy.controller, "K2_DestroyActor").invoke();
    if (IsLive(g_proxy.pawn)) Call(g_proxy.pawn, "K2_DestroyActor").invoke();
    if (g_proxy.pawn) Log("enemies: guest stand-in removed (%s)", why);
    g_proxy.pawn = g_proxy.controller = nullptr;
}

bool SpawnProxy(const FVector& at, double now) {
    g_proxy.nextSpawn = now + 5;
    static UObject* pawnClass = FindClass("SwarmerPawn_C");
    auto library = FindDefaultObject("AIBlueprintHelperLibrary");
    Call faction(g_pawn, "GetFactionType");
    if (!pawnClass || !library || !faction.invoke()) {
        if (!g_proxy.failedLogged) Log("enemies: guest stand-in unavailable (class, spawn library or faction missing)");
        g_proxy.failedLogged = true;
        return false;
    }
    Call spawn(library, "SpawnAIFromClass");
    if (!spawn.set("WorldContextObject", g_pawn).set("PawnClass", pawnClass)
            .set("BehaviorTree", static_cast<UObject*>(nullptr)).set("Location", at)
            .set("Rotation", FRotator{}).setBool("bNoCollisionFail", true).invoke()) return false;
    auto pawn = spawn.get<UObject*>("ReturnValue");
    if (!IsLive(pawn)) return false;
    g_proxy.pawn = pawn;
    g_proxy.controller = Prop(pawn, "Controller");
    // No look is assigned, so the body renders nothing; stopping the brain keeps it from acting.
    static const wchar_t reason[] = L"TheysGone stand-in";
    struct { const wchar_t* data; int32_t num, max; } text{reason, static_cast<int32_t>(std::size(reason)),
        static_cast<int32_t>(std::size(reason))};
    bool stopped = Call(Prop(g_proxy.controller, "BrainComponent"), "StopLogic").set("Reason", text).invoke();
    uint8_t playerFaction = faction.get<uint8_t>("ReturnValue");
    bool factioned = Call(pawn, "SetFactionType").set("newFaction", playerFaction).invoke();
    // The engine initialises health over the next frames; the baseline is taken once it settles.
    g_proxy.health = -1;
    g_proxy.settleAt = now + 1.5;
    Call proxyFaction(pawn, "GetFactionType");
    Log("enemies: guest stand-in %s player faction=%u set=%d now=%d brain stopped=%d", Name(pawn).c_str(), playerFaction,
        factioned, proxyFaction.invoke() ? proxyFaction.get<uint8_t>("ReturnValue") : 255, stopped);
    return true;
}

bool IsGuestProxy(UObject* pawn) { return pawn && pawn == g_proxy.pawn; }

void SendGuestStrike(float damage, double now) {
    if (!++g_proxy.sequence) ++g_proxy.sequence;
    auto packet = EncodeGuestStrike(g_session, g_proxy.sequence, std::min(damage, kEnemyHitMaxDamage));
    g_net->SendRaw(&packet, sizeof packet);
    g_proxy.resend.push_back({packet, now + 0.1});
}

void UpdateGuestProxy(double now) {
    for (auto it = g_proxy.resend.begin(); it != g_proxy.resend.end();) {
        if (now < it->at) { ++it; continue; }
        g_net->SendRaw(&it->packet, sizeof it->packet);
        it = g_proxy.resend.erase(it);
    }
    if (!ProxyWanted(now)) {
        if (g_proxy.pawn) DestroyProxy("partner unavailable");
        return;
    }
    Call where(g_avatar.actor, "K2_GetActorLocation");
    if (!where.invoke()) return;
    FVector at = where.get<FVector>("ReturnValue");
    at.z += 95; // the avatar stands on its feet; the pawn is placed by its capsule centre
    if (!IsLive(g_proxy.pawn)) {
        g_proxy.pawn = g_proxy.controller = nullptr;
        if (now >= g_proxy.nextSpawn) SpawnProxy(at, now);
        return;
    }
    if (!Alive(g_proxy.pawn)) {
        DestroyProxy("it died");
        return;
    }
    Call move(g_proxy.pawn, "K2_SetActorLocation");
    move.set("NewLocation", at).setBool("bSweep", false).invoke();
    float health = 0;
    if (now >= g_proxy.settleAt && GetProp(g_proxy.pawn, "Health", health) && std::isfinite(health)) {
        if (g_proxy.health <= 0) {
            g_proxy.health = health;
            Log("enemies: guest stand-in health settled at %.1f", health);
        } else if (health < g_proxy.health - 0.01f) {
            float damage = g_proxy.health - health;
            if (damage <= kProxyMaxStrike) {
                SendGuestStrike(damage, now);
                Log("enemies: guest stand-in took %.1f; forwarded to the partner", damage);
            } else Log("enemies: guest stand-in health jumped by %.1f; not forwarded", damage);
            WriteFloatProp(g_proxy.pawn, "Health", g_proxy.health);
        }
    }
    if (now >= g_proxy.nextAggro) {
        // Enemies only notice the stand-in on their own when they see it; nearer ones are told about it.
        g_proxy.nextAggro = now + 0.5;
        auto Distance = [](FVector a, FVector b) {
            return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        };
        FVector hostAt{};
        Call self(g_pawn, "K2_GetActorLocation");
        if (self.invoke()) hostAt = self.get<FVector>("ReturnValue");
        for (const auto& e : g_enemySources) {
            if (!IsLive(e.pawn) || !Alive(e.pawn)) continue;
            Call loc(e.pawn, "K2_GetActorLocation");
            if (!loc.invoke()) continue;
            FVector p = loc.get<FVector>("ReturnValue");
            if (Distance(p, at) >= Distance(p, hostAt) || Distance(p, at) > 4000) continue;
            auto controller = Prop(e.pawn, "Controller");
            Call aware(controller, "SetAwarenessOfPawn"), faction(controller, "SetFactionValueOfPawn");
            aware.set("awarePawn", g_proxy.pawn).set("AwarenessLevel", 1.f).invoke();
            faction.set("targetPawn", g_proxy.pawn).set("Value", 1.f).invoke();
        }
    }
    if (now >= g_proxy.nextReport) {
        g_proxy.nextReport = now + 15;
        int onProxy = 0, onHost = 0, total = 0;
        for (const auto& e : g_enemySources) {
            auto controller = Prop(e.pawn, "Controller");
            Call target(controller, "GetCurrentTargetPawn");
            if (!target.invoke()) continue;
            ++total;
            auto t = target.get<UObject*>("ReturnValue");
            onProxy += t == g_proxy.pawn;
            onHost += t == g_pawn;
        }
        if (total) Log("enemies: %d streamed enemies target the partner %d, the host %d", total, onProxy, onHost);
    }
}

// Freaker swings at the stand-in play in full but their hit test only damages the player, so the swing
// itself (the MeleeArmed anim notify) is what gets forwarded when it comes from an enemy within reach.
constexpr float kProxySwingReach = 260.f;
float SwingDamage(UObject* pawn) {
    std::string cls = Name(pawn->cls);
    if (cls.find("Swarmer") != std::string::npos || cls.find("Newt") != std::string::npos) return 12.f;
    if (cls.find("Breaker") != std::string::npos || cls.find("Reacher") != std::string::npos) return 35.f;
    if (cls.find("Wolf") != std::string::npos || cls.find("Rager") != std::string::npos) return 20.f;
    return 15.f;
}

void OnEnemySwing(UObject* notify, UObject* fn, void* parms) {
    static int begin = FindNameIndex("Received_NotifyBegin");
    static UObject* armedClass = nullptr;
    if (!parms || fn->name.index != begin || !IsLive(notify) || !IsHost()) return;
    if (notify->cls != armedClass) {
        if (armedClass || Name(notify->cls) != "AnimNotify_MeleeArmed_C") return;
        armedClass = notify->cls;
    }
    auto mesh = *static_cast<UObject**>(parms);
    Call owner(mesh, "GetOwner");
    if (!IsLive(mesh) || !owner.invoke()) return;
    auto pawn = owner.get<UObject*>("ReturnValue");
    if (!IsLive(pawn) || pawn == g_proxy.pawn || pawn == g_pawn) return;
    Call target(Prop(pawn, "Controller"), "GetCurrentTargetPawn");
    if (!target.invoke()) return;
    auto victim = target.get<UObject*>("ReturnValue");
    if (victim != g_proxy.pawn || !IsLive(g_proxy.pawn)) return;
    Call a(pawn, "K2_GetActorLocation"), b(g_proxy.pawn, "K2_GetActorLocation");
    if (!a.invoke() || !b.invoke()) return;
    FVector p = a.get<FVector>("ReturnValue"), q = b.get<FVector>("ReturnValue");
    float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance > kProxySwingReach) return;
    float damage = SwingDamage(pawn);
    SendGuestStrike(damage, Now());
    Log("enemies: %s swung at the partner from %.0f cm; forwarded %.0f", Name(pawn).c_str(), distance, damage);
}

void ReceiveGuestStrike(const char* data, int len) {
    GuestStrikePacket strike;
    if (!DecodeGuestStrike(data, len, strike) || strike.session != g_peer.session || !g_peer.session) {
        ++g_enemyStats.rejected;
        return;
    }
    if (IsHost() || !g_pawn || !Alive(g_pawn) || !g_proxy.dedupe.Accept(strike.session, strike.sequence)) return;
    float before = 0, after = 0;
    GetProp(g_pawn, "Health", before);
    Call damage(C.statics, "ApplyDamage");
    if (!damage.set("DamagedActor", g_pawn).set("BaseDamage", strike.damage)
            .set("EventInstigator", static_cast<UObject*>(nullptr)).set("DamageCauser", static_cast<UObject*>(nullptr))
            .set("DamageTypeClass", FindClass("DamageType")).invoke()) {
        Log("enemies: ApplyDamage failed for a host enemy strike");
        return;
    }
    GetProp(g_pawn, "Health", after);
    Log("enemies: host enemy hit you for %.1f: health %.1f -> %.1f", strike.damage, before, after);
}

// While the host streams enemies, the guest's own hostile AI is removed and its spawners are paused, so
// the only enemies the guest sees are the host's. Spawners resume when the session ends.
struct LocalAISuppression {
    int32_t cursor = 0;
    bool active = false, masksLogged = false;
    // class -> 0 other, 1 AI pawn, 2 spawn area, 3 crowd generator, 4 ambient generator
    std::unordered_map<UObject*, uint8_t> kinds;
    std::vector<UObject*> areas, generators, ambient;
    std::unordered_map<UObject*, double> despawned; // controller -> last attempt
    uint64_t removed = 0;
    double time = 0;
} g_suppress;

bool SuppressionWanted(double now) {
    return g_cfg.hostSave && !IsHost() && g_pawn && AcceptEnemies(now) && (g_peer.flags & kSyncEnemies);
}

void ReleaseLocalAI() {
    if (!g_suppress.active) return;
    for (auto a : g_suppress.areas)
        if (IsLive(a)) Call(a, "SetAreaActive").setBool("activeArea", true).invoke();
    for (auto g : g_suppress.generators)
        if (IsLive(g)) Call(g, "SetPaused").setBool("bPaused", false).invoke();
    for (auto g : g_suppress.ambient)
        if (IsLive(g)) Call(g, "ResetGlobalSpawnLimit").invoke();
    Log("enemies: local AI resumed (%d spawn areas, %d crowd generators, %d ambient generators)",
        static_cast<int>(g_suppress.areas.size()), static_cast<int>(g_suppress.generators.size()),
        static_cast<int>(g_suppress.ambient.size()));
    g_suppress = LocalAISuppression{};
}

template <typename T> bool Remember(std::vector<T>& list, T item) {
    if (std::find(list.begin(), list.end(), item) != list.end()) return false;
    list.push_back(item);
    return true;
}

// Returns a live AI pawn to its spawner the way the game does when the player rides away.
void DespawnLocalAI(UObject* pawn, double now) {
    auto controller = Prop(pawn, "Controller");
    if (!IsLive(controller) || !Alive(pawn)) return;
    bool pooled = false, inUse = true;
    uint8_t pooledMask = 0, inUseMask = 0;
    bool read = GetBoolProp(controller, "bPooledSpawn", pooled, &pooledMask) &&
        GetBoolProp(controller, "bPooledSpawnInUse", inUse, &inUseMask);
    if (!g_suppress.masksLogged) {
        g_suppress.masksLogged = true;
        Log("enemies: pool flags read=%d masks 0x%02x/0x%02x", read, pooledMask, inUseMask);
    }
    if (read && pooled && !inUse) return; // parked in the pool, not in the world
    if (!SharedEnemy(pawn, g_pawn)) return;
    auto [last, first] = g_suppress.despawned.try_emplace(controller, -100.0);
    if (now - last->second < 5) return;
    last->second = now;
    bool ok = false;
    for (auto generator : g_suppress.ambient) {
        if (!IsLive(generator)) continue;
        ok = Call(generator, "DespawnUnit").set("Controller", controller).setBool("notifyDespawn", true)
            .setBool("runFullDespawn", true).invoke();
        if (ok) break;
    }
    if (!ok) ok = Call(controller, "Despawn").setBool("notifyDespawn", true).invoke();
    ++g_suppress.removed;
    if (first) Log("enemies: despawned local %s%s%s", Name(pawn).c_str(), pooled ? " (pooled)" : "", ok ? "" : " FAILED");
}

void SuppressLocalAI(double now) {
    if (!SuppressionWanted(now)) {
        ReleaseLocalAI();
        return;
    }
    static UObject* aiClass = FindClass("BendAIPawn");
    static UObject* areaClass = FindClass("BendAmbientSpawnArea");
    static UObject* crowdClass = FindClass("BendCrowdGenerator");
    static UObject* ambientClass = FindClass("BendAmbientGenerator");
    if (!g_suppress.active) {
        g_suppress.active = true;
        Log("enemies: the host owns enemies; despawning local hostile AI");
    }
    double start = Now();
    int32_t count = ObjectCount();
    int32_t end = std::min(count, g_suppress.cursor + std::max<int32_t>(2048, count / 60));
    for (int32_t i = g_suppress.cursor; i < end; ++i) {
        auto o = ObjectAt(i);
        if (!o || !o->cls || !o->outer || IsDefaultOrArchetype(o)) continue;
        auto [kind, added] = g_suppress.kinds.try_emplace(o->cls, 0);
        if (added) {
            kind->second = ClassIsA(o->cls, aiClass) ? 1 : ClassIsA(o->cls, areaClass) ? 2 :
                ClassIsA(o->cls, crowdClass) ? 3 : ClassIsA(o->cls, ambientClass) ? 4 : 0;
        }
        if (!kind->second || !IsLive(o)) continue;
        switch (kind->second) {
        case 1: DespawnLocalAI(o, now); break;
        case 2:
            if (Remember(g_suppress.areas, o)) Call(o, "SetAreaActive").setBool("activeArea", false).invoke();
            break;
        case 3:
            if (Remember(g_suppress.generators, o)) {
                Call(o, "SetPaused").setBool("bPaused", true).invoke();
                Call(o, "DestroyGeneratedCrowdMembers").invoke();
            }
            break;
        case 4:
            if (Remember(g_suppress.ambient, o)) {
                bool limited = Call(o, "OverrideGlobalSpawnLimitNEW").set("newLimit", 0).invoke();
                bool cleared = Call(o, "ForceDespawnAll").setBool("resetSpawnedCount", false)
                    .setBool("resetToKilled", false).setBool("Delay", false).invoke();
                Log("enemies: ambient generator %s spawn limit 0=%d despawn all=%d", Name(o).c_str(), limited, cleared);
            }
            break;
        }
    }
    g_suppress.cursor = end >= count ? 0 : end;
    g_suppress.time += Now() - start;
}

bool OwnWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void TeleportToPartner(UObject* pawn, double now) {
    if (g_debug.pawn) {
        DebugError("End the debug test session before teleporting to a partner");
        return;
    }
    if (!pawn || g_motion.Empty() || now - g_lastRecv > kPeerTimeout || !EndPassenger(false)) return;
    const auto& s = g_motion.Latest();
    if (!(s.flags & kHasPawn)) return;
    Call riding(pawn, "IsInVehicle");
    if (riding.invoke() && riding.get<bool>("ReturnValue")) {
        g_settingsError = "Dismount before teleporting";
        return;
    }
    FVector origin = WorldOrigin(pawn);
    float yaw = s.yaw * 3.14159265f / 180.f;
    FVector dest{s.pos.x - origin.x + std::cos(yaw) * 150.f, s.pos.y - origin.y + std::sin(yaw) * 150.f,
                 s.pos.z - origin.z + 120.f};
    if (!Call(pawn, "K2_SetActorLocation").set("NewLocation", dest).setBool("bSweep", false)
            .setBool("bTeleport", true).invoke()) {
        g_settingsError = "Teleport failed; check the plugin log";
        return;
    }
    Log("teleported to partner %s", g_remoteName.c_str());
}

void HandleTeleport(UObject* pawn, double now) {
    bool down = OwnWindowFocused() && (GetAsyncKeyState(g_cfg.teleportKey) & 0x8000);
    bool pressed = down && !g_teleportHeld;
    g_teleportHeld = down;
    if (pressed) TeleportToPartner(pawn, now);
}

bool InGameplay(UObject* pawn) {
    if (!pawn) return false;
    Call mode(C.statics, "GetGameMode");
    if (!mode.set("WorldContextObject", pawn).invoke()) return false;
    auto gameMode = mode.get<UObject*>("ReturnValue");
    uint8_t menu = 0;
    Call loading(gameMode, "GetIsShowingLoadingScreenFromSaveGameLoad");
    if (!IsLive(gameMode) || !GetProp(gameMode, "bIsMainMenuActive", menu) || menu > 1 || !loading.invoke()) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Log("gameplay: could not verify game/menu/loading state; gameplay features blocked");
        }
        g_settingsError = "Game state unavailable; gameplay features are blocked";
        return false;
    }
    return !menu && !loading.get<bool>("ReturnValue");
}

double g_lastHudTick = -1;

bool PartnerConnected(double now) {
    return now - g_lastRecv < kPeerTimeout;
}

std::string PartnerName() {
    std::string steamName = g_kind == session::Kind::Steam ? g_steam.PartnerName() : std::string();
    return !steamName.empty() ? steamName : g_remoteName.empty() ? "Your partner" : g_remoteName;
}

// Forgets the current partner and everything replicated from them.
void ResetPartner() {
    if (g_passenger.pawn) EndPassenger(true);
    DestroyAvatar();
    DestroyBike();
    DestroyEnemyReplicas();
    g_motion.Clear();
    g_bodyPoses.Clear();
    g_bikePoses.Clear();
    g_lastRecv = -100;
    g_peer = {};
    g_remoteName.clear();
    if (!++g_consent) ++g_consent;
}

void LeaveSession(bool announce) {
    if (g_kind == session::Kind::None && !g_steam.InLobby()) return;
    std::string partner = std::move(g_announcedPartner);
    g_announcedPartner.clear();
    if (g_steam.InLobby()) g_steam.Leave();
    else if (announce && g_kind != session::Kind::Steam) g_events.Add("Left the LAN session");
    if (!partner.empty()) g_events.Add(partner + " disconnected");
    g_udp.SetIdle();
    g_kind = session::Kind::None;
    g_net = &g_udp;
    ResetPartner();
}

void HostSteamSession() {
    if (!g_steam.Available() || g_steam.Busy()) return;
    LeaveSession(true);
    g_steam.Host(g_steam.Visibility());
    g_kind = session::Kind::Steam;
    g_net = &g_steam;
}

void JoinSteamSession(uint64_t lobby) {
    if (!g_steam.Available() || g_steam.Busy() || !lobby) return;
    LeaveSession(true);
    g_steam.Join(lobby);
    g_kind = session::Kind::Steam;
    g_net = &g_steam;
}

void HostLanSession() {
    if (!g_udpOpen) {
        g_events.Add("LAN unavailable: UDP port " + std::to_string(g_cfg.port) + " is in use");
        return;
    }
    LeaveSession(true);
    g_udp.Listen();
    g_kind = session::Kind::LanHost;
    g_net = &g_udp;
    g_events.Add("Hosting a LAN game on port " + std::to_string(g_udp.Port()));
}

void JoinLanSession(const session::LanHost& host) {
    if (!g_udpOpen) return;
    LeaveSession(true);
    g_udp.Connect(host.address, host.port);
    g_kind = session::Kind::LanJoin;
    g_net = &g_udp;
    g_events.Add("Connecting to " + host.name + "...");
}

// Follows Steam when it enters or loses a lobby on its own (overlay invites, +connect_lobby, kicks).
void SyncSessionKind() {
    bool steamActive = g_steam.InLobby() || g_steam.Joining();
    if (steamActive && g_kind != session::Kind::Steam) {
        Log("network: switching to the Steam lobby");
        if (g_kind != session::Kind::None) ResetPartner();
        g_udp.SetIdle();
        g_kind = session::Kind::Steam;
        g_net = &g_steam;
    } else if (g_kind == session::Kind::Steam && !steamActive && !g_steam.Busy()) {
        g_kind = session::Kind::None;
        g_net = &g_udp;
        ResetPartner();
    }
}

void SendBeacon(double now) {
    static double next = 0;
    if (g_kind != session::Kind::LanHost || now < next) return;
    next = now + 1;
    session::Beacon beacon;
    beacon.port = g_udp.Port();
    beacon.session = g_session;
    beacon.open = PartnerConnected(now) ? 0 : 1;
    std::snprintf(beacon.name, sizeof beacon.name, "%s", session::TruncateUtf8(LocalName(), sizeof beacon.name - 1).c_str());
    g_udp.Broadcast(&beacon, sizeof beacon,
                    {static_cast<uint16_t>(g_cfg.port), static_cast<uint16_t>(g_cfg.peerPort), static_cast<uint16_t>(27777)});
}

std::string LanHostLabel(const session::LanHost& host) {
    in_addr address{};
    address.s_addr = host.address;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &address, ip, sizeof ip);
    return host.name + " (" + ip + ")";
}

session::View SessionView(double now) {
    session::View view;
    view.inGame = g_pawn != nullptr;
    view.steamAvailable = g_steam.Available();
    view.kind = g_kind;
    view.host = IsHost();
    view.busy = g_steam.Busy() || g_steam.Searching();
    view.visibility = g_steam.Visibility();
    view.connected = PartnerConnected(now);
    view.partner = PartnerName();
    for (const auto& lobby : g_steam.Friends()) view.friends.push_back(lobby.name);
    for (const auto& lobby : g_steam.PublicLobbies()) view.publicLobbies.push_back(lobby.name);
    for (const auto& host : g_lan.Hosts()) view.lanHosts.push_back(LanHostLabel(host));
    return view;
}

void RunMenuCommand(const ui::Command& command) {
    using session::Action;
    switch (command.action) {
    case Action::HostSteam: HostSteamSession(); break;
    case Action::HostLan: HostLanSession(); break;
    case Action::BrowseFriends: break;
    case Action::BrowsePublic: g_steam.RequestPublic(); break;
    case Action::BrowseLan:
        g_lan.Clear();
        if (g_kind == session::Kind::None) g_udp.Browse();
        break;
    case Action::JoinFriend:
        if (command.index < g_steam.Friends().size()) JoinSteamSession(g_steam.Friends()[command.index].lobby);
        break;
    case Action::JoinPublic:
        if (command.index < g_steam.PublicLobbies().size()) JoinSteamSession(g_steam.PublicLobbies()[command.index].lobby);
        break;
    case Action::JoinLan:
        if (command.index < g_lan.Hosts().size()) JoinLanSession(g_lan.Hosts()[command.index]);
        break;
    case Action::Invite: g_steam.Invite(); break;
    case Action::CycleVisibility: {
        auto next = session::NextVisibility(g_steam.Visibility());
        g_steam.SetVisibility(next);
        static const wchar_t* const kIniNames[] = {L"private", L"friends", L"public"};
        if (!WritePrivateProfileStringW(L"Coop", L"Visibility", kIniNames[static_cast<int>(g_steam.Visibility())],
                                       (g_basePath + L".ini").c_str()))
            g_events.Add("Could not save lobby visibility to the INI");
        break;
    }
    case Action::Leave: LeaveSession(true); break;
    case Action::Refresh:
        if (ui::CurrentPage() == session::Page::Public) g_steam.RequestPublic();
        if (ui::CurrentPage() == session::Page::Lan) g_lan.Clear();
        break;
    default: break;
    }
}

uint64_t g_logged = 0;

void UpdateSession(double now) {
    for (auto& text : g_steam.TakeEvents()) g_events.Add(std::move(text));
    SyncSessionKind();
    g_lan.Prune(now);
    SendBeacon(now);
    auto page = ui::CurrentPage();
    if (page == session::Page::Friends) g_steam.RefreshFriends(now);
    if (g_kind == session::Kind::None && g_udp.GetMode() == UdpPeer::Mode::Browse && page != session::Page::Lan)
        g_udp.SetIdle();
    ui::Update(now, SessionView(now), g_events);
    for (const auto& command : ui::TakeCommands()) RunMenuCommand(command);
    // Session events also appear as native HUD notifications during gameplay.
    if (now < g_notifyRetryAt) return;
    for (const auto& entry : g_events.Since(g_notified)) {
        if (entry.serial > g_logged) {
            g_logged = entry.serial;
            Log("session event: %s", entry.text.c_str());
        }
        if (g_pawn && !ui::MenuOpen() && !ui::Notify("CO-OP", entry.text)) {
            g_notifyRetryAt = now + 1;
            break;
        }
        g_notified = entry.serial;
    }
}

bool IsMissionDiscoveryName(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const char* token : {"mission", "objective", "checkpoint", "savegame", "loadgame", "playerprofile"})
        if (lower.find(token) != std::string::npos) return true;
    return false;
}

void TraceMissionEvent(UObject* obj, UObject* fn) {
    if (!g_cfg.missionDiscovery || !IsLive(obj) || !IsLive(obj->cls) || !IsLive(fn)) return;
    auto relevantName = [](int32_t index) {
        static std::unordered_map<int32_t, bool> relevantNames;
        auto found = relevantNames.find(index);
        if (found != relevantNames.end()) return found->second;
        bool relevant = IsMissionDiscoveryName(NameString(index));
        relevantNames.emplace(index, relevant);
        return relevant;
    };
    if (!relevantName(obj->cls->name.index) && !relevantName(fn->name.index)) return;
    std::string className = Name(obj->cls), functionName = Name(fn);

    std::string key = className + "." + functionName;
    static std::unordered_set<std::string> emitted;
    static bool limitReported = false;
    if (emitted.contains(key)) return;
    if (emitted.size() >= 512) {
        if (!limitReported) {
            Log("mission discovery: stopped after 512 unique signatures");
            limitReported = true;
        }
        return;
    }
    emitted.insert(key);

    auto function = reinterpret_cast<const uint8_t*>(fn);
    uint16_t paramsSize = *reinterpret_cast<const uint16_t*>(function + off::FunctionParmsSize);
    std::string signature = className + "." + functionName + " parms=" + std::to_string(paramsSize);
    auto parameter = *reinterpret_cast<UObject* const*>(function + off::StructChildren);
    size_t parameterCount = 0;
    for (; parameter;) {
        if (++parameterCount > 64) {
            signature += " [parameter list truncated]";
            break;
        }
        if (!IsLive(parameter) || !IsLive(parameter->cls)) {
            signature += " [invalid parameter metadata]";
            break;
        }
        std::string propertyType = Name(parameter->cls);
        int offset = -1, size = 0;
        if (propertyType.ends_with("Property")) {
            offset = PropertyOffset(parameter);
            size = *reinterpret_cast<const int32_t*>(
                reinterpret_cast<const uint8_t*>(parameter) + off::PropertyElementSize);
        }
        signature += " [" + Name(parameter) + ":" + propertyType + "@" +
                     std::to_string(offset) + "+" + std::to_string(size) + "]";
        parameter = *reinterpret_cast<UObject* const*>(
            reinterpret_cast<const uint8_t*>(parameter) + off::FieldNext);
    }
    Log("mission discovery: %s", signature.c_str());
}

bool g_teleportAfterLoad = false;

// A guest in gameplay switches to the host's save as soon as it is installed: the newest save in the
// sandbox is the host's, so an in-place reload loads it. Retried while the pins say it has not loaded.
void LoadAdoptedSave(double now) {
    static double nextAttempt = 0;
    if (!SaveGuardPinned() || !IsLive(g_pawn) || g_saveLoading || now < nextAttempt) return;
    if (auto manager = FindWorldManager()) {
        Call cinematic(manager, "GetIsCinematicActive");
        if (cinematic.invoke() && cinematic.get<bool>("ReturnValue")) return;
    }
    nextAttempt = now + 30;
    ui::Notify("Theys Gone", "Loading the host's save");
    if (!ReloadNewestSave()) Log("save transfer: cannot reload in place; quit to the main menu and Continue");
}

bool CinematicActive() {
    auto manager = FindWorldManager();
    if (!manager) return false;
    Call cinematic(manager, "GetIsCinematicActive");
    return cinematic.invoke() && cinematic.get<bool>("ReturnValue");
}

// Story progress: every job the job manager knows (story and side jobs, in the game's order) with its
// completion, activity and state, plus active or unlocked story missions and the episode. Job states
// change when a job starts, ends or is unlocked, not at each objective, so mid-mission checkpoints of the
// host do not reload the guest.
StorySignature ReadStory() {
    struct Array { UObject** data; int32_t count, capacity; };
    auto jobs = FindLive("BendJobManager");
    auto mission = FindLive("BendGameMission");
    if (!IsLive(jobs) || !IsLive(mission)) return {};
    Array refs{};
    if (!GetProp(jobs, "JobRefsInOrder", refs) || refs.count <= 0 || refs.count > 4096 || !refs.data) return {};
    StoryHasher hasher;
    for (int32_t i = 0; i < refs.count; ++i) {
        UObject* job = refs.data[i];
        if (!IsLive(job)) continue;
        Call done(jobs, "GetIsJobComplete"), active(jobs, "GetIsJobActive");
        done.set("JobID", job);
        active.set("JobID", job);
        uint8_t state = 0;
        GetProp(job, "JobCurrentState", state);
        bool isActive = active.invoke() && active.get<bool>("ReturnValue");
        hasher.Add(Name(job), done.invoke() && done.get<bool>("ReturnValue"), static_cast<uint8_t>(state | (isActive ? 0x80 : 0)));
    }
    Call episode(mission, "GetCurrentEpisode"), datas(mission, "GetAllActiveOrUnlockedMissionDatas");
    if (!episode.invoke()) return {};
    if (datas.invoke()) {
        auto list = datas.get<Array>("ReturnValue");
        std::vector<std::pair<std::string, bool>> missions;
        for (int32_t i = 0; i < list.count && list.count < 4096 && list.data; ++i) {
            UObject* data = list.data[i];
            if (!IsLive(data)) continue;
            Call completed(mission, "GetIsMissionCompleted");
            completed.set("MissionData", data);
            missions.emplace_back(Name(data), completed.invoke() && completed.get<bool>("ReturnValue"));
        }
        std::sort(missions.begin(), missions.end());
        for (const auto& [name, done] : missions) hasher.Add(name, done);
    }
    return hasher.Finish(episode.get<uint8_t>("ReturnValue"));
}

// Research: what the job manager exposes about story progress.
void ProbeStory() {
    struct Array { UObject** data; int32_t count, capacity; };
    auto jobs = FindLive("BendJobManager");
    Log("story probe: job manager %s", jobs ? FullName(jobs).c_str() : "(none)");
    if (!jobs) return;
    auto describe = [&](const char* label, const Array& list) {
        int live = 0, complete = 0, active = 0;
        std::string sample;
        for (int32_t i = 0; i < list.count && list.count < 4096; ++i) {
            UObject* job = list.data[i];
            if (!IsLive(job)) continue;
            ++live;
            Call done(jobs, "GetIsJobComplete"), on(jobs, "GetIsJobActive");
            done.set("JobID", job);
            on.set("JobID", job);
            bool isDone = done.invoke() && done.get<bool>("ReturnValue");
            bool isOn = on.invoke() && on.get<bool>("ReturnValue");
            complete += isDone;
            active += isOn;
            if (live <= 12) sample += " " + Name(job) + (isDone ? "+" : "") + (isOn ? "*" : "");
        }
        Log("story probe: %s count=%d live=%d complete=%d active=%d:%s", label, list.count, live, complete, active,
            sample.c_str());
    };
    for (const char* name : {"JobRefsInOrder", "ActiveJobRefsInOrder", "PausedJobRefsInOrder", "WaitingOnTimeJobRefs"}) {
        Array list{};
        if (GetProp(jobs, name, list)) describe(name, list);
    }
    Call datas(jobs, "GetAllActiveJobDatas");
    if (datas.invoke()) describe("GetAllActiveJobDatas", datas.get<Array>("ReturnValue"));
    UObject* tracked = nullptr;
    GetProp(jobs, "CurrentTrackedJob", tracked);
    Log("story probe: tracked job %s", IsLive(tracked) ? FullName(tracked).c_str() : "(none)");
    if (auto mission = FindLive("BendGameMission")) {
        Call current(mission, "GetCurrentTrackedMissionOrJob"), story(mission, "GetStoryLineData");
        if (current.invoke()) {
            auto object = current.get<UObject*>("ReturnValue");
            Log("story probe: tracked mission or job %s", IsLive(object) ? FullName(object).c_str() : "(none)");
        }
        if (story.invoke()) {
            auto object = story.get<UObject*>("ReturnValue");
            Log("story probe: storyline data %s", IsLive(object) ? FullName(object).c_str() : "(none)");
        }
    }
    auto signature = ReadStory();
    Log("story probe: mission signature %08x (%u of %u, episode %u)", signature.hash, signature.completed,
        signature.unlocked, signature.episode);
}

// Test aid, guest only (its saves are sandboxed): unlock one locked job so this world's progress no
// longer matches the host's and the story resync has to restore it.
bool DivergeStory() {
    struct Array { UObject** data; int32_t count, capacity; };
    auto jobs = FindLive("BendJobManager");
    Array refs{};
    if (!SaveGuardActive() || !IsGuest() || !IsLive(jobs) || !GetProp(jobs, "JobRefsInOrder", refs) || !refs.data) return false;
    for (int32_t i = refs.count - 1; i >= 0 && refs.count < 4096; --i) {
        UObject* job = refs.data[i];
        if (!IsLive(job)) continue;
        Call active(jobs, "GetIsJobActive");
        active.set("JobID", job);
        if (!active.invoke() || active.get<bool>("ReturnValue")) continue;
        Call unlock(jobs, "UnlockJobAtID");
        unlock.set("JobID", job);
        bool ok = unlock.invoke() && unlock.get<bool>("ReturnValue");
        Log("story diverge: UnlockJobAtID %s -> %d", Name(job).c_str(), ok);
        auto signature = ReadStory();
        Log("story diverge: our signature is now %08x", signature.hash);
        if (ok) return true;
    }
    return false;
}

struct StoryState {
    StorySignature mine;
    double sampledAt = -1e9, sentAt = -1e9, mismatchSince = -1;
    uint32_t savedSignature = 0, saveSerial = 0, lastResynced = 0, loggedHost = 0;
    bool savePending = false, haveHost = false;
    StoryPacket host{};
} g_story;

void ReceiveStory(const char* data, int len) {
    StoryPacket packet;
    if (!DecodeStory(data, len, packet) || packet.session != g_peer.session || !g_peer.session) return;
    g_story.host = packet;
    g_story.haveHost = true;
}

void SampleStory(double now) {
    if (now - g_story.sampledAt < kStorySampleInterval) return;
    g_story.sampledAt = now;
    g_story.mine = ReadStory();
}

// Host: publish the progress of its last save. Guest: once that differs from the progress it is
// playing, download the host's newer save and reload into it at the next quiet moment.
void UpdateStory(double now) {
    bool connected = now - g_lastRecv < kPeerTimeout;
    if (g_story.savePending && IsLive(g_pawn) && !g_saveLoading) {
        g_story.savePending = false;
        g_story.sampledAt = -1e9;
        SampleStory(now);
        if (g_story.mine.valid) {
            g_story.savedSignature = g_story.mine.hash;
            ++g_story.saveSerial;
            Log("story: saved progress %08x (%u of %u jobs and missions complete, episode %u)", g_story.mine.hash,
                g_story.mine.completed, g_story.mine.unlocked, g_story.mine.episode);
        }
        SaveTransferInvalidate();
    }
    if (!connected || !g_net || !IsLive(g_pawn)) {
        g_story.mismatchSince = -1;
        return;
    }
    if (!IsGuest()) {
        if (!g_cfg.hostSave || now - g_story.sentAt < kStoryInterval) return;
        SampleStory(now);
        if (!g_story.mine.valid) return;
        g_story.sentAt = now;
        auto packet = EncodeStory(g_session, g_story.mine, g_story.savedSignature, g_story.saveSerial);
        g_net->SendRaw(&packet, sizeof packet);
        return;
    }
    if (!g_story.haveHost || !SaveGuardAdopted() || SaveGuardPinned() || g_saveLoading) return;
    SampleStory(now);
    if (g_story.host.savedSignature != g_story.loggedHost) {
        g_story.loggedHost = g_story.host.savedSignature;
        Log("story: host saved progress %08x (%u of %u complete); ours %08x (%u of %u)", g_story.host.savedSignature,
            g_story.host.completed, g_story.host.unlocked, g_story.mine.hash, g_story.mine.completed, g_story.mine.unlocked);
    }
    if (!StoryNeedsResync(g_story.host, g_story.mine, g_story.lastResynced)) {
        g_story.mismatchSince = -1;
        return;
    }
    if (g_story.mismatchSince < 0) g_story.mismatchSince = now;
    if (now - g_story.mismatchSince < kStoryResyncDelay || CinematicActive()) return;
    if (!SaveTransferResync()) return;
    g_story.lastResynced = g_story.host.savedSignature;
    g_story.mismatchSince = -1;
    Log("story: host progress %08x differs from ours %08x; fetching the host's save", g_story.host.savedSignature,
        g_story.mine.hash);
    ui::Notify("Theys Gone", "Host made progress: syncing to their save");
}

void ObserveSaveEvent(UObject* obj, UObject* fn, void* parms) {
    if (!IsLive(obj) || !IsLive(fn)) return;
    enum Kind : uint8_t { kIgnored, kSaveEvent, kLoadStarted, kLoaded };
    static std::unordered_map<int32_t, Kind> saveNames;
    auto found = saveNames.find(fn->name.index);
    if (found == saveNames.end()) {
        std::string name = Name(fn);
        Kind kind = name == "ReceivePreSaveGameLoaded"  ? kLoadStarted :
                    name == "ReceivePostSaveGameLoaded" ? kLoaded :
                    name.starts_with("OnGameSaved") || name.starts_with("DisplaySaveGameUI") ||
                            name == "ReceivePreCheckpointSave" ? kSaveEvent : kIgnored;
        found = saveNames.emplace(fn->name.index, kind).first;
    }
    if (found->second == kIgnored) return;
    if (!SaveGuardActive()) {
        // Host or solo: our own save is authoritative; remember what it now contains.
        if (found->second == kSaveEvent) g_story.savePending = true;
        return;
    }
    // Both are broadcast to every actor around an in-place load. Only a load that started after the host
    // save was adopted has read it, so only that one may release the pins.
    if (found->second == kLoadStarted) {
        if (!g_saveLoading) {
            g_loadAfterAdopt = SaveGuardAdopted();
            g_loadStartedAt = Now();
        }
        g_saveLoading = true;
        return;
    }
    if (found->second == kLoaded) {
        bool adoptedLoad = g_saveLoading && g_loadAfterAdopt;
        g_saveLoading = g_loadAfterAdopt = false;
        if (adoptedLoad && SaveGuardPinned()) {
            SaveGuardLoaded();
            g_teleportAfterLoad = true;
        }
        return;
    }
    std::string name = Name(fn);
    unsigned type = name.starts_with("OnGameSaved") && parms ? *static_cast<const uint8_t*>(parms) : 255u;
    Log("save guard: game event %s.%s type=%u", Name(obj->cls).c_str(), name.c_str(), type);
}

void Tick(bool hudFrame) {
    double wall = Now();
    if (!hudFrame && (wall - g_lastHudTick < 0.5 || wall - g_lastTick < kTickInterval)) return;
    g_lastTick = wall;
    if (!g_ready) {
        if (!ResolveEngine()) {
            g_failed = true;
            return;
        }
        g_ready = true;
    }
    // Engine real time advances once per frame, unlike the variable moment the HUD event runs.
    static bool engineClock = true;
    float engineTime = 0;
    bool engineValid = false;
    if (engineClock && IsLive(g_controller)) {
        Call realTime(C.statics, "GetRealTimeSeconds");
        engineValid = realTime.set("WorldContextObject", g_controller).invoke();
        if (engineValid) {
            engineTime = realTime.get<float>("ReturnValue");
        } else {
            engineClock = false;
            Log("tick: engine real time unavailable; motion uses the wall clock");
        }
    }
    double now = g_frameClock.Update(wall, engineTime, engineValid);
    g_tickNow = now;
    if (now - g_lastScan >= 1.0 || !IsLive(g_controller)) {
        g_lastScan = now;
        auto controller = FindLocalController();
        if (controller != g_controller) {
            Log("local controller: %s", controller ? FullName(controller).c_str() : "none");
            g_controller = controller;
        }
    }
    auto pawn = g_controller ? Prop(g_controller, "Pawn") : nullptr;
    pawn = IsLive(pawn) && InGameplay(pawn) ? pawn : nullptr;
    if (pawn != g_pawn) {
        Log("local pawn: %s", pawn ? FullName(pawn).c_str() : "none");
        // The avatar may belong to the previous pawn's world; respawn it next to the new one.
        DestroyAvatar();
        DestroyBike();
        DestroyEnemyReplicas();
        g_localBike = nullptr;
        g_localAppearance.Clear();
        ++g_consent;
        g_pawn = pawn;
        g_localSettledAt = now + 5.0;
    }
    if (g_pawn && g_cfg.discovery && g_pawn != g_discoveredPawn) {
        g_discoveredPawn = g_pawn;
        Discover(g_pawn);
    }
    if (g_pawn && g_cfg.debugTools && g_pawn != g_debugDiscoveredPawn) {
        g_debugDiscoveredPawn = g_pawn;
        DiscoverDebug(g_pawn);
    }
    static bool wasAlive = false, wasConnected = false;
    bool alive = g_pawn && Alive(g_pawn), connected = now - g_lastRecv < kPeerTimeout;
    if (alive != wasAlive || (wasConnected && !connected)) ++g_consent;
    if (wasConnected && !connected && !g_announcedPartner.empty()) {
        g_events.Add(g_announcedPartner + " disconnected");
        g_announcedPartner.clear();
        // A LAN host forgets the silent partner so the next player can take the slot.
        if (g_kind == session::Kind::LanHost) g_udp.DropPartner();
    }
    wasAlive = alive;
    wasConnected = connected;
    if (g_net != &g_udp) g_udp.Poll(now);
    g_net->Poll(now);
    if (g_net != &g_steam) g_steam.Poll(now);
    ReceiveRemote(now);
    UpdateSession(now);
    if (g_cfg.menuDiscovery) {
        static bool held = false;
        bool down = OwnWindowFocused() && (GetAsyncKeyState(VK_F6) & 0x8000);
        if (down && !held) DiscoverMenus();
        held = down;
    }
    if (g_cfg.hostSave && IsGuest() && !SaveGuardActive()) SaveGuardEnable();
    SaveTransferTick(TransferContext(now));
    LoadAdoptedSave(now);
    UpdateStory(now);
    static double teleportAt = -1;
    if (g_teleportAfterLoad) {
        g_teleportAfterLoad = false;
        teleportAt = now + 5.0;
    }
    if (teleportAt >= 0 && now >= teleportAt && g_pawn && now >= g_localSettledAt && !g_motion.Empty() &&
        now - g_lastRecv < kPeerTimeout) {
        teleportAt = -1;
        Log("save transfer: moving to the host after loading its save");
        TeleportToPartner(g_pawn, now);
    } else if (teleportAt >= 0 && now > teleportAt + 60) {
        teleportAt = -1;
    }
    UpdateWorldSync(g_pawn, now);
    UpdateDebugTools(now);
    UpdateDamageScope(now);
    if (g_pawn && now - g_lastScan < kTickInterval && now >= g_localSettledAt) FindLocalBike(g_pawn);
    UpdateBike(g_pawn, now);
    UpdatePassenger(now);
    PollMelee();
    if (now >= g_nextSend) {
        g_nextSend += kSendInterval;
        if (g_nextSend <= now) g_nextSend = now + kSendInterval;
        SendLocalState(g_pawn, now);
    }
    UpdateAvatar(g_pawn, now);
    UpdateGear(g_pawn, now);
    SendEnemies(g_pawn, now);
    UpdateEnemyReplicas(g_pawn, now);
    ResendEnemyHits(now);
    SuppressLocalAI(now);
    UpdateGuestProxy(now);
    MenuOnline online;
    if (g_steam.Available()) {
        online.enabled = true;
        if (g_menu.IsOpen()) g_steam.RefreshFriends(now);
        const auto& friends = g_steam.Friends();
        if (g_joinChoice >= friends.size()) g_joinChoice = 0;
        online.lobby = g_steam.Busy() ? "Working..." : g_steam.InLobby() ? "Leave" : "Host";
        online.invite = g_steam.InLobby() ? "Open Steam overlay" : "Host first";
        online.join = friends.empty() ? "No friends in a lobby" :
                      friends.size() == 1 ? friends[0].name : "< " + friends[g_joinChoice].name + " >";
    }
    auto actions = g_menu.Update(g_controller, OwnWindowFocused(), g_settings, online);
    if (online.enabled) {
        const auto& friends = g_steam.Friends();
        if (actions.joinStep && !friends.empty())
            g_joinChoice = (g_joinChoice + friends.size() + actions.joinStep) % friends.size();
        if (actions.lobby) {
            if (g_steam.InLobby()) LeaveSession(true);
            else HostSteamSession();
        }
        if (actions.invite) g_steam.Invite();
        if (actions.join && g_joinChoice < friends.size()) JoinSteamSession(friends[g_joinChoice].lobby);
    }
    if (actions.passenger || actions.teleport || actions.invite) g_menu.Close(g_controller);
    if (actions.passenger) TogglePassenger(now);
    if (actions.teleport) TeleportToPartner(g_pawn, now);
    UpdateRidePrompt(now);
    if (g_menu.IsOpen()) {
        Call viewport(g_controller, "GetViewportSize");
        if (viewport.invoke()) {
            std::string status = now - g_lastRecv < kPeerTimeout ? "Connected: " + PartnerName() :
                                 g_kind == session::Kind::Steam ? g_steam.Status() :
                                 g_kind == session::Kind::LanHost ? "Hosting a LAN game; waiting for a partner" :
                                 g_kind == session::Kind::LanJoin ? "Connecting to the LAN host..." :
                                 "Not in a session: open CO-OP in the title or pause menu";
            if (g_settings.friendlyFire)
                status += g_peer.flags & kFriendlyFire ? " / friendly fire ACTIVE" : " / partner has not enabled friendly fire";
            if (auto transfer = SaveTransferStatus(TransferContext(now)); !transfer.empty()) status += " / " + transfer;
            if (!g_settingsError.empty()) status = g_settingsError;
            g_menu.Draw(viewport.get<int>("SizeX"), viewport.get<int>("SizeY"), g_settings, status, online);
        }
    }
    if (actions.changed) {
        ++g_consent;
        if (!g_consent) ++g_consent;
        SaveSettings();
    }
    static double lastLocalAppearance = -10;
    if (g_pawn && !g_passenger.pawn && now >= g_localSettledAt && now - lastLocalAppearance >= 0.25) {
        lastLocalAppearance = now;
        for (auto component : OwnedMeshes(g_characterMeshes, g_pawn, now)) {
            if (IsLive(component)) g_localAppearance.Apply(component, g_settings);
        }
    }
    if (g_cfg.discovery) {
        static bool held = false;
        bool down = OwnWindowFocused() && (GetAsyncKeyState(VK_F6) & 0x8000);
        if (down && !held && g_pawn) Discover(g_pawn);
        held = down;
    }
    if (!g_menu.IsOpen()) HandleTeleport(g_pawn, now);
    static double profileStart = now, total = 0, peak = 0;
    static uint64_t frames = 0, previousReceived = 0, previousSent = 0;
    double elapsed = (Now() - wall) * 1000;
    total += elapsed;
    peak = std::max(peak, elapsed);
    ++frames;
    if (now - profileStart >= 5) {
        double seconds = now - profileStart;
        Log("perf: updates=%.1f/s tick mean=%.2fms peak=%.2fms send=%.1f/s recv=%.1f/s riding lookup=%.2fms (%llu calls) "
            "buffer=%.0fms extrapolated=%llu held=%llu resets=%llu clock resyncs=%llu wall-fallbacks=%llu",
            frames / seconds, total / frames, peak, (g_seq - previousSent) / seconds,
            (g_receivedStates - previousReceived) / seconds,
            g_ridingLookups ? g_ridingLookupTime * 1000 / g_ridingLookups : 0,
            static_cast<unsigned long long>(g_ridingLookups), g_motion.DelaySeconds() * 1000,
            static_cast<unsigned long long>(g_motion.Stats().extrapolated),
            static_cast<unsigned long long>(g_motion.Stats().held),
            static_cast<unsigned long long>(g_motion.Stats().resets), g_frameClock.Resyncs(),
            g_frameClock.Fallbacks());
        Log("perf: pose sent=%.1f pkt/s %.1f kB/s recv=%.1f pkt/s rejected=%llu read mean=%.3fms avatar=%s bike=%s",
            g_poseStats.sent / seconds, g_poseStats.bytes / seconds / 1024, g_poseStats.received / seconds,
            static_cast<unsigned long long>(g_poseStats.rejected),
            g_poseStats.reads ? g_poseStats.readTime * 1000 / g_poseStats.reads : 0,
            g_avatar.posing ? "streamed" : "clips", g_bike.posing ? "streamed" : IsLive(g_bike.actor) ? "neutral" : "none");
        if (!g_enemySources.empty() || !g_enemyReplicas.empty() || g_enemyStats.rejected)
            Log("perf: enemies streamed=%d replicas=%d sent=%.1f pkt/s %.1f kB/s infos=%.1f/s applied=%.1f/s rejected=%llu read mean=%.3fms scan=%.2fms/s",
                static_cast<int>(g_enemySources.size()), static_cast<int>(g_enemyReplicas.size()), g_enemyStats.sent / seconds,
                g_enemyStats.bytes / seconds / 1024, g_enemyStats.infos / seconds, g_enemyStats.applied / seconds,
                static_cast<unsigned long long>(g_enemyStats.rejected),
                g_enemyStats.reads ? g_enemyStats.readTime * 1000 / g_enemyStats.reads : 0, g_enemyScan.time * 1000 / seconds);
        g_enemyStats = {};
        g_enemyScan.time = 0;
        g_poseStats = {};
        profileStart = now;
        previousSent = g_seq;
        previousReceived = g_receivedStates;
        frames = 0;
        total = peak = g_ridingLookupTime = 0;
        g_ridingLookups = 0;
    }
}

void HookedProcessEvent(UObject* obj, UObject* fn, void* parms) {
    DWORD thread = GetCurrentThreadId(), unset = 0;
    g_gameThread.compare_exchange_strong(unset, thread);
    if (thread != g_gameThread.load()) {
        g.processEvent(obj, fn, parms);
        return;
    }
    if (!g_failed && !g_inTick && GetCurrentThreadId() == g_gameThread) {
        g_inTick = true;
        static int hudTick = 0;
        if (g_ready && !hudTick) hudTick = FindNameIndex("ReceiveDrawHUD");
        bool hudFrame = g_ready && fn && fn->name.index == hudTick && IsLive(g_controller) &&
            obj == Prop(g_controller, "MyHUD");
        if (hudFrame) {
            if (Now() - g_lastHudTick >= 0.5) Log("tick: synchronizing visual updates to the local HUD frame");
            g_lastHudTick = Now();
        }
        Tick(hudFrame);
        g_inTick = false;
    }
    if (g_ready && g_cfg.discovery && !g_inTick && fn) {
        static int hud = FindNameIndex("ReceiveDrawHUD");
        static int shot = FindNameIndex("OnWeaponFired_Event");
        static int melee = FindNameIndex("AnimNotify_MeleeDamage");
        if (fn->name.index == hud || fn->name.index == shot || fn->name.index == melee) {
            static bool hudLogged = false;
            if (fn->name.index != hud || !hudLogged) {
                Log("discovery: event %s on %s", Name(fn).c_str(), FullName(obj).c_str());
                if (fn->name.index == hud) hudLogged = true;
            }
        }
    }
    if (g_ready && GetCurrentThreadId() == g_gameThread) {
        bool wasInTick = g_inTick;
        g_inTick = true;
        HandleAppearanceEvent(obj, fn, parms);
        g_inTick = wasInTick;
    }
    if (g_ready && g_cfg.missionDiscovery && GetCurrentThreadId() == g_gameThread)
        TraceMissionEvent(obj, fn);
    if (g_ready && GetCurrentThreadId() == g_gameThread) ride::OnEvent(obj, fn);
    if (g_ready && GetCurrentThreadId() == g_gameThread && !g_inTick && g_proxy.pawn && fn) {
        g_inTick = true;
        OnEnemySwing(obj, fn, parms);
        g_inTick = false;
    }
    if (g_ready && research::Tracing() && GetCurrentThreadId() == g_gameThread && !g_inTick)
        research::TraceEvent(obj, fn, parms);
    bool ownUi = g_ready && !g_inTick && GetCurrentThreadId() == g_gameThread && IsLive(obj) && IsLive(fn);
    if (ownUi) {
        g_inTick = true;
        bool swallow = ui::BeforeEvent(obj, fn, parms);
        g_inTick = false;
        if (swallow) return;
    }
    if (g_ready && g_cfg.menuDiscovery && !g_inTick && IsLive(obj) && IsLive(fn) &&
        Name(obj->cls) == "UI_MainMenuWidget_C") {
        std::string name = Name(fn);
        if (name == "OnConfirmPressed" || name == "ConfirmSelection" || name == "SetCursorPosition" ||
            name == "SetSelectedPage" || name.starts_with("BndEvt__MenuInteract_MainMenu")) {
            int cursor = -1, page = -1, position = -1;
            GetProp(obj, "CursorPosition", cursor);
            GetProp(obj, "ActiveMenuPageIndex", page);
            ReadEventParam(fn, parms, "Position", position);
            Log("menu discovery: %s cursor=%d page=%d position=%d", name.c_str(), cursor, page, position);
        }
    }
    if (g_ready && g_cfg.hostSave && GetCurrentThreadId() == g_gameThread)
        ObserveSaveEvent(obj, fn, parms);
    g.processEvent(obj, fn, parms);
    if (ownUi && IsLive(obj)) {
        g_inTick = true;
        ui::AfterEvent(obj, fn);
        g_inTick = false;
    }
    if (g_ready && !g_inTick && GetCurrentThreadId() == g_gameThread && fn) {
        g_inTick = true;
        ObserveAttack(obj, fn, parms);
        g_inTick = false;
    }
}

bool InstallHook(uint8_t* target) {
    if (std::memcmp(target, kPrologue, sizeof kPrologue) != 0) {
        Log("FATAL: ProcessEvent prologue mismatch; refusing to hook");
        return false;
    }
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, target, sizeof kPrologue);
    uint8_t jmp[14] = {0xFF, 0x25, 0, 0, 0, 0};
    auto back = reinterpret_cast<uint64_t>(target + sizeof kPrologue);
    std::memcpy(jmp + 6, &back, 8);
    std::memcpy(tramp + sizeof kPrologue, jmp, sizeof jmp);
    g.processEvent = reinterpret_cast<ProcessEventFn>(tramp);

    uint8_t patch[sizeof kPrologue];
    std::memset(patch, 0x90, sizeof patch);
    auto hook = reinterpret_cast<uint64_t>(&HookedProcessEvent);
    std::memcpy(jmp + 6, &hook, 8);
    std::memcpy(patch, jmp, sizeof jmp);
    DWORD old;
    if (!VirtualProtect(target, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(target, patch, sizeof patch);
    VirtualProtect(target, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof patch);
    return true;
}

} // namespace

Config LoadConfig(const std::wstring& ini) {
    Config c;
    c.port = GetPrivateProfileIntW(L"Coop", L"Port", c.port, ini.c_str());
    c.peerHost = IniString(ini, L"PeerHost", "");
    c.peerPort = GetPrivateProfileIntW(L"Coop", L"PeerPort", c.port, ini.c_str());
    c.playerName = IniString(ini, L"PlayerName", c.playerName.c_str());
    c.discovery = GetPrivateProfileIntW(L"Coop", L"Discovery", 0, ini.c_str()) != 0;
    c.missionDiscovery = GetPrivateProfileIntW(L"Coop", L"MissionDiscovery", 0, ini.c_str()) != 0;
    c.menuDiscovery = GetPrivateProfileIntW(L"Coop", L"MenuDiscovery", 0, ini.c_str()) != 0;
    c.hostSave = GetPrivateProfileIntW(L"Coop", L"HostSave", 1, ini.c_str()) != 0;
    c.debugTools = GetPrivateProfileIntW(L"Coop", L"DebugTools", 0, ini.c_str()) != 0;
    c.animIdle = IniString(ini, L"AnimIdle", c.animIdle.c_str());
    c.animWalk = IniString(ini, L"AnimWalk", c.animWalk.c_str());
    c.animRun = IniString(ini, L"AnimRun", c.animRun.c_str());
    c.animSprint = IniString(ini, L"AnimSprint", c.animSprint.c_str());
    c.teleportKey = GetPrivateProfileIntW(L"Coop", L"TeleportKey", c.teleportKey, ini.c_str());
    c.poseRate = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Coop", L"PoseRate", c.poseRate, ini.c_str())), 10, 30);
    c.poseFace = GetPrivateProfileIntW(L"Coop", L"PoseFace", 0, ini.c_str()) != 0;
    std::string transport = IniString(ini, L"Transport", "menu");
    std::transform(transport.begin(), transport.end(), transport.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    c.autoLan = transport == "udp";
    if (!c.autoLan && transport != "menu") Log("config: unknown Transport '%s'; using menu", transport.c_str());
    c.lobbyVisibility = IniString(ini, L"Visibility", c.lobbyVisibility.c_str());
    c.steamPumpCallbacks = GetPrivateProfileIntW(L"Coop", L"SteamPumpCallbacks", 0, ini.c_str()) != 0;
    return c;
}

bool Install(const std::wstring& basePath) {
    g_basePath = basePath;
    LogOpen(basePath + L".log");
    Log("Theys Gone %s starting (protocol v%u)", COOP_VERSION, static_cast<unsigned>(kProtocolVersion));
    g_cfg = LoadConfig(basePath + L".ini");
    g_debugRequests.Initialize(GetPrivateProfileIntW(L"Debug", L"Request", 0, (basePath + L"-debug.ini").c_str()));
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&g_session), sizeof g_session,
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        Log("FATAL: could not generate the network session identifier");
        return false;
    }
    if (!g_session) g_session = 1;
    std::wstring ini = basePath + L".ini";
    auto palette = [&](const wchar_t* key) {
        int value = static_cast<int>(GetPrivateProfileIntW(L"Coop", key, 0, ini.c_str()));
        if (value < 0 || value >= kPaletteCount) {
            Log("settings: invalid palette for %s; using Original", Narrow(std::wstring(key)).c_str());
            return 0;
        }
        return value;
    };
    g_settings.jacket = palette(L"JacketColor");
    g_settings.trousers = palette(L"TrousersColor");
    g_settings.capColor = palette(L"CapColor");
    g_settings.cap = GetPrivateProfileIntW(L"Coop", L"ShowCap", 1, ini.c_str()) != 0;
    g_settings.motorcycles = GetPrivateProfileIntW(L"Coop", L"SyncMotorcycles", 1, ini.c_str()) != 0;
    g_settings.enemies = GetPrivateProfileIntW(L"Coop", L"SyncEnemies", 1, ini.c_str()) != 0;
    g_settings.passengers = GetPrivateProfileIntW(L"Coop", L"AllowPassenger", 1, ini.c_str()) != 0;
    if (g_cfg.port == 0 || g_cfg.port > 65535 || g_cfg.peerPort == 0 || g_cfg.peerPort > 65535) {
        Log("FATAL: Port and PeerPort must be between 1 and 65535");
        return false;
    }
    session::Visibility visibility = session::Visibility::Friends;
    if (!session::ParseVisibility(g_cfg.lobbyVisibility, visibility))
        Log("config: unknown Visibility '%s'; using friends", g_cfg.lobbyVisibility.c_str());
    g_steam.SetVisibility(visibility);
    ui::Configure(g_cfg.menuDiscovery);
    Log("config: transport=%s port=%u peer=%s:%u name=%s visibility=%s", g_cfg.autoLan ? "udp (LAN at launch)" : "menu",
        g_cfg.port, g_cfg.peerHost.empty() ? "(host mode)" : g_cfg.peerHost.c_str(), g_cfg.peerPort,
        g_cfg.playerName.empty() ? "(Steam name)" : g_cfg.playerName.c_str(), session::VisibilityName(visibility));
    if (g_cfg.missionDiscovery)
        Log("mission discovery: read-only signature tracing enabled; parameter values are not logged");

    g.imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g.imageBase);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g.imageBase + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExpectedTimestamp || nt->OptionalHeader.SizeOfImage != kExpectedImageSize) {
        Log("FATAL: unsupported DaysGone.exe (timestamp 0x%lx, image size 0x%lx); mod disabled",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }
    if (g_cfg.hostSave) InstallSaveGuard(g_basePath + L"-guest-saves");
    SaveTransferConfigure(g_cfg.hostSave, g_cfg.hostSave);
    g.objectArray = reinterpret_cast<uint8_t*>(g.imageBase + kObjectArrayRva);
    if (!InstallHook(reinterpret_cast<uint8_t*>(g.imageBase + kProcessEventRva))) return false;
    Log("ProcessEvent hook installed");
    StartOverlay();
    return true;
}

} // namespace coop
