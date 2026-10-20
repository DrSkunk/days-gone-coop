#include "../src/coop/protocol.hpp"
#include "../src/coop/combat.hpp"
#include "../src/coop/ue.hpp"
#include "../src/coop/motion.hpp"
#include "../src/coop/frameclock.hpp"
#include "../src/coop/debug_commands.hpp"
#include "../src/coop/pose.hpp"
#include "../src/coop/enemy.hpp"
#include "../src/coop/gear.hpp"
#include "../src/coop/steam_abi.hpp"
#include "../src/coop/save_policy.hpp"
#include "../src/coop/savesync.hpp"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <limits>
#include <cstddef>
#include <string>
#include <vector>

coop::StatePacket MovingPacket(uint32_t milliseconds, float speed = 1000) {
    coop::StatePacket packet{};
    packet.flags = coop::kHasPawn | coop::kHasBike | coop::kInVehicle | coop::kAlive;
    packet.timeMs = milliseconds;
    packet.pos[0] = packet.bikePos[0] = milliseconds * 0.001f * speed;
    return packet;
}

void TestFrameClock() {
    using namespace coop;
    // 60 Hz frames whose plugin callback runs 0-8 ms after frame start.
    FrameClock clock;
    uint32_t seed = 12345;
    auto noise = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % 8001 / 1e6;
    };
    double previous = 0, worstStep = 0, worstRawStep = 0, previousWall = 0;
    for (int frame = 0; frame < 600; ++frame) {
        double start = 100 + frame / 60.0, wall = start + noise();
        double now = clock.Update(wall, static_cast<float>(frame / 60.0), true);
        assert(now >= previous);
        if (frame > 60) {
            worstStep = std::max(worstStep, std::fabs(now - previous - 1 / 60.0));
            worstRawStep = std::max(worstRawStep, std::fabs(wall - previousWall - 1 / 60.0));
            assert(std::fabs(now - wall) < 0.01);
        }
        previous = now;
        previousWall = wall;
    }
    assert(worstRawStep > 0.005);
    assert(worstStep < 0.0005);
    assert(clock.Resyncs() == 0);

    // A world change resets engine time; the clock falls back to the wall clock without going backwards.
    double before = clock.Now();
    double after = clock.Update(before + 0.016, 0.5f, true);
    assert(after >= before && std::fabs(after - (before + 0.016)) < 1e-9);
    assert(clock.Fallbacks() == 1);
    assert(std::fabs(clock.Update(after + 0.016, 0.516f, true) - (after + 0.016)) < 0.002);

    // Missing engine time and long hitches use the wall clock.
    before = clock.Now();
    assert(std::fabs(clock.Update(before + 0.02, 0, false) - (before + 0.02)) < 1e-9);
    assert(std::fabs(clock.Update(before + 0.04, 9, true) - (before + 0.04)) < 1e-9);
    assert(std::fabs(clock.Update(before + 1.04, 10, true) - (before + 1.04)) < 1e-9);

    // Engine time running slower than the wall clock is resynchronized rather than drifting.
    FrameClock drifting;
    double wall = 0;
    for (int frame = 0; frame < 200; ++frame) {
        wall += 1 / 30.0;
        double now = drifting.Update(wall, static_cast<float>(frame / 60.0), true);
        assert(std::fabs(now - wall) <= 0.1 + 1e-9);
    }
    assert(drifting.Resyncs() > 0);

    // The clock never moves backwards, even if the wall clock does.
    FrameClock monotonic;
    monotonic.Update(5, 0, false);
    assert(monotonic.Update(4, 0, false) == 5);
    assert(monotonic.Update(std::numeric_limits<double>::quiet_NaN(), 0, false) == 5);
}

