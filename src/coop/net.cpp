#include "net.hpp"
#include "log.hpp"

#include <ws2tcpip.h>

#include <cmath>
#include <cstring>

namespace coop {

bool UdpPeer::Open(uint16_t port, const std::string& peerHost, uint16_t peerPort, std::string& diag) {
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
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        diag = "bind() failed on UDP port " + std::to_string(port);
        return false;
    }
    if (!peerHost.empty()) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(peerHost.c_str(), nullptr, &hints, &res) != 0 || !res) {
            diag = "cannot resolve peer " + peerHost;
            return false;
        }
        peer_ = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        peer_.sin_port = htons(peerPort);
        freeaddrinfo(res);
        hasPeer_ = true;
    }
    diag = "listening on UDP " + std::to_string(port) +
           (hasPeer_ ? ", peer " + PeerText() : ", waiting for a peer to connect (host mode)");
    return true;
}

void UdpPeer::Send(const StatePacket& p) {
    SendRaw(&p, sizeof p);
}

void UdpPeer::SendRaw(const void* data, int size) {
    if (!hasPeer_) return;
    int n = sendto(sock_, static_cast<const char*>(data), size, 0,
                   reinterpret_cast<const sockaddr*>(&peer_), sizeof peer_);
    int error = n == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (error && error != WSAEWOULDBLOCK && error != lastSendError_)
        Log("network: send failed (%d)", error);
    lastSendError_ = error;
}

bool UdpPeer::Receive(StatePacket& out, const std::function<void(const char*, int)>& other) {
    char buf[1500];
    // Bound work on the game thread even if unrelated traffic floods the port.
    for (int budget = 0; budget < 128; ++budget) {
        sockaddr_in from{};
        int fromLen = sizeof from;
        int n = recvfrom(sock_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEMSGSIZE) continue;
            if (error != WSAEWOULDBLOCK && error != lastReceiveError_)
                Log("network: receive failed (%d)", error);
            lastReceiveError_ = error;
            return false;
        }
        lastReceiveError_ = 0;
        if (hasPeer_ && (from.sin_addr.s_addr != peer_.sin_addr.s_addr ||
                        from.sin_port != peer_.sin_port)) continue;
        if (!DecodeState(buf, n, out)) {
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
