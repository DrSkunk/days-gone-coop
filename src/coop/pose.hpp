#pragma once

#include "ue.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace coop {

constexpr uint32_t kPoseMagic = 0x534F5044; // "DPOS"
constexpr uint16_t kPoseVersion = 2;
constexpr size_t kPoseBonesPerPacket = 64;    // worst case 1132 bytes: below the ~1200-byte safe internet payload
constexpr uint8_t kPoseMaxChunks = 10;
constexpr float kPoseTranslationScale = 50.f; // 0.02 cm steps, +-655 cm range
// Bones carry a translation only in keyframes or shortly after it changed; most animated bones only rotate.
constexpr uint8_t kBoneLargestMask = 0x03;
constexpr uint8_t kBoneHasTranslation = 0x04;
constexpr size_t kPoseBoneRotationBytes = 11;  // name, info, three quaternion components
constexpr size_t kPoseBoneFullBytes = 17;      // plus three translation components
constexpr uint32_t kPoseKeyframeMs = 1000;
constexpr uint32_t kPoseTranslationHoldMs = 250; // repeats a changed translation so one lost datagram heals

enum PoseKind : uint8_t { kPoseBody = 0, kPoseBike = 1, kPoseEnemy = 2 };
enum PoseFlags : uint16_t { kPoseAlive = 1 << 0 };

#pragma pack(push, 1)
// Decoded bone; on the wire it is name, largest, q and, only with kBoneHasTranslation, t.
struct PoseBoneWire {
    uint32_t name;    // BoneHash of the bone name; meshes are matched by name, not index
    int16_t t[3];     // parent-relative translation
    int16_t q[3];     // parent-relative rotation, smallest-three
    uint8_t largest;  // index of the omitted quaternion component, plus kBoneHasTranslation
};
struct PoseHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t kind, count;
    uint32_t session, timeMs, entity;
    uint8_t chunk, chunks;
    uint16_t flags;
    float root[3]; // world mesh location for enemies (absolute coordinates)
    float yaw;
    float health;
};
struct PosePacket {
    PoseHeader header;
    PoseBoneWire bones[kPoseBonesPerPacket];
};
#pragma pack(pop)
static_assert(sizeof(PoseBoneWire) == 17, "pose bone wire format");
static_assert(sizeof(PoseHeader) == 44, "pose header wire format");

struct BonePose {
    float q[4]{0, 0, 0, 1}; // x, y, z, w
    ue::FVector t{};
};

constexpr uint32_t BoneHash(std::string_view name) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        hash = (hash ^ c) * 16777619u;
    }
    return hash ? hash : 1;
}

// Pseudo-bone carrying the sender mesh's world rotation (lean, slope pitch); never matches a real bone.
constexpr uint32_t kRootBoneHash = BoneHash("@root");

constexpr size_t kPosePacketMaxBytes = sizeof(PoseHeader) + kPoseBonesPerPacket * kPoseBoneFullBytes;

inline bool BoneHasTranslation(const PoseBoneWire& wire) { return (wire.largest & kBoneHasTranslation) != 0; }

inline void NormalizeQuat(float q[4]) {
    float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !std::isfinite(n)) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    for (int i = 0; i < 4; ++i) q[i] /= n;
}

inline int16_t QuantizeTranslation(float value) {
    float scaled = std::round(value * kPoseTranslationScale);
    return static_cast<int16_t>(std::clamp(scaled, -32767.f, 32767.f));
}

inline PoseBoneWire EncodeBone(uint32_t name, const BonePose& bone) {
    PoseBoneWire wire{};
    wire.name = name;
    wire.t[0] = QuantizeTranslation(bone.t.x);
    wire.t[1] = QuantizeTranslation(bone.t.y);
    wire.t[2] = QuantizeTranslation(bone.t.z);
    float q[4] = {bone.q[0], bone.q[1], bone.q[2], bone.q[3]};
    NormalizeQuat(q);
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (std::fabs(q[i]) > std::fabs(q[largest])) largest = i;
    float sign = q[largest] < 0 ? -1.f : 1.f;
    constexpr float scale = 32767.f * 1.41421356f;
    for (int i = 0, j = 0; i < 4; ++i) {
        if (i == largest) continue;
        wire.q[j++] = static_cast<int16_t>(std::clamp(std::round(q[i] * sign * scale), -32767.f, 32767.f));
    }
    wire.largest = static_cast<uint8_t>(largest) | kBoneHasTranslation;
    return wire;
}

