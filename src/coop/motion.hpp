#pragma once

#include "protocol.hpp"
#include "ue.hpp"

#include <algorithm>
#include <deque>

namespace coop {

struct MotionSnapshot {
    double t;
    ue::FVector pos;
    float yaw;
    ue::FVector vel;
    uint16_t flags;
    ue::FVector bikePos;
    ue::FRotator bikeRotation;
};

inline float AngleDelta(float from, float to) {
    return std::remainder(to - from, 360.f);
}

class MotionBuffer {
public:
    struct Statistics {
        uint64_t interpolated = 0, extrapolated = 0, held = 0, resets = 0;
    };

    void Clear() {
        samples_.clear();
        clockReady_ = false;
        interval_ = 1.0 / 30;
        jitter_ = 0;
        delay_ = 0.1;
    }

    bool Empty() const { return samples_.empty(); }
    const MotionSnapshot& Latest() const { return samples_.back(); }
    double DelaySeconds() const { return delay_; }
    double PlaybackTime() const { return playback_; }
    const Statistics& Stats() const { return stats_; }
    float BikeSpeed() const {
        auto velocity = samples_.empty() ? ue::FVector{} : Tangent(samples_.size() - 1, true);
        return std::sqrt(DistanceSquared(velocity, {}));
    }

    bool Push(const StatePacket& packet, double arrival) {
        if (!std::isfinite(arrival)) return false;
        double time = 0;
        if (!samples_.empty()) {
            uint32_t milliseconds = packet.timeMs - lastTimestamp_;
            if (milliseconds >= 0x80000000u || arrival < lastArrival_) return false;
            if (milliseconds > 2000) {
                Clear();
                ++stats_.resets;
            } else {
                time = samples_.back().t + milliseconds * 0.001;
                if (milliseconds) {
                    double spacing = milliseconds * 0.001;
                    interval_ += 0.05 * (spacing - interval_);
                    jitter_ += 0.05 * (std::fabs((arrival - lastArrival_) - spacing) - jitter_);
                    double target = std::clamp(3 * interval_ + 2 * jitter_, 0.08, 0.25);
                    delay_ += (target > delay_ ? 0.1 : 0.01) * (target - delay_);
                }
            }
        }
        MotionSnapshot value{time, {packet.pos[0], packet.pos[1], packet.pos[2]}, packet.yaw,
            {packet.vel[0], packet.vel[1], packet.vel[2]}, packet.flags,
            {packet.bikePos[0], packet.bikePos[1], packet.bikePos[2]},
            {packet.bikeRotation[0], packet.bikeRotation[1], packet.bikeRotation[2]}};
        if (!samples_.empty()) {
            const auto& previous = samples_.back();
            bool jump = DistanceSquared(previous.pos, value.pos) > 3000.f * 3000.f ||
                ((previous.flags & value.flags & kHasBike) &&
                 DistanceSquared(previous.bikePos, value.bikePos) > 3000.f * 3000.f);
            if (jump) {
                samples_.clear();
                playback_ = time;
                sampledAt_ = arrival;
                clockReady_ = true;
                ++stats_.resets;
            } else if (time == previous.t) samples_.pop_back();
        }
        samples_.push_back(value);
        while (samples_.size() > 96) samples_.pop_front();
        lastTimestamp_ = packet.timeMs;
        lastArrival_ = arrival;
        return true;
    }

