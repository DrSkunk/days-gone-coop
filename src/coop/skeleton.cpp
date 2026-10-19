#include "skeleton.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>

namespace coop {

using namespace ue;

namespace {

struct RawArray {
    uint8_t* data;
    int32_t count, capacity;
};

UObject* SkinnedClass() {
    static UObject* cls = nullptr;
    if (!IsLive(cls)) cls = FindClass("SkinnedMeshComponent");
    return cls;
}

std::string BoneText(FName name) {
    std::string text = NameString(name.index);
    if (name.number > 0) text += "_" + std::to_string(name.number - 1);
    return text;
}

bool Finite(const FTransform& x) {
    for (float f : {x.rotation.x, x.rotation.y, x.rotation.z, x.rotation.w, x.translation.x, x.translation.y,
                    x.translation.z})
        if (!std::isfinite(f) || std::fabs(f) > 1e7f) return false;
    return true;
}

BonePose ToPose(const FTransform& x) {
    BonePose pose;
    pose.q[0] = x.rotation.x;
    pose.q[1] = x.rotation.y;
    pose.q[2] = x.rotation.z;
    pose.q[3] = x.rotation.w;
    NormalizeQuat(pose.q);
    pose.t = x.translation;
    return pose;
}

int ClassSize(UObject* cls) {
    int size = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(cls) + 0x40); // UStruct::PropertiesSize
    return size >= 0x400 && size <= 0x8000 ? size : 0x2000;
}

} // namespace

bool ReadBoneTable(UObject* component, BoneTable& out) {
    out = {};
    static BoundFunction name, parent;
    if (!IsLive(component) || !IsA(component, SkinnedClass())) return false;
    if (!name.Ready() && !name.Bind(SkinnedClass(), "GetBoneName", {"BoneIndex", "ReturnValue"})) return false;
    if (!parent.Ready() && !parent.Bind(SkinnedClass(), "GetParentBone", {"BoneName", "ReturnValue"})) return false;
    GetProp(component, "SkeletalMesh", out.asset);
    if (!IsLive(out.asset)) return false;
    for (int32_t i = 0; i < 1024; ++i) {
        name.Set(0, i);
        if (!name.Invoke(component)) return false;
        auto bone = name.Get<FName>(1);
        if (bone.index == 0 && bone.number == 0) break; // NAME_None ends the reference skeleton
        out.names.push_back(bone);
    }
    for (size_t i = 0; i < out.names.size(); ++i) {
        parent.Set(0, out.names[i]);
        if (!parent.Invoke(component)) return false;
        auto p = parent.Get<FName>(1);
        int index = -1;
        for (size_t j = 0; j < i; ++j)
            if (out.names[j].index == p.index && out.names[j].number == p.number) index = static_cast<int>(j);
        if (i > 0 && index < 0) {
            Log("skeleton: bone %s has no earlier parent", BoneText(out.names[i]).c_str());
            out = {};
            return false;
        }
        out.parents.push_back(index);
        uint32_t hash = BoneHash(BoneText(out.names[i]));
        out.hashes.push_back(hash);
        out.byHash.emplace(hash, static_cast<int>(i));
    }
    return out.Valid();
}

