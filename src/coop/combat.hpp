#pragma once
#include "protocol.hpp"
#include <deque>

namespace coop {
class DamageChannel {
public:
    struct Attack { uint32_t id, kind; float damage; double queued; };
    void Configure(uint32_t session, uint32_t consent, const StatePacket& peer, bool enabled) {
        constexpr uint16_t required = kHasPawn | kAlive | kFriendlyFire;
        enabled = enabled && session && consent && peer.session && peer.consent && (peer.flags & required) == required;
        if (session != session_ || consent != consent_ || peer.session != peerSession_ ||
            peer.consent != peerConsent_ || enabled != enabled_) {
            queue_.clear();
            received_ = 0;
        }
        session_ = session;
        consent_ = consent;
        peerSession_ = peer.session;
        peerConsent_ = peer.consent;
        enabled_ = enabled;
    }
    bool Queue(uint32_t kind, float damage, double now) {
        if (!enabled_ || queue_.size() >= 64 || kind < 1 || kind > 2 ||
            !std::isfinite(damage) || damage <= 0 || damage > 1000) return false;
        if (!++next_) ++next_;
        queue_.push_back({next_, kind, damage, now});
        return true;
    }
    bool Expire(double now) {
        bool expired = false;
        while (!queue_.empty() && now - queue_.front().queued > 3) {
            queue_.pop_front();
            expired = true;
        }
        return expired;
    }
    void Fill(StatePacket& packet) const {
        packet.attackId = packet.attackKind = 0;
        packet.damage = 0;
        packet.attackAck = received_;
        packet.targetConsent = peerConsent_;
        if (!enabled_ || queue_.empty()) return;
        const auto& attack = queue_.front();
        packet.attackId = attack.id;
        packet.attackKind = attack.kind;
        packet.damage = attack.damage;
    }
    void Acknowledge(const StatePacket& packet) {
        if (SameScope(packet) && !queue_.empty() && packet.attackAck == queue_.front().id) queue_.pop_front();
    }
    bool Accept(const StatePacket& packet) {
        constexpr uint16_t required = kHasPawn | kAlive | kFriendlyFire;
        if (!enabled_ || !SameScope(packet) || (packet.flags & required) != required ||
            packet.attackKind < 1 || packet.attackKind > 2 || !std::isfinite(packet.damage) ||
            packet.damage <= 0 || packet.damage > 1000 || !packet.attackId ||
            !NewerSequence(packet.attackId, received_)) return false;
        received_ = packet.attackId;
        return true;
    }
    size_t Pending() const { return queue_.size(); }
private:
    bool SameScope(const StatePacket& packet) const {
        return packet.session == peerSession_ && packet.targetSession == session_ &&
            packet.consent == peerConsent_ && packet.targetConsent == consent_;
    }
    uint32_t session_ = 0, consent_ = 0, peerSession_ = 0, peerConsent_ = 0;
    uint32_t next_ = 0, received_ = 0;
    bool enabled_ = false;
    std::deque<Attack> queue_;
};
} // namespace coop
