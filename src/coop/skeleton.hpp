#pragma once

#include "pose.hpp"
#include "ue.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace coop {

struct BoneTable {
    ue::UObject* asset = nullptr;
    std::vector<ue::FName> names;
    std::vector<int> parents;
    std::vector<uint32_t> hashes;
    std::unordered_map<uint32_t, int> byHash;
    bool Valid() const { return asset && !names.empty(); }
};

// Enumerates a skinned mesh's bones with GetBoneName/GetParentBone (parents always precede children).
bool ReadBoneTable(ue::UObject* component, BoneTable& out);

// Reads parent-relative poses for bones [0, count) of a live skinned mesh via GetSocketTransform.
class PoseReader {
public:
    bool Read(ue::UObject* component, const BoneTable& table, size_t count, std::vector<BonePose>& out);
    // Component-space transform of an arbitrary scene component relative to `mesh`.
    bool Relative(ue::UObject* mesh, ue::UObject* component, BonePose& out);

private:
    ue::BoundFunction socket_, world_;
    std::vector<BonePose> space_;
};

// Drives a runtime-created PoseableMeshComponent by writing its parent-relative bone array.
class PoseWriter {
public:
    bool Attach(ue::UObject* poseable);
    bool Ready() const { return component_ && offset_ >= 0; }
    ue::UObject* Component() const { return component_; }
    const BoneTable& Table() const { return table_; }
    int Offset() const { return offset_; }
    // Applies all known bones of `frame`; unknown names are ignored.
    bool Apply(const PoseFrame& frame);
    void Reset() { *this = PoseWriter{}; }

private:
    ue::UObject* component_ = nullptr;
    BoneTable table_;
    int offset_ = -1;
};

// Creates a component of a native class on `actor`, attached to its root.
ue::UObject* CreateRuntimeComponent(ue::UObject* actor, const char* className);
// Creates a PoseableMeshComponent on `actor`, attached to its root.
ue::UObject* CreatePoseableComponent(ue::UObject* actor);

} // namespace coop
