#pragma once

#include "sensors-protocol.hpp"

namespace sensors {

// engine-service is the sole source of direction; a lost engine connection
// immediately disables the simulated camera and invalidates its old epoch.
class Activation {
public:
    void engine_availability(bool online) {
        if (engine_online_ == online)
            return;
        engine_online_ = online;
        update(CameraState::Fault);
    }

    void engine_reverse(bool reverse) {
        if (!engine_online_)
            return;
        update(reverse ? CameraState::Active : CameraState::Inactive);
    }

    CameraState state() const { return state_; }
    bool active() const { return state_ == CameraState::Active; }
    std::uint32_t epoch() const { return epoch_; }

private:
    void update(CameraState next) {
        if (next != state_) {
            state_ = next;
            ++epoch_;
        }
    }
    bool engine_online_ = false;
    CameraState state_ = CameraState::Fault;
    std::uint32_t epoch_ = 0;
};

} // namespace sensors