    bool Sample(double now, MotionSnapshot& out) {
        if (samples_.empty() || !std::isfinite(now) || now < lastArrival_) return false;
        bool frame = !clockReady_ || now > sampledAt_;
        if (!clockReady_) {
            playback_ = samples_.back().t - delay_;
            sampledAt_ = now;
            clockReady_ = true;
        } else if (now > sampledAt_) {
            double elapsed = now - sampledAt_;
            double target = samples_.back().t + (now - lastArrival_) - delay_;
            double error = target - (playback_ + elapsed);
            if (std::fabs(error) > 0.5) {
                playback_ = target;
                ++stats_.resets;
            } else {
                double rate = std::clamp(1 + error * 0.5, 0.98, 1.02);
                playback_ += elapsed * rate;
            }
            sampledAt_ = now;
        }
        if (playback_ <= samples_.front().t) {
            out = samples_.front();
            out.vel = Tangent(0, false);
            if (frame) ++stats_.held;
            return true;
        }
        for (size_t i = 1; i < samples_.size(); ++i) {
            const auto& a = samples_[i - 1];
            const auto& b = samples_[i];
            if (playback_ > b.t) continue;
            constexpr uint16_t transitions = kHasPawn | kHasBike | kInVehicle | kPassenger;
            if (((a.flags ^ b.flags) & transitions) && playback_ < b.t) {
                out = a;
                out.vel = {};
                if (frame) ++stats_.held;
                return true;
            }
            float duration = static_cast<float>(b.t - a.t);
            float k = static_cast<float>((playback_ - a.t) / duration);
            out = b;
            out.t = playback_;
            auto va = Tangent(i - 1, false), vb = Tangent(i, false);
            out.pos = Cubic(a.pos, b.pos, va, vb, duration, k);
            out.vel = Mix(va, vb, k);
            out.yaw = a.yaw + AngleDelta(a.yaw, b.yaw) * k;
            out.bikePos = Cubic(a.bikePos, b.bikePos, Tangent(i - 1, true), Tangent(i, true), duration, k);
            out.bikeRotation = {a.bikeRotation.pitch + AngleDelta(a.bikeRotation.pitch, b.bikeRotation.pitch) * k,
                a.bikeRotation.yaw + AngleDelta(a.bikeRotation.yaw, b.bikeRotation.yaw) * k,
                a.bikeRotation.roll + AngleDelta(a.bikeRotation.roll, b.bikeRotation.roll) * k};
            if (frame) ++stats_.interpolated;
            return true;
        }
        out = samples_.back();
        float dt = static_cast<float>(std::clamp(playback_ - out.t, 0.0, 0.15));
        out.vel = Tangent(samples_.size() - 1, false);
        out.pos = Add(out.pos, out.vel, dt);
        out.bikePos = Add(out.bikePos, Tangent(samples_.size() - 1, true), dt);
        if (samples_.size() >= 2) {
            const auto& before = samples_[samples_.size() - 2];
            float duration = static_cast<float>(out.t - before.t);
            if (duration > 0) {
                auto turn = [duration, dt](float a, float b) {
                    return b + std::clamp(AngleDelta(a, b) / duration, -360.f, 360.f) * dt;
                };
                out.yaw = turn(before.yaw, out.yaw);
                out.bikeRotation = {turn(before.bikeRotation.pitch, out.bikeRotation.pitch),
                    turn(before.bikeRotation.yaw, out.bikeRotation.yaw), turn(before.bikeRotation.roll, out.bikeRotation.roll)};
            }
        }
        if (frame) {
            if (playback_ - out.t <= 0.15) ++stats_.extrapolated;
            else ++stats_.held;
        }
        return true;
    }

private:
    std::deque<MotionSnapshot> samples_;
    uint32_t lastTimestamp_ = 0;
    double lastArrival_ = 0, sampledAt_ = 0, playback_ = 0;
    double interval_ = 1.0 / 30, jitter_ = 0, delay_ = 0.1;
    bool clockReady_ = false;
    Statistics stats_;

    static float DistanceSquared(ue::FVector a, ue::FVector b) {
        float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x * x + y * y + z * z;
    }

    static ue::FVector Add(ue::FVector a, ue::FVector v, float t) {
        return {a.x + v.x * t, a.y + v.y * t, a.z + v.z * t};
    }

    static ue::FVector Mix(ue::FVector a, ue::FVector b, float t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    ue::FVector Tangent(size_t index, bool bike) const {
        const auto& sample = samples_[index];
        size_t left = index ? index - 1 : index, right = std::min(index + 1, samples_.size() - 1);
        uint16_t role = bike ? kHasBike : kHasPawn | kInVehicle | kPassenger;
        if ((samples_[left].flags ^ sample.flags) & role) left = index;
        if ((samples_[right].flags ^ sample.flags) & role) right = index;
        double duration = samples_[right].t - samples_[left].t;
        if (duration <= 0) return {};
        const auto& a = bike ? samples_[left].bikePos : samples_[left].pos;
        const auto& b = bike ? samples_[right].bikePos : samples_[right].pos;
        ue::FVector velocity{static_cast<float>((b.x - a.x) / duration),
            static_cast<float>((b.y - a.y) / duration), static_cast<float>((b.z - a.z) / duration)};
        if (!bike && !(sample.flags & kInVehicle)) {
            velocity = sample.vel;
            if (DistanceSquared(a, b) < 0.01f) velocity = {};
        }
        float speed = std::sqrt(DistanceSquared(velocity, {}));
        if (speed > 7000) {
            float scale = 7000 / speed;
            velocity = {velocity.x * scale, velocity.y * scale, velocity.z * scale};
        }
        return velocity;
    }

    static ue::FVector Cubic(ue::FVector a, ue::FVector b, ue::FVector va, ue::FVector vb, float dt, float t) {
        auto axis = [dt, t](float a0, float b0, float v0, float v1) {
            float delta = b0 - a0;
            if (std::fabs(delta) < 1e-5f) return a0;
            float m0 = std::clamp(v0 * dt / delta, 0.f, 3.f);
            float m1 = std::clamp(v1 * dt / delta, 0.f, 3.f);
            float norm = std::sqrt(m0 * m0 + m1 * m1);
            if (norm > 3) { m0 *= 3 / norm; m1 *= 3 / norm; }
            float t2 = t * t, t3 = t2 * t;
            return a0 + delta * ((-2 * t3 + 3 * t2) + (t3 - 2 * t2 + t) * m0 + (t3 - t2) * m1);
        };
        return {axis(a.x, b.x, va.x, vb.x), axis(a.y, b.y, va.y, vb.y), axis(a.z, b.z, va.z, vb.z)};
    }
};

} // namespace coop
