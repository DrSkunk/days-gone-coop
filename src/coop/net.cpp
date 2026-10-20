#include "net.hpp"
#include "log.hpp"
#include "session.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace coop {

bool UdpPeer::Open(uint16_t port, std::string& diag) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        diag = "WSAStartup failed";
        return false;
    }
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        diag = "socket() failed";
        return false;
    }
    u_long nonBlocking = 1;
    if (ioctlsocket(sock_, FIONBIO, &nonBlocking) != 0) {
        diag = "cannot enable non-blocking UDP";
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }
    BOOL broadcast = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof broadcast);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        diag = "bind() failed on UDP port " + std::to_string(port);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }
    port_ = port;
    diag = "UDP port " + std::to_string(port) + " open";
    return true;
}

void UdpPeer::SetIdle() {
    mode_ = Mode::Idle;
    hasPeer_ = false;
}

void UdpPeer::Listen() {
    mode_ = Mode::Listen;
    hasPeer_ = false;
}

void UdpPeer::Browse() {
    mode_ = Mode::Browse;
    hasPeer_ = false;
}

void UdpPeer::DropPartner() {
    if (mode_ == Mode::Listen) hasPeer_ = false;
}

bool UdpPeer::Connect(const std::string& host, uint16_t port, std::string& diag) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        diag = "cannot resolve " + host;
        return false;
    }
    uint32_t address = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    Connect(address, port);
    diag = "partner " + PeerText();
    return true;
}

void UdpPeer::Connect(uint32_t address, uint16_t port) {
    peer_ = {};
    peer_.sin_family = AF_INET;
    peer_.sin_addr.s_addr = address;
    peer_.sin_port = htons(port);
    hasPeer_ = true;
    mode_ = Mode::Connect;
}

void UdpPeer::Broadcast(const void* data, int size, std::initializer_list<uint16_t> ports) {
    if (sock_ == INVALID_SOCKET) return;
    uint16_t sent[4] = {};
    size_t count = 0;
    for (uint16_t port : ports) {
        if (!port || std::find(sent, sent + count, port) != sent + count || count == 4) continue;
        sent[count++] = port;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        to.sin_port = htons(port);
        sendto(sock_, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }
}

bool DecodeIncoming(const char* data, int size, StatePacket& out, uint16_t& lastForeignVersion) {
    if (size == static_cast<int>(sizeof(StatePacket))) {
        uint32_t magic = 0;
        uint16_t version = 0;
        std::memcpy(&magic, data, sizeof magic);
        std::memcpy(&version, data + sizeof magic, sizeof version);
        if (magic == kMagic && version != kProtocolVersion && version != lastForeignVersion) {
            Log("network: ignoring a partner on protocol v%u; this build speaks v%u (both players need the same mod version)",
                version, kProtocolVersion);
            lastForeignVersion = version;
        }
    }
    return DecodeState(data, size, out);
}

void UdpPeer::SendRaw(const void* data, int size) {
    if (!hasPeer_ || sock_ == INVALID_SOCKET) return;
    int n = sendto(sock_, static_cast<const char*>(data), size, 0,
                   reinterpret_cast<const sockaddr*>(&peer_), sizeof peer_);
    int error = n == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error && error != WSAEWOULDBLOCK && error != lastSendError_)
        Log("network: send failed (%d)", error);
    lastSendError_ = error;
}

bool UdpPeer::Receive(StatePacket& out, const std::function<void(const char*, int)>& other) {
    if (sock_ == INVALID_SOCKET) return false;
    char buf[1500];
    // Bound work on the game thread even if unrelated traffic floods the port.
    for (int budget = 0; budget < 128; ++budget) {
        sockaddr_in from{};
        int fromLen = sizeof from;
        int n = recvfrom(sock_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            int error = WSAGetLastError();
            // A previous partner's closed port reports ICMP unreachable as a receive error on Windows.
            if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
            if (error != WSAEWOULDBLOCK && error != lastReceiveError_)
                Log("network: receive failed (%d)", error);
            lastReceiveError_ = error;
            return false;
        }
        lastReceiveError_ = 0;
        uint32_t magic = 0;
        if (n >= 4) std::memcpy(&magic, buf, sizeof magic);
        if (magic == session::kBeaconMagic) {
            if (beacon_) beacon_(buf, n, from.sin_addr.s_addr);
            continue;
        }
        if (hasPeer_ && (from.sin_addr.s_addr != peer_.sin_addr.s_addr ||
                        from.sin_port != peer_.sin_port)) continue;
        if (!hasPeer_ && mode_ != Mode::Listen) continue;
        if (!DecodeIncoming(buf, n, out, lastForeignVersion_)) {
            if (hasPeer_ && other) other(buf, n);
            continue;
        }
        if (!hasPeer_) {
            peer_ = from;
            hasPeer_ = true;
        }
        return true;
    }
    return false;
}

std::string UdpPeer::PeerText() const {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer_.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(peer_.sin_port));
}

} // namespace coop
