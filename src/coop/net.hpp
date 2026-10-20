#pragma once
#include "protocol.hpp"

#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <string>

namespace coop {

// One partner, unreliable datagrams. Implemented by LAN UDP and by the experimental Steam transport.
class Transport {
public:
    virtual ~Transport() = default;
    void Send(const StatePacket& p) { SendRaw(&p, sizeof p); }
    virtual void SendRaw(const void* data, int size) = 0;
    // Returns true if a state packet was received. Other datagrams from the current partner are passed
    // to `other` without validation.
    virtual bool Receive(StatePacket& out, const std::function<void(const char*, int)>& other = {}) = 0;
    virtual bool HasPeer() const = 0;
    virtual std::string PeerText() const = 0;
    virtual void Poll(double /*now*/) {}
};

// Decodes a state packet; logs once when a partner speaks another protocol version.
bool DecodeIncoming(const char* data, int size, StatePacket& out, uint16_t& lastForeignVersion);

class UdpPeer final : public Transport {
public:
    bool Open(uint16_t port, const std::string& peerHost, uint16_t peerPort, std::string& diag);
    void SendRaw(const void* data, int size) override;
    // Learns the peer address in host mode.
    bool Receive(StatePacket& out, const std::function<void(const char*, int)>& other = {}) override;
    bool HasPeer() const override { return hasPeer_; }
    std::string PeerText() const override;

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in peer_{};
    bool hasPeer_ = false;
    int lastSendError_ = 0;
    int lastReceiveError_ = 0;
    uint16_t lastForeignVersion_ = 0;
};

} // namespace coop
