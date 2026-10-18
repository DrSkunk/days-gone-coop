#include "coop.hpp"

#include "log.hpp"
#include "net.hpp"
#include "menu.hpp"
#include "appearance.hpp"
#include "overlay.hpp"
#include "ue.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
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
constexpr double kInterpDelay = 0.1;
constexpr double kPeerTimeout = 5.0;

std::wstring g_basePath;
Config g_cfg;
MenuSettings g_settings;
SettingsMenu g_menu;
DWORD g_gameThread = 0;
bool g_inTick = false;
bool g_failed = false;
bool g_ready = false;
double g_lastTick = 0, g_lastSend = 0, g_lastScan = -10;
UdpPeer g_net;
uint32_t g_seq = 0;
uint32_t g_session = 0, g_consent = 1;
StatePacket g_peer{};
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

struct Classes {
    UObject *playerController, *character, *skelMeshActor, *skelMeshComponent, *animSequence;
    UObject* statics; // GameplayStatics default object
} C{};

UObject* g_controller = nullptr;
UObject* g_pawn = nullptr;
UObject* g_discoveredPawn = nullptr;

struct Snapshot {
    double t;
    FVector pos;
    float yaw;
    FVector vel;
    uint16_t flags;
    FVector bikePos;
    FRotator bikeRotation;
};
std::deque<Snapshot> g_snaps;
std::string g_remoteName;
double g_lastRecv = -100;
double g_lastStatus = -100;
double g_lastSpawnAttempt = -100;
uint32_t g_lastRemoteSeq = 0;

struct Avatar {
    UObject* actor = nullptr;
    UObject* component = nullptr;
    std::vector<UObject*> parts;
    UObject* currentAnim = nullptr;
    bool hidden = false;
} g_avatar;
UObject* g_localMesh = nullptr;
FVector g_localPos{};
double g_localSettledAt = 0;

void DestroyAvatar();
void DestroyBike();
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
    UObject* best = nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || !IsA(o, C.playerController) || IsDefaultOrArchetype(o) || !IsLive(o)) continue;
        auto pawn = Prop(o, "Pawn");
        if (!IsLive(pawn) || Prop(pawn, "Controller") != o) continue;
        if (!best || o->internalIndex > best->internalIndex) best = o;
    }
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
        }
    }
    std::fclose(f);
    Log("discovery: reflection report written to %s", Narrow(path).c_str());
}

void SendLocalState(UObject* pawn, double now) {
    StatePacket p{};
    p.magic = kMagic;
    p.version = kProtocolVersion;
    p.seq = ++g_seq;
    p.timeMs = static_cast<uint32_t>(now * 1000.0);
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
    if (pawn) {
        UObject* mesh = IsA(pawn, C.character) ? Prop(pawn, "Mesh") : nullptr;
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
        Call alive(pawn, "IsAlive");
        if (alive.invoke() && alive.get<bool>("ReturnValue")) p.flags |= kAlive;
        Call mounted(pawn, "IsInVehicle");
        if (mounted.invoke() && mounted.get<bool>("ReturnValue")) p.flags |= kInVehicle;
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
            g_snaps.clear();
            Log("peer %s (%s) connected", p.name, g_net.PeerText().c_str());
            DestroyAvatar();  // Rebuild from the player's current look; also a manual "respawn" lever.
            DestroyBike();
        }
        g_lastRecv = now;
        g_lastRemoteSeq = p.seq;
        g_remoteName = p.name;
        g_peer = p;
        g_snaps.push_back({now, {p.pos[0], p.pos[1], p.pos[2]}, p.yaw, {p.vel[0], p.vel[1], p.vel[2]}, p.flags,
                          {p.bikePos[0], p.bikePos[1], p.bikePos[2]},
                          {p.bikeRotation[0], p.bikeRotation[1], p.bikeRotation[2]}});
        while (g_snaps.size() > 64) g_snaps.pop_front();
    }
}

float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 540.f, 360.f) - 180.f;
    return a + d * t;
}