void TestMotionBuffer() {
    using namespace coop;
    MotionBuffer buffer;
    MotionSnapshot out{};
    assert(!buffer.Sample(0, out));
    assert(buffer.Push(MovingPacket(0), 10));
    assert(buffer.Push(MovingPacket(50), 10.1));
    assert(buffer.Push(MovingPacket(100), 10.1));
    assert(std::fabs(buffer.Latest().t - 0.1) < 1e-6);
    assert(std::fabs(buffer.BikeSpeed() - 1000) < 0.01);
    assert(buffer.Sample(10.1, out));
    assert(buffer.Sample(10.2, out));
    assert(out.pos.x > 50 && out.pos.x < 150);
    assert(std::fabs(out.pos.x - buffer.PlaybackTime() * 1000) < 0.01);
    assert(!buffer.Push(MovingPacket(90), 10.21));
    assert(!buffer.Push(MovingPacket(110), std::numeric_limits<double>::quiet_NaN()));
    assert(buffer.Push(MovingPacket(100), 10.21));
    assert(buffer.Sample(10.3, out));
    assert(out.bikePos.x > 150 && out.bikePos.x <= 250.01);
    assert(buffer.Sample(11, out));
    assert(std::fabs(out.bikePos.x - 250) < 0.01);
    assert(std::fabs(out.pos.x - 250) < 0.01);
    assert(buffer.Stats().held > 0);
    assert(buffer.Push(MovingPacket(1100), 11.01));
    assert(buffer.Sample(11.02, out));
    assert(out.pos.x > 800 && out.pos.x < 1200);

    buffer.Clear();
    auto packet = MovingPacket(0);
    packet.timeMs = 0xfffffff0u;
    assert(buffer.Push(packet, 20));
    packet.timeMs = 0x22;
    packet.pos[0] = packet.bikePos[0] = 50;
    assert(buffer.Push(packet, 20.05));
    assert(std::fabs(buffer.Latest().t - 0.05) < 1e-6);
    packet.timeMs += 50;
    packet.pos[0] = packet.bikePos[0] = 5000;
    assert(buffer.Push(packet, 20.1));
    assert(buffer.Sample(20.1, out));
    assert(out.pos.x == 5000);

    buffer.Clear();
    for (uint32_t ms : {0u, 50u, 100u}) {
        packet = MovingPacket(ms);
        packet.flags = kHasPawn | kAlive;
        packet.pos[0] = 120;
        packet.vel[0] = 1000;
        packet.actions = kAiming | kCrouched;
        packet.weaponType = kWeaponCrossbow;
        packet.shotSequence = 4;
        assert(buffer.Push(packet, 30 + ms * 0.001));
    }
    assert(buffer.Latest().actions == (kAiming | kCrouched));
    assert(buffer.Latest().weaponType == kWeaponCrossbow && buffer.Latest().shotSequence == 4);
    for (int frame = 0; frame < 60; ++frame) {
        assert(buffer.Sample(30.1 + frame / 60.0, out));
        assert(out.pos.x == 120 && out.vel.x == 0);
    }

    buffer.Clear();
    packet = MovingPacket(0);
    packet.yaw = packet.bikeRotation[1] = 179;
    assert(buffer.Push(packet, 40));
    packet = MovingPacket(100);
    packet.yaw = packet.bikeRotation[1] = -179;
    assert(buffer.Push(packet, 40.1));
    assert(buffer.Sample(40.1, out));
    assert(buffer.Sample(40.2, out));
    assert(std::fabs(out.yaw) > 170 && std::fabs(out.bikeRotation.yaw) > 170);

    buffer.Clear();
    struct Delivery { StatePacket packet; double arrival; };
    std::vector<Delivery> deliveries;
    for (uint32_t index = 0; index < 300; ++index) {
        if (index % 7 == 3) continue;
        uint32_t ms = index * 1000 / 30;
        double jitter = (index % 5) * 0.004;
        deliveries.push_back({MovingPacket(ms, 2000), 50 + ms * 0.001 + jitter});
    }
    size_t delivered = 0;
    float previous = 0;
    uint64_t checked = 0;
    for (int frame = 0; frame < 540; ++frame) {
        double now = 50 + frame / 60.0;
        while (delivered < deliveries.size() && deliveries[delivered].arrival <= now) {
            assert(buffer.Push(deliveries[delivered].packet, now));
            ++delivered;
        }
        assert(buffer.Sample(now, out));
        if (frame > 120) {
            float step = out.bikePos.x - previous;
            assert(step >= 2000.f / 60 * 0.979f && step <= 2000.f / 60 * 1.021f);
            assert(std::fabs(out.bikePos.x - buffer.PlaybackTime() * 2000) < 0.02);
            ++checked;
        }
        previous = out.bikePos.x;
    }
    assert(checked > 400);
    assert(buffer.DelaySeconds() >= 0.08 && buffer.DelaySeconds() <= 0.25);
    assert(buffer.Stats().extrapolated < 20);
}

void TestDamageChannel() {
    using namespace coop;
    StatePacket a{}, b{};
    a.session = 11; a.consent = 2; a.targetSession = 22;
    b.session = 22; b.consent = 3; b.targetSession = 11;
    a.flags = b.flags = kHasPawn | kAlive | kFriendlyFire;
    DamageChannel outgoing, incoming;
    outgoing.Configure(a.session, a.consent, b, false);
    assert(!outgoing.Queue(2, 10, 0));
    outgoing.Configure(a.session, a.consent, b, true);
    incoming.Configure(b.session, b.consent, a, true);
    assert(!outgoing.Queue(0, 10, 0));
    assert(!outgoing.Queue(2, 1001, 0));
    assert(!outgoing.Queue(2, std::numeric_limits<float>::quiet_NaN(), 0));
    assert(outgoing.Queue(2, 20, 0));
    assert(outgoing.Queue(1, 30, 0));
    outgoing.Fill(a);
    assert(a.attackId == 1 && a.targetConsent == b.consent && a.damage == 20);
    assert(incoming.Accept(a));
    ++a.seq;
    assert(!incoming.Accept(a));
    incoming.Fill(b);
    auto wrong = b;
    ++wrong.targetConsent;
    outgoing.Acknowledge(wrong);
    assert(outgoing.Pending() == 2);
    outgoing.Acknowledge(b);
    assert(outgoing.Pending() == 1);
    outgoing.Fill(a);
    assert(a.attackId == 2 && a.damage == 30);
    assert(incoming.Accept(a));
    incoming.Fill(b);
    outgoing.Acknowledge(b);
    outgoing.Fill(a);
    assert(outgoing.Pending() == 0 && a.attackKind == 0 && a.damage == 0);
    assert(outgoing.Queue(1, 30, 1));
    assert(!outgoing.Expire(4));
    assert(outgoing.Expire(4.001));
    assert(outgoing.Pending() == 0);
    assert(outgoing.Queue(2, 10, 5));
    outgoing.Fill(a);
    auto stale = a;
    ++b.consent;
    incoming.Configure(b.session, b.consent, a, true);
    assert(!incoming.Accept(stale));
    outgoing.Configure(a.session, a.consent, b, true);
    assert(outgoing.Pending() == 0);
    assert(outgoing.Queue(2, 10, 5));
    outgoing.Configure(a.session, a.consent, b, false);
    assert(outgoing.Pending() == 0 && !outgoing.Queue(2, 10, 5));
    b.flags &= ~kFriendlyFire;
    outgoing.Configure(a.session, a.consent, b, true);
    assert(!outgoing.Queue(2, 10, 5));
    b.flags |= kFriendlyFire;
    outgoing.Configure(a.session, a.consent, b, true);
    for (int i = 0; i < 64; ++i) assert(outgoing.Queue(2, 10, 5));
    assert(!outgoing.Queue(2, 10, 5));
    ++b.session;
    outgoing.Configure(a.session, a.consent, b, true);
    assert(outgoing.Pending() == 0);
}

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

