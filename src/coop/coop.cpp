#include "coop.hpp"

#include "log.hpp"
#include "net.hpp"
#include "menu.hpp"
#include "motion.hpp"
#include "frameclock.hpp"
#include "appearance.hpp"
#include "overlay.hpp"
#include "combat.hpp"
#include "ue.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <vector>

using namespace ue;

namespace coop {

namespace {

constexpr uint8_t kPrologue[19] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                   0x41, 0x57, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00};
constexpr double kTickInterval = 1.0 / 60.0;
constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kPeerTimeout = 5.0;

std::wstring g_basePath;
Config g_cfg;
MenuSettings g_settings;
SettingsMenu g_menu;
std::atomic<DWORD> g_gameThread{0};
bool g_inTick = false;
bool g_failed = false;
bool g_ready = false;
double g_lastTick = 0, g_nextSend = 0, g_lastScan = -10;
UdpPeer g_net;
uint32_t g_seq = 0;
uint32_t g_localShotSequence = 0;
uint32_t g_session = 0, g_consent = 1;
StatePacket g_peer{};
DamageChannel g_damage;
Appearance g_localAppearance, g_remoteAppearance;
UObject* g_localBike = nullptr;
struct BikeAvatar {
    UObject* actor = nullptr;
    UObject* component = nullptr;
    UObject* source = nullptr;
    std::vector<UObject*> parts;
} g_bike;
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
    UObject* statics; // GameplayStatics default object
} C{};

struct OwnedMeshCache {
    UObject* owner = nullptr;
    double refreshed = -10;
    std::vector<UObject*> meshes;
} g_characterMeshes, g_nativeBikeMeshes;