bool SampleRemote(double now, Snapshot& out) {
    if (g_snaps.empty()) return false;
    double t = now - kInterpDelay;
    if (t <= g_snaps.front().t) {
        out = g_snaps.front();
        return true;
    }
    for (size_t i = 1; i < g_snaps.size(); ++i) {
        const auto &a = g_snaps[i - 1], &b = g_snaps[i];
        if (t > b.t) continue;
        float k = static_cast<float>((t - a.t) / std::max(1e-6, b.t - a.t));
        out = b;
        out.pos = {a.pos.x + (b.pos.x - a.pos.x) * k, a.pos.y + (b.pos.y - a.pos.y) * k,
                   a.pos.z + (b.pos.z - a.pos.z) * k};
        out.yaw = LerpAngle(a.yaw, b.yaw, k);
        out.bikePos = {a.bikePos.x + (b.bikePos.x - a.bikePos.x) * k,
                       a.bikePos.y + (b.bikePos.y - a.bikePos.y) * k,
                       a.bikePos.z + (b.bikePos.z - a.bikePos.z) * k};
        out.bikeRotation = {LerpAngle(a.bikeRotation.pitch, b.bikeRotation.pitch, k),
                            LerpAngle(a.bikeRotation.yaw, b.bikeRotation.yaw, k),
                            LerpAngle(a.bikeRotation.roll, b.bikeRotation.roll, k)};
        return true;
    }
    out = g_snaps.back();
    float dt = static_cast<float>(std::min(0.25, t - out.t));
    out.pos = {out.pos.x + out.vel.x * dt, out.pos.y + out.vel.y * dt, out.pos.z + out.vel.z * dt};
    return true;
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

void DestroyAvatar() {
    for (auto a : g_avatar.parts)
        if (IsLive(a)) Call(a, "K2_DestroyActor").invoke();
    if (IsLive(g_avatar.actor)) Call(g_avatar.actor, "K2_DestroyActor").invoke();
    g_avatar = Avatar{};
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
        if (!Call(destination, "SetMaterial").set("ElementIndex", i).set("Material", material).invoke()) {
            Log("avatar: SetMaterial failed for slot %d", i);
            return false;
        }
    }
    return true;
}

void DestroyBike() {
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
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto component = ObjectAt(i);
        if (!IsLive(component) || component == source || component->outer != g_localBike ||
            !IsA(component, C.skelMeshComponent)) continue;
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
    if (animation) Call(g_bike.component, "PlayAnimation").set("NewAnimToPlay", animation).setBool("bLooping", true).invoke();
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

bool SpawnAvatar(UObject* localPawn, const FVector& at) {
    auto localMesh = Prop(localPawn, "Mesh");
    auto skeletal = Prop(localMesh, "SkeletalMesh");
    if (!skeletal) return false;
    g_avatar.actor = SpawnMeshActor(localPawn, at);
    g_avatar.component = Prop(g_avatar.actor, "SkeletalMeshComponent");
    if (!g_avatar.component) {
        Log("avatar: spawn failed");
        DestroyAvatar();
        return false;
    }
    if (!CopyMeshAppearance(localMesh, g_avatar.component)) {
        DestroyAvatar();
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
        Call(part, "K2_AttachRootComponentToActor")
            .set("InParentActor", g_avatar.actor)
            .set("AttachLocationType", static_cast<uint8_t>(2))
            .invoke();
        Call(comp, "SetMasterPoseComponent").set("NewMasterBoneComponent", g_avatar.component).invoke();
        g_avatar.parts.push_back(part);
        Log("avatar: part %s", Name(partMesh).c_str());
    }
    Log("avatar: spawned %s with mesh %s and %d extra parts", Name(g_avatar.actor).c_str(),
        Name(skeletal).c_str(), static_cast<int>(g_avatar.parts.size()));
    return true;
}

void SetAvatarHidden(bool hidden) {
    if (g_avatar.hidden == hidden) return;
    g_avatar.hidden = hidden;
    Call(g_avatar.actor, "SetActorHiddenInGame").setBool("bNewHidden", hidden).invoke();
    for (auto a : g_avatar.parts) Call(a, "SetActorHiddenInGame").setBool("bNewHidden", hidden).invoke();
}

void AnimateAvatar(const Snapshot& s) {
    float speed = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y);
    int tier = speed < 30 ? 0 : speed < 250 ? 1 : speed < 520 ? 2 : 3;
    UObject* anim = nullptr;
    if (s.flags & kInVehicle)
        anim = FindObject(s.flags & kPassenger ?
            "/Game/Animation/Deacon/bike/bikeBuddy/bike_basic_control_buddy.bike_basic_control_buddy" :
            "/Game/Animation/Deacon/bike/bike_basic_control.bike_basic_control", C.animSequence);
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
        if (!SpawnAvatar(localPawn, local)) return;
    }
    SetAvatarHidden(false);
    Call move(g_avatar.actor, "K2_SetActorLocationAndRotation");
    move.set("NewLocation", local).set("NewRotation", FRotator{0, s.yaw, 0});
    bool moved = move.setBool("bSweep", false).setBool("bTeleport", true).invoke();
    AnimateAvatar(s);
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
            "origin (%.0f %.0f %.0f) anim=%s screen=(%.0f %.0f) visible=%d",
            g_remoteName.c_str(), local.x, local.y, local.z, moved, a.x, a.y, a.z, m.x, m.y, m.z, origin.x, origin.y,
            origin.z, Name(g_avatar.currentAnim).c_str(), screen.x, screen.y, vis);
    }
}

