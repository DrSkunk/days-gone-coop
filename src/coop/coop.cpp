#include "coop.hpp"

#include "log.hpp"
#include "net.hpp"
#include "ue.hpp"

#include <windows.h>

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
DWORD g_gameThread = 0;
bool g_inTick = false;
bool g_failed = false;
bool g_ready = false;
double g_lastTick = 0, g_lastSend = 0, g_lastScan = -10;
UdpPeer g_net;
uint32_t g_seq = 0;

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
UObject* g_anims[4] = {};
bool g_teleportHeld = false;

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
}

void SendLocalState(UObject* pawn, double now) {
    StatePacket p{};
    p.magic = kMagic;
    p.version = kProtocolVersion;
    p.seq = ++g_seq;
    p.timeMs = static_cast<uint32_t>(now * 1000.0);
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
        p.pos[0] = pos.x + origin.x;
        p.pos[1] = pos.y + origin.y;
        p.pos[2] = pos.z + origin.z;
        p.yaw = rot.yaw;
        p.pitch = rot.pitch;
        p.vel[0] = v.x;
        p.vel[1] = v.y;
        p.vel[2] = v.z;
    }
    g_net.Send(p);
}

void ReceiveRemote(double now) {
    StatePacket p;
    for (int budget = 0; budget < 64 && g_net.Receive(p); ++budget) {
        bool reconnect = now - g_lastRecv > kPeerTimeout;
        if (!reconnect && !NewerSequence(p.seq, g_lastRemoteSeq)) continue;
        if (reconnect) {
            g_snaps.clear();
            Log("peer %s (%s) connected", p.name, g_net.PeerText().c_str());
            DestroyAvatar();  // Rebuild from the player's current look; also a manual "respawn" lever.
        }
        g_lastRecv = now;
        g_lastRemoteSeq = p.seq;
        g_remoteName = p.name;
        g_snaps.push_back({now, {p.pos[0], p.pos[1], p.pos[2]}, p.yaw, {p.vel[0], p.vel[1], p.vel[2]}, p.flags});
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
        if (!Call(destination, "SetMaterial").set("ElementIndex", i).set("Material", material).invoke()) {
            Log("avatar: SetMaterial failed for slot %d", i);
            return false;
        }
    }
    return true;
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
        g_pawn = pawn;
        g_localSettledAt = now + 5.0;
    }
    if (g_pawn && g_cfg.discovery && g_pawn != g_discoveredPawn) {
        g_discoveredPawn = g_pawn;
        Discover(g_pawn);
    }
    ReceiveRemote(now);
    if (now - g_lastSend >= kSendInterval) {
        g_lastSend = now;
        SendLocalState(g_pawn, now);
    }
    UpdateAvatar(g_pawn, now);
    HandleTeleport(g_pawn, now);
}

void HookedProcessEvent(UObject* obj, UObject* fn, void* parms) {
    if (!g_gameThread) g_gameThread = GetCurrentThreadId();
    if (!g_failed && !g_inTick && GetCurrentThreadId() == g_gameThread) {
        g_inTick = true;
        Tick();
        g_inTick = false;
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
    return true;
}

} // namespace coop
