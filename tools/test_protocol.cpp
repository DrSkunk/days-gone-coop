#include "../src/coop/protocol.hpp"

#include <cassert>
#include <cstdio>
#include <limits>

int main() {
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
    std::puts("C++ protocol validation and sequence tests passed");
}