bool PoseReader::Read(UObject* component, const BoneTable& table, size_t count, std::vector<BonePose>& out) {
    if (!socket_.Ready()) {
        static UObject* scene = FindClass("SceneComponent");
        if (!socket_.Bind(scene, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"})) return false;
    }
    count = std::min(count, table.names.size());
    space_.resize(count);
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        socket_.Set(0, table.names[i]);
        socket_.Set<uint8_t>(1, 2); // RTS_Component
        if (!socket_.Invoke(component)) return false;
        auto x = socket_.Get<FTransform>(2);
        if (!Finite(x)) return false;
        space_[i] = ToPose(x);
        int parent = table.parents[i];
        out[i] = parent >= 0 && static_cast<size_t>(parent) < i ? RelativePose(space_[i], space_[parent]) : space_[i];
    }
    return true;
}

bool PoseReader::Relative(UObject* mesh, UObject* component, BonePose& out) {
    if (!world_.Ready()) {
        static UObject* scene = FindClass("SceneComponent");
        if (!world_.Bind(scene, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"})) return false;
    }
    world_.Set(0, FName{0, 0});
    world_.Set<uint8_t>(1, 0); // RTS_World
    if (!world_.Invoke(mesh)) return false;
    auto parent = world_.Get<FTransform>(2);
    if (!world_.Invoke(component)) return false;
    auto child = world_.Get<FTransform>(2);
    if (!Finite(parent) || !Finite(child)) return false;
    out = RelativePose(ToPose(child), ToPose(parent));
    return true;
}

bool PoseWriter::Attach(UObject* poseable) {
    *this = PoseWriter{};
    static UObject* poseableClass = FindClass("PoseableMeshComponent");
    if (!IsLive(poseable) || !poseableClass || !IsA(poseable, poseableClass) || !ReadBoneTable(poseable, table_))
        return false;
    BoundFunction set;
    if (!set.Bind(poseableClass, "SetBoneTransformByName", {"BoneName", "InTransform", "BoneSpace"})) return false;
    // The root has no parent, so the native setter stores this probe verbatim in the local-space array.
    FTransform probe{{0.1825742f, 0.3651484f, 0.5477226f, 0.7302967f}, {12.5f, -7.25f, 3.0f}, 0, {1, 1, 1}, 0};
    set.Set(0, table_.names[0]);
    set.Set(1, probe);
    set.Set<uint8_t>(2, 1); // EBoneSpaces::ComponentSpace
    if (!set.Invoke(poseable)) return false;
    auto base = reinterpret_cast<uint8_t*>(poseable);
    int limit = ClassSize(poseable->cls), found = -1, matches = 0;
    for (int offset = 0x100; offset + static_cast<int>(sizeof(RawArray)) <= limit; offset += 8) {
        if (!ReadableMemory(base + offset, sizeof(RawArray))) break;
        RawArray array{};
        std::memcpy(&array, base + offset, sizeof array);
        if (array.count != static_cast<int32_t>(table_.names.size()) || array.capacity < array.count ||
            array.capacity > 65536 || !ReadableMemory(array.data, static_cast<size_t>(array.count) * sizeof(FTransform)))
            continue;
        FTransform first{};
        std::memcpy(&first, array.data, sizeof first);
        bool same = std::fabs(first.rotation.x - probe.rotation.x) < 1e-5f &&
            std::fabs(first.rotation.w - probe.rotation.w) < 1e-5f &&
            std::fabs(first.translation.x - probe.translation.x) < 1e-4f &&
            std::fabs(first.translation.y - probe.translation.y) < 1e-4f &&
            std::fabs(first.translation.z - probe.translation.z) < 1e-4f;
        if (same) {
            found = offset;
            ++matches;
        }
    }
    FTransform identity{{0, 0, 0, 1}, {0, 0, 0}, 0, {1, 1, 1}, 0};
    set.Set(1, identity);
    set.Invoke(poseable);
    if (matches != 1) {
        Log("skeleton: poseable bone array not identified (%d candidates, %d bones)", matches,
            static_cast<int>(table_.names.size()));
        *this = PoseWriter{};
        return false;
    }
    component_ = poseable;
    offset_ = found;
    return true;
}

bool PoseWriter::Apply(const PoseFrame& frame) {
    if (!Ready() || !IsLive(component_)) return false;
    RawArray array{};
    std::memcpy(&array, reinterpret_cast<uint8_t*>(component_) + offset_, sizeof array);
    if (array.count != static_cast<int32_t>(table_.names.size()) ||
        !ReadableMemory(array.data, static_cast<size_t>(array.count) * sizeof(FTransform)))
        return false;
    auto atoms = reinterpret_cast<FTransform*>(array.data);
    for (size_t i = 0; i < frame.names.size() && i < frame.bones.size(); ++i) {
        auto it = table_.byHash.find(frame.names[i]);
        if (it == table_.byHash.end()) continue;
        auto& atom = atoms[it->second];
        const auto& bone = frame.bones[i];
        atom.rotation = {bone.q[0], bone.q[1], bone.q[2], bone.q[3]};
        atom.translation = bone.t;
    }
    return true;
}

UObject* CreateRuntimeComponent(UObject* actor, const char* className) {
    static UObject *host = nullptr, *context = nullptr;
    auto componentClass = FindClass(className);
    auto cdo = FindDefaultObject(className);
    if (!IsLive(actor) || !componentClass || !IsLive(cdo)) return nullptr;
    if (!IsLive(host) || !IsLive(context)) {
        host = context = nullptr;
        auto bpgc = FindClass("BlueprintGeneratedClass");
        for (int32_t i = 0, n = ObjectCount(); i < n && !context; ++i) {
            auto o = ObjectAt(i);
            if (!o || !IsLive(o) || !IsLive(o->cls) || !IsA(o->cls, bpgc) || !(o->flags & RF_ClassDefaultObject)) continue;
            RawArray templates{};
            if (!GetProp(o->cls, "ComponentTemplates", templates) || !templates.data || templates.count < 1 ||
                templates.capacity < templates.count || templates.count > 256)
                continue;
            host = o->cls;
            context = o;
        }
        if (!context) {
            Log("skeleton: no blueprint class with component templates for poseable creation");
            return nullptr;
        }
        Log("skeleton: poseable template host %s", FullName(host).c_str());
    }
    RawArray templates{};
    if (!GetProp(host, "ComponentTemplates", templates) || !templates.data || templates.count < 1) return nullptr;
    auto slot = reinterpret_cast<UObject**>(templates.data);
    UObject* original = slot[0];
    // AddComponent duplicates a named template from the context's blueprint class. Present the
    // component class default object as that template for this call only; it is copied without the CDO flag.
    int32_t flags = cdo->flags;
    slot[0] = cdo;
    cdo->flags = flags & ~RF_ClassDefaultObject;
    FTransform identity{{0, 0, 0, 1}, {0, 0, 0}, 0, {1, 1, 1}, 0};
    Call add(actor, "AddComponent");
    add.set("TemplateName", cdo->name).setBool("bManualAttachment", false).set("RelativeTransform", identity)
        .set("ComponentTemplateContext", context);
    bool invoked = add.invoke();
    cdo->flags = flags;
    slot[0] = original;
    auto component = invoked ? add.get<UObject*>("ReturnValue") : nullptr;
    if (!IsLive(component) || !IsA(component, componentClass) || IsDefaultOrArchetype(component)) {
        Log("skeleton: AddComponent did not create a %s", className);
        return nullptr;
    }
    return component;
}

UObject* CreatePoseableComponent(UObject* actor) {
    return CreateRuntimeComponent(actor, "PoseableMeshComponent");
}

} // namespace coop
