#pragma once
#include "protocol.hpp"

#include <winsock2.h>

#include <cstdint>
#include <functional>
#include <string>

namespace coop {

class UdpPeer {
public:
    bool Open(uint16_t port, const std::string& peerHost, uint16_t peerPort, std::string& diag);
    void Send(const StatePacket& p);
    void SendRaw(const void* data, int size);
    // Returns true if a state packet was received; learns the peer address in host mode.
    // Other datagrams from the pinned peer are passed to `other` without validation.
    bool Receive(StatePacket& out, const std::function<void(const char*, int)>& other = {});
    bool HasPeer() const { return hasPeer_; }
    std::string PeerText() const;

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in peer_{};
    bool hasPeer_ = false;
    int lastSendError_ = 0;
    int lastReceiveError_ = 0;
};

} // namespace coop
