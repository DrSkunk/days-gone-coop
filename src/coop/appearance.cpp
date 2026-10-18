#include "appearance.hpp"
#include "log.hpp"

#include <algorithm>

using namespace ue;

namespace coop {
namespace {
struct Color { float r, g, b, a; };
constexpr Color kColors[kPaletteCount] = {{1, 1, 1, 1}, {0.65f, 0.025f, 0.02f, 1},
    {0.025f, 0.15f, 0.65f, 1}, {0.04f, 0.3f, 0.07f, 1}, {0.6f, 0.4f, 0.2f, 1},
    {0.045f, 0.045f, 0.05f, 1}, {0.32f, 0.025f, 0.5f, 1}};
struct TextureJob {
    UObject* target;
    UObject* source;
    int palette;
    bool painted;
    FName targetName;
    std::string sourceName;
};
std::vector<TextureJob> jobs;
constexpr const char* kColorTexture = "Base Color (C/O) Map";

bool LiveJob(const TextureJob& job) {
    // UObject addresses can be reused after collection.
    return IsLive(job.target) && job.target->name.index == job.targetName.index &&
        job.target->name.number == job.targetName.number && IsLive(job.source) && FullName(job.source) == job.sourceName;
}

UObject* ColorTexture(UObject* material) {
    int index = FindNameIndex(kColorTexture);
    struct Array { uint8_t* data; int32_t count, capacity; };
    for (int depth = 0; IsLive(material) && depth < 12; ++depth) {
        Array values{};
        if (GetProp(material, "TextureParameterValues", values) && values.data && values.count >= 0 &&
            values.count <= 128 && values.count <= values.capacity) {
            for (int i = 0; i < values.count; ++i) {
                FName name{};
                UObject* texture = nullptr;
                auto entry = values.data + i * 0x20;
                if (ReadStructFields(entry, 0x20, {{"TextureParameterValue", "ParameterName"}}, &name, sizeof name) &&
                    name.index == index &&
                    ReadStructFields(entry, 0x20, {{"TextureParameterValue", "ParameterValue"}}, &texture, sizeof texture) &&
                    IsLive(texture)) return texture;
            }
        }
        UObject* parent = nullptr;
        material = GetProp(material, "Parent", parent) ? parent : nullptr;
    }
    return nullptr;
}

bool TintMaterial(UObject* component, UObject* dynamic, UObject* original, int palette) {
    auto source = ColorTexture(original);
    if (!source) {
        Log("appearance: base-color texture unavailable for %s", FullName(original).c_str());
        return false;
    }
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const TextureJob& job) {
        return !LiveJob(job);
    }), jobs.end());
    auto found = std::find_if(jobs.begin(), jobs.end(), [=](const TextureJob& job) {
        return job.source == source && job.palette == palette && job.painted;
    });
    UObject* target = found == jobs.end() ? nullptr : found->target;
    if (!target) {
        auto cls = FindClass("CanvasRenderTarget2D");
        Call create(FindDefaultObject("CanvasRenderTarget2D"), "CreateCanvasRenderTarget2D");
        if (!create.set("WorldContextObject", component).set("CanvasRenderTarget2DClass", cls)
                .set("Width", static_cast<int32_t>(1024)).set("Height", static_cast<int32_t>(1024)).invoke()) return false;
        target = create.get<UObject*>("ReturnValue");
        if (!IsLive(target)) {
            Log("appearance: could not create a private color render target");
            return false;
        }
        jobs.push_back({target, source, palette, false, target->name, FullName(source)});
    }
    if (!Call(dynamic, "SetTextureParameterValue").set("ParameterName", FName{FindNameIndex(kColorTexture), 0})
            .set("Value", target).invoke()) return false;
    auto job = std::find_if(jobs.begin(), jobs.end(), [target](const TextureJob& value) { return value.target == target; });
    if (!job->painted && (!Call(target, "UpdateResource").invoke() || !job->painted)) {
        Log("appearance: canvas render target did not paint its color texture");
        return false;
    }
    return true;
}

UObject* Mesh(UObject* component) {
    UObject* mesh = nullptr;
    return IsLive(component) && GetProp(component, "SkeletalMesh", mesh) ? mesh : nullptr;
}
} // namespace

void HandleAppearanceEvent(UObject* object, UObject* function, void* parameters) {
    static int receiveUpdate = FindNameIndex("ReceiveUpdate");
    if (!function || function->name.index != receiveUpdate || !parameters) return;
    auto found = std::find_if(jobs.begin(), jobs.end(), [object](const TextureJob& value) {
        return value.target == object && LiveJob(value);
    });
    if (found == jobs.end()) return;
    UObject* canvas = nullptr;
    int32_t width = 0, height = 0;
    if (!ReadEventParam(function, parameters, "Canvas", canvas) || !ReadEventParam(function, parameters, "Width", width) ||
        !ReadEventParam(function, parameters, "Height", height) || width <= 0 || height <= 0 || !IsLive(canvas)) {
        Log("appearance: invalid render-target update parameters");
        return;
    }
    struct Vec2 { float x, y; };
    found->painted = Call(canvas, "K2_DrawTexture").set("RenderTexture", found->source)
        .set("ScreenPosition", Vec2{0, 0}).set("ScreenSize", Vec2{static_cast<float>(width), static_cast<float>(height)})
        .set("CoordinatePosition", Vec2{0, 0}).set("CoordinateSize", Vec2{1, 1}).set("RenderColor", kColors[found->palette])
        .set("BlendMode", static_cast<uint8_t>(0)).set("Rotation", 0.f).set("PivotPoint", Vec2{0, 0}).invoke();
}