// Leaves bone.t untouched when the wire bone carries no translation.
inline bool DecodeBone(const PoseBoneWire& wire, BonePose& bone) {
    if ((wire.largest & ~(kBoneLargestMask | kBoneHasTranslation)) || !wire.name) return false;
    int largest = wire.largest & kBoneLargestMask;
    constexpr float scale = 1.f / (32767.f * 1.41421356f);
    float sum = 0;
    for (int i = 0, j = 0; i < 4; ++i) {
        if (i == largest) continue;
        bone.q[i] = wire.q[j++] * scale;
        sum += bone.q[i] * bone.q[i];
    }
    bone.q[largest] = std::sqrt(std::max(0.f, 1.f - sum));
    NormalizeQuat(bone.q);
    if (BoneHasTranslation(wire))
        bone.t = {wire.t[0] / kPoseTranslationScale, wire.t[1] / kPoseTranslationScale, wire.t[2] / kPoseTranslationScale};
    return true;
}

inline size_t WriteBone(const PoseBoneWire& wire, char* out) {
    std::memcpy(out, &wire.name, 4);
    out[4] = static_cast<char>(wire.largest);
    std::memcpy(out + 5, wire.q, 6);
    if (!BoneHasTranslation(wire)) return kPoseBoneRotationBytes;
    std::memcpy(out + 11, wire.t, 6);
    return kPoseBoneFullBytes;
}

inline bool DecodePose(const char* data, int len, PosePacket& out) {
    if (len < static_cast<int>(sizeof(PoseHeader)) || len > static_cast<int>(kPosePacketMaxBytes)) return false;
    std::memcpy(&out.header, data, sizeof out.header);
    const auto& h = out.header;
    if (h.magic != kPoseMagic || h.version != kPoseVersion || h.kind > kPoseEnemy || !h.count ||
        h.count > kPoseBonesPerPacket || !h.chunks || h.chunks > kPoseMaxChunks || h.chunk >= h.chunks ||
        (h.flags & ~kPoseAlive))
        return false;
    if ((h.kind == kPoseEnemy) != (h.entity != 0)) return false;
    for (float f : {h.root[0], h.root[1], h.root[2], h.yaw, h.health})
        if (!std::isfinite(f) || std::fabs(f) > 1e8f) return false;
    if (h.health < 0 || h.health > 100000) return false;
    size_t at = sizeof(PoseHeader), size = static_cast<size_t>(len);
    for (uint8_t i = 0; i < h.count; ++i) {
        if (at + kPoseBoneRotationBytes > size) return false;
        auto& bone = out.bones[i];
        bone = PoseBoneWire{};
        std::memcpy(&bone.name, data + at, 4);
        bone.largest = static_cast<uint8_t>(data[at + 4]);
        std::memcpy(bone.q, data + at + 5, 6);
        at += kPoseBoneRotationBytes;
        if ((bone.largest & ~(kBoneLargestMask | kBoneHasTranslation)) || !bone.name) return false;
        if (BoneHasTranslation(bone)) {
            if (at + 6 > size) return false;
            std::memcpy(bone.t, data + at, 6);
            at += 6;
        }
    }
    return at == size;
}