bool SameRotation(const float a[4], const float b[4], float eps) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    return std::fabs(std::fabs(dot) - 1.f) <= eps;
}

void TestPose() {
    using namespace coop;
    static_assert(kPosePacketMaxBytes == 44 + 64 * 17 && kPosePacketMaxBytes <= 1200, "pose datagrams stay below the safe internet payload");
    assert(BoneHash("Spine") == BoneHash("spine") && BoneHash("spine") != BoneHash("spine1") && BoneHash("") != 0);
    assert(kRootBoneHash == BoneHash("@ROOT"));

    // Quantisation keeps translation within 0.01 cm and rotation within a tiny fraction of a degree.
    for (int i = 0; i < 2000; ++i) {
        BonePose bone;
        float pitch = static_cast<float>((i * 37) % 360 - 180), yaw = static_cast<float>((i * 53) % 360 - 180),
              roll = static_cast<float>((i * 71) % 360 - 180);
        RotatorToQuat(pitch, yaw, roll, bone.q);
        bone.t = {i * 0.317f - 300.f, 250.f - i * 0.21f, (i % 97) * 1.37f};
        BonePose decoded;
        assert(DecodeBone(EncodeBone(BoneHash("b"), bone), decoded));
        assert(SameRotation(bone.q, decoded.q, 2e-6f));
        assert(Near(decoded.t.x, bone.t.x, 0.0101f) && Near(decoded.t.y, bone.t.y, 0.0101f) && Near(decoded.t.z, bone.t.z, 0.0101f));
    }
    BonePose far;
    far.t = {5000, -5000, 0};
    BonePose clamped;
    DecodeBone(EncodeBone(1, far), clamped);
    assert(Near(clamped.t.x, 655.34f, 0.01f) && Near(clamped.t.y, -655.34f, 0.01f));

    // Rotator <-> quaternion match Unreal's conventions away from gimbal lock.
    for (float pitch : {-80.f, -30.f, 0.f, 45.f, 85.f})
        for (float yaw : {-170.f, -90.f, 0.f, 33.f, 179.f})
            for (float roll : {-120.f, 0.f, 15.f, 175.f}) {
                float q[4], p, y, r;
                RotatorToQuat(pitch, yaw, roll, q);
                QuatToRotator(q, p, y, r);
                assert(Near(p, pitch, 0.01f) && Near(std::remainder(y - yaw, 360.f), 0, 0.01f) &&
                       Near(std::remainder(r - roll, 360.f), 0, 0.01f));
            }
    {
        float q[4];
        RotatorToQuat(0, 90, 0, q);
        auto v = QuatRotate(q, {1, 0, 0});
        assert(Near(v.x, 0, 1e-5f) && Near(v.y, 1, 1e-5f) && Near(v.z, 0, 1e-5f)); // +90 yaw turns X into Y
    }

    // Parent-relative poses recompose into component space.
    BonePose parent, child;
    RotatorToQuat(10, 40, -5, parent.q);
    parent.t = {10, 20, 30};
    RotatorToQuat(-25, 5, 60, child.q);
    child.t = {-4, 50, 7};
    auto local = RelativePose(child, parent);
    auto composed = ComposePose(local, parent);
    assert(SameRotation(composed.q, child.q, 1e-5f));
    assert(Near(composed.t.x, child.t.x, 1e-3f) && Near(composed.t.y, child.t.y, 1e-3f) && Near(composed.t.z, child.t.z, 1e-3f));
    auto mid = BlendPose(parent, child, 0.5f);
    assert(Near(mid.t.x, 3, 1e-4f) && Near(mid.t.y, 35, 1e-4f));

    // A 529-bone frame splits into nine validated chunks that reassemble regardless of arrival order.
    std::vector<uint32_t> names;
    std::vector<BonePose> bones;
    for (int i = 0; i < 529; ++i) {
        names.push_back(BoneHash("bone" + std::to_string(i)));
        BonePose b;
        b.t = {static_cast<float>(i), 0, 0};
        bones.push_back(b);
    }
    auto make = [&](uint32_t timeMs, float offset, std::vector<std::vector<char>>& out) {
        PoseHeader header{};
        header.kind = kPoseBody;
        header.session = 7;
        header.timeMs = timeMs;
        header.yaw = offset;
        auto shifted = bones;
        for (auto& b : shifted) b.t.y = offset;
        out.clear();
        assert(EncodePose(header, names, shifted, [&](const char* bytes, size_t size) {
            out.emplace_back(bytes, bytes + size);
        }));
    };
    std::vector<std::vector<char>> a, b;
    make(1000, 0, a);
    make(1033, 10, b);
    assert(a.size() == 9 && a.back().size() == 44 + (529 - 8 * 64) * kPoseBoneFullBytes);
    PoseBuffer buffer;
    PosePacket packet;
    for (auto it = a.rbegin(); it != a.rend(); ++it) {
        assert(DecodePose(it->data(), static_cast<int>(it->size()), packet));
        assert(buffer.Push(packet, 1.0));
    }
    assert(!buffer.Push(packet, 1.0)); // duplicate chunk
    PoseFrame frame;
    assert(buffer.Sample(1000, frame) && frame.Complete());
    assert(FindBone(frame, names[528]) && Near(FindBone(frame, names[528])->t.x, 528, 0.02f));
    // An incomplete newer frame is never sampled.
    for (size_t i = 0; i + 1 < b.size(); ++i) {
        assert(DecodePose(b[i].data(), static_cast<int>(b[i].size()), packet));
        assert(buffer.Push(packet, 1.03));
    }
    assert(buffer.Sample(1020, frame) && Near(FindBone(frame, names[3])->t.y, 0, 0.02f));
    assert(DecodePose(b.back().data(), static_cast<int>(b.back().size()), packet));
    assert(buffer.Push(packet, 1.04));
    assert(buffer.Sample(1016.5, frame) && Near(FindBone(frame, names[3])->t.y, 5, 0.03f) && Near(frame.yaw, 5, 0.01f));
    assert(buffer.Sample(5000, frame) && Near(FindBone(frame, names[3])->t.y, 10, 0.02f));

    // Malformed packets are rejected.
    auto bad = a[0];
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()) - 1, packet));
    reinterpret_cast<PoseHeader*>(bad.data())->magic ^= 1;
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    bad = a[0];
    reinterpret_cast<PoseHeader*>(bad.data())->kind = kPoseEnemy; // enemies need an entity id
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    bad = a[0];
    reinterpret_cast<PoseHeader*>(bad.data())->chunk = 9;
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    bad = a[0];
    reinterpret_cast<PoseHeader*>(bad.data())->health = std::numeric_limits<float>::quiet_NaN();
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    bad = a[0];
    bad[sizeof(PoseHeader) + 4] = 8; // unknown bone flag
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    bad = a[0];
    bad[sizeof(PoseHeader) + 4] &= ~kBoneHasTranslation; // the declared bones no longer fill the datagram
    assert(!DecodePose(bad.data(), static_cast<int>(bad.size()), packet));
    std::vector<uint32_t> tooMany(kPoseBonesPerPacket * kPoseMaxChunks + 1, 1);
    std::vector<BonePose> tooManyBones(tooMany.size());
    assert(!EncodePose(PoseHeader{}, tooMany, tooManyBones, [](const char*, size_t) { assert(false); }));

    // With a tracker, unchanged translations are omitted between keyframes and restored by the receiver.
    {
        std::vector<uint32_t> hashes{BoneHash("hips"), BoneHash("spine"), BoneHash("hand")};
        std::vector<BonePose> pose(3);
        pose[0].t = {1, 2, 3};
        pose[1].t = {0, 0, 40};
        pose[2].t = {5, 0, 0};
        PoseTranslationTracker tracker;
        PoseBuffer receiver, late;
        auto send = [&](uint32_t timeMs, PoseBuffer& to, size_t& bytes) {
            PoseHeader header{};
            header.kind = kPoseBody;
            header.timeMs = timeMs;
            bytes = 0;
            assert(EncodePose(header, hashes, pose, [&](const char* data, size_t size) {
                PosePacket in;
                bytes += size;
                assert(DecodePose(data, static_cast<int>(size), in));
                assert(to.Push(in, timeMs / 1000.0));
            }, &tracker));
        };
        size_t bytes = 0;
        send(0, receiver, bytes);
        assert(bytes == 44 + 3 * kPoseBoneFullBytes); // first frame is a keyframe
        send(300, receiver, bytes);
        assert(bytes == 44 + 3 * kPoseBoneRotationBytes); // nothing moved for longer than the hold time
        pose[0].t = {10, 2, 3};
        send(350, receiver, bytes);
        assert(bytes == 44 + kPoseBoneFullBytes + 2 * kPoseBoneRotationBytes);
        send(500, receiver, bytes); // still inside the hold window: repeated in case the last datagram was lost
        assert(bytes == 44 + kPoseBoneFullBytes + 2 * kPoseBoneRotationBytes);
        send(650, receiver, bytes);
        assert(bytes == 44 + 3 * kPoseBoneRotationBytes);
        PoseFrame got;
        assert(receiver.Sample(650, got) && Near(FindBone(got, hashes[0])->t.x, 10, 0.02f) &&
               Near(FindBone(got, hashes[1])->t.z, 40, 0.02f));
        // A receiver that missed the keyframe leaves rotation-only bones alone until the next keyframe.
        send(700, late, bytes);
        assert(late.Sample(700, got) && !FindBone(got, hashes[1]));
        send(1000, late, bytes);
        assert(bytes == 44 + 3 * kPoseBoneFullBytes);
        assert(late.Sample(1000, got) && Near(FindBone(got, hashes[1])->t.z, 40, 0.02f));
    }

    // The player stream drops the face rig below the head but keeps the head, hands and fingers.
    {
        std::vector<std::string> body{"root", "hips", "spine", "neck", "Head", "face_root", "jaw", "hair_01",
                                      "r_hand", "r_index_01", "l_eye_helper", "r_thumb_01"};
        std::vector<int> parents{-1, 0, 1, 2, 3, 4, 4, 4, 2, 8, 2, 8};
        std::vector<int> expected{0, 1, 2, 3, 4, 8, 9, 11};
        assert(BodyBoneSubset(body, parents, false) == expected);
        assert(BodyBoneSubset(body, parents, true).size() == body.size());
    }
}

