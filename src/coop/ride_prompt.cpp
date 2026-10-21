#include "ride_prompt.hpp"

#include "log.hpp"
#include "native_ui.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace coop::ride {

using namespace ue;

namespace {

constexpr const char* kInteractClass =
    "/Game/Libraries/Blueprints/Interacts/BP_BasicInteractBase.BP_BasicInteractBase_C";

struct FStringView {
    const wchar_t* data;
    int32_t num, max;
};

UObject* g_actor = nullptr;
std::string g_label;
FVector g_at{};
bool g_failed = false;
bool g_succeeded = false;

UObject* Prop(UObject* obj, const char* name) {
    UObject* value = nullptr;
    return IsLive(obj) && GetProp(obj, name, value) && IsLive(value) ? value : nullptr;
}

template <typename T> bool SetMember(UObject* obj, const char* name, const T& value) {
    auto property = IsLive(obj) ? FindChild(obj->cls, name) : nullptr;
    if (!property) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(obj) + PropertyOffset(property), &value, sizeof(T));
    return true;
}

std::string Narrow(const FStringView& text) {
    std::string out;
    if (!text.data || text.num <= 1 || text.num > 256 || !ReadableMemory(text.data, text.num * sizeof(wchar_t)))
        return out;
    for (int i = 0; i + 1 < text.num; ++i) out += text.data[i] < 128 ? static_cast<char>(text.data[i]) : '?';
    return out;
}

// ActorInteractDetails: PreButtonText @8, Text @24 (FString each), 112 bytes total.
bool SetText(const std::string& label) {
    std::wstring wide(label.begin(), label.end()), empty;
    FStringView pre{empty.c_str(), 1, 1}, post{wide.c_str(), static_cast<int32_t>(wide.size() + 1),
                                               static_cast<int32_t>(wide.size() + 1)};
    Call(g_actor, "UpdateInteractText").set("PreText", pre).set("PostText", post).invoke();
    auto slate = Prop(g_actor, "SlateInputComponent");
    auto property = IsLive(slate) ? FindChild(slate->cls, "InteractDetails") : nullptr;
    if (!property) return false;
    uint8_t details[112];
    std::memcpy(details, reinterpret_cast<uint8_t*>(slate) + PropertyOffset(property), sizeof details);
    std::memcpy(details + 24, &post, sizeof post);
    bool set = Call(slate, "SetInteractDetails").set("Details", details).invoke();
    FStringView now{};
    std::memcpy(&now, reinterpret_cast<uint8_t*>(slate) + PropertyOffset(property) + 24, sizeof now);
    Log("ride prompt: label set=%d now='%s'", set ? 1 : 0, Narrow(now).c_str());
    return set;
}

UObject* Spawn(UObject* context, const FVector& at) {
    static UObject* statics = FindDefaultObject("GameplayStatics");
    auto cls = FindObject(kInteractClass);
    if (!statics || !cls) {
        Log("ride prompt: native interact class is not loaded");
        return nullptr;
    }
    FTransform xf{};
    xf.rotation = {0, 0, 0, 1};
    xf.translation = at;
    xf.scale = {1, 1, 1};
    Call begin(statics, "BeginSpawningActorFromClass");
    begin.set("WorldContextObject", context).set("ActorClass", cls).set("SpawnTransform", xf)
        .setBool("bNoCollisionFail", true);
    if (!begin.invoke()) return nullptr;
    auto actor = begin.get<UObject*>("ReturnValue");
    if (!IsLive(actor)) return nullptr;
    uint8_t on = 1;
    SetMember(actor, "SinglePressInteract", on);
    SetMember(actor, "InteractCurrentlyEnabled", on);
    Call finish(statics, "FinishSpawningActor");
    finish.set("Actor", actor).set("SpawnTransform", xf).invoke();
    return IsLive(actor) ? actor : nullptr;
}

} // namespace

void OnEvent(UObject* obj, UObject* fn) {
    static int succeeded = FindNameIndex("OnInputSucceeded");
    if (obj && obj == g_actor && fn && fn->name.index == succeeded) g_succeeded = true;
}

void Destroy() {
    ui::Hint("");
    if (IsLive(g_actor)) Call(g_actor, "K2_DestroyActor").invoke();
    g_actor = nullptr;
    g_label.clear();
    g_succeeded = false;
}

bool Update(UObject* context, bool visible, const FVector& at, const std::string& label) {
    if (!visible || g_failed || !IsLive(context)) {
        if (g_actor) Destroy();
        return false;
    }
    if (g_actor && !IsLive(g_actor)) {
        g_actor = nullptr;
        g_label.clear();
    }
    if (!g_actor) {
        g_actor = Spawn(context, at);
        if (!g_actor) {
            g_failed = true;
            Log("ride prompt: spawning the native interact failed; the prompt is disabled this session");
            return false;
        }
        Call(g_actor, "EnableOrDisableInteraction").setBool("Enabled", true).invoke();
        g_at = at;
        g_succeeded = false;
        Log("ride prompt: native interact spawned");
    }
    if (label != g_label && SetText(label)) g_label = label;
    float dx = at.x - g_at.x, dy = at.y - g_at.y, dz = at.z - g_at.z;
    if (dx * dx + dy * dy + dz * dz > 4) {
        Call(g_actor, "K2_SetActorLocation").set("NewLocation", at).setBool("bSweep", false)
            .setBool("bTeleport", true).invoke();
        g_at = at;
    }
    uint8_t inside = 0;
    GetProp(g_actor, "InsideInteractRange", inside);
    ui::Hint(inside ? label : "");
    bool fired = g_succeeded;
    g_succeeded = false;
    if (fired) {
        Log("ride prompt: interaction completed");
        Call(g_actor, "ResetTheInteraction").invoke();
    }
    return fired;
}

} // namespace coop::ride