bool OwnWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void HandleTeleport(UObject* pawn, double now) {
    bool down = OwnWindowFocused() && (GetAsyncKeyState(g_cfg.teleportKey) & 0x8000);
    bool pressed = down && !g_teleportHeld;
    g_teleportHeld = down;
    if (!pressed || !pawn || g_snaps.empty() || now - g_lastRecv > kPeerTimeout) return;
    const auto& s = g_snaps.back();
    if (!(s.flags & kHasPawn)) return;
    FVector origin = WorldOrigin(pawn);
    float yaw = s.yaw * 3.14159265f / 180.f;
    // Land beside the partner; actor origin is the capsule centre, roughly 1 m above the feet.
    FVector dest{s.pos.x - origin.x + std::cos(yaw) * 150.f, s.pos.y - origin.y + std::sin(yaw) * 150.f,
                 s.pos.z - origin.z + 120.f};
    Call tp(pawn, "K2_SetActorLocation");
    tp.set("NewLocation", dest).setBool("bSweep", false).setBool("bTeleport", true).invoke();
    Log("teleported to partner %s", g_remoteName.c_str());
}

void Tick() {
    double now = Now();
    if (now - g_lastTick < kTickInterval) return;
    g_lastTick = now;
    if (!g_ready) {
        if (!ResolveEngine()) {
            g_failed = true;
            return;
        }
        g_ready = true;
    }
    if (now - g_lastScan >= 1.0 || !IsLive(g_controller)) {
        g_lastScan = now;
        auto controller = FindLocalController();
        if (controller != g_controller) {
            Log("local controller: %s", controller ? FullName(controller).c_str() : "none");
            g_controller = controller;
        }
    }
    auto pawn = g_controller ? Prop(g_controller, "Pawn") : nullptr;
    pawn = IsLive(pawn) ? pawn : nullptr;
    if (pawn != g_pawn) {
        Log("local pawn: %s", pawn ? FullName(pawn).c_str() : "none");
        // The avatar may belong to the previous pawn's world; respawn it next to the new one.
        DestroyAvatar();
        DestroyBike();
        g_localBike = nullptr;
        g_localAppearance.Clear();
        g_pawn = pawn;
        g_localSettledAt = now + 5.0;
    }
    if (g_pawn && g_cfg.discovery && g_pawn != g_discoveredPawn) {
        g_discoveredPawn = g_pawn;
        Discover(g_pawn);
    }
    ReceiveRemote(now);
    if (g_pawn && now - g_lastScan < kTickInterval && now >= g_localSettledAt) FindLocalBike(g_pawn);
    UpdateBike(g_pawn, now);
    if (now - g_lastSend >= kSendInterval) {
        g_lastSend = now;
        SendLocalState(g_pawn, now);
    }
    UpdateAvatar(g_pawn, now);
    auto actions = g_menu.Update(g_controller, OwnWindowFocused(), g_settings);
    if (g_menu.IsOpen()) {
        Call viewport(g_controller, "GetViewportSize");
        if (viewport.invoke()) {
            std::string status = now - g_lastRecv < kPeerTimeout ? "Connected: " + g_remoteName : "Waiting for a LAN partner";
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
    if (g_pawn && now >= g_localSettledAt && now - lastLocalAppearance >= 0.25) {
        lastLocalAppearance = now;
        for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
            auto component = ObjectAt(i);
            if (IsLive(component) && component->outer == g_pawn && IsA(component, C.skelMeshComponent))
                g_localAppearance.Apply(component, g_settings);
        }
    }
    if (g_cfg.discovery) {
        static bool held = false;
        bool down = OwnWindowFocused() && (GetAsyncKeyState(VK_F6) & 0x8000);
        if (down && !held && g_pawn) Discover(g_pawn);
        held = down;
    }
    if (!g_menu.IsOpen()) HandleTeleport(g_pawn, now);
}

void HookedProcessEvent(UObject* obj, UObject* fn, void* parms) {
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    if (!g_failed && !g_inTick && GetCurrentThreadId() == g_gameThread) {
        g_inTick = true;
        Tick();
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
    g.processEvent(obj, fn, parms);
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