void TestEnemy() {
    using namespace coop;
    // Descriptors round-trip and reject malformed paths, sizes and counts.
    EnemyInfoPacket info, decoded;
    std::vector<std::string> paths{"/Game/Characters/Freaker/Swarmer/parts_combo/swarmer_horde_c.swarmer_horde_c",
                                   "/Game/Characters/Human/hat.hat"};
    assert(EncodeEnemyInfo(3, 9, paths, info));
    const char* raw = reinterpret_cast<const char*>(&info);
    assert(DecodeEnemyInfo(raw, sizeof info, decoded) && decoded.entity == 9 && decoded.paths == 2 &&
           std::string(decoded.path[0]) == paths[0] && std::string(decoded.path[1]) == paths[1]);
    assert(!DecodeEnemyInfo(raw, sizeof info - 1, decoded));
    auto bad = info;
    bad.entity = 0;
    assert(!DecodeEnemyInfo(reinterpret_cast<const char*>(&bad), sizeof bad, decoded));
    bad = info;
    bad.path[0][0] = 'G'; // not an object path
    assert(!DecodeEnemyInfo(reinterpret_cast<const char*>(&bad), sizeof bad, decoded));
    bad = info;
    std::memset(bad.path[1], 'a', kEnemyPathLength); // unterminated
    assert(!DecodeEnemyInfo(reinterpret_cast<const char*>(&bad), sizeof bad, decoded));
    bad = info;
    bad.path[2][0] = '/'; // data past the declared count
    assert(!DecodeEnemyInfo(reinterpret_cast<const char*>(&bad), sizeof bad, decoded));
    bad = info;
    bad.path[0][5] = '\n';
    assert(!DecodeEnemyInfo(reinterpret_cast<const char*>(&bad), sizeof bad, decoded));
    assert(!EncodeEnemyInfo(3, 9, {}, info));
    assert(!EncodeEnemyInfo(3, 0, paths, info));

    // Face/finger subtrees are dropped without orphaning any streamed bone.
    std::vector<std::string> names{"Align", "hips", "spine", "neck", "Head", "face_root", "face_lip_l", "jaw",
                                   "l_hand", "l_index_01", "l_index_02", "l_thumb_01", "r_leg", "r_foot", "cam01"};
    std::vector<int> parents{-1, 0, 1, 2, 3, 4, 5, 4, 2, 8, 9, 8, 1, 12, 0};
    auto subset = EnemyBoneSubset(names, parents);
    std::vector<int> expected{0, 1, 2, 3, 4, 7, 8, 12, 13};
    assert(subset == expected);
    std::vector<bool> kept(names.size());
    for (int i : subset) {
        kept[i] = true;
        assert(parents[i] < 0 || kept[parents[i]]);
    }
    // Large rigs are capped so a frame never exceeds three packets including the root.
    std::vector<std::string> many(400, "bone");
    std::vector<int> chain(400);
    for (int i = 0; i < 400; ++i) chain[i] = i - 1;
    assert(EnemyBoneSubset(many, chain).size() == kEnemyMaxBones);

    // An enemy frame carries its entity and absolute root through the pose buffer.
    PoseHeader header{};
    header.kind = kPoseEnemy;
    header.session = 3;
    header.timeMs = 500;
    header.entity = 9;
    header.root[0] = 250000.f;
    header.flags = kPoseAlive;
    std::vector<uint32_t> hashes{BoneHash("hips"), kRootBoneHash};
    std::vector<BonePose> bones(2);
    PoseBuffer buffer;
    assert(EncodePose(header, hashes, bones, [&](const char* data, size_t size) {
        PosePacket in;
        assert(DecodePose(data, static_cast<int>(size), in) && in.header.entity == 9);
        assert(buffer.Push(in, 1.0));
    }));
    PoseFrame frame;
    assert(buffer.Sample(500, frame) && frame.root[0] == 250000.f && (frame.flags & kPoseAlive) && FindBone(frame, kRootBoneHash));
}

