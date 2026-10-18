#include "../src/coop/protocol.hpp"
#include "../src/coop/combat.hpp"
#include "../src/coop/ue.hpp"
#include "../src/coop/motion.hpp"

#include <cassert>
#include <cstdio>
#include <limits>
#include <cstddef>
#include <vector>

coop::StatePacket MovingPacket(uint32_t milliseconds, float speed = 1000) {
    coop::StatePacket packet{};
    packet.flags = coop::kHasPawn | coop::kHasBike | coop::kInVehicle | coop::kAlive;
    packet.timeMs = milliseconds;
    packet.pos[0] = packet.bikePos[0] = milliseconds * 0.001f * speed;
    return packet;
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
        assert(buffer.Push(packet, 30 + ms * 0.001));
    }
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

int main() {
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
    assert(coop::NewerSequence(43, 42));
    assert(!coop::NewerSequence(42, 42));
    assert(!coop::NewerSequence(41, 42));
    assert(coop::NewerSequence(0, 0xffffffffu));
    assert(!coop::NewerSequence(0xffffffffu, 0));
    TestDamageChannel();
    TestMotionBuffer();
    std::puts("C++ protocol, consent, retransmission, deduplication, and motion/jitter tests passed");
}