UObject* g_controller = nullptr;
UObject* g_localPlayer = nullptr;
UObject* g_pawn = nullptr;
UObject* g_discoveredPawn = nullptr;

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
void ReceiveDamage(const StatePacket& packet);
void UpdateDamageScope(double now);
bool EndPassenger(bool forced);
bool SpawnAvatar(UObject* localPawn, const FVector& at, Avatar& avatar);
UObject* g_anims[4] = {};
bool g_teleportHeld = false;
bool OwnWindowFocused();
std::string Narrow(const std::wstring& w);
void SaveSettings() {
    std::wstring ini = g_basePath + L".ini";
    const std::pair<const wchar_t*, int> values[] = {{L"JacketColor", g_settings.jacket}, {L"TrousersColor", g_settings.trousers},
        {L"CapColor", g_settings.capColor}, {L"ShowCap", g_settings.cap}, {L"SyncMotorcycles", g_settings.motorcycles},
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
    Log("classes: PlayerController=%p Character=%p SkeletalMeshActor=%p SkeletalMeshComponent=%p "
        "AnimSequence=%p GameplayStatics CDO=%p",
        (void*)C.playerController, (void*)C.character, (void*)C.skelMeshActor, (void*)C.skelMeshComponent,
        (void*)C.animSequence, (void*)C.statics);
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
    if (!g_net.Open(g_cfg.port, g_cfg.peerHost, g_cfg.peerPort, net)) {
        Log("FATAL: network: %s", net.c_str());
        return false;
    }
    Log("network: %s", net.c_str());
    return true;
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

void DumpClass(FILE* file, UObject* cls) {
    for (auto type = cls; type; type = Super(type)) {
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
    const char* terms[] = {"time", "weather", "climate", "daynight", "freaker", "enemy",
                           "infected", "aicontroller", "spawner"};
    auto matches = [&](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto* term : terms)
            if (text.find(term) != std::string::npos) return true;
        return false;
    };
    auto actorClass = FindClass("Actor");
    std::vector<UObject*> emittedClasses;
    int reportedObjects = 0;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto obj = ObjectAt(i);
        if (!IsLive(obj) || IsDefaultOrArchetype(obj) || !IsLive(obj->cls)) continue;
        const std::string className = Name(obj->cls);
        const std::string objectName = Name(obj);
        if (!matches(className) && !matches(objectName)) continue;
        if (reportedObjects < 512) {
            std::fprintf(f, "\nDISCOVERY_MATCH %s class=%s\n", FullName(obj).c_str(), FullName(obj->cls).c_str());
            ++reportedObjects;
        }
        if (actorClass && IsA(obj, actorClass))
            std::fprintf(f, "  ACTOR %s\n", FullName(obj).c_str());
        if (emittedClasses.size() < 64 &&
            std::find(emittedClasses.begin(), emittedClasses.end(), obj->cls) == emittedClasses.end()) {
            emittedClasses.push_back(obj->cls);
            DumpClass(f, obj->cls);
        }
    }
    Log("discovery: wrote %d time/weather/enemy candidate objects and %d candidate classes",
        reportedObjects, static_cast<int>(emittedClasses.size()));
    std::fclose(f);
    Log("discovery: reflection report written to %s", Narrow(path).c_str());
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
    if (g_settings.friendlyFire) p.flags |= kFriendlyFire;
    if (g_settings.motorcycles) p.flags |= kSyncBikes;
    if (g_settings.passengers) p.flags |= kAcceptPassengers;
    std::snprintf(p.name, sizeof p.name, "%s", g_cfg.playerName.c_str());
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
    g_net.Send(p);
}

void ReceiveRemote(double now) {
    StatePacket p;
    for (int budget = 0; budget < 64 && g_net.Receive(p); ++budget) {
        bool reconnect = now - g_lastRecv > kPeerTimeout || p.session != g_peer.session;
        if (!reconnect && !NewerSequence(p.seq, g_lastRemoteSeq)) continue;
        if (reconnect) {
            ++g_consent;
            g_motion.Clear();
            Log("peer %s (%s) connected", p.name, g_net.PeerText().c_str());
            DestroyAvatar();  // Rebuild from the player's current look; also a manual "respawn" lever.
            DestroyBike();
        }
        g_lastRecv = now;
        ++g_receivedStates;
        g_lastRemoteSeq = p.seq;
        g_remoteName = p.name;
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
    std::vector<UObject*> copied{g_bike.source};
    for (auto component : OwnedMeshes(g_nativeBikeMeshes, g_localBike, Now())) {
        if (!IsLive(component) || component == source) continue;
        auto mesh = Prop(component, "SkeletalMesh");
        if (!IsLive(mesh) || std::find(copied.begin(), copied.end(), mesh) != copied.end()) continue;
        auto part = SpawnMeshActor(pawn, at);
        auto destination = Prop(part, "SkeletalMeshComponent");
        if (!IsLive(destination) || !CopyMeshAppearance(component, destination)) {
            if (IsLive(part)) Call(part, "K2_DestroyActor").invoke();
            Log("bike: could not clone part %s", Name(mesh).c_str());
            DestroyBike();
            return false;
        }
        if (!Call(part, "K2_AttachRootComponentToActor").set("InParentActor", g_bike.actor)
                .set("AttachLocationType", static_cast<uint8_t>(2)).invoke() ||
            !Call(destination, "SetMasterPoseComponent").set("NewMasterBoneComponent", g_bike.component).invoke()) {
            Call(part, "K2_DestroyActor").invoke();
            DestroyBike();
            return false;
        }
        copied.push_back(mesh);
        g_bike.parts.push_back(part);
    }
    auto animation = FindObject("/Game/Vehicles/BikeHero/bike_basic_anim.bike_basic_anim", C.animSequence);
    if (!animation || !Call(g_bike.component, "PlayAnimation").set("NewAnimToPlay", animation)
            .setBool("bLooping", true).invoke() || !Call(g_bike.component, "Stop").invoke()) {
        Log("bike: neutral chassis pose unavailable");
        DestroyBike();
        return false;
    }
    Log("bike: cloned chassis and %d modular parts", static_cast<int>(g_bike.parts.size()));
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
    Call(g_bike.actor, "K2_SetActorLocationAndRotation").set("NewLocation", position).set("NewRotation", sample.bikeRotation)
        .setBool("bSweep", false).setBool("bTeleport", true).invoke();
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

void TogglePassenger(double now) {
    if (g_passenger.pawn) { EndPassenger(false); return; }
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
    int32_t minimum = 0, maximum = 0;
    bool read = melee ?
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "MeleeData"}, {"WeaponMeleeData", "MeleeHitDamage"}}, minimum) &&
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "MeleeData"}, {"WeaponMeleeData", "MeleeHitDamageMax"}}, maximum) :
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "CoreData"}, {"WeaponCoreData", "MinHitDamage"}}, minimum) &&
        GetStructField(weapon, "CurWeaponConfig", {{"WeaponData", "CoreData"}, {"WeaponCoreData", "MaxHitDamage"}}, maximum);
    if (!read || minimum <= 0 || maximum < minimum || maximum > 1000) {
        g_settingsError = "Weapon damage data unavailable; attack was not sent";
        Log("combat: invalid native damage range %d..%d for %s", minimum, maximum, FullName(weapon).c_str());
        return;
    }
    float amount = (minimum + maximum) * 0.5f;
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
}