void TestGear() {
    static_assert(sizeof(coop::GearPacket) == 240, "gear wire size");
    using namespace coop;
    GearPacket packet, decoded;
    float q[4] = {0, 0, 0.7071068f, 0.7071068f}, t[3] = {1, -2, 3}, scale[3] = {1, 1, 1};
    const std::string path = "/Game/Weapons/Pistol/pistol_a.pistol_a";
    assert(EncodeGear(7, kGearSkeletal, "weapon_r", 0, path, q, t, scale, packet));
    assert(DecodeGear(reinterpret_cast<const char*>(&packet), sizeof packet, decoded));
    assert(decoded.kind == kGearSkeletal && decoded.session == 7 && !std::strcmp(decoded.socket, "weapon_r"));
    assert(path == decoded.path && decoded.t[1] == -2 && SameGear(packet, decoded));
    assert(!DecodeGear(reinterpret_cast<const char*>(&packet), sizeof packet - 1, decoded));
    // Empty hands carry no socket or path; anything else must be a rooted object path and a sane transform.
    GearPacket empty;
    assert(EncodeGear(7, kGearNone, {}, 0, {}, nullptr, nullptr, nullptr, empty) && !SameGear(empty, packet));
    auto bad = [&](auto mutate) {
        GearPacket copy = packet;
        mutate(copy);
        return !DecodeGear(reinterpret_cast<const char*>(&copy), sizeof copy, decoded);
    };
    assert(bad([](GearPacket& p) { p.kind = 3; }));
    assert(bad([](GearPacket& p) { p.socket[0] = 0; }));
    assert(bad([](GearPacket& p) { p.path[0] = 'G'; }));
    assert(bad([](GearPacket& p) { p.path[5] = ' '; }));
    assert(bad([](GearPacket& p) { std::memset(p.socket, 'a', sizeof p.socket); }));
    assert(bad([](GearPacket& p) { p.q[3] = 2; }));
    assert(bad([](GearPacket& p) { p.t[0] = std::numeric_limits<float>::quiet_NaN(); }));
    assert(bad([](GearPacket& p) { p.t[2] = 900; }));
    assert(bad([](GearPacket& p) { p.scale[1] = 0; }));
    assert(bad([](GearPacket& p) { p.kind = kGearNone; }));
    assert(!EncodeGear(7, kGearStatic, "", 0, path, q, t, scale, packet));
    // Sub-millimetre jitter is the same mount; a moved item is not.
    GearPacket jitter = decoded;
    jitter.t[0] += 0.05f;
    assert(SameGear(jitter, decoded));
    jitter.t[0] += 1;
    assert(!SameGear(jitter, decoded));
    std::puts("gear ok");
}