// Quaternion helpers using Unreal's convention: (a * b) applies b first, then a.
inline void QuatMul(const float a[4], const float b[4], float out[4]) {
    float r[4] = {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    std::memcpy(out, r, sizeof r);
}

inline ue::FVector QuatRotate(const float q[4], ue::FVector v) {
    // v + 2w(u x v) + 2u x (u x v)
    float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    float cx = uy * v.z - uz * v.y, cy = uz * v.x - ux * v.z, cz = ux * v.y - uy * v.x;
    float ccx = uy * cz - uz * cy, ccy = uz * cx - ux * cz, ccz = ux * cy - uy * cx;
    return {v.x + 2 * (w * cx + ccx), v.y + 2 * (w * cy + ccy), v.z + 2 * (w * cz + ccz)};
}

// Parent-relative pose from two component-space poses (unit scale).
inline BonePose RelativePose(const BonePose& child, const BonePose& parent) {
    float inverse[4] = {-parent.q[0], -parent.q[1], -parent.q[2], parent.q[3]};
    BonePose local;
    QuatMul(inverse, child.q, local.q);
    NormalizeQuat(local.q);
    local.t = QuatRotate(inverse, {child.t.x - parent.t.x, child.t.y - parent.t.y, child.t.z - parent.t.z});
    return local;
}

inline BonePose ComposePose(const BonePose& local, const BonePose& parent) {
    BonePose out;
    QuatMul(parent.q, local.q, out.q);
    NormalizeQuat(out.q);
    auto t = QuatRotate(parent.q, local.t);
    out.t = {parent.t.x + t.x, parent.t.y + t.y, parent.t.z + t.z};
    return out;
}

inline BonePose BlendPose(const BonePose& a, const BonePose& b, float k) {
    BonePose out;
    float dot = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
    float sign = dot < 0 ? -1.f : 1.f;
    for (int i = 0; i < 4; ++i) out.q[i] = a.q[i] + (b.q[i] * sign - a.q[i]) * k;
    NormalizeQuat(out.q);
    out.t = {a.t.x + (b.t.x - a.t.x) * k, a.t.y + (b.t.y - a.t.y) * k, a.t.z + (b.t.z - a.t.z) * k};
    return out;
}

// Unreal's FRotator::Quaternion and FQuat::Rotator, in degrees.
inline void RotatorToQuat(float pitch, float yaw, float roll, float q[4]) {
    constexpr float half = 3.14159265358979f / 360.f;
    float sp = std::sin(pitch * half), cp = std::cos(pitch * half);
    float sy = std::sin(yaw * half), cy = std::cos(yaw * half);
    float sr = std::sin(roll * half), cr = std::cos(roll * half);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

inline void QuatToRotator(const float q[4], float& pitch, float& yaw, float& roll) {
    constexpr float toDegrees = 180.f / 3.14159265358979f;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float singularity = z * x - w * y;
    yaw = std::atan2(2.f * (w * z + x * y), 1.f - 2.f * (y * y + z * z)) * toDegrees;
    if (singularity < -0.4999995f) {
        pitch = -90.f;
        roll = std::remainder(-yaw - 2.f * std::atan2(x, w) * toDegrees, 360.f);
    } else if (singularity > 0.4999995f) {
        pitch = 90.f;
        roll = std::remainder(yaw - 2.f * std::atan2(x, w) * toDegrees, 360.f);
    } else {
        pitch = std::asin(2.f * singularity) * toDegrees;
        roll = std::atan2(-2.f * (w * x + y * z), 1.f - 2.f * (x * x + y * y)) * toDegrees;
    }
}

// Sender-side memory of which translations the receiver already has, per streamed mesh.
struct PoseTranslationTracker {
    std::vector<uint32_t> names;
    std::vector<std::array<int16_t, 3>> sent;
    std::vector<uint32_t> changedMs;
    uint32_t keyMs = 0;
    bool keyed = false;
    void Reset() { *this = PoseTranslationTracker{}; }
};

// Splits a pose into chunk datagrams; `send` receives each datagram's bytes and size.
// Without a tracker every bone carries its translation.
template <typename Send>
bool EncodePose(PoseHeader header, const std::vector<uint32_t>& names, const std::vector<BonePose>& bones, Send&& send,
                PoseTranslationTracker* tracker = nullptr) {
    size_t count = std::min(names.size(), bones.size());
    size_t chunks = (count + kPoseBonesPerPacket - 1) / kPoseBonesPerPacket;
    if (!count || chunks > kPoseMaxChunks) return false;
    header.magic = kPoseMagic;
    header.version = kPoseVersion;
    header.chunks = static_cast<uint8_t>(chunks);
    bool key = true;
    if (tracker) {
        if (tracker->names.size() != count || !std::equal(tracker->names.begin(), tracker->names.end(), names.begin())) {
            tracker->Reset();
            tracker->names.assign(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(count));
            tracker->sent.assign(count, {});
            tracker->changedMs.assign(count, header.timeMs);
        }
        int32_t sinceKey = static_cast<int32_t>(header.timeMs - tracker->keyMs);
        key = !tracker->keyed || sinceKey < 0 || sinceKey >= static_cast<int32_t>(kPoseKeyframeMs);
        if (key) {
            tracker->keyMs = header.timeMs;
            tracker->keyed = true;
        }
    }
    char packet[kPosePacketMaxBytes];
    for (size_t c = 0; c < chunks; ++c) {
        size_t first = c * kPoseBonesPerPacket, n = std::min(kPoseBonesPerPacket, count - first);
        PoseHeader h = header;
        h.chunk = static_cast<uint8_t>(c);
        h.count = static_cast<uint8_t>(n);
        std::memcpy(packet, &h, sizeof h);
        size_t at = sizeof h;
        for (size_t i = first; i < first + n; ++i) {
            PoseBoneWire wire = EncodeBone(names[i], bones[i]);
            bool withTranslation = key;
            if (tracker) {
                std::array<int16_t, 3> t{wire.t[0], wire.t[1], wire.t[2]};
                if (t != tracker->sent[i]) {
                    tracker->sent[i] = t;
                    tracker->changedMs[i] = header.timeMs;
                }
                withTranslation = withTranslation ||
                    static_cast<uint32_t>(header.timeMs - tracker->changedMs[i]) < kPoseTranslationHoldMs;
            }
            if (!withTranslation) wire.largest &= ~kBoneHasTranslation;
            at += WriteBone(wire, packet + at);
        }
        send(static_cast<const char*>(packet), at);
    }
    return true;
}

struct PoseFrame {
    uint32_t timeMs = 0;
    uint16_t flags = 0;
    float root[3]{};
    float yaw = 0, health = 0;
    uint8_t chunks = 0;
    uint32_t chunkMask = 0;
    std::vector<uint32_t> names;
    std::vector<BonePose> bones;
    bool Complete() const { return chunks && chunkMask == (1u << chunks) - 1; }
};

// Collects chunked pose frames and samples them at a sender timestamp.
class PoseBuffer {
public:
    void Clear() {
        frames_.clear();
        translations_.clear();
    }
    bool Empty() const { return frames_.empty(); }
    double LastArrival() const { return lastArrival_; }

    bool Push(const PosePacket& packet, double arrival) {
        const auto& h = packet.header;
        PoseFrame* frame = nullptr;
        for (auto& f : frames_)
            if (f.timeMs == h.timeMs) frame = &f;
        if (!frame) {
            if (!frames_.empty() && Delta(h.timeMs, frames_.back().timeMs) < -1000) return false;
            if (!frames_.empty() && Delta(h.timeMs, frames_.back().timeMs) > 5000) frames_.clear();
            auto at = std::find_if(frames_.begin(), frames_.end(),
                [&](const PoseFrame& f) { return Delta(h.timeMs, f.timeMs) < 0; });
            frame = &*frames_.insert(at, PoseFrame{});
            frame->timeMs = h.timeMs;
            frame->chunks = h.chunks;
            // Bones keep their chunk slot so frames from the same sender line up index-for-index.
            frame->names.assign(static_cast<size_t>(h.chunks) * kPoseBonesPerPacket, 0);
            frame->bones.resize(frame->names.size());
        }
        if (frame->chunks != h.chunks || (frame->chunkMask & (1u << h.chunk))) return false;
        for (uint8_t i = 0; i < h.count; ++i)
            if ((packet.bones[i].largest & ~(kBoneLargestMask | kBoneHasTranslation)) || !packet.bones[i].name) return false;
        frame->chunkMask |= 1u << h.chunk;
        frame->flags = h.flags;
        std::memcpy(frame->root, h.root, sizeof frame->root);
        frame->yaw = h.yaw;
        frame->health = h.health;
        size_t base = static_cast<size_t>(h.chunk) * kPoseBonesPerPacket;
        if (translations_.size() > 4096) translations_.clear();
        for (uint8_t i = 0; i < h.count; ++i) {
            const auto& wire = packet.bones[i];
            auto& bone = frame->bones[base + i];
            if (BoneHasTranslation(wire)) {
                DecodeBone(wire, bone);
                translations_[wire.name] = bone.t;
            } else if (auto known = translations_.find(wire.name); known != translations_.end()) {
                bone.t = known->second;
                DecodeBone(wire, bone);
            } else {
                continue; // translation unknown until the next keyframe: leave the bone to the receiver's mesh
            }
            frame->names[base + i] = wire.name;
        }
        while (frames_.size() > 16) frames_.pop_front();
        lastArrival_ = arrival;
        return true;
    }

    // senderMs is a sender clock timestamp (milliseconds, may be fractional).
    bool Sample(double senderMs, PoseFrame& out) const {
        const PoseFrame* before = nullptr;
        const PoseFrame* after = nullptr;
        for (const auto& f : frames_) {
            if (!f.Complete()) continue;
            if (Offset(senderMs, f.timeMs) >= 0) before = &f;
            else if (!after) after = &f;
        }
        if (!before && !after) return false;
        if (!before || !after) {
            out = before ? *before : *after;
            return true;
        }
        double span = Delta(after->timeMs, before->timeMs);
        float k = span > 0 ? static_cast<float>(std::clamp(Offset(senderMs, before->timeMs) / span, 0.0, 1.0)) : 1.f;
        out.timeMs = before->timeMs;
        out.flags = after->flags;
        out.health = after->health;
        out.chunks = after->chunks;
        out.chunkMask = after->chunkMask;
        for (int i = 0; i < 3; ++i) out.root[i] = before->root[i] + (after->root[i] - before->root[i]) * k;
        out.yaw = before->yaw + std::remainder(after->yaw - before->yaw, 360.f) * k;
        out.names = after->names;
        out.bones.resize(after->bones.size());
        for (size_t i = 0; i < after->names.size(); ++i) {
            if (!after->names[i]) continue;
            bool same = i < before->names.size() && before->names[i] == after->names[i];
            out.bones[i] = same ? BlendPose(before->bones[i], after->bones[i], k) : after->bones[i];
        }
        return true;
    }

    const PoseFrame* Latest() const {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it)
            if (it->Complete()) return &*it;
        return nullptr;
    }

private:
    static double Delta(uint32_t a, uint32_t b) { return static_cast<double>(static_cast<int32_t>(a - b)); }
    static double Offset(double senderMs, uint32_t t) {
        double d = std::fmod(senderMs - static_cast<double>(t), 4294967296.0);
        if (d >= 2147483648.0) d -= 4294967296.0;
        if (d < -2147483648.0) d += 4294967296.0;
        return d;
    }
    std::deque<PoseFrame> frames_;
    std::unordered_map<uint32_t, ue::FVector> translations_;
    double lastArrival_ = -100;
};

inline const BonePose* FindBone(const PoseFrame& frame, uint32_t name) {
    for (size_t i = frame.names.size(); i-- > 0;)
        if (frame.names[i] == name) return &frame.bones[i];
    return nullptr;
}

// The player's face rig (everything under the head, plus face-named helpers) is not streamed; the receiver's
// mesh keeps its neutral face. Hands and fingers stay so weapons are gripped correctly.
inline bool BodyBoneSkipped(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* term : {"face", "tongue", "palate", "eye", "lip", "brow", "cheek", "jaw", "teeth", "nose", "mouth"})
        if (lower.find(term) != std::string::npos) return true;
    return false;
}

// Bones to stream for a player body; a skipped bone drops its whole subtree so every sent bone's parent is sent.
inline std::vector<int> BodyBoneSubset(const std::vector<std::string>& names, const std::vector<int>& parents, bool full) {
    std::vector<int> subset;
    std::vector<bool> kept(names.size(), false);
    size_t limit = kPoseBonesPerPacket * kPoseMaxChunks - 1; // room for @root
    for (size_t i = 0; i < names.size() && subset.size() < limit; ++i) {
        int parent = i < parents.size() ? parents[i] : -1;
        bool parentKept = parent < 0 || (static_cast<size_t>(parent) < i && kept[parent]);
        if (!parentKept) continue;
        if (!full && BodyBoneSkipped(names[i])) continue;
        kept[i] = true;
        subset.push_back(static_cast<int>(i));
        std::string lower(names[i]);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!full && lower == "head") kept[i] = false; // keep the head itself, drop everything below it
    }
    return subset;
}

} // namespace coop
