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

UObject* Mesh(UObject* component) {
    UObject* mesh = nullptr;
    return IsLive(component) && GetProp(component, "SkeletalMesh", mesh) ? mesh : nullptr;
}
} // namespace

UObject* Appearance::BaseMaterial(UObject* component, int slot, UObject* current) const {
    for (const auto& entry : entries_)
        if (entry.component == component && entry.mesh == Mesh(component) && slot >= 0 &&
            static_cast<size_t>(slot) < entry.originals.size() && entry.applied[slot] == current)
            return entry.originals[slot];
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
    if (entry.visible != visible) {
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
            entry.originals[slot] = current;
            changed = true;
        }
        if (!changed) continue;
        auto original = entry.originals[slot];
        if (!IsLive(original)) {
            Log("appearance: original material no longer live");
            continue;
        }
        std::string materialName = Name(original);
        bool tintable = hat ? materialName.find("_hat_") != std::string::npos && materialName.find("_fray") == std::string::npos :
            upper ? materialName.find("_thermal") != std::string::npos || materialName.find("_hoodie") != std::string::npos ||
                    materialName.find("_cut") != std::string::npos :
                    materialName.find("_jeans") != std::string::npos;
        if (palette == 0 || !tintable) {
            if (Call(component, "SetMaterial").set("ElementIndex", slot).set("Material", original).invoke())
                entry.applied[slot] = original;
            continue;
        }
        Call create(component, "CreateDynamicMaterialInstance");
        if (!create.set("ElementIndex", slot).set("SourceMaterial", original).invoke()) return;
        auto dynamic = create.get<UObject*>("ReturnValue");
        if (!IsLive(dynamic)) {
            Log("appearance: could not create a private material instance");
            return;
        }
        int parameter = FindNameIndex("Fabric Tint 1");
        if (parameter < 0 || !Call(dynamic, "SetVectorParameterValue").set("ParameterName", FName{parameter, 0})
                .set("Value", kColors[palette]).invoke()) {
            Log("appearance: cloth tint parameter unavailable");
            Call(component, "SetMaterial").set("ElementIndex", slot).set("Material", original).invoke();
            return;
        }
        entry.applied[slot] = dynamic;
    }
    if (changed) Log("appearance: %s palette=%s cap=%d", name.c_str(), PaletteName(palette), visible);
    entry.palette = palette;
}

} // namespace coop