void TestSteamAbi() {
    using coop::steam::ParseConnectLobby;
    // Layout checks live in steam_abi.hpp as static_asserts; this exercises the invite parser.
    assert(ParseConnectLobby("DaysGone.exe +connect_lobby 109775241234567890") == 109775241234567890ull);
    assert(ParseConnectLobby("\"C:\\Games\\DaysGone.exe\" -dx11 +connect_lobby\t42 -windowed") == 42);
    assert(ParseConnectLobby("+connect_lobby 7\"") == 7);
    assert(!ParseConnectLobby("DaysGone.exe"));
    assert(!ParseConnectLobby("+connect_lobby"));
    assert(!ParseConnectLobby("+connect_lobby "));
    assert(!ParseConnectLobby("+connect_lobbyx 12"));
    assert(!ParseConnectLobby("+connect_lobby 12abc"));
    assert(!ParseConnectLobby("+connect_lobby -5"));
    assert(!ParseConnectLobby("+connect_lobby 18446744073709551616"));
    assert(ParseConnectLobby("+connect_lobby 18446744073709551615") == 18446744073709551615ull);
    assert(coop::steam::GameAppId(0x010000000013379Cull) == coop::steam::kDaysGoneAppId);
    std::puts("steam abi ok");
}

void TestSavePolicy() {
    using namespace coop::savepolicy;
    const std::wstring base = L"C:\\users\\steamuser\\AppData\\Local\\BendGame\\Saved\\7656\\SaveGames";
    assert(SaveGamesRelative(base + L"\\autosave04.sav") == std::wstring(L"autosave04.sav"));
    assert(SaveGamesRelative(L"c:/USERS/x/BendGame/SAVED/7656/savegames/sub/profile.sav") == std::wstring(L"sub\\profile.sav"));
    assert(SaveGamesRelative(base) == std::wstring());
    assert(SaveGamesRelative(base + L"\\") == std::wstring());
    assert(!SaveGamesRelative(L"C:\\BendGame\\Saved\\Config\\WindowsNoEditor\\Engine.ini"));
    assert(!SaveGamesRelative(L"C:\\BendGame\\Saved\\SaveGamesBackup\\a.sav"));
    assert(!SaveGamesRelative(L"C:\\Games\\SaveGames\\a.sav"));
    assert(SafeRelative(L"autosave01.sav") && SafeRelative(L"") && SafeRelative(L"a\\..b"));
    assert(!SafeRelative(L"..\\x.sav") && !SafeRelative(L"a\\..\\..\\x") && !SafeRelative(L"\\x") && !SafeRelative(L"C:x"));
    assert(SandboxPath(L"D:\\sbx", L"a.sav") == L"D:\\sbx\\a.sav" && SandboxPath(L"D:\\sbx", L"") == L"D:\\sbx");
    assert(!IsWriteOpen(0x80000000u, kOpenExisting));  // GENERIC_READ
    assert(IsWriteOpen(0x80000000u, kOpenAlways));
    assert(IsWriteOpen(kGenericWrite, kOpenExisting) && IsWriteOpen(kDelete, kOpenExisting));
    assert(IsWriteOpen(kWriteAttributes, kOpenExisting) && IsWriteOpen(kAppendData, kOpenAlways));
    assert(IsWriteOpen(0x80000000u, kCreateAlways) && IsWriteOpen(0x80000000u, kTruncateExisting));
    assert(NeedsCopyOnWrite(kOpenExisting) && NeedsCopyOnWrite(kOpenAlways));
    assert(!NeedsCopyOnWrite(kCreateAlways) && !NeedsCopyOnWrite(kTruncateExisting) && !NeedsCopyOnWrite(kCreateNew));
    std::puts("save policy ok");
}

std::string Hex(const coop::Sha256::Digest& digest) {
    std::string out;
    char buffer[3];
    for (auto byte : digest) {
        std::snprintf(buffer, sizeof buffer, "%02x", byte);
        out += buffer;
    }
    return out;
}