void PollMelee() {
    static UObject* lastPawn = nullptr;
    static bool previousWindow = false, previousAttack = false;
    if (lastPawn != g_pawn) {
        lastPawn = g_pawn;
        previousWindow = previousAttack = false;
    }
    if (!g_pawn || !g_settings.friendlyFire) { previousWindow = false; return; }
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
    if (window && !previousWindow) QueueAttack(true, weapon);
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
    Log("avatar: spawned %s with mesh %s and %d extra parts", Name(avatar.actor).c_str(),
        Name(skeletal).c_str(), static_cast<int>(avatar.parts.size()));
    return true;
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
        g_avatar.pelvis = HipBone(g_avatar.component, names);
        if (!g_avatar.pelvis.index) return false;
    }
    Call seat(g_avatar.seatMesh, "GetSocketLocation"), hip(g_avatar.component, "GetSocketTransform");
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
    AnimateAvatar(s, now);
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
    move.set("NewLocation", local).set("NewRotation", FRotator{0, s.yaw, 0});
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
        Call visible(g_avatar.component, "IsVisible");
        int vis = visible.invoke() ? visible.get<bool>("ReturnValue") : -1;
        Log("status: partner %s at (%.0f %.0f %.0f) moved=%d, avatar at (%.0f %.0f %.0f), you at (%.0f %.0f %.0f) "
            "origin (%.0f %.0f %.0f) anim=%s actions=0x%x weapon=%u shot=%u screen=(%.0f %.0f) visible=%d",
            g_remoteName.c_str(), local.x, local.y, local.z, moved, a.x, a.y, a.z, m.x, m.y, m.z, origin.x, origin.y,
            origin.z, Name(g_avatar.currentAnim).c_str(), s.actions, s.weaponType, s.shotSequence, screen.x, screen.y, vis);
    }
}

bool OwnWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void TeleportToPartner(UObject* pawn, double now) {
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
    static bool wasAlive = false, wasConnected = false;
    bool alive = g_pawn && Alive(g_pawn), connected = now - g_lastRecv < kPeerTimeout;
    if (alive != wasAlive || (wasConnected && !connected)) ++g_consent;
    wasAlive = alive;
    wasConnected = connected;
    ReceiveRemote(now);
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
    auto actions = g_menu.Update(g_controller, OwnWindowFocused(), g_settings);
    if (actions.passenger || actions.teleport) g_menu.Close(g_controller);
    if (actions.passenger) TogglePassenger(now);
    if (actions.teleport) TeleportToPartner(g_pawn, now);
    if (g_menu.IsOpen()) {
        Call viewport(g_controller, "GetViewportSize");
        if (viewport.invoke()) {
            std::string status = now - g_lastRecv < kPeerTimeout ? "Connected: " + g_remoteName : "Waiting for a LAN partner";
            if (g_settings.friendlyFire)
                status += g_peer.flags & kFriendlyFire ? " / friendly fire ACTIVE" : " / partner has not enabled friendly fire";
            if (!g_settingsError.empty()) status = g_settingsError;
            g_menu.Draw(viewport.get<int>("SizeX"), viewport.get<int>("SizeY"), g_settings, status);
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
    g.processEvent(obj, fn, parms);
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
    c.animIdle = IniString(ini, L"AnimIdle", c.animIdle.c_str());
    c.animWalk = IniString(ini, L"AnimWalk", c.animWalk.c_str());
    c.animRun = IniString(ini, L"AnimRun", c.animRun.c_str());
    c.animSprint = IniString(ini, L"AnimSprint", c.animSprint.c_str());
    c.teleportKey = GetPrivateProfileIntW(L"Coop", L"TeleportKey", c.teleportKey, ini.c_str());
    return c;
}

bool Install(const std::wstring& basePath) {
    g_basePath = basePath;
    LogOpen(basePath + L".log");
    Log("Days Gone Co-op starting");
    g_cfg = LoadConfig(basePath + L".ini");
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
    g_settings.passengers = GetPrivateProfileIntW(L"Coop", L"AllowPassenger", 1, ini.c_str()) != 0;
    if (g_cfg.port == 0 || g_cfg.port > 65535 || g_cfg.peerPort == 0 || g_cfg.peerPort > 65535) {
        Log("FATAL: Port and PeerPort must be between 1 and 65535");
        return false;
    }
    Log("config: port=%u peer=%s:%u name=%s", g_cfg.port, g_cfg.peerHost.empty() ? "(host mode)" : g_cfg.peerHost.c_str(),
        g_cfg.peerPort, g_cfg.playerName.c_str());

    g.imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g.imageBase);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g.imageBase + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kExpectedTimestamp || nt->OptionalHeader.SizeOfImage != kExpectedImageSize) {
        Log("FATAL: unsupported DaysGone.exe (timestamp 0x%lx, image size 0x%lx); mod disabled",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }
    g.objectArray = reinterpret_cast<uint8_t*>(g.imageBase + kObjectArrayRva);
    if (!InstallHook(reinterpret_cast<uint8_t*>(g.imageBase + kProcessEventRva))) return false;
    Log("ProcessEvent hook installed");
    StartOverlay();
    return true;
}

} // namespace coop