UObject* Appearance::BaseMaterial(UObject* component, int slot, UObject* current) const {
    for (const auto& entry : entries_)
        if (entry.component == component && entry.mesh == Mesh(component) && slot >= 0 &&
            static_cast<size_t>(slot) < entry.originals.size() && entry.applied[slot] == current)
            return entry.originals[slot].get();
    return current;
}

void Appearance::Apply(UObject* component, const MenuSettings& settings) {
    auto mesh = Mesh(component);
    if (!IsLive(mesh)) return;
    std::string name = Name(mesh);
    bool hat = name.find("deacon_hat") != std::string::npos;
    bool upper = name.find("deacon_up") != std::string::npos;
    bool lower = name.find("deacon_lo") != std::string::npos;
    if (!hat && !upper && !lower) return;
    int palette = hat ? settings.capColor : upper ? settings.jacket : settings.trousers;
    if (palette < 0 || palette >= kPaletteCount) {
        Log("appearance: invalid palette %d", palette);
        return;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry& e) {
        return !IsLive(e.component);
    }), entries_.end());
    auto found = std::find_if(entries_.begin(), entries_.end(), [component](const Entry& e) { return e.component == component; });
    if (found == entries_.end()) {
        entries_.push_back({});
        found = entries_.end() - 1;
    }
    auto& entry = *found;
    if (entry.mesh != mesh) {
        entry = {};
        entry.component = component;
        entry.mesh = mesh;
    }
    bool visible = !hat || settings.cap;
    Call visibility(component, "IsVisible");
    if (!visibility.invoke()) return;
    if (entry.visible != visible || visibility.get<bool>("ReturnValue") != visible) {
        if (!Call(component, "SetVisibility").setBool("bNewVisibility", visible).setBool("bPropagateToChildren", false).invoke())
            return;
        entry.visible = visible;
    }
    Call count(component, "GetNumMaterials");
    if (!count.invoke()) return;
    int size = count.get<int32_t>("ReturnValue");
    if (size < 0 || size > 64) {
        Log("appearance: invalid material count %d", size);
        return;
    }
    bool changed = entry.palette != palette || entry.originals.size() != static_cast<size_t>(size);
    entry.originals.resize(size, nullptr);
    entry.applied.resize(size, nullptr);
    for (int slot = 0; slot < size; ++slot) {
        Call get(component, "GetMaterial");
        if (!get.set("ElementIndex", slot).invoke()) return;
        auto current = get.get<UObject*>("ReturnValue");
        if (!IsLive(current)) continue;
        if (current != entry.applied[slot]) {
            entry.originals[slot] = RetainObject(current);
            if (!entry.originals[slot]) return;
            changed = true;
        }
        if (!changed) continue;
        auto original = entry.originals[slot].get();
        if (!IsLive(original)) {
            Log("appearance: original material no longer live");
            continue;
        }
        std::string materialName = Name(original);
        UObject* parent = original;
        for (int depth = 0; depth < 12; ++depth) {
            UObject* next = nullptr;
            if (!GetProp(parent, "Parent", next) || !IsLive(next)) break;
            materialName += " " + Name(next);
            parent = next;
        }
        bool tintable = hat ? materialName.find("_hat_") != std::string::npos && materialName.find("_fray") == std::string::npos :
            upper ? materialName.find("_thermal") != std::string::npos || materialName.find("_hoodie") != std::string::npos ||
                    materialName.find("_cut") != std::string::npos :
                    materialName.find("_jeans") != std::string::npos;
        if (palette == 0 || !tintable) {
            if (Call(component, "SetMaterial").set("ElementIndex", slot).set("Material", original).invoke())
                entry.applied[slot] = original;
            continue;
        }
        UObject* materialParent = original;
        auto dynamicClass = FindClass("MaterialInstanceDynamic");
        for (int depth = 0; depth < 12 && IsA(materialParent, dynamicClass); ++depth) {
            UObject* next = nullptr;
            if (!GetProp(materialParent, "Parent", next) || !IsLive(next)) break;
            materialParent = next;
        }
        Call create(FindDefaultObject("KismetMaterialLibrary"), "CreateDynamicMaterialInstance");
        if (!create.set("WorldContextObject", component).set("Parent", materialParent).invoke()) return;
        auto dynamic = create.get<UObject*>("ReturnValue");
        if (!IsLive(dynamic) || dynamic == original) {
            Log("appearance: could not create a private material instance");
            return;
        }
        if (!Call(dynamic, "K2_CopyMaterialInstanceParameters").set("Source", original).invoke() ||
            !Call(component, "SetMaterial").set("ElementIndex", slot).set("Material", dynamic).invoke()) return;
        if (!TintMaterial(component, dynamic, original, palette)) {
            Log("appearance: private texture tint failed");
            Call(component, "SetMaterial").set("ElementIndex", slot).set("Material", original).invoke();
            entry.applied[slot] = original;
            return;
        }
        entry.applied[slot] = dynamic;
    }
    if (changed) Log("appearance: %s palette=%s cap=%d", name.c_str(), PaletteName(palette), visible);
    entry.palette = palette;
}

} // namespace coop
