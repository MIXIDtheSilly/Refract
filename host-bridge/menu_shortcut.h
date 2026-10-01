#pragma once
#include "pose_frame.h"
#include <cstdint>

namespace refract::host {
// Use a monotonic host clock; guest prediction times can jump during recentering.
class MenuShortcut {
public:
    void reset() { holding_ = false; }

    void apply(refract::protocol::ControllerInput (&hands)[2], uint64_t nowNs) {
        using namespace refract::protocol;
        const bool chord = hands[0].active && hands[1].active &&
            (hands[0].buttons & StickClick) && (hands[1].buttons & StickClick);
        if (!chord) { reset(); return; }
        if (!holding_ || nowNs < startedNs_) {
            holding_ = true;
            startedNs_ = nowNs;
        }
        if (nowNs - startedNs_ < 500000000ULL) return;
        hands[0].buttons |= MenuClick;
        // Once recognized, hold menu until release rather than repeatedly pulsing it.
        hands[0].buttons &= ~StickClick;
        hands[1].buttons &= ~StickClick;
    }
private:
    bool holding_ = false;
    uint64_t startedNs_ = 0;
};

// Holding Y and B together for a second toggles the performance panel, once per hold. The game still
// sees both buttons; few games give that chord a meaning, and none while it's held that long.
class StatsShortcut {
public:
    bool toggled(const refract::protocol::ControllerInput (&hands)[2], uint64_t nowNs) {
        using namespace refract::protocol;
        const bool chord = hands[0].active && hands[1].active &&
            (hands[0].buttons & SecondaryClick) && (hands[1].buttons & SecondaryClick);
        if (!chord) { holding_ = fired_ = false; return false; }
        if (!holding_ || nowNs < startedNs_) {
            holding_ = true;
            startedNs_ = nowNs;
        }
        if (fired_ || nowNs - startedNs_ < 1000000000ULL) return false;
        fired_ = true;
        return true;
    }
private:
    bool holding_ = false, fired_ = false;
    uint64_t startedNs_ = 0;
};
}
