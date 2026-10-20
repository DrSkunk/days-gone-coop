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

// LAN UDP. The socket stays bound for the whole game; the mode decides which partner it talks to.
class UdpPeer final : public Transport {
public:
    enum class Mode { Idle, Listen, Connect, Browse };

    bool Open(uint16_t port, std::string& diag);
    // Idle: ignore everything but beacons. Listen: adopt the first partner that sends state (host).
    // Connect: talk to one host. Browse: like Idle, while the menu lists beaconing hosts.
    void SetIdle();
    void Listen();
    void Browse();
    bool Connect(const std::string& host, uint16_t port, std::string& diag);
    void Connect(uint32_t address, uint16_t port);
    // Forgets a partner adopted in Listen mode so another player can join.
    void DropPartner();
    Mode GetMode() const { return mode_; }
    uint16_t Port() const { return port_; }

    void SendRaw(const void* data, int size) override;
    bool Receive(StatePacket& out, const std::function<void(const char*, int)>& other = {}) override;
    bool HasPeer() const override { return hasPeer_; }
    std::string PeerText() const override;
    // Broadcasts a LAN discovery beacon to each listed port.
    void Broadcast(const void* data, int size, std::initializer_list<uint16_t> ports);
    // Called for each LAN discovery beacon with its payload and the sender IPv4 (network byte order).
    void SetBeaconHandler(std::function<void(const char*, int, uint32_t)> handler) { beacon_ = std::move(handler); }

private:
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_in peer_{};
    Mode mode_ = Mode::Idle;
    uint16_t port_ = 0;
    bool hasPeer_ = false;
    int lastSendError_ = 0;
    int lastReceiveError_ = 0;
    uint16_t lastForeignVersion_ = 0;
    std::function<void(const char*, int, uint32_t)> beacon_;
};

} // namespace coop