void TestSaveSync() {
    using namespace coop::savesync;
    assert(Hex(coop::Sha256::Of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Hex(coop::Sha256::Of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string longer = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert(Hex(coop::Sha256::Of(longer.data(), longer.size())) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    coop::Sha256 split;
    std::string million(1000000, 'a');
    for (size_t i = 0; i < million.size(); i += 777) split.Update(million.data() + i, std::min<size_t>(777, million.size() - i));
    assert(Hex(split.Finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    for (auto name : {"autosave01.sav", "profile.sav", "slot-1_A.SAV", "console.sav", "com10.sav", "nul_.sav"}) assert(ValidName(name));
    for (auto name : {"", ".sav", "..\\x.sav", "a/b.sav", "a\\b.sav", ".hidden.sav", "x.txt", "a..sav", "C:x.sav",
                      "abcdefghijklmnopqrstuvwxyz12.sav", "nul.sav", "CON.sav", "com1.sav", "Lpt9.x.sav", "aux.backup.sav"})
        assert(!ValidName(name));

    std::vector<File> files{{"profile.sav", std::vector<uint8_t>(10114, 7)}, {"autosave01.sav", {}},
                            {"checkpoint.sav", std::vector<uint8_t>(3000)}};
    for (size_t i = 0; i < files[2].data.size(); ++i) files[2].data[i] = static_cast<uint8_t>(i * 31);
    auto bundle = BuildBundle(files);
    assert(!bundle.empty());
    std::vector<File> parsed;
    assert(ParseBundle(bundle, parsed) && parsed.size() == 3);
    for (size_t i = 0; i < files.size(); ++i) assert(parsed[i].name == files[i].name && parsed[i].data == files[i].data);
    auto tampered = bundle;
    tampered.back() ^= 1;
    assert(!ParseBundle(tampered, parsed) && parsed.empty());
    auto truncated = bundle;
    truncated.pop_back();
    assert(!ParseBundle(truncated, parsed));
    auto extended = bundle;
    extended.push_back(0);
    assert(!ParseBundle(extended, parsed));
    assert(BuildBundle({}).empty() && BuildBundle({{"../x.sav", {1}}}).empty());
    assert(BuildBundle({{"a.sav", {1}}, {"A.SAV", {2}}}).size() > 0);
    assert(!ParseBundle(BuildBundle({{"a.sav", {1}}, {"A.SAV", {2}}}), parsed)); // case-insensitive duplicate

    // Same bytes as tools/test_fake_peer.py BUNDLE_DIGEST, so the Python test peer stays compatible.
    auto shared = BuildBundle({{"profile.sav", {1, 2, 3}}, {"autosave01.sav", {}}});
    assert(shared.size() == 143 && Hex(coop::Sha256::Of(shared.data(), shared.size())) ==
                                       "0f03dc917d9011dc65edf488090f15183862dceb586457648ad1c0453a017163");

    auto digest = coop::Sha256::Of(bundle.data(), bundle.size());
    Receiver receiver;
    assert(!receiver.Offer(0, 10, digest.data()) && !receiver.Offer(5, 0, digest.data()));
    assert(!receiver.Offer(5, kMaxBundleBytes + 1, digest.data()));
    assert(receiver.Offer(9, static_cast<uint32_t>(bundle.size()), digest.data()));
    uint32_t chunks = ChunkCount(static_cast<uint32_t>(bundle.size()));
    assert(chunks == (bundle.size() + kChunkBytes - 1) / kChunkBytes);
    double now = 0;
    int rounds = 0, sent = 0;
    while (!receiver.Complete() && rounds < 1000) {
        for (uint32_t index : receiver.NextPull(now, 4)) {
            if (++sent % 3 == 0) continue; // drop every third chunk
            size_t offset = index * kChunkBytes, size = std::min(kChunkBytes, bundle.size() - offset);
            assert(receiver.Chunk(9, index, bundle.data() + offset, size));
            assert(!receiver.Chunk(9, index, bundle.data() + offset, size));
        }
        now += 0.1;
        ++rounds;
    }
    assert(receiver.Complete() && receiver.Verified() && receiver.Bundle() == bundle);
    assert(receiver.NextPull(now, 4).empty());
    assert(receiver.Offer(9, static_cast<uint32_t>(bundle.size()), digest.data()) && receiver.Complete());
    Receiver wrong;
    assert(wrong.Offer(3, static_cast<uint32_t>(bundle.size()), digest.data()));
    assert(!wrong.Chunk(4, 0, bundle.data(), kChunkBytes));
    assert(!wrong.Chunk(3, 0, bundle.data(), kChunkBytes - 1));
    assert(!wrong.Chunk(3, chunks, bundle.data(), 1));
    for (uint32_t index = 0; index < chunks; ++index) {
        size_t offset = index * kChunkBytes, size = std::min(kChunkBytes, bundle.size() - offset);
        std::vector<uint8_t> piece(bundle.begin() + offset, bundle.begin() + offset + size);
        if (index == 1) piece[0] ^= 0xff;
        assert(wrong.Chunk(3, index, piece.data(), piece.size()));
    }
    assert(wrong.Complete() && !wrong.Verified());

    Header header = MakeHeader(kRequest, 77, 0), decoded{};
    assert(DecodeHeader(reinterpret_cast<const char*>(&header), sizeof header, decoded) && decoded.session == 77);
    header = MakeHeader(kOffer, 77, 0);
    assert(!DecodeHeader(reinterpret_cast<const char*>(&header), sizeof header, decoded));
    header = MakeHeader(kChunk, 0, 5);
    assert(!DecodeHeader(reinterpret_cast<const char*>(&header), sizeof header, decoded));
    header = MakeHeader(static_cast<Type>(9), 1, 5);
    assert(!DecodeHeader(reinterpret_cast<const char*>(&header), sizeof header, decoded));
    assert(!DecodeHeader(reinterpret_cast<const char*>(&header), sizeof header - 1, decoded));
    std::puts("save sync ok");
}

int main() {
    TestSavePolicy();
    TestSaveSync();
    TestPose();
    TestSteamAbi();
    TestEnemy();
    TestGear();
    using coop::DebugCommand;
    for (auto text : {"", "God", "god", "god-on extra", "spawn", "DestroyAll", "begin\n", "spawn-pack; end"})
        assert(coop::ParseDebugCommand(text) == DebugCommand::unknown);
    assert(coop::ParseDebugCommand("god-on") == DebugCommand::godOn);
    assert(coop::ParseDebugCommand("god-off") == DebugCommand::godOff);
    assert(coop::ParseDebugCommand("spawn-single") == DebugCommand::spawnSingle);
    assert(coop::ParseDebugCommand("spawn-pack") == DebugCommand::spawnPack);
    assert(coop::ParseDebugCommand("begin") == DebugCommand::begin);
    assert(coop::ParseDebugCommand("end") == DebugCommand::end);
    assert(coop::ParseDebugCommand("cleanup") == DebugCommand::cleanup);
    assert(coop::ParseDebugCommand("probe-player") == DebugCommand::probePlayer);
    assert(coop::ParseDebugCommand("probe-enemy") == DebugCommand::probeEnemy);
    assert(coop::ParseDebugCommand("time-shift") == DebugCommand::timeShift);
    assert(coop::ParseDebugCommand("weather-next") == DebugCommand::weatherNext);
    assert(coop::ParseDebugCommand("inspect") == DebugCommand::inspect);
    assert(coop::ParseDebugCommand("status") == DebugCommand::status);
    coop::DebugRequestGate gate;
    gate.Initialize(42);
    assert(!gate.Accept(0) && !gate.Accept(42));
    assert(gate.Accept(43) && !gate.Accept(43));
    gate.Initialize(43);
    assert(!gate.Accept(43));
    assert(gate.Accept(44));
    for (int byte = 0; byte <= 255; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t mask = static_cast<uint8_t>(1 << bit);
            assert(coop::DebugDamageMask(byte, byte ^ mask) == mask);
            assert(coop::DebugMaskedFlag(byte, mask, false) == (byte & ~mask));
            assert(coop::DebugMaskedFlag(byte, mask, true) == (byte | mask));
        }
        assert(coop::DebugDamageMask(byte, byte) == 0);
        assert(coop::DebugDamageMask(byte, byte ^ 3) == 0);
    }
    static_assert(coop::kDebugPackSize <= coop::kDebugEnemyLimit);
    auto point = ue::AlignBoneToSocket({10, 20, 30}, {1, 2, 3}, 0);
    assert(point.x == 9 && point.y == 18 && point.z == 27);
    point = ue::AlignBoneToSocket({10, 20, 30}, {1, 2, 3}, 90);
    assert(std::fabs(point.x - 12) < 0.001f && std::fabs(point.y - 19) < 0.001f && point.z == 27);
    point = ue::AlignBoneToSocket({10, 20, 30}, {1, 2, 3}, -90);
    assert(std::fabs(point.x - 8) < 0.001f && std::fabs(point.y - 21) < 0.001f && point.z == 27);
    auto translated = ue::AlignBoneToSocket({110, -180, 330}, {1, 2, 3}, -90);
    assert(std::fabs(translated.x - point.x - 100) < 0.001f &&
           std::fabs(translated.y - point.y + 200) < 0.001f && translated.z == point.z + 300);
    static_assert(offsetof(coop::StatePacket, session) == 64);
    static_assert(offsetof(coop::StatePacket, attackId) == 80);
    static_assert(offsetof(coop::StatePacket, damage) == 88);
    static_assert(offsetof(coop::StatePacket, bikePos) == 92);
    static_assert(offsetof(coop::StatePacket, bikeRotation) == 104);
    static_assert(offsetof(coop::StatePacket, jacket) == 116);
    static_assert(offsetof(coop::StatePacket, attackKind) == 120);
    static_assert(offsetof(coop::StatePacket, health) == 124);
    static_assert(offsetof(coop::StatePacket, actions) == 128);
    static_assert(offsetof(coop::StatePacket, weaponType) == 129);
    static_assert(offsetof(coop::StatePacket, shotSequence) == 130);
    static_assert(offsetof(coop::StatePacket, worldTimeOfDay) == 134);
    static_assert(offsetof(coop::StatePacket, worldRelativeTime) == 138);
    static_assert(offsetof(coop::StatePacket, worldWeather) == 142);
    static_assert(offsetof(coop::StatePacket, worldTransitionRate) == 143);
    coop::StatePacket packet{};
    packet.magic = coop::kMagic;
    packet.version = coop::kProtocolVersion;
    packet.flags = coop::kHasPawn;
    packet.seq = 42;
    packet.pos[0] = -123.5f;
    std::memset(packet.name, 'x', sizeof packet.name);
    coop::StatePacket decoded{};
    auto data = reinterpret_cast<const char*>(&packet);
    assert(coop::DecodeState(data, sizeof packet, decoded));
    assert(decoded.seq == 42 && decoded.pos[0] == -123.5f && decoded.name[15] == 0);
    assert(!coop::DecodeState(data, sizeof packet - 1, decoded));
    assert(!coop::DecodeState(data, sizeof packet + 1, decoded));
    packet.magic = 0;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.magic = coop::kMagic;
    packet.pos[0] = 0;
    packet.jacket = 7;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.jacket = 6;
    packet.bikeRotation[2] = std::numeric_limits<float>::infinity();
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.bikeRotation[2] = 45;
    packet.flags = coop::kPassenger | coop::kHasPawn;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.flags |= coop::kInVehicle;
    assert(coop::DecodeState(data, sizeof packet, decoded));
    packet.attackKind = 2;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.session = 1; packet.targetSession = 2; packet.consent = 3; packet.targetConsent = 4;
    packet.attackId = 1; packet.damage = 25;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.flags |= coop::kFriendlyFire;
    assert(coop::DecodeState(data, sizeof packet, decoded));
    packet.damage = 1001;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet = {};
    packet.magic = coop::kMagic;
    packet.version = coop::kProtocolVersion;
    packet.flags = coop::kHasPawn;
    packet.version++;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.version = coop::kProtocolVersion;
    packet.flags = 0x8000;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.flags = coop::kHasPawn;
    packet.pos[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.pos[0] = std::numeric_limits<float>::infinity();
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.pos[0] = 1e9f;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.pos[0] = 0;
    packet.actions = 0x80;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.actions = 0;
    packet.weaponType = 4;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.weaponType = coop::kWeaponGun;
    packet.shotSequence = 99;
    packet.flags |= coop::kWorldState;
    packet.worldTimeOfDay = 21.5f;
    packet.worldRelativeTime = 0.75f;
    packet.worldWeather = 3;
    packet.worldTransitionRate = 2;
    assert(coop::DecodeState(data, sizeof packet, decoded));
    assert(decoded.weaponType == coop::kWeaponGun && decoded.shotSequence == 99);
    assert(decoded.worldTimeOfDay == 21.5f && decoded.worldRelativeTime == 0.75f);
    assert(decoded.worldWeather == 3 && decoded.worldTransitionRate == 2);
    packet.worldTimeOfDay = 25;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.worldTimeOfDay = 21.5f;
    packet.worldRelativeTime = -0.1f;
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    packet.worldRelativeTime = 0.75f;
    packet.worldTimeOfDay = std::numeric_limits<float>::quiet_NaN();
    assert(!coop::DecodeState(data, sizeof packet, decoded));
    assert(coop::NewerSequence(43, 42));
    assert(!coop::NewerSequence(42, 42));
    assert(!coop::NewerSequence(41, 42));
    assert(coop::NewerSequence(0, 0xffffffffu));
    assert(!coop::NewerSequence(0xffffffffu, 0));
    TestDamageChannel();
    TestMotionBuffer();
    TestFrameClock();
    std::puts("C++ protocol, combat, motion, frame-clock and debug-control tests passed");
}
