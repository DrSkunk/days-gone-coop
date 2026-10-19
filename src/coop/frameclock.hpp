#pragma once

#include <algorithm>
#include <cmath>

namespace coop {

// Converts the variable moment at which the plugin runs inside a frame into a
// steady per-frame time. Engine real time advances once per frame; the wall
// clock only corrects slow drift. The result never moves backwards.
class FrameClock {
public:
    double Update(double wall, double engine, bool engineValid) {
        if (!std::isfinite(wall)) return now_;
        double predicted = wall;
        bool stepped = false;
        if (ready_ && engineValid && haveEngine_ && std::isfinite(engine)) {
            double delta = engine - engine_;
            if (delta > 0 && delta < 0.25) {
                predicted = now_ + delta;
                stepped = true;
            }
        }
        if (stepped) {
            double error = wall - predicted;
            if (std::fabs(error) > 0.1) {
                predicted = wall;
                ++resyncs_;
            } else {
                predicted += 0.05 * error;
            }
        } else if (ready_) {
            ++fallbacks_;
        }
        haveEngine_ = engineValid && std::isfinite(engine);
        if (haveEngine_) engine_ = engine;
        now_ = ready_ ? std::max(now_, predicted) : predicted;
        ready_ = true;
        return now_;
    }

    double Now() const { return now_; }
    unsigned long long Resyncs() const { return resyncs_; }
    unsigned long long Fallbacks() const { return fallbacks_; }

private:
    double now_ = 0, engine_ = 0;
    bool ready_ = false, haveEngine_ = false;
    unsigned long long resyncs_ = 0, fallbacks_ = 0;
};

} // namespace coop
